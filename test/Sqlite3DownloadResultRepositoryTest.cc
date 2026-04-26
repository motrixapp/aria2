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
#include "Sqlite3DownloadResultRepository.h"

#ifdef HAVE_SQLITE3

#include <cstdlib>

#include <sqlite3.h>

#include <cppunit/extensions/HelperMacros.h>

#include "DownloadResult.h"
#include "FileEntry.h"
#include "GroupId.h"
#include "Option.h"
#include "Sqlite3PersistenceStore.h"
#include "error_code.h"
#include "prefs.h"

namespace aria2 {

class Sqlite3DownloadResultRepositoryTest : public CppUnit::TestFixture {
  CPPUNIT_TEST_SUITE(Sqlite3DownloadResultRepositoryTest);
  CPPUNIT_TEST(testInsertWritesAllThreeTables);
  CPPUNIT_TEST(testRangeAscDesc);
  CPPUNIT_TEST(testTrimToCapFifo);
  CPPUNIT_TEST(testTrimRespectsKeepUnfinished);
  CPPUNIT_TEST(testSearchByStatus);
  CPPUNIT_TEST(testSearchByPathLike);
  CPPUNIT_TEST(testSearchByInfoHash);
  CPPUNIT_TEST(testCountWithFilter);
  CPPUNIT_TEST(testReconstructedDrHasFullUris);
  CPPUNIT_TEST_SUITE_END();

private:
  std::shared_ptr<Option> option_;
  std::unique_ptr<Sqlite3PersistenceStore> store_;
  std::string dbPath_;

  // Helper: build a minimal DownloadResult with the given result code.
  std::shared_ptr<DownloadResult> makeDr(error_code::Value result,
                                          const std::string& path = "")
  {
    auto dr = std::make_shared<DownloadResult>();
    dr->gid = GroupId::create();
    dr->option = option_;
    dr->result = result;
    dr->totalLength = 1024;
    dr->completedLength = 1024;
    dr->uploadLength = 0;
    dr->numPieces = 0;
    dr->pieceLength = 0;
    dr->belongsTo = 0;
    dr->following = 0;
    dr->inMemoryDownload = false;
    if (!path.empty()) {
      auto fe = std::make_shared<FileEntry>(path, 1024, 0);
      fe->setRequested(true);
      dr->fileEntries.push_back(fe);
    }
    return dr;
  }

