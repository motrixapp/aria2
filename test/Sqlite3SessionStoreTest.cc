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

#include <cstdlib>

#include <sqlite3.h>

#include <cppunit/extensions/HelperMacros.h>

#include "DownloadContext.h"
#include "FileEntry.h"
#include "GroupId.h"
#include "Option.h"
#include "RequestGroup.h"
#include "RequestGroupMan.h"
#include "Sqlite3PersistenceStore.h"
#include "TestUtil.h"
#include "prefs.h"

namespace aria2 {

class Sqlite3SessionStoreTest : public CppUnit::TestFixture {
  CPPUNIT_TEST_SUITE(Sqlite3SessionStoreTest);
  CPPUNIT_TEST(testSaveAllTasksUpsertsRows);
  CPPUNIT_TEST_SUITE_END();

private:
  std::shared_ptr<Option> option_;
  std::unique_ptr<Sqlite3PersistenceStore> store_;
  std::string dbPath_;

public:
  void setUp() override;
  void tearDown() override;
  void testSaveAllTasksUpsertsRows();
};

CPPUNIT_TEST_SUITE_REGISTRATION(Sqlite3SessionStoreTest);

void Sqlite3SessionStoreTest::setUp()
{
  option_.reset(new Option());
  option_->put(PREF_DIR, A2_TEST_OUT_DIR);

  dbPath_ = std::string(A2_TEST_OUT_DIR) + "/sqlite3-session-store-test.db";
  std::remove(dbPath_.c_str());
  std::remove((dbPath_ + "-wal").c_str());
  std::remove((dbPath_ + "-shm").c_str());

  store_.reset(new Sqlite3PersistenceStore(dbPath_));
  store_->open();
}

void Sqlite3SessionStoreTest::tearDown()
{
  if (store_) {
    store_->finalCheckpointAndClose();
  }
  store_.reset();
  std::remove(dbPath_.c_str());
  std::remove((dbPath_ + "-wal").c_str());
  std::remove((dbPath_ + "-shm").c_str());
}

void Sqlite3SessionStoreTest::testSaveAllTasksUpsertsRows()
{
  // Build two reserved RequestGroups with downloadable URIs.
  auto makeRG = [&](const std::string& uri) {
    auto dctx = std::make_shared<DownloadContext>(0, 0, "");
    dctx->getFirstFileEntry()->addUri(uri);
    auto rg =
        std::make_shared<RequestGroup>(GroupId::create(), option_);
    rg->setDownloadContext(dctx);
    return rg;
  };

  auto rg1 = makeRG("http://example.com/file1.bin");
  auto rg2 = makeRG("http://example.com/file2.bin");

  RequestGroupMan rgman{std::vector<std::shared_ptr<RequestGroup>>(), 4,
                        option_.get()};
  rgman.addReservedGroup(rg1);
  rgman.addReservedGroup(rg2);

  Sqlite3SessionStore session(store_.get());
  session.saveAllTasks(&rgman);

  // Verify row count == 2.
  {
    sqlite3_stmt* stmt = nullptr;
    CPPUNIT_ASSERT_EQUAL(
        SQLITE_OK,
        sqlite3_prepare_v2(store_->raw(), "SELECT COUNT(*) FROM task", -1,
                           &stmt, nullptr));
    CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
    int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    CPPUNIT_ASSERT_EQUAL(2, count);
  }

  // Verify queue_position values are exactly {0, 1} in two distinct rows.
  {
    sqlite3_stmt* stmt = nullptr;
    CPPUNIT_ASSERT_EQUAL(
        SQLITE_OK,
        sqlite3_prepare_v2(
            store_->raw(),
            "SELECT queue_position FROM task ORDER BY queue_position ASC", -1,
            &stmt, nullptr));
    CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
    CPPUNIT_ASSERT_EQUAL(0, sqlite3_column_int(stmt, 0));
    CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
    CPPUNIT_ASSERT_EQUAL(1, sqlite3_column_int(stmt, 0));
    CPPUNIT_ASSERT_EQUAL(SQLITE_DONE, sqlite3_step(stmt));
    sqlite3_finalize(stmt);
  }
}

} // namespace aria2

#endif // HAVE_SQLITE3
