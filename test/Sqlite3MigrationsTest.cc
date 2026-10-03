#include "Sqlite3PersistenceStore.h"
#include "Sqlite3Migrations.h"
#include "RecoverableException.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <sqlite3.h>
#include <cppunit/extensions/HelperMacros.h>

namespace aria2 {

class Sqlite3MigrationsTest : public CppUnit::TestFixture {
  CPPUNIT_TEST_SUITE(Sqlite3MigrationsTest);
  CPPUNIT_TEST(testMigrateFreshDbToV2);
  CPPUNIT_TEST(testMigrateExistingV1ToV2);
  CPPUNIT_TEST(testMigrateV2ToV3DetachesProgressFromTask);
  CPPUNIT_TEST(testMigrateV3ToV4PreservesProgress);
  CPPUNIT_TEST(testReopenIsIdempotent);
  CPPUNIT_TEST(testFutureVersionRejected);
  CPPUNIT_TEST_SUITE_END();

public:
  void testMigrateFreshDbToV2();
  void testMigrateExistingV1ToV2();
  void testMigrateV2ToV3DetachesProgressFromTask();
  void testMigrateV3ToV4PreservesProgress();
  void testReopenIsIdempotent();
  void testFutureVersionRejected();
};

CPPUNIT_TEST_SUITE_REGISTRATION(Sqlite3MigrationsTest);

namespace {
std::vector<std::string> tableNames(Sqlite3PersistenceStore& store) {
  std::vector<std::string> names;
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(store.raw(),
                     "SELECT name FROM sqlite_master WHERE type='table' "
                     "ORDER BY name",
                     -1, &stmt, nullptr);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    names.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
  }
  sqlite3_finalize(stmt);
  return names;
}
} // namespace

void Sqlite3MigrationsTest::testMigrateFreshDbToV2() {
  std::string path = std::string(A2_TEST_OUT_DIR) + "/test_migrate_v2.db";
  std::remove(path.c_str());
  Sqlite3PersistenceStore store(path);
  store.open();
  auto names = tableNames(store);
  for (auto t :
       {"task", "task_progress", "download_history", "download_history_files",
        "download_history_file_uris", "task_cookie_context", "task_cookie",
        "legacy_checkpoint_import", "legacy_checkpoint_file", "meta"}) {
    CPPUNIT_ASSERT_MESSAGE(std::string("missing table: ") + t,
                           std::find(names.begin(), names.end(), t) != names.end());
  }
  CPPUNIT_ASSERT_EQUAL(std::to_string(kCurrentSchemaVersion),
                         store.queryPragma("user_version"));
}

