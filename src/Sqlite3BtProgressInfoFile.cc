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

#include <cstring>
#include <chrono>
#include <vector>

#include <sqlite3.h>

#include "BitfieldMan.h"
#include "BtConstants.h"
#include "ContextAttribute.h"
#include "DlAbortEx.h"
#include "DownloadContext.h"
#include "DownloadFailureException.h"
#include "GroupId.h"
#include "MessageDigest.h"
#include "Option.h"
#include "Piece.h"
#include "PieceStorage.h"
#include "RequestGroup.h"
#include "Sqlite3PersistenceStore.h"
#include "fmt.h"
#include "a2functional.h"
#include "LogFactory.h"
#include "Logger.h"
#include "prefs.h"
#include "util.h"

#ifdef ENABLE_BITTORRENT
#  include "BtRuntime.h"
#  include "PeerStorage.h"
#  include "bittorrent_helper.h"
#endif

namespace aria2 {

namespace {

// RAII wrapper for sqlite3_stmt.
struct StmtGuard {
  sqlite3_stmt* stmt{nullptr};
  ~StmtGuard()
  {
    if (stmt) {
      sqlite3_finalize(stmt);
    }
  }
  operator sqlite3_stmt*() { return stmt; }
};

// Returns current Unix time in milliseconds.
int64_t currentUnixMs()
{
  return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

// Serializes in-flight pieces into a byte vector using the same layout as the
// .aria2 trailer section (all integers in network byte order).
std::vector<unsigned char>
serializeInFlightPieces(const std::shared_ptr<PieceStorage>& pieceStorage)
{
  std::vector<unsigned char> buf;

  std::vector<std::shared_ptr<Piece>> inFlightPieces;
  inFlightPieces.reserve(pieceStorage->countInFlightPiece());
  pieceStorage->getInFlightPieces(inFlightPieces);

  uint32_t numNL = htonl(static_cast<uint32_t>(inFlightPieces.size()));
  const unsigned char* p = reinterpret_cast<const unsigned char*>(&numNL);
  buf.insert(buf.end(), p, p + sizeof(numNL));

  for (const auto& piece : inFlightPieces) {
    uint32_t indexNL = htonl(static_cast<uint32_t>(piece->getIndex()));
    uint32_t lengthNL = htonl(static_cast<uint32_t>(piece->getLength()));
    uint32_t bfLenNL =
        htonl(static_cast<uint32_t>(piece->getBitfieldLength()));

    p = reinterpret_cast<const unsigned char*>(&indexNL);
    buf.insert(buf.end(), p, p + sizeof(indexNL));

    p = reinterpret_cast<const unsigned char*>(&lengthNL);
    buf.insert(buf.end(), p, p + sizeof(lengthNL));

    p = reinterpret_cast<const unsigned char*>(&bfLenNL);
    buf.insert(buf.end(), p, p + sizeof(bfLenNL));

    const unsigned char* bf = piece->getBitfield();
    buf.insert(buf.end(), bf, bf + piece->getBitfieldLength());
  }

  return buf;
}

const char* const kUpsertSql =
    "INSERT INTO task_progress"
    " (gid, ctrl_version, is_torrent, info_hash, piece_length, total_length,"
    "  upload_length, bitfield, in_flight_blob, digest, updated_at)"
    " VALUES (?, 1, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
    " ON CONFLICT(gid) DO UPDATE SET"
    "  ctrl_version   = excluded.ctrl_version,"
    "  is_torrent     = excluded.is_torrent,"
    "  info_hash      = excluded.info_hash,"
    "  piece_length   = excluded.piece_length,"
    "  total_length   = excluded.total_length,"
    "  upload_length  = excluded.upload_length,"
    "  bitfield       = excluded.bitfield,"
    "  in_flight_blob = excluded.in_flight_blob,"
    "  digest         = excluded.digest,"
    "  updated_at     = excluded.updated_at";

const char* const kSelectSql =
    "SELECT is_torrent, info_hash, piece_length, total_length, upload_length,"
    "       bitfield, in_flight_blob"
    " FROM task_progress WHERE gid = ? LIMIT 1";

const char* const kExistsSql =
    "SELECT 1 FROM task_progress WHERE gid = ? LIMIT 1";

const char* const kDeleteSql =
    "DELETE FROM task_progress WHERE gid = ?";

} // namespace

Sqlite3BtProgressInfoFile::Sqlite3BtProgressInfoFile(
    const std::shared_ptr<DownloadContext>& dctx,
    const std::shared_ptr<PieceStorage>& pieceStorage, const Option* option,
    Sqlite3PersistenceStore* store)
    : dctx_(dctx),
      pieceStorage_(pieceStorage),
      option_(option),
      store_(store)
{
  updateFilename();
}

Sqlite3BtProgressInfoFile::~Sqlite3BtProgressInfoFile() = default;

void Sqlite3BtProgressInfoFile::updateFilename()
{
  RequestGroup* rg = dctx_->getOwnerRequestGroup();
  if (rg) {
    gidHex_ = GroupId::toHex(rg->getGID());
  }
  else {
    A2_LOG_WARN("sqlite3-persistence: Sqlite3BtProgressInfoFile constructed"
                " without owning RequestGroup; gidHex remains empty");
  }
  filename_ = "sqlite3://" + store_->path() + "#" + gidHex_;
}

bool Sqlite3BtProgressInfoFile::exists()
{
  StmtGuard stmt;
  sqlite3* db = store_->raw();
  if (sqlite3_prepare_v2(db, kExistsSql, -1, &stmt.stmt, nullptr) !=
      SQLITE_OK) {
    A2_LOG_WARN(fmt("sqlite3-persistence: prepare exists() failed: %s",
                    sqlite3_errmsg(db)));
    return false;
  }
  sqlite3_bind_text(stmt, 1, gidHex_.data(), gidHex_.size(), SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  return rc == SQLITE_ROW;
}

void Sqlite3BtProgressInfoFile::save()
{
  bool isTorrent = false;
#ifdef ENABLE_BITTORRENT
  isTorrent = dctx_->hasAttribute(CTX_ATTR_BT);
#endif

  int64_t uploadLength = 0;
#ifdef ENABLE_BITTORRENT
  if (isTorrent && btRuntime_) {
    uploadLength = btRuntime_->getUploadLengthAtStartup() +
                   static_cast<int64_t>(
                       dctx_->getNetStat().getSessionUploadLength());
  }
#endif

  auto payload = serializeInFlightPieces(pieceStorage_);

  // Compute SHA1 dirty-skip digest.
  auto md = MessageDigest::sha1();
  uint32_t plNL =
      htonl(static_cast<uint32_t>(dctx_->getPieceLength()));
  uint64_t tlNL = hton64(static_cast<uint64_t>(dctx_->getTotalLength()));
  uint64_t ulNL = hton64(static_cast<uint64_t>(uploadLength));
  md->update(&plNL, sizeof(plNL));
  md->update(&tlNL, sizeof(tlNL));
  md->update(&ulNL, sizeof(ulNL));
  md->update(pieceStorage_->getBitfield(),
             pieceStorage_->getBitfieldLength());
  md->update(payload.data(), payload.size());
  auto digest = md->digest();

  if (digest == lastDigest_) {
    return;
  }

  A2_LOG_INFO(fmt("sqlite3-persistence: saving task_progress for gid=%s",
                  gidHex_.c_str()));

  sqlite3* db = store_->raw();

  store_->withTransaction([&]() {
    StmtGuard stmt;
    if (sqlite3_prepare_v2(db, kUpsertSql, -1, &stmt.stmt, nullptr) !=
        SQLITE_OK) {
      throw DL_ABORT_EX(fmt("sqlite3-persistence: prepare UPSERT task_progress"
                            " failed: %s",
                            sqlite3_errmsg(db)));
    }

    // 1: gid
    sqlite3_bind_text(stmt, 1, gidHex_.data(), gidHex_.size(), SQLITE_STATIC);
    // 2: is_torrent
    sqlite3_bind_int(stmt, 2, isTorrent ? 1 : 0);
    // 3: info_hash
#ifdef ENABLE_BITTORRENT
    if (isTorrent) {
      const unsigned char* infoHash = bittorrent::getInfoHash(dctx_);
      sqlite3_bind_blob(stmt, 3, infoHash, INFO_HASH_LENGTH, SQLITE_STATIC);
    }
    else {
      sqlite3_bind_null(stmt, 3);
    }
#else
    sqlite3_bind_null(stmt, 3);
#endif
    // 4: piece_length
    sqlite3_bind_int64(stmt, 4,
                       static_cast<sqlite3_int64>(dctx_->getPieceLength()));
    // 5: total_length
    sqlite3_bind_int64(stmt, 5,
                       static_cast<sqlite3_int64>(dctx_->getTotalLength()));
    // 6: upload_length
    sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(uploadLength));
    // 7: bitfield
    sqlite3_bind_blob(stmt, 7, pieceStorage_->getBitfield(),
                      static_cast<int>(pieceStorage_->getBitfieldLength()),
                      SQLITE_STATIC);
    // 8: in_flight_blob
    sqlite3_bind_blob(stmt, 8, payload.data(),
                      static_cast<int>(payload.size()), SQLITE_TRANSIENT);
    // 9: digest
    sqlite3_bind_blob(stmt, 9, digest.data(),
                      static_cast<int>(digest.size()), SQLITE_TRANSIENT);
    // 10: updated_at
    sqlite3_bind_int64(stmt, 10, static_cast<sqlite3_int64>(currentUnixMs()));

    if (sqlite3_step(stmt) != SQLITE_DONE) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: UPSERT task_progress failed: %s",
              sqlite3_errmsg(db)));
    }
  });

  lastDigest_ = std::move(digest);
  A2_LOG_INFO(fmt("sqlite3-persistence: saved task_progress for gid=%s",
                  gidHex_.c_str()));
}

