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

#include <chrono>
#include <functional>
#include <sstream>

#include <sqlite3.h>

#include "DlAbortEx.h"
#include "DownloadResult.h"
#include "FileEntry.h"
#include "GroupId.h"
#include "SessionSerializer.h"
#include "Sqlite3PersistenceStore.h"
#include "error_code.h"
#include "fmt.h"
#include "util.h"

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

const char* const kDeleteByGidSql =
    "DELETE FROM download_history WHERE gid = ?";

const char* const kInsertHistorySql =
    "INSERT INTO download_history"
    " (gid, status, result_code, result_message, total_length, completed_length,"
    "  upload_length, num_pieces, piece_length, bitfield, info_hash, dir,"
    "  belongs_to, following, followed_by, in_memory, serialized, metadata_uri,"
    "  bt_name, bt_announce_list, bt_comment, bt_creation_date, bt_mode,"
    "  bt_is_private, bt_local_path, finished_at)"
    " VALUES"
    " (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

const char* const kInsertFilesSql =
    "INSERT INTO download_history_files"
    " (history_id, file_index, path, length, selected)"
    " VALUES (?, ?, ?, ?, ?)";

const char* const kInsertUrisSql =
    "INSERT INTO download_history_file_uris"
    " (history_id, file_index, uri, status) VALUES (?, ?, ?, ?)";

// SELECT columns shared by range() and search() — returns one row per history.
const char* const kSelectCols =
    "SELECT id, gid, status, result_code, result_message, total_length,"
    "       completed_length, upload_length, num_pieces, piece_length,"
    "       bitfield, info_hash, dir, belongs_to, following, followed_by,"
    "       in_memory, serialized, metadata_uri,"
    "       bt_name, bt_announce_list, bt_comment, bt_creation_date, bt_mode,"
    "       bt_is_private, bt_local_path, finished_at"
    " FROM download_history h";

const char* const kRangeAscSql =
    "SELECT id, gid, status, result_code, result_message, total_length,"
    "       completed_length, upload_length, num_pieces, piece_length,"
    "       bitfield, info_hash, dir, belongs_to, following, followed_by,"
    "       in_memory, serialized, metadata_uri,"
    "       bt_name, bt_announce_list, bt_comment, bt_creation_date, bt_mode,"
    "       bt_is_private, bt_local_path, finished_at"
    " FROM download_history"
    " ORDER BY finished_at ASC, id ASC"
    " LIMIT ? OFFSET ?";

const char* const kRangeDescSql =
    "SELECT id, gid, status, result_code, result_message, total_length,"
    "       completed_length, upload_length, num_pieces, piece_length,"
    "       bitfield, info_hash, dir, belongs_to, following, followed_by,"
    "       in_memory, serialized, metadata_uri,"
    "       bt_name, bt_announce_list, bt_comment, bt_creation_date, bt_mode,"
    "       bt_is_private, bt_local_path, finished_at"
    " FROM download_history"
    " ORDER BY finished_at DESC, id DESC"
    " LIMIT ? OFFSET ?";

const char* const kSelectFilesSql =
    "SELECT file_index, path, length, selected"
    " FROM download_history_files"
    " WHERE history_id = ?"
    " ORDER BY file_index ASC";

const char* const kSelectUrisSql =
    "SELECT uri, status FROM download_history_file_uris"
    " WHERE history_id = ? AND file_index = ?";

