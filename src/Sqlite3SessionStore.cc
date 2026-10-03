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
#include <iterator>
#include <sstream>
#include <vector>

#include <sqlite3.h>

#include "Cookie.h"
#include "CookieStorage.h"
#include "DlAbortEx.h"
#include "GroupId.h"
#include "MessageDigest.h"
#include "Option.h"
#include "RequestGroup.h"
#include "RequestGroupMan.h"
#include "SessionSerializer.h"
#include "Sqlite3PersistenceStore.h"
#  include "LegacyCheckpointImport.h"
#  include "download_helper.h"
#  include "fmt.h"
#  include "prefs.h"

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
    "SELECT serialized,gid FROM task ORDER BY queue_position ASC";

const char* const kShiftForwardSql =
    "UPDATE task SET queue_position = queue_position - 1"
    " WHERE queue_position > ? AND queue_position <= ?";

const char* const kShiftBackwardSql =
    "UPDATE task SET queue_position = queue_position + 1"
    " WHERE queue_position >= ? AND queue_position < ?";

const char* const kPlaceTaskPositionSql =
    "UPDATE task SET queue_position = ? WHERE gid = ?";

const char* const kUpsertTaskCookieContextSql =
    "INSERT INTO task_cookie_context (gid, updated_at) VALUES (?, ?)"
    " ON CONFLICT(gid) DO UPDATE SET updated_at = excluded.updated_at";

const char* const kDeleteTaskCookiesSql =
    "DELETE FROM task_cookie_context WHERE gid = ?";

const char* const kInsertTaskCookieSql =
    "INSERT INTO task_cookie"
    " (gid, name, value, domain, path, host_only, secure, http_only,"
    "  persistent, expires_at_unix_s, creation_time_unix_s,"
    "  last_access_time_unix_s)"
    " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

const char* const kSelectTaskCookieContextSql =
    "SELECT 1 FROM task_cookie_context WHERE gid = ?";

const char* const kSelectTaskCookiesSql =
    "SELECT name, value, domain, path, host_only, secure, http_only,"
    " persistent, expires_at_unix_s, creation_time_unix_s,"
    " last_access_time_unix_s"
    " FROM task_cookie WHERE gid = ?";

const char* const kDeleteExpiredTaskCookiesSql =
    "DELETE FROM task_cookie"
    " WHERE gid = ? AND persistent = 1 AND expires_at_unix_s < ?";

void bindText(sqlite3_stmt* stmt, int index, const std::string& value)
{
  sqlite3_bind_text(stmt, index, value.data(), static_cast<int>(value.size()),
                    SQLITE_TRANSIENT);
}

