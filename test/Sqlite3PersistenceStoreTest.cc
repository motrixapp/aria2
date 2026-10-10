#include "Sqlite3PersistenceStore.h"
#include "Sqlite3Migrations.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <dirent.h>

#include <cppunit/extensions/HelperMacros.h>

#include "RecoverableException.h"

namespace aria2 {

class Sqlite3PersistenceStoreTest : public CppUnit::TestFixture {
  CPPUNIT_TEST_SUITE(Sqlite3PersistenceStoreTest);
  CPPUNIT_TEST(testOpenAndPragmas);
  CPPUNIT_TEST(testCorruptDbRenamedAndRebuilt);
  CPPUNIT_TEST(testUnwritablePathThrows);
  CPPUNIT_TEST(testMigrationBackupIncludesWal);
  CPPUNIT_TEST_SUITE_END();

public:
  void testOpenAndPragmas();
  void testCorruptDbRenamedAndRebuilt();
  void testUnwritablePathThrows();
  void testMigrationBackupIncludesWal();
};

CPPUNIT_TEST_SUITE_REGISTRATION(Sqlite3PersistenceStoreTest);

void Sqlite3PersistenceStoreTest::testOpenAndPragmas()
{
  std::string dbPath =
      std::string(A2_TEST_OUT_DIR) + "/test_sqlite3_store.db";
  std::remove(dbPath.c_str());
  Sqlite3PersistenceStore store(dbPath);
  store.open();
  CPPUNIT_ASSERT_EQUAL(std::string("wal"), store.queryPragma("journal_mode"));
  CPPUNIT_ASSERT_EQUAL(std::string("1"), store.queryPragma("foreign_keys"));
}

void Sqlite3PersistenceStoreTest::testCorruptDbRenamedAndRebuilt()
{
  std::string dbPath =
      std::string(A2_TEST_OUT_DIR) + "/test_corrupt.db";

  // Remove any pre-existing file (and WAL siblings).
  std::remove(dbPath.c_str());
  std::remove((dbPath + "-wal").c_str());
  std::remove((dbPath + "-shm").c_str());

  // First open: create a valid current-schema database.
  {
    Sqlite3PersistenceStore store(dbPath);
    store.open();
    // store goes out of scope, DB file closes cleanly.
  }

  // Corrupt the database body past the header / first page boundary.
  // Offset 1024 is well past the 100-byte SQLite header, so sqlite3_open_v2
  // still succeeds but PRAGMA quick_check detects corruption.
  {
    std::ofstream f(dbPath, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(1024);
    std::vector<char> garbage(4096, '\xFF');
    f.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
  }

  // Second open: corruption recovery should kick in.
  {
    Sqlite3PersistenceStore store(dbPath);
    store.open();

    // Fresh DB must have been migrated to the current schema.
    CPPUNIT_ASSERT_EQUAL(std::to_string(kCurrentSchemaVersion),
                         store.queryPragma("user_version"));
  }

  // Verify at least one sibling .corrupt.* file was created.
  const std::string prefix = "test_corrupt.db.corrupt.";
  DIR* d = opendir(A2_TEST_OUT_DIR);
  CPPUNIT_ASSERT(d != nullptr);
  struct dirent* ent;
  bool foundCorrupt = false;
  while ((ent = readdir(d)) != nullptr) {
    std::string name = ent->d_name;
    if (name.compare(0, prefix.size(), prefix) == 0) {
      foundCorrupt = true;
      break;
    }
  }
  closedir(d);
  CPPUNIT_ASSERT(foundCorrupt);
}

void Sqlite3PersistenceStoreTest::testUnwritablePathThrows()
{
  Sqlite3PersistenceStore store("/this/path/does/not/exist/x.db");
  CPPUNIT_ASSERT_THROW(store.open(), RecoverableException);
}

void Sqlite3PersistenceStoreTest::testMigrationBackupIncludesWal()
{
  const std::string dbPath =
      std::string(A2_TEST_OUT_DIR) + "/test_upgrade_backup.db";
  std::remove(dbPath.c_str());
  Sqlite3PersistenceStore writer(dbPath);
  writer.open();
  CPPUNIT_ASSERT_EQUAL(SQLITE_OK, sqlite3_exec(writer.raw(),
      "PRAGMA wal_autocheckpoint=0;"
      "DROP TABLE legacy_torrent_metadata;"
      "DROP TABLE legacy_checkpoint_file;"
      "DROP TABLE legacy_checkpoint_import;"
      "CREATE TABLE backup_sentinel(value TEXT);"
      "INSERT INTO backup_sentinel VALUES('committed in WAL');"
      "UPDATE meta SET value='3' WHERE key='schema_version';"
      "PRAGMA user_version=3;", nullptr, nullptr, nullptr));

  // The writer remains open: a raw copy of the database body loses these rows.
  Sqlite3PersistenceStore upgraded(dbPath);
  upgraded.open();
  CPPUNIT_ASSERT_EQUAL(std::string("5"), upgraded.queryPragma("user_version"));
  DIR* directory = opendir(A2_TEST_OUT_DIR);
  CPPUNIT_ASSERT(directory != nullptr);
  bool verified = false;
  struct dirent* entry;
  while ((entry = readdir(directory)) != nullptr) {
    const std::string name = entry->d_name;
    if (name.find("test_upgrade_backup.db.pre-schema5-v3.") != 0 ||
        name.size() < 3 || name.substr(name.size() - 3) != ".db") {
      continue;
    }
    sqlite3* backup = nullptr;
    const auto backupPath = std::string(A2_TEST_OUT_DIR) + "/" + name;
    CPPUNIT_ASSERT_EQUAL(SQLITE_OK, sqlite3_open_v2(backupPath.c_str(), &backup,
        SQLITE_OPEN_READONLY, nullptr));
    sqlite3_stmt* statement = nullptr;
    CPPUNIT_ASSERT_EQUAL(SQLITE_OK, sqlite3_prepare_v2(backup,
        "SELECT value FROM backup_sentinel", -1, &statement, nullptr));
    CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(statement));
    CPPUNIT_ASSERT_EQUAL(std::string("committed in WAL"),
        std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0))));
    sqlite3_finalize(statement);
    CPPUNIT_ASSERT_EQUAL(SQLITE_OK, sqlite3_prepare_v2(backup,
        "PRAGMA user_version", -1, &statement, nullptr));
    CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(statement));
    CPPUNIT_ASSERT_EQUAL(3, sqlite3_column_int(statement, 0));
    sqlite3_finalize(statement);
    sqlite3_close(backup);
    verified = true;
    std::remove(backupPath.c_str());
  }
  closedir(directory);
  CPPUNIT_ASSERT(verified);
}

} // namespace aria2