void Sqlite3BtProgressInfoFile::load()
{
  A2_LOG_INFO(fmt("sqlite3-persistence: loading task_progress for gid=%s",
                  gidHex_.c_str()));

  sqlite3* db = store_->raw();
  StmtGuard stmt;
  if (sqlite3_prepare_v2(db, kSelectSql, -1, &stmt.stmt, nullptr) !=
      SQLITE_OK) {
    throw DL_ABORT_EX(
        fmt("sqlite3-persistence: prepare SELECT task_progress failed: %s",
            sqlite3_errmsg(db)));
  }
  sqlite3_bind_text(stmt, 1, gidHex_.data(), gidHex_.size(), SQLITE_STATIC);

  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    throw DL_ABORT_EX(
        fmt("sqlite3-persistence: no task_progress row for gid=%s",
            gidHex_.c_str()));
  }

  // Column 0: is_torrent
  bool storedIsTorrent = (sqlite3_column_int(stmt, 0) != 0);

  // Column 1: info_hash
  bool infoHashCheckEnabled = false;
#ifdef ENABLE_BITTORRENT
  if (storedIsTorrent && dctx_->hasAttribute(CTX_ATTR_BT)) {
    infoHashCheckEnabled = true;
  }
#endif

  if (infoHashCheckEnabled) {
#ifdef ENABLE_BITTORRENT
    const void* storedHash = sqlite3_column_blob(stmt, 1);
    int storedHashLen = sqlite3_column_bytes(stmt, 1);
    if (storedHashLen != INFO_HASH_LENGTH) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: invalid info_hash length: %d",
              storedHashLen));
    }
    const unsigned char* infoHash = bittorrent::getInfoHash(dctx_);
    if (memcmp(storedHash, infoHash, INFO_HASH_LENGTH) != 0) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: info hash mismatch. expected: %s,"
              " actual: %s",
              util::toHex(infoHash, INFO_HASH_LENGTH).c_str(),
              util::toHex(
                  static_cast<const unsigned char*>(storedHash),
                  static_cast<size_t>(storedHashLen))
                  .c_str()));
    }