void replaceTaskCookiesInTransaction(
    sqlite3* db, const std::string& gidHex,
    const std::shared_ptr<CookieStorage>& storage, int64_t nowMs)
{
  if (!storage) {
    throw DL_ABORT_EX("sqlite3-persistence: missing task cookie storage");
  }

  {
    StmtGuard stmt;
    if (sqlite3_prepare_v2(db, kUpsertTaskCookieContextSql, -1, &stmt.stmt,
                           nullptr) != SQLITE_OK) {
      throw DL_ABORT_EX(fmt(
          "sqlite3-persistence: prepare UPSERT task cookie context failed: %s",
          sqlite3_errmsg(db)));
    }
    bindText(stmt, 1, gidHex);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(nowMs));
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      throw DL_ABORT_EX(fmt(
          "sqlite3-persistence: UPSERT task cookie context failed: %s",
          sqlite3_errmsg(db)));
    }
  }

  {
    StmtGuard stmt;
    if (sqlite3_prepare_v2(db, "DELETE FROM task_cookie WHERE gid = ?", -1,
                           &stmt.stmt, nullptr) != SQLITE_OK) {
      throw DL_ABORT_EX(fmt(
          "sqlite3-persistence: prepare DELETE task cookies failed: %s",
          sqlite3_errmsg(db)));
    }
    bindText(stmt, 1, gidHex);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      throw DL_ABORT_EX(fmt(
          "sqlite3-persistence: DELETE task cookies failed: %s",
          sqlite3_errmsg(db)));
    }
  }

  std::vector<const Cookie*> cookies;
  storage->dumpCookie(std::back_inserter(cookies));
  if (cookies.empty()) {
    return;
  }

  StmtGuard stmt;
  if (sqlite3_prepare_v2(db, kInsertTaskCookieSql, -1, &stmt.stmt, nullptr) !=
      SQLITE_OK) {
    throw DL_ABORT_EX(fmt(
        "sqlite3-persistence: prepare INSERT task cookie failed: %s",
        sqlite3_errmsg(db)));
  }

  const auto now = static_cast<time_t>(nowMs / 1000);
  for (const Cookie* cookie : cookies) {
    if (cookie->isExpired(now)) {
      continue;
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    bindText(stmt, 1, gidHex);
    bindText(stmt, 2, cookie->getName());
    bindText(stmt, 3, cookie->getValue());
    bindText(stmt, 4, cookie->getDomain());
    bindText(stmt, 5, cookie->getPath());
    sqlite3_bind_int(stmt, 6, cookie->getHostOnly() ? 1 : 0);
    sqlite3_bind_int(stmt, 7, cookie->getSecure() ? 1 : 0);
    sqlite3_bind_int(stmt, 8, cookie->getHttpOnly() ? 1 : 0);
    sqlite3_bind_int(stmt, 9, cookie->getPersistent() ? 1 : 0);
    sqlite3_bind_int64(stmt, 10, cookie->getExpiryTime());
    sqlite3_bind_int64(stmt, 11, cookie->getCreationTime());
    sqlite3_bind_int64(stmt, 12, cookie->getLastAccessTime());
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      throw DL_ABORT_EX(fmt(
          "sqlite3-persistence: INSERT task cookie failed: %s",
          sqlite3_errmsg(db)));
    }
  }
}

std::string columnText(sqlite3_stmt* stmt, int index)
{
  const auto* value =
      reinterpret_cast<const char*>(sqlite3_column_text(stmt, index));
  return value ? value : "";
}

bool taskCookieContextExists(sqlite3* db, const std::string& gidHex)
{
  StmtGuard stmt;
  if (sqlite3_prepare_v2(db, kSelectTaskCookieContextSql, -1, &stmt.stmt,
                         nullptr) != SQLITE_OK) {
    throw DL_ABORT_EX(fmt(
        "sqlite3-persistence: prepare SELECT task cookie context failed: %s",
        sqlite3_errmsg(db)));
  }
  bindText(stmt, 1, gidHex);
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    return true;
  }
  if (rc == SQLITE_DONE) {
    return false;
  }
  throw DL_ABORT_EX(fmt(
      "sqlite3-persistence: SELECT task cookie context failed: %s",
      sqlite3_errmsg(db)));
}

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
  // rows. (task_progress is path-addressed and independent of task rows
  // since schema v3; see Sqlite3BtProgressInfoFile::pruneDefunct.)
  for (const auto& rg : rgman->getRequestGroups()) {
    upsertTask(rg, false);
    liveGids.push_back(rg->getGID());
  }
  for (const auto& rg : rgman->getReservedGroups()) {
    upsertTask(rg, false);
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
  // with a RG / DownloadResult in this session. Its checkpoint, if any, is
  // kept while the data file exists so a front-end retry can resume it.
  removeOrphanTasks(liveGids);
}