  // Helper: set finished_at for the last inserted row to a fixed value.
  void setLastFinishedAt(int64_t ts)
  {
    sqlite3* db = store_->raw();
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE download_history SET finished_at = ? WHERE id = "
        "(SELECT MAX(id) FROM download_history)";
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, ts);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  // Helper: count rows in a table.
  int countOf(const char* sql)
  {
    sqlite3_stmt* stmt = nullptr;
    CPPUNIT_ASSERT_EQUAL(SQLITE_OK,
                         sqlite3_prepare_v2(store_->raw(), sql, -1, &stmt,
                                            nullptr));
    CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
    int n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
  }

public:
  void setUp() CXX11_OVERRIDE;
  void tearDown() CXX11_OVERRIDE;
  void testInsertWritesAllThreeTables();
  void testRangeAscDesc();
  void testTrimToCapFifo();
  void testTrimRespectsKeepUnfinished();
  void testSearchByStatus();
  void testSearchByPathLike();
  void testSearchByInfoHash();
  void testCountWithFilter();
  void testReconstructedDrHasFullUris();
};

CPPUNIT_TEST_SUITE_REGISTRATION(Sqlite3DownloadResultRepositoryTest);

void Sqlite3DownloadResultRepositoryTest::setUp()
{
  option_.reset(new Option());
  option_->put(PREF_DIR, A2_TEST_OUT_DIR);

  dbPath_ =
      std::string(A2_TEST_OUT_DIR) + "/sqlite3-download-result-repo-test.db";
  std::remove(dbPath_.c_str());
  std::remove((dbPath_ + "-wal").c_str());
  std::remove((dbPath_ + "-shm").c_str());

  store_.reset(new Sqlite3PersistenceStore(dbPath_));
  store_->open();
}

void Sqlite3DownloadResultRepositoryTest::tearDown()
{
  if (store_) {
    store_->finalCheckpointAndClose();
  }
  store_.reset();
  std::remove(dbPath_.c_str());
  std::remove((dbPath_ + "-wal").c_str());
  std::remove((dbPath_ + "-shm").c_str());
}

void Sqlite3DownloadResultRepositoryTest::testInsertWritesAllThreeTables()
{
  auto dr = std::make_shared<DownloadResult>();
  dr->gid = GroupId::create();
  dr->option = option_;
  dr->result = error_code::FINISHED;
  dr->totalLength = 1024;
  dr->completedLength = 1024;
  dr->uploadLength = 0;
  dr->numPieces = 0;
  dr->pieceLength = 0;
  dr->belongsTo = 0;
  dr->following = 0;
  dr->inMemoryDownload = false;

  // 2 fileEntries, each with 2 URIs (both go to remainingUris via addUri)
  auto fe1 = std::make_shared<FileEntry>("/path/file1", 512, 0);
  fe1->addUri("http://example.com/file1.bin");
  fe1->addUri("http://mirror.com/file1.bin");
  auto fe2 = std::make_shared<FileEntry>("/path/file2", 512, 512);
  fe2->addUri("http://example.com/file2.bin");
  fe2->addUri("http://mirror.com/file2.bin");
  dr->fileEntries = {fe1, fe2};

  Sqlite3DownloadResultRepository repo(store_.get());
  repo.insert(dr);

  CPPUNIT_ASSERT_EQUAL(1,
                       countOf("SELECT COUNT(*) FROM download_history"));
  CPPUNIT_ASSERT_EQUAL(2,
                       countOf("SELECT COUNT(*) FROM download_history_files"));
  CPPUNIT_ASSERT_EQUAL(4,
                       countOf("SELECT COUNT(*) FROM download_history_file_uris"));
}

void Sqlite3DownloadResultRepositoryTest::testRangeAscDesc()
{
  Sqlite3DownloadResultRepository repo(store_.get());

  // Insert 3 DRs with distinct finished_at timestamps
  repo.insert(makeDr(error_code::FINISHED));
  setLastFinishedAt(1000);

  repo.insert(makeDr(error_code::FINISHED));
  setLastFinishedAt(2000);

  repo.insert(makeDr(error_code::FINISHED));
  setLastFinishedAt(3000);

  // countAll should be 3
  CPPUNIT_ASSERT_EQUAL(static_cast<int64_t>(3), repo.countAll());

  // range ASC: should return oldest first
  auto asc = repo.range(0, 3, false);
  CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(3), asc.size());

  // range DESC: should return newest first
  auto desc = repo.range(0, 3, true);
  CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(3), desc.size());

  // Pagination: range(0, 2, false) should return 2, range(2, 2, false) → 1
  auto page1 = repo.range(0, 2, false);
  CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(2), page1.size());

  auto page2 = repo.range(2, 2, false);
  CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(1), page2.size());

  // Empty range (offset beyond total)
  auto empty = repo.range(10, 2, false);
  CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(0), empty.size());
}

void Sqlite3DownloadResultRepositoryTest::testTrimToCapFifo()
{
  Sqlite3DownloadResultRepository repo(store_.get());

  // Insert 5 rows
  for (int i = 0; i < 5; ++i) {
    repo.insert(makeDr(error_code::FINISHED));
    setLastFinishedAt(static_cast<int64_t>(1000 * (i + 1)));
  }
  CPPUNIT_ASSERT_EQUAL(static_cast<int64_t>(5), repo.countAll());

  // Trim to 3 — oldest 2 should be removed
  repo.trimToCap(3, false);
  CPPUNIT_ASSERT_EQUAL(static_cast<int64_t>(3), repo.countAll());

  // Trim to 0 — all removed
  repo.trimToCap(0, false);
  CPPUNIT_ASSERT_EQUAL(static_cast<int64_t>(0), repo.countAll());

  // trimToCap(-1) does nothing
  repo.insert(makeDr(error_code::FINISHED));
  repo.trimToCap(-1, false);
  CPPUNIT_ASSERT_EQUAL(static_cast<int64_t>(1), repo.countAll());
}