#endif
  }

  // Column 2: piece_length
  int64_t pieceLength = sqlite3_column_int64(stmt, 2);
  if (pieceLength == 0) {
    throw DL_ABORT_EX("sqlite3-persistence: piece length must not be 0");
  }

  // Column 3: total_length
  int64_t totalLength = sqlite3_column_int64(stmt, 3);
  if (totalLength != dctx_->getTotalLength()) {
    throw DL_ABORT_EX(
        fmt("sqlite3-persistence: total length mismatch."
            " expected: %" PRId64 ", actual: %" PRId64 "",
            dctx_->getTotalLength(), totalLength));
  }

  // Column 4: upload_length
  int64_t uploadLength = sqlite3_column_int64(stmt, 4);
#ifdef ENABLE_BITTORRENT
  if (storedIsTorrent && btRuntime_) {
    btRuntime_->setUploadLengthAtStartup(uploadLength);
  }
#endif

  // Column 5: bitfield
  const void* bfData = sqlite3_column_blob(stmt, 5);
  int bfLen = sqlite3_column_bytes(stmt, 5);

  // Validate bitfield length.
  int64_t expectedBfLen =
      ((totalLength + pieceLength - 1) / pieceLength + 7) / 8;
  if (static_cast<int64_t>(bfLen) != expectedBfLen) {
    throw DL_ABORT_EX(
        fmt("sqlite3-persistence: bitfield length mismatch."
            " expected: %" PRId64 ", actual: %d",
            expectedBfLen, bfLen));
  }

  // Column 6: in_flight_blob
  const void* ifpData = sqlite3_column_blob(stmt, 6);
  int ifpLen = sqlite3_column_bytes(stmt, 6);

  // Copy data out before the statement is reset/finalized.
  auto savedBitfield = make_unique<unsigned char[]>(static_cast<size_t>(bfLen));
  if (bfData && bfLen > 0) {
    memcpy(savedBitfield.get(), bfData, static_cast<size_t>(bfLen));
  }
  std::vector<unsigned char> ifpBuf;
  if (ifpData && ifpLen > 0) {
    const unsigned char* p = static_cast<const unsigned char*>(ifpData);
    ifpBuf.assign(p, p + static_cast<size_t>(ifpLen));
  }

  // Done reading from the row; finalize early so we can mutate piece storage.
  sqlite3_finalize(stmt.stmt);
  stmt.stmt = nullptr;

  if (pieceLength == static_cast<int64_t>(dctx_->getPieceLength())) {
    pieceStorage_->setBitfield(savedBitfield.get(),
                               static_cast<size_t>(bfLen));

    // Decode the in-flight blob.
    const unsigned char* cur = ifpBuf.data();
    const unsigned char* end = cur + ifpBuf.size();

    auto readU32 = [&](uint32_t& out) -> bool {
      if (end - cur < static_cast<ptrdiff_t>(sizeof(uint32_t))) {
        return false;
      }
      uint32_t v;
      memcpy(&v, cur, sizeof(v));
      cur += sizeof(v);
      out = ntohl(v);
      return true;
    };

    uint32_t numInFlightPiece = 0;
    if (!readU32(numInFlightPiece)) {
      // Empty or truncated blob — treat as zero in-flight pieces.
      A2_LOG_INFO(fmt("sqlite3-persistence: loaded task_progress for gid=%s",
                      gidHex_.c_str()));
      return;
    }

    std::vector<std::shared_ptr<Piece>> inFlightPieces;
    inFlightPieces.reserve(numInFlightPiece);

    while (numInFlightPiece--) {
      uint32_t index, length, pieceBfLen;
      if (!readU32(index) || !readU32(length) || !readU32(pieceBfLen)) {
        throw DL_ABORT_EX(
            "sqlite3-persistence: truncated in_flight_blob");
      }
      if (!(index < dctx_->getNumPieces())) {
        throw DL_ABORT_EX(
            fmt("sqlite3-persistence: piece index out of range: %u", index));
      }
      if (!(static_cast<int64_t>(length) <=
            static_cast<int64_t>(dctx_->getPieceLength()))) {
        throw DL_ABORT_EX(
            fmt("sqlite3-persistence: piece length out of range: %u", length));
      }
      auto piece = std::make_shared<Piece>(index, length);
      if (piece->getBitfieldLength() != pieceBfLen) {
        throw DL_ABORT_EX(
            fmt("sqlite3-persistence: piece bitfield length mismatch."
                " expected: %lu actual: %u",
                static_cast<unsigned long>(piece->getBitfieldLength()),
                pieceBfLen));
      }
      if (end - cur < static_cast<ptrdiff_t>(pieceBfLen)) {
        throw DL_ABORT_EX(
            "sqlite3-persistence: truncated in_flight_blob (bitfield)");
      }
      auto pieceBf = make_unique<unsigned char[]>(pieceBfLen);
      memcpy(pieceBf.get(), cur, pieceBfLen);
      cur += pieceBfLen;
      piece->setBitfield(pieceBf.get(), pieceBfLen);
      piece->setHashType(dctx_->getPieceHashType());
      inFlightPieces.push_back(piece);
    }
    pieceStorage_->addInFlightPiece(inFlightPieces);
  }
  else {
    // Piece length changed — convert bitfield, no in-flight pieces.
    // Parse the in-flight piece count to detect if there is any progress.
    uint32_t numInFlightPiece = 0;
    if (ifpBuf.size() >= sizeof(uint32_t)) {
      memcpy(&numInFlightPiece, ifpBuf.data(), sizeof(numInFlightPiece));
      numInFlightPiece = ntohl(numInFlightPiece);
    }
    BitfieldMan src(static_cast<int32_t>(pieceLength), totalLength);
    src.setBitfield(savedBitfield.get(), static_cast<size_t>(bfLen));
    if ((src.getCompletedLength() || numInFlightPiece) &&
        !option_->getAsBool(PREF_ALLOW_PIECE_LENGTH_CHANGE)) {
      throw DOWNLOAD_FAILURE_EXCEPTION2(
          "WARNING: Detected a change in piece length. You can proceed with"
          " --allow-piece-length-change=true, but you may lose some download"
          " progress.",
          error_code::PIECE_LENGTH_CHANGED);
    }
    BitfieldMan dest(dctx_->getPieceLength(), totalLength);
    util::convertBitfield(&dest, &src);
    pieceStorage_->setBitfield(dest.getBitfield(), dest.getBitfieldLength());
  }
  A2_LOG_INFO(fmt("sqlite3-persistence: loaded task_progress for gid=%s",
                  gidHex_.c_str()));
}

void Sqlite3BtProgressInfoFile::removeFile()
{
  sqlite3* db = store_->raw();
  StmtGuard stmt;
  if (sqlite3_prepare_v2(db, kDeleteSql, -1, &stmt.stmt, nullptr) !=
      SQLITE_OK) {
    throw DL_ABORT_EX(
        fmt("sqlite3-persistence: prepare DELETE task_progress failed: %s",
            sqlite3_errmsg(db)));
  }
  sqlite3_bind_text(stmt, 1, gidHex_.data(), gidHex_.size(), SQLITE_STATIC);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    throw DL_ABORT_EX(
        fmt("sqlite3-persistence: DELETE task_progress failed: %s",
            sqlite3_errmsg(db)));
  }
}

#ifdef ENABLE_BITTORRENT
void Sqlite3BtProgressInfoFile::setBtRuntime(
    const std::shared_ptr<BtRuntime>& btRuntime)
{
  btRuntime_ = btRuntime;
}

void Sqlite3BtProgressInfoFile::setPeerStorage(
    const std::shared_ptr<PeerStorage>& peerStorage)
{
  peerStorage_ = peerStorage;
}
#endif

} // namespace aria2

#endif // HAVE_SQLITE3