void Sqlite3SessionStore::removeOrphanTasks(
    const std::vector<a2_gid_t>& liveGids)
{
  sqlite3* db = store_->raw();

  store_->withTransaction([&]() {
    if (liveGids.empty()) {
      // Keep invalidated metadata bindings as durable recovery evidence.
      if (sqlite3_exec(db,
                       "DELETE FROM task WHERE gid NOT IN (SELECT gid FROM "
                       "legacy_torrent_metadata WHERE invalidated=1)",
                       nullptr, nullptr, nullptr) != SQLITE_OK) {
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
    sql += ") AND gid NOT IN (SELECT gid FROM legacy_torrent_metadata "
           "WHERE invalidated=1)";

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

  const auto firstLoaded = out.size();
  int rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char* text =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    if (text) {
      const auto gid = columnText(stmt, 1);
      if (!restoreLegacyTorrentTask(*store_, gid, text, op, out)) {
        std::stringstream input(text);
        createRequestGroupForUriList(out, op, input);
      }
    }
  }
  if (rc != SQLITE_DONE) {
    throw DL_ABORT_EX(
        fmt("sqlite3-persistence: SELECT serialized step failed: %s",
            sqlite3_errmsg(db)));
  }

  for (auto i = firstLoaded; i < out.size(); ++i) {
    restoreTaskCookies(out[i]);
  }
}

void Sqlite3SessionStore::upsertTask(const std::shared_ptr<RequestGroup>& rg,
                                     bool persistTaskCookieSnapshot,
                                     bool useExistingTransaction)
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
  const bool requiresTaskCookies =
      rg->getOption()->getAsBool(PREF_REQUIRE_TASK_COOKIES);
  bool persistedTaskCookies = false;

  auto persist = [&]() {
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

    if (requiresTaskCookies) {
      if (rg->getTaskCookieStorage() &&
          (persistTaskCookieSnapshot ||
           dirtyTaskCookieGids_.count(gidHex) != 0 ||
           !taskCookieContextExists(db, gidHex))) {
        // Periodic task saves avoid rewriting an unchanged jar. A missing
        // context is still initialized here so derived tasks cannot lose
        // their inherited cookies before their first HTTP response.
        replaceTaskCookiesInTransaction(db, gidHex,
                                        rg->getTaskCookieStorage(), now);
        persistedTaskCookies = true;
      }
    }
    else {
      StmtGuard cookieStmt;
      if (sqlite3_prepare_v2(db, kDeleteTaskCookiesSql, -1,
                             &cookieStmt.stmt, nullptr) != SQLITE_OK) {
        throw DL_ABORT_EX(fmt(
            "sqlite3-persistence: prepare DELETE task cookie context failed: %s",
            sqlite3_errmsg(db)));
      }
      bindText(cookieStmt, 1, gidHex);
      if (sqlite3_step(cookieStmt) != SQLITE_DONE) {
        throw DL_ABORT_EX(fmt(
            "sqlite3-persistence: DELETE task cookie context failed: %s",
            sqlite3_errmsg(db)));
      }
    }
  };
  if (useExistingTransaction) {
    if (sqlite3_get_autocommit(db)) {
      throw DL_ABORT_EX(
          "sqlite3-persistence: existing task transaction required");
    }
    persist();
  }
  else {
    store_->withTransaction(persist);
  }
  if (persistedTaskCookies || !requiresTaskCookies) {
    dirtyTaskCookieGids_.erase(gidHex);
  }
}

void Sqlite3SessionStore::replaceTaskCookies(
    const std::string& gidHex,
    const std::shared_ptr<CookieStorage>& storage)
{
  sqlite3* db = store_->raw();
  const auto now = currentUnixMs();
  store_->withTransaction(
      [&]() { replaceTaskCookiesInTransaction(db, gidHex, storage, now); });
  dirtyTaskCookieGids_.erase(gidHex);
}

void Sqlite3SessionStore::markTaskCookiesDirty(const std::string& gidHex)
{
  dirtyTaskCookieGids_.insert(gidHex);
}

void Sqlite3SessionStore::deleteTaskCookies(const std::string& gidHex)
{
  sqlite3* db = store_->raw();
  store_->withTransaction([&]() {
    StmtGuard stmt;
    if (sqlite3_prepare_v2(db, kDeleteTaskCookiesSql, -1, &stmt.stmt,
                           nullptr) != SQLITE_OK) {
      throw DL_ABORT_EX(fmt(
          "sqlite3-persistence: prepare DELETE task cookie context failed: %s",
          sqlite3_errmsg(db)));
    }
    bindText(stmt, 1, gidHex);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      throw DL_ABORT_EX(fmt(
          "sqlite3-persistence: DELETE task cookie context failed: %s",
          sqlite3_errmsg(db)));
    }
  });
  dirtyTaskCookieGids_.erase(gidHex);
}

void Sqlite3SessionStore::restoreTaskCookies(
    const std::shared_ptr<RequestGroup>& rg)
{
  if (!rg->getOption()->getAsBool(PREF_REQUIRE_TASK_COOKIES)) {
    return;
  }

  sqlite3* db = store_->raw();
  const auto gidHex = GroupId::toHex(rg->getGID());
  {
    StmtGuard stmt;
    if (sqlite3_prepare_v2(db, kSelectTaskCookieContextSql, -1, &stmt.stmt,
                           nullptr) != SQLITE_OK) {
      throw DL_ABORT_EX(fmt(
          "sqlite3-persistence: prepare SELECT task cookie context failed: %s",
          sqlite3_errmsg(db)));
    }
    bindText(stmt, 1, gidHex);
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
      return;
    }
    if (rc != SQLITE_ROW) {
      throw DL_ABORT_EX(fmt(
          "sqlite3-persistence: SELECT task cookie context failed: %s",
          sqlite3_errmsg(db)));
    }
  }

  const auto now = static_cast<time_t>(currentUnixMs() / 1000);
  store_->withTransaction([&]() {
    StmtGuard expiredStmt;
    if (sqlite3_prepare_v2(db, kDeleteExpiredTaskCookiesSql, -1,
                           &expiredStmt.stmt, nullptr) != SQLITE_OK) {
      throw DL_ABORT_EX(fmt(
          "sqlite3-persistence: prepare DELETE expired task cookies failed: %s",
          sqlite3_errmsg(db)));
    }
    bindText(expiredStmt, 1, gidHex);
    sqlite3_bind_int64(expiredStmt, 2, static_cast<sqlite3_int64>(now));
    if (sqlite3_step(expiredStmt) != SQLITE_DONE) {
      throw DL_ABORT_EX(fmt(
          "sqlite3-persistence: DELETE expired task cookies failed: %s",
          sqlite3_errmsg(db)));
    }
  });

  auto storage = std::make_shared<CookieStorage>();
  StmtGuard stmt;
  if (sqlite3_prepare_v2(db, kSelectTaskCookiesSql, -1, &stmt.stmt, nullptr) !=
      SQLITE_OK) {
    throw DL_ABORT_EX(fmt(
        "sqlite3-persistence: prepare SELECT task cookies failed: %s",
        sqlite3_errmsg(db)));
  }
  bindText(stmt, 1, gidHex);

  int rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    auto cookie = make_unique<Cookie>(
        columnText(stmt, 0), columnText(stmt, 1),
        static_cast<time_t>(sqlite3_column_int64(stmt, 8)),
        sqlite3_column_int(stmt, 7) != 0, columnText(stmt, 2),
        sqlite3_column_int(stmt, 4) != 0, columnText(stmt, 3),
        sqlite3_column_int(stmt, 5) != 0,
        sqlite3_column_int(stmt, 6) != 0,
        static_cast<time_t>(sqlite3_column_int64(stmt, 9)));
    cookie->setLastAccessTime(
        static_cast<time_t>(sqlite3_column_int64(stmt, 10)));
    storage->store(std::move(cookie), now);
  }
  if (rc != SQLITE_DONE) {
    throw DL_ABORT_EX(fmt(
        "sqlite3-persistence: SELECT task cookies failed: %s",
        sqlite3_errmsg(db)));
  }
  rg->setTaskCookieStorage(std::move(storage));
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
  dirtyTaskCookieGids_.erase(gidHex);
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