void Sqlite3MigrationsTest::testMigrateExistingV1ToV2() {
  std::string path = std::string(A2_TEST_OUT_DIR) + "/test_migrate_v1_to_v2.db";
  std::remove(path.c_str());
  {
    Sqlite3PersistenceStore store(path);
    store.open();
    sqlite3_exec(store.raw(),
                 "DROP TABLE legacy_checkpoint_file; DROP TABLE "
                 "legacy_checkpoint_import",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(store.raw(), "DROP TABLE task_cookie", nullptr, nullptr, nullptr);
    sqlite3_exec(store.raw(), "DROP TABLE task_cookie_context", nullptr, nullptr,
                 nullptr);
    sqlite3_exec(store.raw(),
                 "UPDATE meta SET value='1' WHERE key='schema_version'",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(store.raw(), "PRAGMA user_version=1", nullptr, nullptr, nullptr);
  }
  {
    Sqlite3PersistenceStore store(path);
    store.open();
    auto names = tableNames(store);
    CPPUNIT_ASSERT(std::find(names.begin(), names.end(),
                             "task_cookie_context") != names.end());
    CPPUNIT_ASSERT(std::find(names.begin(), names.end(), "task_cookie") !=
                   names.end());
    CPPUNIT_ASSERT_EQUAL(std::to_string(kCurrentSchemaVersion),
                         store.queryPragma("user_version"));
  }
}

void Sqlite3MigrationsTest::testMigrateV2ToV3DetachesProgressFromTask() {
  std::string path = std::string(A2_TEST_OUT_DIR) + "/test_migrate_v2_to_v3.db";
  std::remove(path.c_str());
  std::remove((path + "-wal").c_str());
  std::remove((path + "-shm").c_str());
  auto exec = [](sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    std::string msg = err ? err : "";
    sqlite3_free(err);
    CPPUNIT_ASSERT_EQUAL_MESSAGE(msg, SQLITE_OK, rc);
  };
  // Rebuild a schema-v2 database: task_progress keyed by gid with the
  // CASCADE into task, holding one checkpoint.
  {
    Sqlite3PersistenceStore store(path);
    store.open();
    sqlite3* db = store.raw();
    exec(db, "DROP TABLE task_progress");
    exec(db,
         "CREATE TABLE task_progress ("
         "  gid TEXT PRIMARY KEY,"
         "  ctrl_version INTEGER NOT NULL DEFAULT 1,"
         "  is_torrent INTEGER NOT NULL DEFAULT 0,"
         "  info_hash BLOB,"
         "  piece_length INTEGER NOT NULL,"
         "  total_length INTEGER NOT NULL,"
         "  upload_length INTEGER NOT NULL DEFAULT 0,"
         "  bitfield BLOB NOT NULL,"
         "  in_flight_blob BLOB NOT NULL DEFAULT X'',"
         "  digest BLOB NOT NULL,"
         "  updated_at INTEGER NOT NULL,"
         "  FOREIGN KEY (gid) REFERENCES task(gid) ON DELETE CASCADE)");
    exec(db,
         "INSERT INTO task(gid, state, serialized, queue_position, digest,"
         " created_at, updated_at)"
         " VALUES ('00000000000000aa', 'waiting', '', 0, X'', 0, 0)");
    exec(db,
         "INSERT INTO task_progress(gid, piece_length, total_length, bitfield,"
         " digest, updated_at)"
         " VALUES ('00000000000000aa', 1024, 2048, X'80', X'', 1)");
    exec(db, "DROP TABLE legacy_checkpoint_file; DROP TABLE "
             "legacy_checkpoint_import");
    exec(db, "UPDATE meta SET value='2' WHERE key='schema_version'");
    exec(db, "PRAGMA user_version=2");
  }
  {
    Sqlite3PersistenceStore store(path);
    store.open();
    sqlite3* db = store.raw();
    CPPUNIT_ASSERT_EQUAL(std::to_string(kCurrentSchemaVersion),
                         store.queryPragma("user_version"));

    auto countRows = [&](const char* sql) {
      sqlite3_stmt* stmt = nullptr;
      CPPUNIT_ASSERT_EQUAL(SQLITE_OK,
                           sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr));
      CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
      int n = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);
      return n;
    };
    // The legacy checkpoint survives, marked as not yet path-addressed.
    CPPUNIT_ASSERT_EQUAL(
        1, countRows("SELECT COUNT(*) FROM task_progress"
                     " WHERE gid = '00000000000000aa' AND out_path IS NULL"));
    // Deleting the task row no longer destroys the checkpoint.
    exec(db, "DELETE FROM task WHERE gid = '00000000000000aa'");
    CPPUNIT_ASSERT_EQUAL(
        1, countRows("SELECT COUNT(*) FROM task_progress"
                     " WHERE gid = '00000000000000aa'"));
    // One checkpoint per output path.
    exec(db,
         "INSERT INTO task_progress(gid, out_path, piece_length, total_length,"
         " bitfield, digest, updated_at)"
         " VALUES ('00000000000000bb', '/d/x', 1024, 2048, X'80', X'', 1)");
    CPPUNIT_ASSERT(sqlite3_exec(db,
                                "INSERT INTO task_progress(gid, out_path,"
                                " piece_length, total_length, bitfield,"
                                " digest, updated_at) VALUES"
                                " ('00000000000000cc', '/d/x', 1024, 2048,"
                                " X'80', X'', 1)",
                                nullptr, nullptr, nullptr) != SQLITE_OK);
  }
}

