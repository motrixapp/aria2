#include "Sqlite3PersistenceStore.h"

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
  CPPUNIT_TEST_SUITE_END();

public:
  void testOpenAndPragmas();
  void testCorruptDbRenamedAndRebuilt();
  void testUnwritablePathThrows();
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

  // First open: create a valid v1 database.
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

    // Fresh DB must have been migrated to v1.
    CPPUNIT_ASSERT_EQUAL(std::string("1"), store.queryPragma("user_version"));
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

} // namespace aria2
