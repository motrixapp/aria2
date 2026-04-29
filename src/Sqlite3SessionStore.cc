/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2026 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#include "Sqlite3SessionStore.h"

#ifdef HAVE_SQLITE3

#include <chrono>
#include <sstream>

#include <sqlite3.h>

#include "DlAbortEx.h"
#include "GroupId.h"
#include "MessageDigest.h"
#include "Option.h"
#include "RequestGroup.h"
#include "RequestGroupMan.h"
#include "SessionSerializer.h"
#include "Sqlite3PersistenceStore.h"
#include "download_helper.h"
#include "fmt.h"
#include "prefs.h"

namespace aria2 {

namespace {

// RAII wrapper for sqlite3_stmt.
struct StmtGuard {
  sqlite3_stmt* stmt{nullptr};
  ~StmtGuard()
  {
    if (stmt) {
      sqlite3_finalize(stmt);
    }
  }
  operator sqlite3_stmt*() { return stmt; }
};

// Returns current Unix time in milliseconds.
int64_t currentUnixMs()
{
  return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

const char* const kInsertTaskSql =
    "INSERT INTO task"
    " (gid, state, serialized, queue_position, digest, created_at, updated_at)"
    " VALUES (?, ?, ?, ?, ?, ?, ?)";

const char* const kUpsertTaskSql =
    "INSERT INTO task"
    " (gid, state, serialized, queue_position, digest, created_at, updated_at,"
    "  bt_local_path)"
    " VALUES (?, ?, ?,"
    "  COALESCE((SELECT MAX(queue_position)+1 FROM task), 0),"
    "  ?, ?, ?, ?)"
    " ON CONFLICT(gid) DO UPDATE SET"
    "  state         = excluded.state,"
    "  serialized    = excluded.serialized,"
    "  digest        = excluded.digest,"
    "  updated_at    = excluded.updated_at,"
    "  bt_local_path = excluded.bt_local_path";

const char* const kDeleteTaskSql = "DELETE FROM task WHERE gid = ?";

const char* const kUpdateTaskStateSql =
    "UPDATE task SET state = ?, updated_at = ? WHERE gid = ?";

const char* const kSelectQueuePosSql =
    "SELECT queue_position FROM task WHERE gid = ?";

const char* const kSelectSerializedSql =
    "SELECT serialized FROM task ORDER BY queue_position ASC";

const char* const kShiftForwardSql =
    "UPDATE task SET queue_position = queue_position - 1"
    " WHERE queue_position > ? AND queue_position <= ?";

const char* const kShiftBackwardSql =
    "UPDATE task SET queue_position = queue_position + 1"
    " WHERE queue_position >= ? AND queue_position < ?";

const char* const kPlaceTaskPositionSql =
    "UPDATE task SET queue_position = ? WHERE gid = ?";

} // namespace

Sqlite3SessionStore::Sqlite3SessionStore(Sqlite3PersistenceStore* store)
    : store_(store)
{
}

Sqlite3SessionStore::~Sqlite3SessionStore() = default;

void Sqlite3SessionStore::saveAllTasks(RequestGroupMan* rgman)
{
  // Collect gids of all RGs that should remain in the table after this save.
  std::vector<a2_gid_t> liveGids;
  liveGids.reserve(rgman->getRequestGroups().size() +
                   rgman->getReservedGroups().size() +
                   rgman->getDownloadResults().size());

  // Step 1: UPSERT each active + reserved RG. upsertTask preserves
  // created_at and queue_position on conflict (Task 17), and the INSERT path
  // appends via COALESCE(MAX(queue_position)+1, 0). No DELETE on existing
  // rows means no CASCADE on task_progress.
  for (const auto& rg : rgman->getRequestGroups()) {
    upsertTask(rg);
    liveGids.push_back(rg->getGID());
  }
  for (const auto& rg : rgman->getReservedGroups()) {
    upsertTask(rg);
    liveGids.push_back(rg->getGID());
  }

  // Step 1.5: count `downloadResults_` (and unfinishedDownloadResults_) as
  // live too. At shutdown, `onEndOfRun → removeStoppedGroup` moves
  // currently-active RGs into `downloadResults_`. The transition site at
  // RequestGroupMan.cc:466 already handles per-task deletion when
  // `--force-save=false` via deleteTask(). With `--force-save=true` the
  // user wants those rows preserved across restart, but they're no longer
  // in active/reserved — so the orphan-removal pass below would
  // incorrectly delete them. Including them in liveGids keeps the
  // periodic-save semantics (orphan rows still get cleaned up) while
  // preventing the shutdown race from wiping in-flight tasks.
  for (const auto& dr : rgman->getDownloadResults()) {
    if (dr && dr->gid) {
      liveGids.push_back(dr->gid->getNumericId());
    }
  }
  for (const auto& dr : rgman->getUnfinishedDownloadResult()) {
    if (dr && dr->gid) {
      liveGids.push_back(dr->gid->getNumericId());
    }
  }

  // Step 2: orphan removal. Any task row whose gid is no longer in the
  // active+reserved+stopped set represents a row that was never paired
  // with a RG / DownloadResult in this session — CASCADE on its
  // task_progress is then correct.
  removeOrphanTasks(liveGids);
}

void Sqlite3SessionStore::removeOrphanTasks(
    const std::vector<a2_gid_t>& liveGids)
{
  sqlite3* db = store_->raw();

  store_->withTransaction([&]() {
    if (liveGids.empty()) {
      // No live tasks → delete all task rows.
      if (sqlite3_exec(db, "DELETE FROM task", nullptr, nullptr, nullptr) !=
          SQLITE_OK) {
        throw DL_ABORT_EX(
            fmt("sqlite3-persistence: DELETE FROM task failed: %s",
                sqlite3_errmsg(db)));
      }
      return;
    }

    // Build "DELETE FROM task WHERE gid NOT IN (?, ?, ...)" with N
    // placeholders.
    std::string sql = "DELETE FROM task WHERE gid NOT IN (";
    for (size_t i = 0; i < liveGids.size(); ++i) {
      sql += (i == 0 ? "?" : ", ?");
    }
    sql += ")";

    StmtGuard stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt.stmt, nullptr) !=
        SQLITE_OK) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: prepare orphan DELETE failed: %s",
              sqlite3_errmsg(db)));
    }

    // gidHex strings must outlive the bind/step pair. Hold them in a vector.
    std::vector<std::string> gidHexHolder;
    gidHexHolder.reserve(liveGids.size());
    for (auto gid : liveGids) {
      gidHexHolder.push_back(GroupId::toHex(gid));
    }
    for (size_t i = 0; i < gidHexHolder.size(); ++i) {
      sqlite3_bind_text(stmt, static_cast<int>(i + 1),
                        gidHexHolder[i].data(),
                        static_cast<int>(gidHexHolder[i].size()),
                        SQLITE_STATIC);
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: orphan DELETE failed: %s",
              sqlite3_errmsg(db)));
    }
  });
}

