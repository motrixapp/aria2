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
#include <set>
#include <vector>

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
  CPPUNIT_TEST(testSaveLoadRoundTrip);
  CPPUNIT_TEST(testQueuePositionMove);
  CPPUNIT_TEST(testUpsertTaskInsertsThenUpdates);
  CPPUNIT_TEST(testDeleteTaskRemovesRow);
  CPPUNIT_TEST(testUpdateTaskState);
  CPPUNIT_TEST_SUITE_END();

private:
  std::shared_ptr<Option> option_;
  std::unique_ptr<Sqlite3PersistenceStore> store_;
  std::string dbPath_;

public:
  void setUp() override;
  void tearDown() override;
  void testSaveAllTasksUpsertsRows();
  void testSaveLoadRoundTrip();
  void testQueuePositionMove();
  void testUpsertTaskInsertsThenUpdates();
  void testDeleteTaskRemovesRow();
  void testUpdateTaskState();
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

void Sqlite3SessionStoreTest::testSaveLoadRoundTrip()
{
  auto makeRG = [&](const std::string& uri) {
    auto dctx = std::make_shared<DownloadContext>(0, 0, "");
    dctx->getFirstFileEntry()->addUri(uri);
    auto rg = std::make_shared<RequestGroup>(GroupId::create(), option_);
    rg->setDownloadContext(dctx);
    return rg;
  };

  a2_gid_t gid1, gid2;

  // Save phase: create RGs, persist them, then release all references so
  // the GroupId slots are freed before the load phase re-imports them.
  {
    auto rg1 = makeRG("http://example.com/file1.bin");
    auto rg2 = makeRG("http://example.com/file2.bin");

    gid1 = rg1->getGID();
    gid2 = rg2->getGID();

    RequestGroupMan rgman{std::vector<std::shared_ptr<RequestGroup>>(), 4,
                          option_.get()};
    rgman.addReservedGroup(rg1);
    rgman.addReservedGroup(rg2);

    Sqlite3SessionStore session(store_.get());
    session.saveAllTasks(&rgman);
  }
  // At this point rg1, rg2, and rgman are destroyed; GroupId slots freed.

  std::vector<std::shared_ptr<RequestGroup>> loaded;
  Sqlite3SessionStore session(store_.get());
  session.loadActiveTasksInto(loaded, option_);

  CPPUNIT_ASSERT_EQUAL((size_t)2, loaded.size());

  std::set<a2_gid_t> gotGids;
  for (const auto& rg : loaded) {
    gotGids.insert(rg->getGID());
  }

  std::set<a2_gid_t> wantGids{gid1, gid2};
  CPPUNIT_ASSERT(gotGids == wantGids);
}

namespace {

// Helper: directly INSERT a task row for tests that need pre-seeded data.
void insertTestRow(sqlite3* db, const std::string& gid, int pos,
                   const std::string& state = "waiting")
{
  std::string sql =
      "INSERT INTO task (gid, state, serialized, queue_position, digest,"
      " created_at, updated_at) VALUES ('" +
      gid + "', '" + state + "', '', " + std::to_string(pos) +
      ", X'', 0, 0)";
  char* errmsg = nullptr;
  int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errmsg);
  if (rc != SQLITE_OK) {
    std::string msg = errmsg ? errmsg : "unknown";
    sqlite3_free(errmsg);
    CPPUNIT_FAIL(("insertTestRow failed: " + msg).c_str());
  }
}

} // namespace

void Sqlite3SessionStoreTest::testQueuePositionMove()
{
  sqlite3* db = store_->raw();
  for (int i = 0; i < 5; ++i) {
    insertTestRow(db, "g" + std::to_string(i), i);
  }

  Sqlite3SessionStore session(store_.get());
  session.moveTaskPosition("g3", 0);

  sqlite3_stmt* stmt = nullptr;
  CPPUNIT_ASSERT_EQUAL(
      SQLITE_OK,
      sqlite3_prepare_v2(db, "SELECT gid FROM task ORDER BY queue_position ASC",
                         -1, &stmt, nullptr));

  std::vector<std::string> expected = {"g3", "g0", "g1", "g2", "g4"};
  for (const auto& want : expected) {
    CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
    const char* got =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    CPPUNIT_ASSERT_EQUAL(want, std::string(got ? got : ""));
  }
  CPPUNIT_ASSERT_EQUAL(SQLITE_DONE, sqlite3_step(stmt));
  sqlite3_finalize(stmt);
}