// Reconstruct a DownloadResult from one row of the main SELECT.
// Column order matches kRangeAscSql / kSelectCols:
//  0=id 1=gid 2=status 3=result_code 4=result_message 5=total_length
//  6=completed_length 7=upload_length 8=num_pieces 9=piece_length
//  10=bitfield 11=info_hash 12=dir 13=belongs_to 14=following 15=followed_by
//  16=in_memory 17=serialized 18=metadata_uri
//  19=bt_name ... (ignored for v1)
//  26=finished_at (unused in DR struct)
std::shared_ptr<DownloadResult> rowToDr(sqlite3_stmt* stmt)
{
  auto dr = std::make_shared<DownloadResult>();

  // col 1: gid (text hex)
  const char* gidTxt =
      reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  if (gidTxt && gidTxt[0]) {
    a2_gid_t gidNum = 0;
    if (GroupId::toNumericId(gidNum, gidTxt) == 0) {
      dr->gid = GroupId::import(gidNum);
    }
  }

  // col 3: result_code
  dr->result =
      static_cast<error_code::Value>(sqlite3_column_int(stmt, 3));

  // col 4: result_message
  const char* msg =
      reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
  if (msg) {
    dr->resultMessage = msg;
  }

  // col 5-7: lengths
  dr->totalLength = sqlite3_column_int64(stmt, 5);
  dr->completedLength = sqlite3_column_int64(stmt, 6);
  dr->uploadLength = sqlite3_column_int64(stmt, 7);

  // col 8: num_pieces
  dr->numPieces = static_cast<size_t>(sqlite3_column_int64(stmt, 8));

  // col 9: piece_length
  dr->pieceLength = sqlite3_column_int(stmt, 9);

  // col 10: bitfield (blob)
  if (sqlite3_column_type(stmt, 10) != SQLITE_NULL) {
    const char* bfData =
        reinterpret_cast<const char*>(sqlite3_column_blob(stmt, 10));
    int bfLen = sqlite3_column_bytes(stmt, 10);
    if (bfData && bfLen > 0) {
      dr->bitfield.assign(bfData, bfLen);
    }
  }

  // col 11: info_hash (blob)
  if (sqlite3_column_type(stmt, 11) != SQLITE_NULL) {
    const char* ihData =
        reinterpret_cast<const char*>(sqlite3_column_blob(stmt, 11));
    int ihLen = sqlite3_column_bytes(stmt, 11);
    if (ihData && ihLen > 0) {
      dr->infoHash.assign(ihData, ihLen);
    }
  }

  // col 12: dir
  const char* dir =
      reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
  if (dir) {
    dr->dir = dir;
  }

  // col 13: belongs_to (text hex or NULL)
  if (sqlite3_column_type(stmt, 13) != SQLITE_NULL) {
    const char* bto =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
    if (bto && bto[0]) {
      a2_gid_t n = 0;
      if (GroupId::toNumericId(n, bto) == 0) {
        dr->belongsTo = n;
      }
    }
  }

  // col 14: following (text hex or NULL)
  if (sqlite3_column_type(stmt, 14) != SQLITE_NULL) {
    const char* fw =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 14));
    if (fw && fw[0]) {
      a2_gid_t n = 0;
      if (GroupId::toNumericId(n, fw) == 0) {
        dr->following = n;
      }
    }
  }

  // col 15: followed_by JSON array — skip for now (reconstruction light)
  // col 16: in_memory
  dr->inMemoryDownload = sqlite3_column_int(stmt, 16) != 0;

  // col 17: serialized — not needed for reconstruction shape
  // col 18: metadata_uri — skip for now
  // BT cols 19-25: skip for v1

  return dr;
}