void Sqlite3SessionStore::loadActiveTasksInto(
    std::vector<std::shared_ptr<RequestGroup>>& out,
    const std::shared_ptr<Option>& op)
{
  sqlite3* db = store_->raw();

  StmtGuard stmt;
  if (sqlite3_prepare_v2(db, kSelectSerializedSql, -1, &stmt.stmt, nullptr) !=
      SQLITE_OK) {
    throw DL_ABORT_EX(
        fmt("sqlite3-persistence: prepare SELECT serialized failed: %s",
            sqlite3_errmsg(db)));
  }

  std::string combined;
  int rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char* text =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    if (text) {
      combined += text;
    }
  }
  if (rc != SQLITE_DONE) {
    throw DL_ABORT_EX(
        fmt("sqlite3-persistence: SELECT serialized step failed: %s",
            sqlite3_errmsg(db)));
  }

  if (combined.empty()) {
    return;
  }

  std::stringstream ss(combined);
  createRequestGroupForUriList(out, op, ss);
}

void Sqlite3SessionStore::upsertTask(const std::shared_ptr<RequestGroup>& rg)
{
  // Option A: pass nullptr — renderOneInto never dereferences rgman_.
  SessionSerializer ser(nullptr);
  std::string text = ser.renderOne(rg);
  if (text.empty()) {
    return;
  }
  std::string state = rg->isPauseRequested() ? "paused" : "waiting";

  auto md = MessageDigest::sha1();
  md->update(text.data(), text.size());
  md->update(state.data(), state.size());
  std::string digest = md->digest();

  const int64_t now = currentUnixMs();
  auto gidHex = GroupId::toHex(rg->getGID());

  sqlite3* db = store_->raw();

  store_->withTransaction([&]() {
    StmtGuard stmt;
    if (sqlite3_prepare_v2(db, kUpsertTaskSql, -1, &stmt.stmt, nullptr) !=
        SQLITE_OK) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: prepare UPSERT task failed: %s",
              sqlite3_errmsg(db)));
    }
    sqlite3_bind_text(stmt, 1, gidHex.data(),
                      static_cast<int>(gidHex.size()), SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, state.data(),
                      static_cast<int>(state.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, text.data(),
                      static_cast<int>(text.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 4, digest.data(),
                      static_cast<int>(digest.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(now));
    sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(now));

    const std::string& torrentFile = rg->getOption()->get(PREF_TORRENT_FILE);
    const std::string& metalinkFile = rg->getOption()->get(PREF_METALINK_FILE);
    const std::string& btLocalPath =
        !torrentFile.empty() ? torrentFile : metalinkFile;
    if (btLocalPath.empty()) {
      sqlite3_bind_null(stmt, 7);
    }
    else {
      sqlite3_bind_text(stmt, 7, btLocalPath.data(),
                        static_cast<int>(btLocalPath.size()),
                        SQLITE_TRANSIENT);
    }

    if (sqlite3_step(stmt) != SQLITE_DONE) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: UPSERT task failed: %s",
              sqlite3_errmsg(db)));
    }
  });
}

