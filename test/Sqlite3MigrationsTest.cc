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
  CPPUNIT_TEST(testMigrateFreshDbToV1);
  CPPUNIT_TEST(testReopenIsIdempotent);
  CPPUNIT_TEST(testFutureVersionRejected);
  CPPUNIT_TEST_SUITE_END();

public:
  void testMigrateFreshDbToV1();
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

void Sqlite3MigrationsTest::testMigrateFreshDbToV1() {
  std::string path = std::string(A2_TEST_OUT_DIR) + "/test_migrate_v1.db";
  std::remove(path.c_str());
  Sqlite3PersistenceStore store(path);
  store.open();
  auto names = tableNames(store);
  for (auto t : {"task", "task_progress", "download_history",
                 "download_history_files", "download_history_file_uris", "meta"}) {
    CPPUNIT_ASSERT_MESSAGE(std::string("missing table: ") + t,
                           std::find(names.begin(), names.end(), t) != names.end());
  }
  CPPUNIT_ASSERT_EQUAL(std::string("1"), store.queryPragma("user_version"));
}

void Sqlite3MigrationsTest::testReopenIsIdempotent() {
  std::string path = std::string(A2_TEST_OUT_DIR) + "/test_migrate_idempotent.db";
  std::remove(path.c_str());
  // First open: migrate v0 -> v1.
  {
    Sqlite3PersistenceStore store(path);
    store.open();
    CPPUNIT_ASSERT_EQUAL(std::string("1"), store.queryPragma("user_version"));
  }
  // Second open: must not throw, must keep user_version=1.
  {
    Sqlite3PersistenceStore store(path);
    store.open();
    CPPUNIT_ASSERT_EQUAL(std::string("1"), store.queryPragma("user_version"));
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
