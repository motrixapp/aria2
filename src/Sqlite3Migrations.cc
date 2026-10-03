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

void migrate_v1_to_v2(Sqlite3PersistenceStore& store)
{
  static const char* const sqls[] = {
      // The context row deliberately exists even when the cookie list is
      // empty. An empty task-scoped jar is materially different from a
      // missing jar: the former must remain isolated after restart, while the
      // latter must keep failing closed.
      "CREATE TABLE task_cookie_context ("
      "  gid        TEXT    PRIMARY KEY,"
      "  updated_at INTEGER NOT NULL,"
      "  FOREIGN KEY (gid) REFERENCES task(gid) ON DELETE CASCADE"
      ");",

      "CREATE TABLE task_cookie ("
      "  gid                     TEXT    NOT NULL,"
      "  name                    TEXT    NOT NULL,"
      "  value                   TEXT    NOT NULL,"
      "  domain                  TEXT    NOT NULL,"
      "  path                    TEXT    NOT NULL,"
      "  host_only               INTEGER NOT NULL,"
      "  secure                  INTEGER NOT NULL,"
      "  http_only               INTEGER NOT NULL,"
      "  persistent              INTEGER NOT NULL,"
      "  expires_at_unix_s       INTEGER NOT NULL,"
      "  creation_time_unix_s    INTEGER NOT NULL,"
      "  last_access_time_unix_s INTEGER NOT NULL,"
      "  PRIMARY KEY (gid, name, domain, path),"
      "  FOREIGN KEY (gid) REFERENCES task_cookie_context(gid)"
      "    ON DELETE CASCADE"
      ");",

      "CREATE INDEX idx_task_cookie_expiry"
      " ON task_cookie(gid, persistent, expires_at_unix_s);",

      "UPDATE meta SET value = '2' WHERE key = 'schema_version';",
      "PRAGMA user_version = 2;",
  };

  store.withTransaction([&]() {
    for (const char* sql : sqls) {
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
  });
}

void migrate_v2_to_v3(Sqlite3PersistenceStore& store)
{
  // Address checkpoints by output path, and detach them from `task`.
  //
  // v2 keyed task_progress by gid with ON DELETE CASCADE into task. That made
  // the checkpoint die with its task row: removeDownloadResult on an errored
  // download deleted the row and, through the cascade, the only record of
  // which pieces were already on disk. A front-end retry also re-adds the
  // download under a new gid, which a gid-keyed row can never match. The
  // `.aria2` control file this table replaces has neither problem — it is
  // named after the output file and outlives the task — so v3 restores those
  // semantics (Motrix#2187).
  //
  // Existing rows keep out_path = NULL and remain reachable by gid; the first
  // save of each rewrites it path-addressed. Unique out_path still admits any
  // number of NULLs.
  static const char* const sqls[] = {
      "CREATE TABLE task_progress_v3 ("
      "  gid              TEXT    NOT NULL,"
      "  out_path         TEXT,"
      "  ctrl_version     INTEGER NOT NULL DEFAULT 1,"
      "  is_torrent       INTEGER NOT NULL DEFAULT 0,"
      "  info_hash        BLOB,"
      "  piece_length     INTEGER NOT NULL,"
      "  total_length     INTEGER NOT NULL,"
      "  upload_length    INTEGER NOT NULL DEFAULT 0,"
      "  bitfield         BLOB    NOT NULL,"
      "  in_flight_blob   BLOB    NOT NULL DEFAULT X'',"
      "  digest           BLOB    NOT NULL,"
      "  updated_at       INTEGER NOT NULL"
      ");",

      "INSERT INTO task_progress_v3"
      " (gid, out_path, ctrl_version, is_torrent, info_hash, piece_length,"
      "  total_length, upload_length, bitfield, in_flight_blob, digest,"
      "  updated_at)"
      " SELECT gid, NULL, ctrl_version, is_torrent, info_hash, piece_length,"
      "        total_length, upload_length, bitfield, in_flight_blob, digest,"
      "        updated_at"
      " FROM task_progress;",

      "DROP TABLE task_progress;",
      "ALTER TABLE task_progress_v3 RENAME TO task_progress;",

      "CREATE UNIQUE INDEX idx_task_progress_out_path"
      " ON task_progress(out_path);",
      "CREATE INDEX idx_task_progress_gid ON task_progress(gid);",

      "UPDATE meta SET value = '3' WHERE key = 'schema_version';",
      "PRAGMA user_version = 3;",
  };

  store.withTransaction([&]() {
    for (const char* sql : sqls) {
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
  });
}

void migrate_v3_to_v4(Sqlite3PersistenceStore& store)
{
  store.withTransaction([&]() {
    const char* sql =
        "CREATE TABLE legacy_checkpoint_import ("
        " token TEXT PRIMARY KEY, fingerprint TEXT NOT NULL, gid TEXT NOT NULL,"
        " target_path TEXT NOT NULL UNIQUE, control_digest TEXT NOT NULL,"
        " progress_digest BLOB NOT NULL, consumed INTEGER NOT NULL DEFAULT 0,"
        " created_at INTEGER NOT NULL);"
        "CREATE TABLE legacy_checkpoint_file ("
        " token TEXT NOT NULL REFERENCES legacy_checkpoint_import(token),"
        " path TEXT NOT NULL UNIQUE, device TEXT NOT NULL, inode TEXT NOT NULL,"
        " size TEXT NOT NULL, mtime_ns TEXT NOT NULL, ctime_ns TEXT NOT NULL,"
        " logical_offset INTEGER NOT NULL, logical_length INTEGER NOT NULL,"
        " PRIMARY KEY(token,path));"
        "UPDATE meta SET value='4' WHERE key='schema_version';"
        "PRAGMA user_version=4;";
    if (sqlite3_exec(store.raw(), sql, nullptr, nullptr, nullptr) !=
        SQLITE_OK) {
      throw DL_ABORT_EX("sqlite3-persistence: legacy import migration failed");
    }
  });
}

void migrate_v4_to_v5(Sqlite3PersistenceStore& store)
{
  store.withTransaction([&]() {
    const char* sql =
        "CREATE TABLE legacy_torrent_metadata ("
        " token TEXT PRIMARY KEY REFERENCES legacy_checkpoint_import(token),"
        " gid TEXT NOT NULL UNIQUE, metadata_path TEXT NOT NULL,"
        " metadata_digest TEXT NOT NULL, device TEXT NOT NULL, inode TEXT NOT "
        "NULL,"
        " size TEXT NOT NULL, mtime_ns TEXT NOT NULL, ctime_ns TEXT NOT NULL,"
        " invalidated INTEGER NOT NULL DEFAULT 0);"
        "UPDATE meta SET value='5' WHERE key='schema_version';"
        "PRAGMA user_version=5;";
    if (sqlite3_exec(store.raw(), sql, nullptr, nullptr, nullptr) !=
        SQLITE_OK) {
      throw DL_ABORT_EX(
          "sqlite3-persistence: legacy metadata migration failed");
    }
  });
}

struct Migration {
  int from;
  int to;
  void (*fn)(Sqlite3PersistenceStore&);
};

static const Migration kMigrations[] = {
    {0, 1, &migrate_v0_to_v1}, {1, 2, &migrate_v1_to_v2},
    {2, 3, &migrate_v2_to_v3}, {3, 4, &migrate_v3_to_v4},
    {4, 5, &migrate_v4_to_v5},
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