void Sqlite3SessionStore::deleteTask(const std::string& gidHex)
{
  sqlite3* db = store_->raw();

  store_->withTransaction([&]() {
    StmtGuard stmt;
    if (sqlite3_prepare_v2(db, kDeleteTaskSql, -1, &stmt.stmt, nullptr) !=
        SQLITE_OK) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: prepare DELETE task failed: %s",
              sqlite3_errmsg(db)));
    }
    sqlite3_bind_text(stmt, 1, gidHex.data(),
                      static_cast<int>(gidHex.size()), SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: DELETE task failed: %s",
              sqlite3_errmsg(db)));
    }
  });
}

void Sqlite3SessionStore::updateTaskState(const std::string& gidHex,
                                         const std::string& state)
{
  const int64_t now = currentUnixMs();
  sqlite3* db = store_->raw();

  store_->withTransaction([&]() {
    StmtGuard stmt;
    if (sqlite3_prepare_v2(db, kUpdateTaskStateSql, -1, &stmt.stmt, nullptr) !=
        SQLITE_OK) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: prepare UPDATE task state failed: %s",
              sqlite3_errmsg(db)));
    }
    sqlite3_bind_text(stmt, 1, state.data(),
                      static_cast<int>(state.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(now));
    sqlite3_bind_text(stmt, 3, gidHex.data(),
                      static_cast<int>(gidHex.size()), SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: UPDATE task state failed: %s",
              sqlite3_errmsg(db)));
    }
  });
}

void Sqlite3SessionStore::moveTaskPosition(const std::string& gidHex,
                                          int newPos)
{
  sqlite3* db = store_->raw();

  store_->withTransaction([&]() {
    // 1) Read oldPos
    int oldPos = -1;
    {
      StmtGuard stmt;
      if (sqlite3_prepare_v2(db, kSelectQueuePosSql, -1, &stmt.stmt,
                             nullptr) != SQLITE_OK) {
        throw DL_ABORT_EX(
            fmt("sqlite3-persistence: prepare SELECT queue_position failed: %s",
                sqlite3_errmsg(db)));
      }
      sqlite3_bind_text(stmt, 1, gidHex.data(),
                        static_cast<int>(gidHex.size()), SQLITE_STATIC);
      int rc = sqlite3_step(stmt);
      if (rc == SQLITE_DONE) {
        return; // gid not found — no-op
      }
      if (rc != SQLITE_ROW) {
        throw DL_ABORT_EX(
            fmt("sqlite3-persistence: SELECT queue_position step failed: %s",
                sqlite3_errmsg(db)));
      }
      oldPos = sqlite3_column_int(stmt, 0);
    }

    if (oldPos == newPos) {
      return; // already in place — no-op
    }

    // 2) Shift the affected range
    {
      const bool movingForward = oldPos < newPos;
      const char* shiftSql = movingForward ? kShiftForwardSql : kShiftBackwardSql;
      const int lower = movingForward ? oldPos : newPos;
      const int upper = movingForward ? newPos : oldPos;

      StmtGuard stmt;
      if (sqlite3_prepare_v2(db, shiftSql, -1, &stmt.stmt, nullptr) !=
          SQLITE_OK) {
        throw DL_ABORT_EX(
            fmt("sqlite3-persistence: prepare shift UPDATE failed: %s",
                sqlite3_errmsg(db)));
      }
      sqlite3_bind_int(stmt, 1, lower);
      sqlite3_bind_int(stmt, 2, upper);

      if (sqlite3_step(stmt) != SQLITE_DONE) {
        throw DL_ABORT_EX(
            fmt("sqlite3-persistence: shift UPDATE failed: %s",
                sqlite3_errmsg(db)));
      }
    }

    // 3) Place the moved row
    {
      StmtGuard stmt;
      if (sqlite3_prepare_v2(db, kPlaceTaskPositionSql, -1, &stmt.stmt, nullptr) !=
          SQLITE_OK) {
        throw DL_ABORT_EX(
            fmt("sqlite3-persistence: prepare place UPDATE failed: %s",
                sqlite3_errmsg(db)));
      }
      sqlite3_bind_int(stmt, 1, newPos);
      sqlite3_bind_text(stmt, 2, gidHex.data(),
                        static_cast<int>(gidHex.size()), SQLITE_STATIC);
      if (sqlite3_step(stmt) != SQLITE_DONE) {
        throw DL_ABORT_EX(
            fmt("sqlite3-persistence: place UPDATE failed: %s",
                sqlite3_errmsg(db)));
      }
    }
  });
}

} // namespace aria2

#endif // HAVE_SQLITE3
