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
#  include "A2STR.h"
#  include "DlAbortEx.h"
#  include "GroupId.h"
#  include "MessageDigest.h"
#  include "Option.h"
#  include "RequestGroup.h"
#  include "RequestGroupMan.h"
#  include "SessionSerializer.h"
#  include "util.h"
#  include "Sqlite3PersistenceStore.h"
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
    "SELECT serialized,gid,state FROM task ORDER BY queue_position ASC";

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

void writeHttpResumeContext(sqlite3* db,
                            const std::shared_ptr<RequestGroup>& rg)
{
  const auto& context = rg->getHttpResumeContext();
  if (!context)
    return;
  StmtGuard stmt;
  const char* sql =
      "INSERT INTO task_http_context VALUES (?,?,?,?,?,?,?)"
      " ON CONFLICT(gid) DO UPDATE SET source_hash=excluded.source_hash,"
      " resource_hash=excluded.resource_hash, output_path=excluded.output_path,"
      " validator_kind=excluded.validator_kind, validator=excluded.validator,"
      " total_length=excluded.total_length";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt.stmt, nullptr) != SQLITE_OK) {
    throw DL_ABORT_EX("sqlite3-persistence: prepare HTTP context failed");
  }
  bindText(stmt, 1, GroupId::toHex(rg->getGID()));
  bindText(stmt, 2, context->sourceHash);
  bindText(stmt, 3, context->resourceHash);
  bindText(stmt, 4, context->outputPath);
  bindText(stmt, 5, context->validatorKind);
  bindText(stmt, 6, context->validator);
  sqlite3_bind_int64(stmt, 7, context->totalLength);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    throw DL_ABORT_EX("sqlite3-persistence: save HTTP context failed");
  }
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
  // active+reserved+stopped set may be orphaned, except explicitly retained
  // failures: FIFO eviction does not retire their request context. Its
  // checkpoint, if any, is kept while the data file exists so a front-end retry
  // can resume it.
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
                       "DELETE FROM task WHERE state != 'error' AND gid NOT IN "
                       "(SELECT gid FROM "
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
    std::string sql =
        "DELETE FROM task WHERE state != 'error' AND gid NOT IN (";
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
      const auto loadedBefore = out.size();
      const auto gid = columnText(stmt, 1);
      if (!restoreLegacyTorrentTask(*store_, gid, text, op, out)) {
        std::stringstream input(text);
        createRequestGroupForUriList(out, op, input);
      }
      if (columnText(stmt, 2) == "error") {
        for (auto i = loadedBefore; i < out.size(); ++i) {
          // Failure is not permission to start again during engine startup.
          out[i]->setPauseRequested(true);
        }
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
    restoreHttpResumeContext(out[i]);
    if (out[i]->getHttpResumeContext()) {
      constrainHttpRetryOptions(*out[i]->getOption());
    }
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
    writeHttpResumeContext(db, rg);
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

std::string Sqlite3SessionStore::httpUriHash(const std::string& uri)
{
  auto digest = MessageDigest::create("sha-256");
  digest->update(uri.data(), uri.size());
  return util::toHex(digest->digest());
}

void Sqlite3SessionStore::restoreHttpResumeContext(
    const std::shared_ptr<RequestGroup>& rg)
{
  StmtGuard stmt;
  const char* sql = "SELECT source_hash,resource_hash,output_path,"
                    "validator_kind,validator,total_length"
                    " FROM task_http_context WHERE gid=?";
  if (sqlite3_prepare_v2(store_->raw(), sql, -1, &stmt.stmt, nullptr) !=
      SQLITE_OK) {
    throw DL_ABORT_EX("sqlite3-persistence: read HTTP context failed");
  }
  bindText(stmt, 1, GroupId::toHex(rg->getGID()));
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE)
    return;
  if (rc != SQLITE_ROW) {
    throw DL_ABORT_EX("sqlite3-persistence: read HTTP context failed");
  }
  auto context = std::make_shared<HttpResumeContext>();
  context->sourceHash = columnText(stmt, 0);
  context->resourceHash = columnText(stmt, 1);
  context->outputPath = columnText(stmt, 2);
  context->validatorKind = columnText(stmt, 3);
  context->validator = columnText(stmt, 4);
  context->totalLength = sqlite3_column_int64(stmt, 5);
  if (context->sourceHash.size() != 64 || context->resourceHash.size() != 64 ||
      context->outputPath.empty() || context->totalLength <= 0 ||
      context->validator.empty() || context->validator.size() > 512 ||
      (context->validatorKind != "etag" &&
       context->validatorKind != "last-modified")) {
    throw DL_ABORT_EX("sqlite3-persistence: invalid HTTP context");
  }
  rg->setHttpResumeContext(std::move(context));
}

