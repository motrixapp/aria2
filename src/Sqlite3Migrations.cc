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
#include "Sqlite3Migrations.h"

#ifdef HAVE_SQLITE3

#include "Sqlite3PersistenceStore.h"
#include "DlAbortEx.h"
#include "fmt.h"

#include <chrono>
#include <stdexcept>
#include <string>

#include <sqlite3.h>

namespace aria2 {

namespace {

static const char* const kSchemaV1Sqls[] = {
    // 1. meta table
    "CREATE TABLE meta ("
    "  key   TEXT PRIMARY KEY,"
    "  value TEXT NOT NULL"
    ");",

    // 2. task table
    "CREATE TABLE task ("
    "  gid             TEXT    PRIMARY KEY,"
    "  state           TEXT    NOT NULL,"
    "  serialized      TEXT    NOT NULL,"
    "  belongs_to      TEXT,"
    "  following       TEXT,"
    "  followed_by     TEXT,"
    "  metadata_uri    TEXT,"
    "  bt_local_path   TEXT,"
    "  queue_position  INTEGER NOT NULL,"
    "  digest          BLOB    NOT NULL,"
    "  created_at      INTEGER NOT NULL,"
    "  updated_at      INTEGER NOT NULL"
    ");",

    "CREATE INDEX idx_task_state          ON task(state);",
    "CREATE INDEX idx_task_queue_position ON task(queue_position);",

    // 3. task_progress table
    "CREATE TABLE task_progress ("
    "  gid              TEXT    PRIMARY KEY,"
    "  ctrl_version     INTEGER NOT NULL DEFAULT 1,"
    "  is_torrent       INTEGER NOT NULL DEFAULT 0,"
    "  info_hash        BLOB,"
    "  piece_length     INTEGER NOT NULL,"
    "  total_length     INTEGER NOT NULL,"
    "  upload_length    INTEGER NOT NULL DEFAULT 0,"
    "  bitfield         BLOB    NOT NULL,"
    "  in_flight_blob   BLOB    NOT NULL DEFAULT X'',"
    "  digest           BLOB    NOT NULL,"
    "  updated_at       INTEGER NOT NULL,"
    "  FOREIGN KEY (gid) REFERENCES task(gid) ON DELETE CASCADE"
    ");",

    // 4. download_history table
    "CREATE TABLE download_history ("
    "  id               INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  gid              TEXT    NOT NULL,"
    "  status           TEXT    NOT NULL,"
    "  result_code      INTEGER NOT NULL,"
    "  result_message   TEXT    NOT NULL DEFAULT '',"
    "  total_length     INTEGER NOT NULL,"
    "  completed_length INTEGER NOT NULL,"
    "  upload_length    INTEGER NOT NULL DEFAULT 0,"
    "  num_pieces       INTEGER NOT NULL DEFAULT 0,"
    "  piece_length     INTEGER NOT NULL DEFAULT 0,"
    "  bitfield         BLOB,"
    "  info_hash        BLOB,"
    "  dir              TEXT    NOT NULL DEFAULT '',"
    "  belongs_to       TEXT,"
    "  following        TEXT,"
    "  followed_by      TEXT,"
    "  in_memory        INTEGER NOT NULL DEFAULT 0,"
    "  serialized       TEXT    NOT NULL DEFAULT '',"
    "  metadata_uri     TEXT,"
    "  bt_name          TEXT,"
    "  bt_announce_list TEXT,"
    "  bt_comment       TEXT,"
    "  bt_creation_date INTEGER,"
    "  bt_mode          TEXT,"
    "  bt_is_private    INTEGER NOT NULL DEFAULT 0,"
    "  bt_local_path    TEXT,"
    "  finished_at      INTEGER NOT NULL"
    ");",

    "CREATE INDEX idx_history_finished_at ON download_history(finished_at DESC);",
    "CREATE INDEX idx_history_status      ON download_history(status, finished_at DESC);",
    "CREATE INDEX idx_history_gid         ON download_history(gid);",
    "CREATE INDEX idx_history_info_hash   ON download_history(info_hash) WHERE info_hash IS NOT NULL;",

    // 5. download_history_files table
    "CREATE TABLE download_history_files ("
    "  history_id  INTEGER NOT NULL,"
    "  file_index  INTEGER NOT NULL,"
    "  path        TEXT    NOT NULL,"
    "  length      INTEGER NOT NULL,"
    "  selected    INTEGER NOT NULL DEFAULT 1,"
    "  PRIMARY KEY (history_id, file_index),"
    "  FOREIGN KEY (history_id) REFERENCES download_history(id) ON DELETE CASCADE"
    ");",

    "CREATE INDEX idx_history_files_path ON download_history_files(path);",

    // 6. download_history_file_uris table
    "CREATE TABLE download_history_file_uris ("
    "  history_id  INTEGER NOT NULL,"
    "  file_index  INTEGER NOT NULL,"
    "  uri         TEXT    NOT NULL,"
    "  status      TEXT    NOT NULL,"
    "  PRIMARY KEY (history_id, file_index, uri),"
    "  FOREIGN KEY (history_id) REFERENCES download_history(id) ON DELETE CASCADE"
    ");",

    "CREATE INDEX idx_history_file_uris_uri ON download_history_file_uris(uri);",
};

void migrate_v0_to_v1(Sqlite3PersistenceStore& store)
{
  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();

  store.withTransaction([&]() {
    // Execute all CREATE TABLE + CREATE INDEX statements.
    for (const char* sql : kSchemaV1Sqls) {
      char* errmsg = nullptr;
      int rc = sqlite3_exec(store.raw(), sql, nullptr, nullptr, &errmsg);
      if (rc != SQLITE_OK) {
        std::string errstr;
        if (errmsg) {
          errstr = errmsg;
          sqlite3_free(errmsg);
        }
        throw DL_ABORT_EX(fmt(
            "sqlite3-persistence: migration step failed: %s", errstr.c_str()));
      }
    }

    // Seed meta table (static literal seeds).
    const char* seedSqls[] = {
        "INSERT INTO meta(key, value) VALUES('schema_version', '1')",
        "INSERT INTO meta(key, value) VALUES('last_clean_shutdown', '0')",
    };
    for (const char* sql : seedSqls) {
      char* errmsg = nullptr;
      int rc = sqlite3_exec(store.raw(), sql, nullptr, nullptr, &errmsg);
      if (rc != SQLITE_OK) {
        std::string errstr;
        if (errmsg) {
          errstr = errmsg;
          sqlite3_free(errmsg);
        }
        throw DL_ABORT_EX(fmt(
            "sqlite3-persistence: migration step failed: %s", errstr.c_str()));
      }
    }

    // Seed aria2_version with a bound parameter (defensive against any
    // single-quote characters that could appear in PACKAGE_VERSION).
    {
      sqlite3_stmt* stmt = nullptr;
      int rc = sqlite3_prepare_v2(
          store.raw(),
          "INSERT INTO meta(key, value) VALUES('aria2_version', ?)",
          -1, &stmt, nullptr);
      if (rc != SQLITE_OK) {
        throw DL_ABORT_EX(fmt(
            "sqlite3-persistence: migration step failed: %s",
            sqlite3_errmsg(store.raw())));
      }
      sqlite3_bind_text(stmt, 1, PACKAGE_VERSION, -1, SQLITE_STATIC);
      rc = sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      if (rc != SQLITE_DONE) {
        throw DL_ABORT_EX(fmt(
            "sqlite3-persistence: migration step failed: %s",
            sqlite3_errmsg(store.raw())));
      }
    }

    // Seed created_at with a bound parameter.
    {
      sqlite3_stmt* stmt = nullptr;
      int rc = sqlite3_prepare_v2(
          store.raw(),
          "INSERT INTO meta(key, value) VALUES('created_at', ?)",
          -1, &stmt, nullptr);
      if (rc != SQLITE_OK) {
        throw DL_ABORT_EX(fmt(
            "sqlite3-persistence: migration step failed: %s",
            sqlite3_errmsg(store.raw())));
      }
      sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(now_ms));
      rc = sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      if (rc != SQLITE_DONE) {
        throw DL_ABORT_EX(fmt(
            "sqlite3-persistence: migration step failed: %s",
            sqlite3_errmsg(store.raw())));
      }
    }

    // Set user_version to 1 inside the transaction.
    {
      char* errmsg = nullptr;
      int rc = sqlite3_exec(store.raw(), "PRAGMA user_version = 1;",
                            nullptr, nullptr, &errmsg);
      if (rc != SQLITE_OK) {
        std::string errstr;
        if (errmsg) {
          errstr = errmsg;
          sqlite3_free(errmsg);
        }
        throw DL_ABORT_EX(fmt(
            "sqlite3-persistence: migration step failed: %s", errstr.c_str()));
      }
    }
  });
}

struct Migration {
  int from;
  int to;
  void (*fn)(Sqlite3PersistenceStore&);
};

static const Migration kMigrations[] = {
    {0, 1, &migrate_v0_to_v1},
    // future: {1, 2, &migrate_v1_to_v2},
};

} // namespace