void Sqlite3MigrationsTest::testMigrateV3ToV4PreservesProgress()
{
  std::string path = std::string(A2_TEST_OUT_DIR) + "/test_migrate_v3_v4.db";
  std::remove(path.c_str());
  {
    Sqlite3PersistenceStore store(path);
    store.open();
    CPPUNIT_ASSERT_EQUAL(
        SQLITE_OK,
        sqlite3_exec(store.raw(),
                     "DROP TABLE legacy_checkpoint_file; DROP TABLE "
                     "legacy_checkpoint_import;"
                     "INSERT INTO task (gid, state, serialized, "
                     "queue_position, digest, created_at, updated_at) "
                     "VALUES ('00000000000000aa', 'waiting', '', 0, "
                     "X'', 0, 0);"
                     "INSERT INTO task_progress (gid, out_path, "
                     "ctrl_version, is_torrent, info_hash, "
                     "piece_length, total_length, upload_length, "
                     "bitfield, in_flight_blob, digest, updated_at) "
                     "VALUES ('00000000000000aa', '/download/old', "
                     "1, 1, X'11223344', 32768, 98305, 42, X'80', "
                     "X'0000000100000001000080000000000180', "
                     "X'aabbccdd', 123456789);"
                     "UPDATE meta SET value='3' WHERE "
                     "key='schema_version'; PRAGMA user_version=3;",
                     nullptr, nullptr, nullptr));
  }
  Sqlite3PersistenceStore store(path);
  store.open();
  CPPUNIT_ASSERT_EQUAL(std::string("4"), store.queryPragma("user_version"));
  auto names = tableNames(store);
  CPPUNIT_ASSERT(std::find(names.begin(), names.end(),
                           "legacy_checkpoint_import") != names.end());
  CPPUNIT_ASSERT(std::find(names.begin(), names.end(),
                           "legacy_checkpoint_file") != names.end());
  sqlite3_stmt* stmt = nullptr;
  CPPUNIT_ASSERT_EQUAL(
      SQLITE_OK,
      sqlite3_prepare_v2(
          store.raw(),
          "SELECT gid, out_path, ctrl_version, is_torrent, hex(info_hash), "
          "piece_length, total_length, upload_length, hex(bitfield), "
          "hex(in_flight_blob), hex(digest), updated_at FROM task_progress",
          -1, &stmt, nullptr));
  CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
  auto text = [&](int column) {
    return std::string(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, column)));
  };
  CPPUNIT_ASSERT_EQUAL(std::string("00000000000000aa"), text(0));
  CPPUNIT_ASSERT_EQUAL(std::string("/download/old"), text(1));
  CPPUNIT_ASSERT_EQUAL(sqlite3_int64(1), sqlite3_column_int64(stmt, 2));
  CPPUNIT_ASSERT_EQUAL(sqlite3_int64(1), sqlite3_column_int64(stmt, 3));
  CPPUNIT_ASSERT_EQUAL(std::string("11223344"), text(4));
  CPPUNIT_ASSERT_EQUAL(sqlite3_int64(32768), sqlite3_column_int64(stmt, 5));
  CPPUNIT_ASSERT_EQUAL(sqlite3_int64(98305), sqlite3_column_int64(stmt, 6));
  CPPUNIT_ASSERT_EQUAL(sqlite3_int64(42), sqlite3_column_int64(stmt, 7));
  CPPUNIT_ASSERT_EQUAL(std::string("80"), text(8));
  CPPUNIT_ASSERT_EQUAL(std::string("0000000100000001000080000000000180"),
                       text(9));
  CPPUNIT_ASSERT_EQUAL(std::string("AABBCCDD"), text(10));
  CPPUNIT_ASSERT_EQUAL(sqlite3_int64(123456789),
                       sqlite3_column_int64(stmt, 11));
  CPPUNIT_ASSERT_EQUAL(SQLITE_DONE, sqlite3_step(stmt));
  sqlite3_finalize(stmt);
}

void Sqlite3MigrationsTest::testReopenIsIdempotent() {
  std::string path = std::string(A2_TEST_OUT_DIR) + "/test_migrate_idempotent.db";
  std::remove(path.c_str());
    // First open: migrate v0 -> current.
  {
    Sqlite3PersistenceStore store(path);
    store.open();
    CPPUNIT_ASSERT_EQUAL(std::to_string(kCurrentSchemaVersion),
                         store.queryPragma("user_version"));
  }
  // Second open: must not throw and must keep the current schema version.
  {
    Sqlite3PersistenceStore store(path);
    store.open();
    CPPUNIT_ASSERT_EQUAL(std::to_string(kCurrentSchemaVersion),
                         store.queryPragma("user_version"));
  }
}

void Sqlite3MigrationsTest::testFutureVersionRejected() {
  std::string path = std::string(A2_TEST_OUT_DIR) + "/test_migrate_future.db";
  std::remove(path.c_str());
  // Plant a DB with user_version=99.
  {
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(path.c_str(), &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    CPPUNIT_ASSERT_EQUAL(SQLITE_OK, rc);
    rc = sqlite3_exec(db, "PRAGMA user_version = 99;", nullptr, nullptr, nullptr);
    CPPUNIT_ASSERT_EQUAL(SQLITE_OK, rc);
    sqlite3_close_v2(db);
  }
  // Open via store -- must throw.
  Sqlite3PersistenceStore store(path);
  CPPUNIT_ASSERT_THROW(store.open(), RecoverableException);
}

} // namespace aria2