std::shared_ptr<RequestGroup>
Sqlite3SessionStore::loadHttpRetry(const std::shared_ptr<GroupId>& gid,
                                   const std::shared_ptr<Option>& op)
{
  StmtGuard stmt;
  if (sqlite3_prepare_v2(store_->raw(),
                         "SELECT serialized FROM task WHERE gid=?", -1,
                         &stmt.stmt, nullptr) != SQLITE_OK) {
    throw DL_ABORT_EX("sqlite3-persistence: read retry task failed");
  }
  bindText(stmt, 1, gid->toHex());
  const auto rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE)
    return nullptr;
  if (rc != SQLITE_ROW)
    throw DL_ABORT_EX("sqlite3-persistence: read retry task failed");
  std::istringstream input(columnText(stmt, 0));
  std::ostringstream filtered;
  std::string line;
  while (std::getline(input, line)) {
    auto start = line.find_first_not_of(" \t");
    if (start != std::string::npos && start > 0 &&
        line.compare(start, 4, "gid=") == 0)
      continue;
    filtered << line << '\n';
  }
  std::istringstream serialized(filtered.str());
  std::vector<std::shared_ptr<RequestGroup>> groups;
  createRequestGroupForUriList(groups, op, serialized);
  if (groups.size() != 1)
    return nullptr;
  auto rg = groups.front();
  rg->adoptRetryGroupId(gid);
  restoreTaskCookies(rg);
  restoreHttpResumeContext(rg);
  constrainHttpRetryOptions(*rg->getOption());
  return rg;
}

void Sqlite3SessionStore::constrainHttpRetryOptions(Option& option)
{
  option.put(PREF_ALLOW_OVERWRITE, A2_V_FALSE);
  option.put(PREF_AUTO_FILE_RENAMING, A2_V_FALSE);
  option.put(PREF_ALWAYS_RESUME, A2_V_TRUE);
  option.put(PREF_CONTINUE, A2_V_FALSE);
  option.put(PREF_CONDITIONAL_GET, A2_V_FALSE);
  option.put(PREF_CHECK_INTEGRITY, A2_V_FALSE);
  option.put(PREF_HTTP_RESUME_CONTEXT, A2_V_TRUE);
  std::istringstream input(option.get(PREF_HEADER));
  std::string line, headers;
  while (std::getline(input, line)) {
    if (util::istartsWith(line, "If-Modified-Since:") ||
        util::istartsWith(line, "If-None-Match:") ||
        util::istartsWith(line, "If-Range:") ||
        util::istartsWith(line, "If-Match:") ||
        util::istartsWith(line, "Range:"))
      continue;
    headers += line + '\n';
  }
  option.put(PREF_HEADER, headers);
}

void Sqlite3SessionStore::commitHttpRetry(
    const std::shared_ptr<RequestGroup>& rg)
{
  store_->withTransaction([&]() {
    upsertTask(rg, true, true);
    StmtGuard stmt;
    if (sqlite3_prepare_v2(store_->raw(),
                           "DELETE FROM download_history WHERE gid=?", -1,
                           &stmt.stmt, nullptr) != SQLITE_OK) {
      throw DL_ABORT_EX(
          "sqlite3-persistence: prepare retry history retirement failed");
    }
    bindText(stmt, 1, GroupId::toHex(rg->getGID()));
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      throw DL_ABORT_EX("sqlite3-persistence: retry history retirement failed");
    }
  });
}