// Build WHERE clause string + bind-closure vector for a SearchFilter.
// Writes to sqlOss and appends closures to bindFns.
void appendWhereClauses(
    std::ostringstream& sqlOss,
    std::vector<std::function<void(sqlite3_stmt*, int)>>& bindFns,
    const SearchFilter& f,
    bool needJoin)
{
  bool first = true;
  auto sep = [&]() -> const char* {
    if (first) {
      first = false;
      return " WHERE ";
    }
    return " AND ";
  };

  if (!f.statuses.empty()) {
    sqlOss << sep() << "h.status IN (";
    for (size_t i = 0; i < f.statuses.size(); ++i) {
      if (i) {
        sqlOss << ",";
      }
      sqlOss << "?";
      std::string s = f.statuses[i];
      bindFns.push_back([s](sqlite3_stmt* st, int idx) {
        sqlite3_bind_text(st, idx, s.data(), static_cast<int>(s.size()),
                          SQLITE_TRANSIENT);
      });
    }
    sqlOss << ")";
  }

  if (f.since >= 0) {
    sqlOss << sep() << "h.finished_at >= ?";
    int64_t v = f.since;
    bindFns.push_back([v](sqlite3_stmt* st, int idx) {
      sqlite3_bind_int64(st, idx, v);
    });
  }

  if (f.until >= 0) {
    sqlOss << sep() << "h.finished_at <= ?";
    int64_t v = f.until;
    bindFns.push_back([v](sqlite3_stmt* st, int idx) {
      sqlite3_bind_int64(st, idx, v);
    });
  }

  if (!f.infoHashHex.empty()) {
    // info_hash stored as blob; compare raw bytes (spec §8.6)
    sqlOss << sep() << "h.info_hash = ?";
    // Decode hex string to raw bytes using aria2::util::fromHex
    std::string rawHash =
        aria2::util::fromHex(f.infoHashHex.begin(), f.infoHashHex.end());
    if (rawHash.empty()) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: invalid hex in infoHashHex: %s",
              f.infoHashHex.c_str()));
    }
    bindFns.push_back([rawHash](sqlite3_stmt* st, int idx) {
      sqlite3_bind_blob(st, idx, rawHash.data(),
                        static_cast<int>(rawHash.size()), SQLITE_TRANSIENT);
    });
  }

  if (!f.gidPrefix.empty()) {
    sqlOss << sep() << "h.gid LIKE ? || '%'";
    std::string p = f.gidPrefix;
    bindFns.push_back([p](sqlite3_stmt* st, int idx) {
      sqlite3_bind_text(st, idx, p.data(), static_cast<int>(p.size()),
                        SQLITE_TRANSIENT);
    });
  }

  if (f.minSize >= 0) {
    sqlOss << sep() << "h.total_length >= ?";
    int64_t v = f.minSize;
    bindFns.push_back([v](sqlite3_stmt* st, int idx) {
      sqlite3_bind_int64(st, idx, v);
    });
  }

  if (f.maxSize >= 0) {
    sqlOss << sep() << "h.total_length <= ?";
    int64_t v = f.maxSize;
    bindFns.push_back([v](sqlite3_stmt* st, int idx) {
      sqlite3_bind_int64(st, idx, v);
    });
  }

  if (!f.pathLike.empty()) {
    sqlOss << sep()
           << "EXISTS (SELECT 1 FROM download_history_files f2"
           << " WHERE f2.history_id = h.id AND f2.path LIKE ?)";
    std::string p = f.pathLike;
    bindFns.push_back([p](sqlite3_stmt* st, int idx) {
      sqlite3_bind_text(st, idx, p.data(), static_cast<int>(p.size()),
                        SQLITE_TRANSIENT);
    });
  }
  (void)needJoin;
}

// Load file entries (with URIs) for the given history_id.
std::vector<std::shared_ptr<FileEntry>>
loadFileEntriesForHistoryId(sqlite3* db, int64_t historyId)
{
  std::vector<std::shared_ptr<FileEntry>> entries;

  StmtGuard fstmt;
  if (sqlite3_prepare_v2(db, kSelectFilesSql, -1, &fstmt.stmt, nullptr) !=
      SQLITE_OK) {
    throw DL_ABORT_EX(
        fmt("sqlite3-persistence: prepare SELECT files failed: %s",
            sqlite3_errmsg(db)));
  }

  StmtGuard ustmt;
  if (sqlite3_prepare_v2(db, kSelectUrisSql, -1, &ustmt.stmt, nullptr) !=
      SQLITE_OK) {
    throw DL_ABORT_EX(
        fmt("sqlite3-persistence: prepare SELECT uris failed: %s",
            sqlite3_errmsg(db)));
  }

  sqlite3_bind_int64(fstmt, 1, historyId);

  while (sqlite3_step(fstmt) == SQLITE_ROW) {
    int fileIndex = sqlite3_column_int(fstmt, 0);
    const char* path =
        reinterpret_cast<const char*>(sqlite3_column_text(fstmt, 1));
    int64_t length = sqlite3_column_int64(fstmt, 2);
    bool selected = sqlite3_column_int(fstmt, 3) != 0;

    auto fe = std::make_shared<FileEntry>(path ? path : "", length,
                                          static_cast<int64_t>(0));
    fe->setRequested(selected);

    // Load URIs for this file
    sqlite3_reset(ustmt);
    sqlite3_clear_bindings(ustmt);
    sqlite3_bind_int64(ustmt, 1, historyId);
    sqlite3_bind_int(ustmt, 2, fileIndex);

    while (sqlite3_step(ustmt) == SQLITE_ROW) {
      const char* uri =
          reinterpret_cast<const char*>(sqlite3_column_text(ustmt, 0));
      const char* status =
          reinterpret_cast<const char*>(sqlite3_column_text(ustmt, 1));
      if (!uri) {
        continue;
      }
      if (status && std::string(status) == "used") {
        fe->getSpentUris().push_back(uri);
      }
      else {
        fe->addUri(uri);
      }
    }

    entries.push_back(std::move(fe));
  }

  return entries;
}

