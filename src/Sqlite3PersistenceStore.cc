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
#include "Sqlite3PersistenceStore.h"

#ifdef HAVE_SQLITE3

#include <cstdio>
#include <ctime>
#include <string>

#include "DlAbortEx.h"
#include "fmt.h"
#include "LogFactory.h"
#include "Logger.h"
#include "Sqlite3Migrations.h"

namespace aria2 {

namespace {
static const char* kPragmas[] = {
    "PRAGMA journal_mode = WAL;",
    "PRAGMA synchronous = NORMAL;",
    "PRAGMA foreign_keys = ON;",
    "PRAGMA busy_timeout = 5000;",
    "PRAGMA temp_store = MEMORY;",
    "PRAGMA cache_size = -8000;",
};

bool runQuickCheck(sqlite3* db)
{
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "PRAGMA quick_check;", -1, &stmt, nullptr) !=
      SQLITE_OK) {
    return false;
  }
  bool ok = false;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char* result =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    if (result && std::string(result) == "ok") {
      ok = true;
    }
  }
  sqlite3_finalize(stmt);
  return ok;
}

} // namespace

Sqlite3PersistenceStore::Sqlite3PersistenceStore(std::string dbPath)
    : dbPath_(std::move(dbPath)), db_(nullptr), commitCounter_(0)
{
}

Sqlite3PersistenceStore::~Sqlite3PersistenceStore()
{
  sqlite3_close_v2(db_);
}

void Sqlite3PersistenceStore::open()
{
  int ret = sqlite3_open_v2(dbPath_.c_str(), &db_,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                            nullptr);
  if (ret != SQLITE_OK) {
    std::string errMsg = sqlite3_errmsg(db_);
    sqlite3_close_v2(db_);
    db_ = nullptr;
    throw DL_ABORT_EX(
        fmt("sqlite3-persistence: failed to open %s: %s", dbPath_.c_str(),
            errMsg.c_str()));
  }
  // Try to apply pragmas and run quick_check. If either fails (e.g., WAL
  // replay on a corrupt body fails pragma execution) treat as corruption.
  bool needsRecovery = false;
  try {
    applyPragmas();
    if (!runQuickCheck(db_)) {
      needsRecovery = true;
    }
  }
  catch (...) {
    needsRecovery = true;
  }

  if (needsRecovery) {
    sqlite3_close_v2(db_);
    db_ = nullptr;
    auto ts = std::to_string(static_cast<long long>(std::time(nullptr)));
    std::string corruptPath = dbPath_ + ".corrupt." + ts;
    if (std::rename(dbPath_.c_str(), corruptPath.c_str()) != 0) {
      throw DL_ABORT_EX(fmt(
          "sqlite3-persistence: corrupt db at '%s' could not be renamed to '%s'",
          dbPath_.c_str(), corruptPath.c_str()));
    }
    // Remove WAL/SHM siblings so SQLite starts fully fresh.
    std::remove((corruptPath + "-wal").c_str());
    std::remove((corruptPath + "-shm").c_str());
    std::remove((dbPath_ + "-wal").c_str());
    std::remove((dbPath_ + "-shm").c_str());
    A2_LOG_NOTICE(fmt("sqlite3-persistence: detected corruption at '%s'; "
                      "renamed to '%s'; starting with a fresh database.",
                      dbPath_.c_str(), corruptPath.c_str()));
    int rc = sqlite3_open_v2(dbPath_.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                             nullptr);
    if (rc != SQLITE_OK) {
      std::string errMsg = sqlite3_errmsg(db_);
      sqlite3_close_v2(db_);
      db_ = nullptr;
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: failed to recreate db: %s",
              errMsg.c_str()));
    }
    applyPragmas();
  }

  migrateIfNeeded(*this);
}

void Sqlite3PersistenceStore::applyPragmas()
{
  for (const char* pragma : kPragmas) {
    char* errmsg = nullptr;
    int ret = sqlite3_exec(db_, pragma, nullptr, nullptr, &errmsg);
    if (ret != SQLITE_OK) {
      std::string errstr;
      if (errmsg) {
        errstr = errmsg;
        sqlite3_free(errmsg);
      }
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: PRAGMA failed: %s", errstr.c_str()));
    }
  }
}

std::string Sqlite3PersistenceStore::queryPragma(const std::string& name) const
{
  std::string sql = "PRAGMA " + name + ";";
  sqlite3_stmt* stmt = nullptr;
  int ret = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (ret != SQLITE_OK) {
    throw DL_ABORT_EX(
        fmt("sqlite3-persistence: failed to prepare PRAGMA %s: %s",
            name.c_str(), sqlite3_errmsg(db_)));
  }
  std::string result;
  int stepRet = sqlite3_step(stmt);
  if (stepRet == SQLITE_ROW) {
    const char* val =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    if (val) {
      result = val;
    }
  }
  else {
    sqlite3_finalize(stmt);
    throw DL_ABORT_EX(
        fmt("sqlite3-persistence: PRAGMA %s returned no row (rc=%d): %s",
            name.c_str(), stepRet, sqlite3_errmsg(db_)));
  }
  sqlite3_finalize(stmt);
  return result;
}

void Sqlite3PersistenceStore::withTransaction(const std::function<void()>& fn)
{
  char* errmsg = nullptr;
  int ret = sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &errmsg);
  if (ret != SQLITE_OK) {
    std::string errstr;
    if (errmsg) {
      errstr = errmsg;
      sqlite3_free(errmsg);
    }
    throw DL_ABORT_EX(
        fmt("sqlite3-persistence: BEGIN IMMEDIATE failed: %s", errstr.c_str()));
  }
  try {
    fn();
  }
  catch (...) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    throw;
  }
  errmsg = nullptr;
  ret = sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &errmsg);
  if (ret != SQLITE_OK) {
    std::string errstr;
    if (errmsg) {
      errstr = errmsg;
      sqlite3_free(errmsg);
    }
    // SQLite auto-rolls-back on failed COMMIT; this is defensive belt-and-braces.
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    throw DL_ABORT_EX(
        fmt("sqlite3-persistence: COMMIT failed: %s", errstr.c_str()));
  }
  ++commitCounter_;
  maybeCheckpoint();
}

void Sqlite3PersistenceStore::maybeCheckpoint()
{
  if (commitCounter_ % 64 != 0) {
    return;
  }
  // TODO: also trigger checkpoint when WAL file size > 8 MB (future opt)
  int nLog = 0;
  int nCkpt = 0;
  int ret = sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_TRUNCATE,
                                      &nLog, &nCkpt);
  if (ret != SQLITE_OK) {
    A2_LOG_WARN(fmt("sqlite3-persistence: WAL checkpoint failed: %s",
                    sqlite3_errmsg(db_)));
  }
}

void Sqlite3PersistenceStore::finalCheckpointAndClose()
{
  if (db_ == nullptr) {
    return;
  }
  int ret = sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_TRUNCATE,
                                      nullptr, nullptr);
  if (ret != SQLITE_OK) {
    A2_LOG_WARN(fmt("sqlite3-persistence: final WAL checkpoint failed: %s",
                    sqlite3_errmsg(db_)));
  }
  sqlite3_close_v2(db_);
  db_ = nullptr;
}

} // namespace aria2

#endif // HAVE_SQLITE3
