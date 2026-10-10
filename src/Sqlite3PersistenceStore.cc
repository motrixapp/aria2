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
#include "a2io.h"
#include "fmt.h"
#include "LogFactory.h"
#include "Logger.h"
#include "Sqlite3BtProgressInfoFile.h"
#include "Sqlite3Migrations.h"
#include "util.h"

namespace aria2 {

namespace {
static const char* kPragmas[] = {
    "PRAGMA journal_mode = WAL;",
    "PRAGMA synchronous = NORMAL;",
    "PRAGMA foreign_keys = ON;",
    // Scrub deleted cookie payloads from active database pages while avoiding
    // the full freelist rewrite cost of secure_delete=ON.
    "PRAGMA secure_delete = FAST;",
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
  // Before any session restore reads the table: reclaim checkpoints that no
  // task owns and whose data file is gone (see pruneDefunct).
  Sqlite3BtProgressInfoFile::pruneDefunct(*this);
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

void Sqlite3PersistenceStore::backupBeforeMigration(int schemaVersion)
{
  // Copy through SQLite so committed WAL pages are included. Never replace a
  // previous snapshot: a failed migration can be retried against newer data.
  const auto prefix = dbPath_ + ".pre-schema" +
                      std::to_string(kCurrentSchemaVersion) + "-v" +
                      std::to_string(schemaVersion) + ".";
  std::string backupPath;
  int fd = -1;
  for (unsigned int attempt = 0; attempt < 1000; ++attempt) {
    backupPath = prefix + std::to_string(static_cast<long long>(std::time(nullptr))) +
                 "." + std::to_string(attempt) + ".db";
    fd = ::a2open(utf8ToWPath(backupPath).c_str(),
                O_RDWR | O_CREAT | O_EXCL | O_BINARY, S_IRUSR | S_IWUSR);
    if (fd >= 0) {
      break;
    }
    if (errno != EEXIST) {
      throw DL_ABORT_EX("sqlite3-persistence: cannot create pre-migration backup; "
                        "database was not upgraded");
    }
  }
  if (fd < 0) {
    throw DL_ABORT_EX("sqlite3-persistence: pre-migration backup names exhausted");
  }

  sqlite3* destination = nullptr;
  try {
    if (sqlite3_open_v2(backupPath.c_str(), &destination,
                        SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK ||
        sqlite3_exec(destination, "PRAGMA synchronous=FULL;", nullptr,
                      nullptr, nullptr) != SQLITE_OK) {
      throw DL_ABORT_EX("sqlite3-persistence: cannot open pre-migration backup");
    }
    auto* backup = sqlite3_backup_init(destination, "main", db_, "main");
    if (!backup) {
      throw DL_ABORT_EX("sqlite3-persistence: cannot initialize pre-migration backup");
    }
    const int stepResult = sqlite3_backup_step(backup, -1);
    const int finishResult = sqlite3_backup_finish(backup);
    if (stepResult != SQLITE_DONE || finishResult != SQLITE_OK) {
      throw DL_ABORT_EX("sqlite3-persistence: pre-migration backup failed; "
                        "database was not upgraded");
    }
    // Backup copies the source header, including its WAL journal mode. Reopen
    // before switching modes so SQLite does not use the empty target's cached
    // DELETE mode and leave a WAL-marked header in the supposedly standalone DB.
    sqlite3_close_v2(destination);
    destination = nullptr;
    if (sqlite3_open_v2(backupPath.c_str(), &destination,
                        SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK ||
        sqlite3_exec(destination, "PRAGMA synchronous=FULL; "
                                  "PRAGMA journal_mode=DELETE;", nullptr,
                      nullptr, nullptr) != SQLITE_OK ||
        !runQuickCheck(destination)) {
      throw DL_ABORT_EX("sqlite3-persistence: cannot finalize pre-migration backup");
    }
    sqlite3_close_v2(destination);
    destination = nullptr;
    // The initial backup handle can leave an orphan shared-memory file after
    // its copied WAL header is converted. Both destination handles are closed.
    a2unlink(utf8ToWPath((backupPath + "-wal")).c_str());
    a2unlink(utf8ToWPath((backupPath + "-shm")).c_str());
#ifndef __MINGW32__
    if (fsync(fd) != 0) {
#else
    if (_commit(fd) != 0) {
#endif
      throw DL_ABORT_EX("sqlite3-persistence: cannot sync pre-migration backup");
    }
#ifndef __MINGW32__
    // Persist the directory entry before allowing schema changes.
    const auto separator = backupPath.find_last_of('/');
    const auto directory = separator == std::string::npos
                               ? std::string(".")
                               : backupPath.substr(0, separator + 1);
    const int directoryFd = ::open(directory.c_str(), O_RDONLY);
    if (directoryFd < 0) {
      throw DL_ABORT_EX("sqlite3-persistence: cannot open backup directory");
    }
    const int syncResult = fsync(directoryFd);
    ::close(directoryFd);
    if (syncResult != 0) {
      throw DL_ABORT_EX("sqlite3-persistence: cannot sync backup directory");
    }
#endif
    ::close(fd);
    fd = -1;
    A2_LOG_NOTICE(fmt("sqlite3-persistence: saved schema %d backup at '%s' "
                      "before upgrading to schema %d", schemaVersion,
                      backupPath.c_str(), kCurrentSchemaVersion));
  }
  catch (...) {
    sqlite3_close_v2(destination);
    if (fd >= 0) {
      ::close(fd);
    }
    a2unlink(utf8ToWPath(backupPath).c_str());
    a2unlink(utf8ToWPath((backupPath + "-wal")).c_str());
    a2unlink(utf8ToWPath((backupPath + "-shm")).c_str());
    a2unlink(utf8ToWPath((backupPath + "-journal")).c_str());
    throw;
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