// Execute a query (already prepared and bound) and collect DownloadResults.
// Also loads file entries for each row.
std::vector<std::shared_ptr<DownloadResult>>
collectResults(sqlite3_stmt* stmt, sqlite3* db)
{
  std::vector<std::shared_ptr<DownloadResult>> results;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    int64_t historyId = sqlite3_column_int64(stmt, 0);
    auto dr = rowToDr(stmt);
    dr->fileEntries = loadFileEntriesForHistoryId(db, historyId);
    results.push_back(std::move(dr));
  }
  return results;
}

} // namespace

Sqlite3DownloadResultRepository::Sqlite3DownloadResultRepository(
    Sqlite3PersistenceStore* store)
    : store_{store}
{
}

Sqlite3DownloadResultRepository::~Sqlite3DownloadResultRepository() = default;

void Sqlite3DownloadResultRepository::insert(
    const std::shared_ptr<DownloadResult>& dr)
{
  // Best-effort drain pending queue first.
  while (!pendingHistoryWrites_.empty()) {
    auto pending = pendingHistoryWrites_.front();
    pendingHistoryWrites_.pop_front();
    try {
      doInsert(pending);
    }
    catch (RecoverableException&) {
      // Re-enqueue and stop draining; we'll try again next time.
      pendingHistoryWrites_.push_front(pending);
      break;
    }
  }
  doInsert(dr);
}