void Sqlite3SessionStoreTest::testUpsertTaskInsertsThenUpdates()
{
  auto makeRG = [&](const std::string& uri) {
    auto dctx = std::make_shared<DownloadContext>(0, 0, "");
    dctx->getFirstFileEntry()->addUri(uri);
    auto rg = std::make_shared<RequestGroup>(GroupId::create(), option_);
    rg->setDownloadContext(dctx);
    return rg;
  };

  auto rg = makeRG("http://example.com/file1.bin");
  Sqlite3SessionStore session(store_.get());
  session.upsertTask(rg);

  sqlite3* db = store_->raw();

  {
    sqlite3_stmt* stmt = nullptr;
    CPPUNIT_ASSERT_EQUAL(
        SQLITE_OK,
        sqlite3_prepare_v2(
            db, "SELECT COUNT(*), queue_position, state FROM task", -1, &stmt,
            nullptr));
    CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
    CPPUNIT_ASSERT_EQUAL(1, sqlite3_column_int(stmt, 0));
    CPPUNIT_ASSERT_EQUAL(0, sqlite3_column_int(stmt, 1));
    const char* state =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    CPPUNIT_ASSERT_EQUAL(std::string("waiting"),
                         std::string(state ? state : ""));
    sqlite3_finalize(stmt);
  }

  int64_t createdAt = 0;
  {
    sqlite3_stmt* stmt = nullptr;
    CPPUNIT_ASSERT_EQUAL(
        SQLITE_OK,
        sqlite3_prepare_v2(db, "SELECT created_at FROM task", -1, &stmt,
                           nullptr));
    CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
    createdAt = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
  }

  rg->setPauseRequested(true);
  session.upsertTask(rg);

  {
    sqlite3_stmt* stmt = nullptr;
    CPPUNIT_ASSERT_EQUAL(
        SQLITE_OK,
        sqlite3_prepare_v2(
            db,
            "SELECT COUNT(*), queue_position, state, created_at FROM task", -1,
            &stmt, nullptr));
    CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
    CPPUNIT_ASSERT_EQUAL(1, sqlite3_column_int(stmt, 0));
    CPPUNIT_ASSERT_EQUAL(0, sqlite3_column_int(stmt, 1));
    const char* state =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    CPPUNIT_ASSERT_EQUAL(std::string("paused"),
                         std::string(state ? state : ""));
    CPPUNIT_ASSERT_EQUAL(createdAt, static_cast<int64_t>(sqlite3_column_int64(stmt, 3)));
    sqlite3_finalize(stmt);
  }
}

void Sqlite3SessionStoreTest::testDeleteTaskRemovesRow()
{
  sqlite3* db = store_->raw();
  insertTestRow(db, "abc", 0);

  Sqlite3SessionStore session(store_.get());
  session.deleteTask("abc");

  sqlite3_stmt* stmt = nullptr;
  CPPUNIT_ASSERT_EQUAL(
      SQLITE_OK,
      sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM task", -1, &stmt, nullptr));
  CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
  CPPUNIT_ASSERT_EQUAL(0, sqlite3_column_int(stmt, 0));
  sqlite3_finalize(stmt);
}

void Sqlite3SessionStoreTest::testUpdateTaskState()
{
  sqlite3* db = store_->raw();
  insertTestRow(db, "abc", 0, "waiting");

  Sqlite3SessionStore session(store_.get());
  session.updateTaskState("abc", "paused");

  sqlite3_stmt* stmt = nullptr;
  CPPUNIT_ASSERT_EQUAL(
      SQLITE_OK,
      sqlite3_prepare_v2(db, "SELECT state FROM task WHERE gid='abc'", -1,
                         &stmt, nullptr));
  CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
  const char* state =
      reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  CPPUNIT_ASSERT_EQUAL(std::string("paused"), std::string(state ? state : ""));
  sqlite3_finalize(stmt);
}

} // namespace aria2

#endif // HAVE_SQLITE3
