#include "Sqlite3PersistenceStore.h"

#include <cstdio>
#include <string>

#include <cppunit/extensions/HelperMacros.h>

namespace aria2 {

class Sqlite3PersistenceStoreTest : public CppUnit::TestFixture {
  CPPUNIT_TEST_SUITE(Sqlite3PersistenceStoreTest);
  CPPUNIT_TEST(testOpenAndPragmas);
  CPPUNIT_TEST_SUITE_END();

public:
  void testOpenAndPragmas();
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

} // namespace aria2