void Sqlite3DownloadResultRepository::doInsert(
    const std::shared_ptr<DownloadResult>& dr)
{
  // Status mapping (spec §8.5):
  //   FINISHED → "complete", REMOVED → "removed", else → "error"
  std::string status;
  switch (dr->result) {
  case error_code::FINISHED:
    status = "complete";
    break;
  case error_code::REMOVED:
    status = "removed";
    break;
  default:
    status = "error";
    break;
  }

  // followed_by JSON serialization: '["<gidHex>", ...]' or NULL.
  std::string followedByJson;
  if (!dr->followedBy.empty()) {
    followedByJson = "[";
    for (size_t i = 0; i < dr->followedBy.size(); ++i) {
      if (i) {
        followedByJson += ",";
      }
      followedByJson += "\"" + GroupId::toHex(dr->followedBy[i]) + "\"";
    }
    followedByJson += "]";
  }

  std::string belongsToHex =
      dr->belongsTo == 0 ? std::string{} : GroupId::toHex(dr->belongsTo);
  std::string followingHex =
      dr->following == 0 ? std::string{} : GroupId::toHex(dr->following);
  std::string gidHex = dr->gid ? dr->gid->toHex() : std::string{};

  // Serialized text via SessionSerializer::renderResult.
  // Pass nullptr rgman — renderResult does not dereference it.
  SessionSerializer ser(nullptr);
  std::string serialized = ser.renderResult(dr);

  // Metadata URI
  std::string metadataUri =
      dr->metadataInfo ? dr->metadataInfo->getUri() : "";

  const int64_t now = currentUnixMs();
  sqlite3* db = store_->raw();

  store_->withTransaction([&]() {
    // Step 1: INSERT download_history
    StmtGuard hist;
    if (sqlite3_prepare_v2(db, kInsertHistorySql, -1, &hist.stmt, nullptr) !=
        SQLITE_OK) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: prepare INSERT download_history failed: %s",
              sqlite3_errmsg(db)));
    }

    int idx = 0;
    sqlite3_bind_text(hist, ++idx, gidHex.data(),
                      static_cast<int>(gidHex.size()), SQLITE_STATIC);
    sqlite3_bind_text(hist, ++idx, status.data(),
                      static_cast<int>(status.size()), SQLITE_STATIC);
    sqlite3_bind_int(hist, ++idx, static_cast<int>(dr->result));
    sqlite3_bind_text(hist, ++idx, dr->resultMessage.data(),
                      static_cast<int>(dr->resultMessage.size()), SQLITE_STATIC);
    sqlite3_bind_int64(hist, ++idx, dr->totalLength);
    sqlite3_bind_int64(hist, ++idx, dr->completedLength);
    sqlite3_bind_int64(hist, ++idx, dr->uploadLength);
    sqlite3_bind_int64(hist, ++idx, static_cast<sqlite3_int64>(dr->numPieces));
    sqlite3_bind_int(hist, ++idx, dr->pieceLength);
    if (dr->bitfield.empty()) {
      sqlite3_bind_null(hist, ++idx);
    }
    else {
      sqlite3_bind_blob(hist, ++idx, dr->bitfield.data(),
                        static_cast<int>(dr->bitfield.size()), SQLITE_STATIC);
    }
    if (dr->infoHash.empty()) {
      sqlite3_bind_null(hist, ++idx);
    }
    else {
      sqlite3_bind_blob(hist, ++idx, dr->infoHash.data(),
                        static_cast<int>(dr->infoHash.size()), SQLITE_STATIC);
    }
    sqlite3_bind_text(hist, ++idx, dr->dir.data(),
                      static_cast<int>(dr->dir.size()), SQLITE_STATIC);
    if (belongsToHex.empty()) {
      sqlite3_bind_null(hist, ++idx);
    }
    else {
      sqlite3_bind_text(hist, ++idx, belongsToHex.data(),
                        static_cast<int>(belongsToHex.size()), SQLITE_STATIC);
    }
    if (followingHex.empty()) {
      sqlite3_bind_null(hist, ++idx);
    }
    else {
      sqlite3_bind_text(hist, ++idx, followingHex.data(),
                        static_cast<int>(followingHex.size()), SQLITE_STATIC);
    }
    if (followedByJson.empty()) {
      sqlite3_bind_null(hist, ++idx);
    }
    else {
      sqlite3_bind_text(hist, ++idx, followedByJson.data(),
                        static_cast<int>(followedByJson.size()), SQLITE_STATIC);
    }
    sqlite3_bind_int(hist, ++idx, dr->inMemoryDownload ? 1 : 0);
    sqlite3_bind_text(hist, ++idx, serialized.data(),
                      static_cast<int>(serialized.size()), SQLITE_STATIC);
    if (metadataUri.empty()) {
      sqlite3_bind_null(hist, ++idx);
    }
    else {
      sqlite3_bind_text(hist, ++idx, metadataUri.data(),
                        static_cast<int>(metadataUri.size()), SQLITE_STATIC);
    }
    // BT detail columns: bind nulls for v1. Future tasks will extract from
    // dr->attrs.
    sqlite3_bind_null(hist, ++idx);  // bt_name
    sqlite3_bind_null(hist, ++idx);  // bt_announce_list
    sqlite3_bind_null(hist, ++idx);  // bt_comment
    sqlite3_bind_null(hist, ++idx);  // bt_creation_date
    sqlite3_bind_null(hist, ++idx);  // bt_mode
    sqlite3_bind_int(hist, ++idx, 0); // bt_is_private (NOT NULL DEFAULT 0)
    sqlite3_bind_null(hist, ++idx);  // bt_local_path
    sqlite3_bind_int64(hist, ++idx, now);

    if (sqlite3_step(hist) != SQLITE_DONE) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: INSERT download_history failed: %s",
              sqlite3_errmsg(db)));
    }

    int64_t historyId = sqlite3_last_insert_rowid(db);

    // Step 2: INSERT download_history_files (one row per fileEntry)
    StmtGuard files;
    if (sqlite3_prepare_v2(db, kInsertFilesSql, -1, &files.stmt, nullptr) !=
        SQLITE_OK) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: prepare INSERT files failed: %s",
              sqlite3_errmsg(db)));
    }
    StmtGuard uris;
    if (sqlite3_prepare_v2(db, kInsertUrisSql, -1, &uris.stmt, nullptr) !=
        SQLITE_OK) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: prepare INSERT uris failed: %s",
              sqlite3_errmsg(db)));
    }

    for (size_t fi = 0; fi < dr->fileEntries.size(); ++fi) {
      const auto& fe = dr->fileEntries[fi];
      sqlite3_bind_int64(files, 1, historyId);
      sqlite3_bind_int(files, 2, static_cast<int>(fi));
      // SQLITE_TRANSIENT because the const-ref is owned elsewhere.
      sqlite3_bind_text(files, 3, fe->getPath().data(),
                        static_cast<int>(fe->getPath().size()),
                        SQLITE_TRANSIENT);
      sqlite3_bind_int64(files, 4, fe->getLength());
      sqlite3_bind_int(files, 5, fe->isRequested() ? 1 : 0);

      if (sqlite3_step(files) != SQLITE_DONE) {
        throw DL_ABORT_EX(
            fmt("sqlite3-persistence: INSERT files failed: %s",
                sqlite3_errmsg(db)));
      }
      sqlite3_reset(files);
      sqlite3_clear_bindings(files);

      // Step 3: INSERT one row per URI on this file.
      auto bindUri = [&](const std::string& uri, const char* state) {
        sqlite3_bind_int64(uris, 1, historyId);
        sqlite3_bind_int(uris, 2, static_cast<int>(fi));
        sqlite3_bind_text(uris, 3, uri.data(), static_cast<int>(uri.size()),
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(uris, 4, state, -1, SQLITE_STATIC);
        if (sqlite3_step(uris) != SQLITE_DONE) {
          throw DL_ABORT_EX(
              fmt("sqlite3-persistence: INSERT uris failed: %s",
                  sqlite3_errmsg(db)));
        }
        sqlite3_reset(uris);
        sqlite3_clear_bindings(uris);
      };
      for (const auto& u : fe->getSpentUris()) {
        bindUri(u, "used");
      }
      for (const auto& u : fe->getRemainingUris()) {
        bindUri(u, "waiting");
      }
    }
  });
}