void Sqlite3SessionStore::retainFailedHttpTask(
    const std::shared_ptr<RequestGroup>& rg)
{
  store_->withTransaction([&]() {
    // Flush the final jar too, including a response rotation whose earlier
    // immediate write failed. Cookie snapshot and paused recovery intent commit
    // together; startup must not run the old request between those writes.
    upsertTask(rg, true, true);
    StmtGuard stmt;
    if (sqlite3_prepare_v2(store_->raw(),
                           "UPDATE task SET state='error' WHERE gid=?", -1,
                           &stmt.stmt, nullptr) != SQLITE_OK) {
      throw DL_ABORT_EX("sqlite3-persistence: prepare failed HTTP task failed");
    }
    bindText(stmt, 1, GroupId::toHex(rg->getGID()));
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      throw DL_ABORT_EX("sqlite3-persistence: retain failed HTTP task failed");
    }
  });
}

void Sqlite3SessionStore::purgeStoppedTasks(
    const std::vector<a2_gid_t>& liveGids,
    const std::vector<a2_gid_t>& stoppedGids)
{
  store_->withTransaction([&]() {
    auto* db = store_->raw();
    if (sqlite3_exec(
            db,
            "CREATE TEMP TABLE IF NOT EXISTS purge_live_tasks(gid TEXT PRIMARY "
            "KEY);"
            "CREATE TEMP TABLE IF NOT EXISTS purge_stopped_tasks(gid TEXT "
            "PRIMARY KEY);"
            "DELETE FROM purge_live_tasks; DELETE FROM purge_stopped_tasks;",
            nullptr, nullptr, nullptr) != SQLITE_OK) {
      throw DL_ABORT_EX("sqlite3-persistence: prepare stopped purge failed");
    }
    const auto populate = [&](const char* sql,
                              const std::vector<a2_gid_t>& gids) {
      StmtGuard stmt;
      if (sqlite3_prepare_v2(db, sql, -1, &stmt.stmt, nullptr) != SQLITE_OK)
        throw DL_ABORT_EX("sqlite3-persistence: prepare purge owners failed");
      for (auto gid : gids) {
        sqlite3_reset(stmt);
        bindText(stmt, 1, GroupId::toHex(gid));
        if (sqlite3_step(stmt) != SQLITE_DONE)
          throw DL_ABORT_EX("sqlite3-persistence: save purge owners failed");
      }
    };
    populate("INSERT OR IGNORE INTO purge_live_tasks VALUES (?)", liveGids);
    populate("INSERT OR IGNORE INTO purge_stopped_tasks VALUES (?)",
             stoppedGids);
    if (sqlite3_exec(
            db,
            "DELETE FROM task WHERE gid NOT IN (SELECT gid FROM "
            "purge_live_tasks)"
            " AND (state='error' OR gid IN (SELECT gid FROM download_history)"
            " OR gid IN (SELECT gid FROM purge_stopped_tasks));"
            "DELETE FROM download_history;"
            "DELETE FROM purge_live_tasks; DELETE FROM purge_stopped_tasks;",
            nullptr, nullptr, nullptr) != SQLITE_OK) {
      throw DL_ABORT_EX("sqlite3-persistence: stopped purge failed");
    }
  });
}

void Sqlite3SessionStore::deleteHttpResumeContext(const std::string& gidHex)
{
  store_->withTransaction([&]() {
    StmtGuard stmt;
    if (sqlite3_prepare_v2(store_->raw(),
                           "DELETE FROM task_http_context WHERE gid=?", -1,
                           &stmt.stmt, nullptr) != SQLITE_OK) {
      throw DL_ABORT_EX(
          "sqlite3-persistence: prepare HTTP context retirement failed");
    }
    bindText(stmt, 1, gidHex);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      throw DL_ABORT_EX("sqlite3-persistence: HTTP context retirement failed");
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