void Sqlite3DownloadResultRepositoryTest::testTrimRespectsKeepUnfinished()
{
  Sqlite3DownloadResultRepository repo(store_.get());

  // Insert 3 complete + 2 error rows
  for (int i = 0; i < 3; ++i) {
    repo.insert(makeDr(error_code::FINISHED));
    setLastFinishedAt(static_cast<int64_t>(1000 * (i + 1)));
  }
  for (int i = 0; i < 2; ++i) {
    repo.insert(makeDr(error_code::UNKNOWN_ERROR));
    setLastFinishedAt(static_cast<int64_t>(4000 + 1000 * (i + 1)));
  }
  CPPUNIT_ASSERT_EQUAL(static_cast<int64_t>(5), repo.countAll());

  // trimToCap(2, keepUnfinished=true): only non-error rows counted; 3 non-error
  // rows − limit 2 = 1 removed (oldest complete). Error rows stay.
  repo.trimToCap(2, true);

  // 2 complete + 2 error = 4 remaining
  CPPUNIT_ASSERT_EQUAL(static_cast<int64_t>(4), repo.countAll());

  // Verify 2 error rows survived
  int errCount =
      countOf("SELECT COUNT(*) FROM download_history WHERE status='error'");
  CPPUNIT_ASSERT_EQUAL(2, errCount);
}

void Sqlite3DownloadResultRepositoryTest::testSearchByStatus()
{
  Sqlite3DownloadResultRepository repo(store_.get());

  // Insert 2 complete, 1 error, 1 removed
  repo.insert(makeDr(error_code::FINISHED));
  repo.insert(makeDr(error_code::FINISHED));
  repo.insert(makeDr(error_code::UNKNOWN_ERROR));
  repo.insert(makeDr(error_code::REMOVED));

  SearchFilter f;
  f.statuses = {"complete"};

  auto results = repo.search(f, 0, 10);
  CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(2), results.size());

  // Filter by error only
  SearchFilter f2;
  f2.statuses = {"error"};
  auto errResults = repo.search(f2, 0, 10);
  CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(1), errResults.size());

  // Filter by multiple statuses
  SearchFilter f3;
  f3.statuses = {"complete", "error"};
  auto multi = repo.search(f3, 0, 10);
  CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(3), multi.size());
}

void Sqlite3DownloadResultRepositoryTest::testSearchByPathLike()
{
  Sqlite3DownloadResultRepository repo(store_.get());

  // Insert DRs with different paths
  repo.insert(makeDr(error_code::FINISHED, "/downloads/video.mp4"));
  repo.insert(makeDr(error_code::FINISHED, "/downloads/audio.mp3"));
  repo.insert(makeDr(error_code::FINISHED, "/other/file.bin"));

  SearchFilter f;
  f.pathLike = "/downloads/%";
  auto results = repo.search(f, 0, 10);
  CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(2), results.size());

  SearchFilter f2;
  f2.pathLike = "%.bin";
  auto results2 = repo.search(f2, 0, 10);
  CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(1), results2.size());

  // No match
  SearchFilter f3;
  f3.pathLike = "/nonexistent/%";
  auto results3 = repo.search(f3, 0, 10);
  CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(0), results3.size());
}