int64_t Sqlite3DownloadResultRepository::countAll() const
{
  sqlite3* db = store_->raw();
  StmtGuard stmt;
  const char* sql = "SELECT COUNT(*) FROM download_history";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt.stmt, nullptr) != SQLITE_OK) {
    throw DL_ABORT_EX(
        fmt("sqlite3-persistence: prepare COUNT(*) failed: %s",
            sqlite3_errmsg(db)));
  }
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    return 0;
  }
  return sqlite3_column_int64(stmt, 0);
}

int64_t
Sqlite3DownloadResultRepository::countWithFilter(const SearchFilter& f) const
{
  sqlite3* db = store_->raw();

  std::ostringstream sqlOss;
  sqlOss << "SELECT COUNT(*) FROM download_history h";
  std::vector<std::function<void(sqlite3_stmt*, int)>> bindFns;
  appendWhereClauses(sqlOss, bindFns, f, false);

  std::string sql = sqlOss.str();
  StmtGuard stmt;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt.stmt, nullptr) !=
      SQLITE_OK) {
    throw DL_ABORT_EX(
        fmt("sqlite3-persistence: prepare countWithFilter failed: %s",
            sqlite3_errmsg(db)));
  }

  int paramIdx = 1;
  for (auto& fn : bindFns) {
    fn(stmt, paramIdx++);
  }

  if (sqlite3_step(stmt) != SQLITE_ROW) {
    return 0;
  }
  return sqlite3_column_int64(stmt, 0);
}

std::vector<std::shared_ptr<DownloadResult>>
Sqlite3DownloadResultRepository::range(int offset, int num, bool desc) const
{
  sqlite3* db = store_->raw();

  const char* sql = desc ? kRangeDescSql : kRangeAscSql;
  StmtGuard stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt.stmt, nullptr) != SQLITE_OK) {
    throw DL_ABORT_EX(
        fmt("sqlite3-persistence: prepare range() failed: %s",
            sqlite3_errmsg(db)));
  }
  sqlite3_bind_int(stmt, 1, num);
  sqlite3_bind_int(stmt, 2, offset);

  return collectResults(stmt, db);
}

