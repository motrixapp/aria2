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
#include "Sqlite3BtProgressInfoFile.h"

#ifdef HAVE_SQLITE3

#include <cstdlib>
#include <cstdio>

#include <cppunit/extensions/HelperMacros.h>

#include "BitfieldMan.h"
#include "DownloadContext.h"
#include "FileEntry.h"
#include "GroupId.h"
#include "MockPieceStorage.h"
#include "Option.h"
#include "RequestGroup.h"
#include "Sqlite3PersistenceStore.h"
#include "TestUtil.h"
#include "array_fun.h"
#include "prefs.h"
#include "util.h"

#ifdef ENABLE_BITTORRENT
#  include "BtRuntime.h"
#  include "MockPeerStorage.h"
#  include "bittorrent_helper.h"
#endif

namespace aria2 {

class Sqlite3BtProgressInfoFileTest : public CppUnit::TestFixture {
  CPPUNIT_TEST_SUITE(Sqlite3BtProgressInfoFileTest);
#ifdef ENABLE_BITTORRENT
  CPPUNIT_TEST(testSaveRoundTrip);
#endif
  CPPUNIT_TEST_SUITE_END();

private:
  std::shared_ptr<Option> option_;
  std::shared_ptr<DownloadContext> dctx_;
  std::shared_ptr<MockPieceStorage> pieceStorage_;
  std::shared_ptr<BitfieldMan> bitfield_;
  std::shared_ptr<RequestGroup> rg_;
  std::unique_ptr<Sqlite3PersistenceStore> store_;
  std::string dbPath_;
#ifdef ENABLE_BITTORRENT
  std::shared_ptr<MockPeerStorage> peerStorage_;
  std::shared_ptr<BtRuntime> btRuntime_;
#endif

public:
  void setUp() override
  {
    option_.reset(new Option());
    option_->put(PREF_DIR, A2_TEST_OUT_DIR);

    dbPath_ = std::string(A2_TEST_OUT_DIR) + "/sqlite3-bt-progress-test.db";
    std::remove(dbPath_.c_str());
    std::remove((dbPath_ + "-wal").c_str());
    std::remove((dbPath_ + "-shm").c_str());

    store_.reset(new Sqlite3PersistenceStore(dbPath_));
    store_->open();

    bitfield_.reset(new BitfieldMan(1_k, 80_k));
    pieceStorage_.reset(new MockPieceStorage());
    pieceStorage_->setBitfield(bitfield_.get());

#ifdef ENABLE_BITTORRENT
    static unsigned char infoHash[] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa,
        0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0xff, 0xff, 0xff, 0xff};
    dctx_.reset(new DownloadContext());
    {
      auto torrentAttrs = make_unique<TorrentAttribute>();
      torrentAttrs->infoHash.assign(std::begin(infoHash), std::end(infoHash));
      dctx_->setAttribute(CTX_ATTR_BT, std::move(torrentAttrs));
    }
    const std::shared_ptr<FileEntry> fileEntries[] = {
        std::shared_ptr<FileEntry>(new FileEntry("/path/to/file", 80_k, 0))};
    dctx_->setFileEntries(std::begin(fileEntries), std::end(fileEntries));
    dctx_->setPieceLength(1_k);
    peerStorage_.reset(new MockPeerStorage());
    btRuntime_.reset(new BtRuntime());
#endif

    // gid required so Sqlite3BtProgressInfoFile can derive gidHex_.
    auto gid = GroupId::import(0xdeadbeefcafebabeULL);
    rg_.reset(new RequestGroup(gid, option_));
    rg_->setDownloadContext(dctx_);
    dctx_->setOwnerRequestGroup(rg_.get());

    // task_progress has FOREIGN KEY (gid) REFERENCES task(gid) ON DELETE
    // CASCADE. Insert a parent task row so the foreign-key insert succeeds.
    sqlite3* db = store_->raw();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(
        db,
        "INSERT INTO task(gid, state, serialized, queue_position, digest,"
        " created_at, updated_at)"
        " VALUES (?, 'waiting', '', 0, X'', 0, 0)",
        -1, &stmt, nullptr);
    auto gidHex = GroupId::toHex(rg_->getGID());
    sqlite3_bind_text(stmt, 1, gidHex.data(), gidHex.size(), SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  void tearDown() override
  {
    rg_.reset();
    if (store_) {
      store_->finalCheckpointAndClose();
    }
    store_.reset();
    std::remove(dbPath_.c_str());
    std::remove((dbPath_ + "-wal").c_str());
    std::remove((dbPath_ + "-shm").c_str());
  }

#ifdef ENABLE_BITTORRENT
  void testSaveRoundTrip();
#endif
};

CPPUNIT_TEST_SUITE_REGISTRATION(Sqlite3BtProgressInfoFileTest);

#ifdef ENABLE_BITTORRENT
void Sqlite3BtProgressInfoFileTest::testSaveRoundTrip()
{
  bitfield_->setBit(0);
  bitfield_->setBit(3);
  bitfield_->setBit(70);

  Sqlite3BtProgressInfoFile saver(dctx_, pieceStorage_, option_.get(),
                                  store_.get());
  saver.setBtRuntime(btRuntime_);
  saver.setPeerStorage(peerStorage_);
  saver.save();

  // Fresh BitfieldMan + PieceStorage on the load side to avoid trivial pass.
  auto loadBf = std::make_shared<BitfieldMan>(1_k, 80_k);
  auto loadPs = std::make_shared<MockPieceStorage>();
  loadPs->setBitfield(loadBf.get());

  Sqlite3BtProgressInfoFile loader(dctx_, loadPs, option_.get(), store_.get());
  loader.setBtRuntime(btRuntime_);
  loader.setPeerStorage(peerStorage_);
  CPPUNIT_ASSERT(loader.exists());
  loader.load();

  CPPUNIT_ASSERT(loadBf->isBitSet(0));
  CPPUNIT_ASSERT(loadBf->isBitSet(3));
  CPPUNIT_ASSERT(loadBf->isBitSet(70));
  CPPUNIT_ASSERT(!loadBf->isBitSet(1));
  CPPUNIT_ASSERT(!loadBf->isBitSet(50));
}
#endif

} // namespace aria2

#endif // HAVE_SQLITE3
