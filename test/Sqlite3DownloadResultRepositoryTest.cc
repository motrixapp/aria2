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
  CPPUNIT_TEST_SUITE_END();

private:
  std::shared_ptr<Option> option_;
  std::unique_ptr<Sqlite3PersistenceStore> store_;
  std::string dbPath_;

public:
  void setUp() CXX11_OVERRIDE;
  void tearDown() CXX11_OVERRIDE;
  void testInsertWritesAllThreeTables();
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

  // Helper: run a COUNT(*) query and return the integer result.
  auto countOf = [&](const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    CPPUNIT_ASSERT_EQUAL(SQLITE_OK,
                         sqlite3_prepare_v2(store_->raw(), sql, -1, &stmt,
                                            nullptr));
    CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
    int n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
  };

  CPPUNIT_ASSERT_EQUAL(1,
                       countOf("SELECT COUNT(*) FROM download_history"));
  CPPUNIT_ASSERT_EQUAL(2,
                       countOf("SELECT COUNT(*) FROM download_history_files"));
  CPPUNIT_ASSERT_EQUAL(4,
                       countOf("SELECT COUNT(*) FROM download_history_file_uris"));
}

} // namespace aria2

#endif // HAVE_SQLITE3