void migrateIfNeeded(Sqlite3PersistenceStore& store)
{
  int v;
  try {
    v = std::stoi(store.queryPragma("user_version"));
  }
  catch (const std::invalid_argument&) {
    throw DL_ABORT_EX(
        "sqlite3-persistence: PRAGMA user_version returned non-numeric value");
  }
  catch (const std::out_of_range&) {
    throw DL_ABORT_EX(
        "sqlite3-persistence: PRAGMA user_version out of int range");
  }

  if (v == kCurrentSchemaVersion) {
    return;
  }

  if (v > kCurrentSchemaVersion) {
    throw DL_ABORT_EX(fmt(
        "sqlite3-persistence: DB schema version %d is newer than this build "
        "supports (%d); refusing to open.",
        v, kCurrentSchemaVersion));
  }

  // v < kCurrentSchemaVersion: run migrations step by step.
  while (v < kCurrentSchemaVersion) {
    bool found = false;
    for (const auto& m : kMigrations) {
      if (m.from == v && m.to == v + 1) {
        m.fn(store);
        v = m.to;
        found = true;
        break;
      }
    }
    if (!found) {
      throw DL_ABORT_EX(fmt(
          "sqlite3-persistence: no migration path from schema version %d to %d.",
          v, v + 1));
    }
  }
}

} // namespace aria2

#endif // HAVE_SQLITE3