void Sqlite3DownloadResultRepositoryTest::testSearchByInfoHash()
{
  Sqlite3DownloadResultRepository repo(store_.get());

  // Insert DR with specific info_hash (20 raw bytes)
  auto drBt = makeDr(error_code::FINISHED);
  drBt->infoHash = std::string(20, '\xab');  // 20 bytes of 0xab
  repo.insert(drBt);

  // Insert a plain HTTP download
  auto drHttp = makeDr(error_code::FINISHED);
  repo.insert(drHttp);

  // Search by the hex representation: 20 bytes × 2 hex chars = 40 chars
  SearchFilter f;
  f.infoHashHex = "abababababababababababababababababababab";
  auto results = repo.search(f, 0, 10);
  CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(1), results.size());

  // No match: search with different hex
  SearchFilter f2;
  f2.infoHashHex = "00000000000000000000000000000000000000000000";
  auto results2 = repo.search(f2, 0, 10);
  CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(0), results2.size());
}

void Sqlite3DownloadResultRepositoryTest::testCountWithFilter()
{
  Sqlite3DownloadResultRepository repo(store_.get());

  // Insert 3 complete + 2 error
  repo.insert(makeDr(error_code::FINISHED));
  setLastFinishedAt(1000);
  repo.insert(makeDr(error_code::FINISHED));
  setLastFinishedAt(2000);
  repo.insert(makeDr(error_code::FINISHED));
  setLastFinishedAt(3000);
  repo.insert(makeDr(error_code::UNKNOWN_ERROR));
  setLastFinishedAt(4000);
  repo.insert(makeDr(error_code::UNKNOWN_ERROR));
  setLastFinishedAt(5000);

  // countAll
  CPPUNIT_ASSERT_EQUAL(static_cast<int64_t>(5), repo.countAll());

  // count by status
  SearchFilter f1;
  f1.statuses = {"complete"};
  CPPUNIT_ASSERT_EQUAL(static_cast<int64_t>(3), repo.countWithFilter(f1));

  SearchFilter f2;
  f2.statuses = {"error"};
  CPPUNIT_ASSERT_EQUAL(static_cast<int64_t>(2), repo.countWithFilter(f2));

  // count by time range
  SearchFilter f3;
  f3.since = 2000;
  f3.until = 4000;
  CPPUNIT_ASSERT_EQUAL(static_cast<int64_t>(3), repo.countWithFilter(f3));

  // Empty filter matches all
  SearchFilter f4;
  CPPUNIT_ASSERT_EQUAL(static_cast<int64_t>(5), repo.countWithFilter(f4));
}

void Sqlite3DownloadResultRepositoryTest::testReconstructedDrHasFullUris()
{
  Sqlite3DownloadResultRepository repo(store_.get());

  auto dr = std::make_shared<DownloadResult>();
  dr->gid = GroupId::create();
  dr->option = option_;
  dr->result = error_code::FINISHED;
  dr->totalLength = 512;
  dr->completedLength = 512;
  dr->uploadLength = 0;
  dr->numPieces = 0;
  dr->pieceLength = 0;
  dr->belongsTo = 0;
  dr->following = 0;
  dr->inMemoryDownload = false;

  // One FileEntry with 1 spent URI + 1 waiting URI
  auto fe = std::make_shared<FileEntry>("/path/test.bin", 512, 0);
  fe->setRequested(true);
  // Add spent URI directly via mutable ref
  fe->getSpentUris().push_back("http://used.example.com/test.bin");
  fe->addUri("http://waiting.example.com/test.bin");
  dr->fileEntries.push_back(fe);

  repo.insert(dr);

  // Reconstruct via range()
  auto results = repo.range(0, 1, false);
  CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(1), results.size());

  const auto& reconstructed = results[0];
  CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(1),
                        reconstructed->fileEntries.size());

  const auto& rfe = reconstructed->fileEntries[0];
  CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(1), rfe->getSpentUris().size());
  CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(1), rfe->getRemainingUris().size());
  CPPUNIT_ASSERT_EQUAL(std::string("http://used.example.com/test.bin"),
                        rfe->getSpentUris()[0]);
  CPPUNIT_ASSERT_EQUAL(std::string("http://waiting.example.com/test.bin"),
                        rfe->getRemainingUris()[0]);
}

} // namespace aria2

#endif // HAVE_SQLITE3
