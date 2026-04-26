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

#include <sqlite3.h>

#include "DlAbortEx.h"
#include "DownloadResult.h"
#include "FileEntry.h"
#include "GroupId.h"
#include "SessionSerializer.h"
#include "Sqlite3PersistenceStore.h"
#include "error_code.h"
#include "fmt.h"

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
    sqlite3_bind_null(hist, ++idx); // bt_name
    sqlite3_bind_null(hist, ++idx); // bt_announce_list
    sqlite3_bind_null(hist, ++idx); // bt_comment
    sqlite3_bind_null(hist, ++idx); // bt_creation_date
    sqlite3_bind_null(hist, ++idx); // bt_mode
    sqlite3_bind_null(hist, ++idx); // bt_is_private
    sqlite3_bind_null(hist, ++idx); // bt_local_path
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

} // namespace aria2

#endif // HAVE_SQLITE3