std::vector<std::shared_ptr<DownloadResult>>
Sqlite3DownloadResultRepository::search(const SearchFilter& f, int offset,
                                         int num) const
{
  sqlite3* db = store_->raw();

  std::ostringstream sqlOss;
  sqlOss << kSelectCols;
  std::vector<std::function<void(sqlite3_stmt*, int)>> bindFns;
  appendWhereClauses(sqlOss, bindFns, f, false);
  sqlOss << " ORDER BY h.finished_at DESC, h.id DESC LIMIT ? OFFSET ?";

  std::string sql = sqlOss.str();
  StmtGuard stmt;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt.stmt, nullptr) !=
      SQLITE_OK) {
    throw DL_ABORT_EX(
        fmt("sqlite3-persistence: prepare search() failed: %s",
            sqlite3_errmsg(db)));
  }

  int paramIdx = 1;
  for (auto& fn : bindFns) {
    fn(stmt, paramIdx++);
  }
  sqlite3_bind_int(stmt, paramIdx++, num);
  sqlite3_bind_int(stmt, paramIdx, offset);

  return collectResults(stmt, db);
}

void Sqlite3DownloadResultRepository::trimToCap(int historyLimit,
                                                  bool keepUnfinished)
{
  if (historyLimit < 0) {
    return; // unlimited
  }

  sqlite3* db = store_->raw();

  store_->withTransaction([&]() {
    // Build the DELETE with an optional WHERE status != 'error' filter.
    std::ostringstream sqlOss;
    sqlOss << "DELETE FROM download_history WHERE id IN ("
           << "SELECT id FROM download_history";
    if (keepUnfinished) {
      sqlOss << " WHERE status != 'error'";
    }
    sqlOss << " ORDER BY finished_at ASC, id ASC"
           << " LIMIT MAX(0, (SELECT COUNT(*) FROM download_history";
    if (keepUnfinished) {
      sqlOss << " WHERE status != 'error'";
    }
    sqlOss << ") - ?))";

    std::string sql = sqlOss.str();
    StmtGuard stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt.stmt, nullptr) !=
        SQLITE_OK) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: prepare trimToCap failed: %s",
              sqlite3_errmsg(db)));
    }
    sqlite3_bind_int(stmt, 1, historyLimit);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: trimToCap DELETE failed: %s",
              sqlite3_errmsg(db)));
    }
  });
}

bool Sqlite3DownloadResultRepository::deleteByGid(a2_gid_t gid)
{
  bool deleted = false;
  store_->withTransaction([&]() {
    StmtGuard stmt;
    if (sqlite3_prepare_v2(store_->raw(), kDeleteByGidSql, -1, &stmt.stmt,
                           nullptr) != SQLITE_OK) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: prepare DELETE history by gid failed: %s",
              sqlite3_errmsg(store_->raw())));
    }
    auto gidHex = GroupId::toHex(gid);
    sqlite3_bind_text(stmt, 1, gidHex.data(),
                      static_cast<int>(gidHex.size()), SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: DELETE history by gid failed: %s",
              sqlite3_errmsg(store_->raw())));
    }
    deleted = (sqlite3_changes(store_->raw()) > 0);
  });
  return deleted;
}

void Sqlite3DownloadResultRepository::purgeAll()
{
  store_->withTransaction([&]() {
    if (sqlite3_exec(store_->raw(), "DELETE FROM download_history",
                     nullptr, nullptr, nullptr) != SQLITE_OK) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: purgeAll failed: %s",
              sqlite3_errmsg(store_->raw())));
    }
    pendingHistoryWrites_.clear();
  });
}

void Sqlite3DownloadResultRepository::enqueuePending(
    const std::shared_ptr<DownloadResult>& dr)
{
  if (pendingHistoryWrites_.size() >= kMaxPendingWrites) {
    pendingHistoryWrites_.pop_front();
  }
  pendingHistoryWrites_.push_back(dr);
}

} // namespace aria2

#endif // HAVE_SQLITE3
