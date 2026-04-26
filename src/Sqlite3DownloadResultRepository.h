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
#ifndef D_SQLITE3_DOWNLOAD_RESULT_REPOSITORY_H
#define D_SQLITE3_DOWNLOAD_RESULT_REPOSITORY_H

#include "common.h"

#ifdef HAVE_SQLITE3

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "GroupId.h"

namespace aria2 {

class Sqlite3PersistenceStore;
struct DownloadResult;

// Filter used by countWithFilter() and search().
struct SearchFilter {
  // Direct download_history columns
  std::vector<std::string> statuses; // empty = no filter
  int64_t since = -1;                // -1 = no filter; else finished_at >= since
  int64_t until = -1;                // -1 = no filter; else finished_at <= until
  std::string infoHashHex;           // empty = no filter; hex bytes → info_hash
  std::string gidPrefix;             // empty = no filter; gid LIKE ?||'%'
  int64_t minSize = -1;              // -1 = no filter; else total_length >= ?
  int64_t maxSize = -1;              // -1 = no filter; else total_length <= ?
  // JOIN required
  std::string pathLike; // empty = no filter; path LIKE ?
};

class Sqlite3DownloadResultRepository {
public:
  explicit Sqlite3DownloadResultRepository(Sqlite3PersistenceStore* store);
  ~Sqlite3DownloadResultRepository();

  Sqlite3DownloadResultRepository(const Sqlite3DownloadResultRepository&) =
      delete;
  Sqlite3DownloadResultRepository& operator=(
      const Sqlite3DownloadResultRepository&) = delete;

  // Inserts a single DownloadResult into download_history (parent),
  // download_history_files (one row per fileEntry), and
  // download_history_file_uris (one row per URI on each fileEntry).
  // Single transaction; partial failure rolls back the whole 3-table write.
  // Drains pendingHistoryWrites_ (best-effort) before inserting dr.
  void insert(const std::shared_ptr<DownloadResult>& dr);

  // Deletes the row in download_history matching gid.  CASCADE removes
  // child rows in download_history_files and download_history_file_uris.
  // Returns true if at least one row was deleted.
  bool deleteByGid(a2_gid_t gid);

  // Deletes all rows from download_history (and cascaded children).
  // Also discards any pending writes.
  void purgeAll();

  // Appends dr to the pending-retry queue.  If the queue exceeds
  // kMaxPendingWrites entries the oldest entry is dropped to bound growth.
  void enqueuePending(const std::shared_ptr<DownloadResult>& dr);

  // Returns total count of rows in download_history.
  int64_t countAll() const;

  // Returns count of rows matching the given filter.
  int64_t countWithFilter(const SearchFilter& f) const;

  // Returns up to num DownloadResults starting at offset, ordered by
  // finished_at (and id) ascending (desc=false) or descending (desc=true).
  std::vector<std::shared_ptr<DownloadResult>> range(int offset, int num,
                                                      bool desc) const;

  // Returns up to num DownloadResults matching f, starting at offset,
  // ordered by finished_at DESC, id DESC.
  std::vector<std::shared_ptr<DownloadResult>> search(const SearchFilter& f,
                                                       int offset,
                                                       int num) const;

  // Removes oldest rows (FIFO) so that at most historyLimit rows remain.
  // If historyLimit < 0, does nothing (unlimited).
  // If keepUnfinished is true, rows with status='error' are exempt.
  void trimToCap(int historyLimit, bool keepUnfinished);

private:
  // Actual 3-table INSERT logic; called by insert() and the drain loop.
  void doInsert(const std::shared_ptr<DownloadResult>& dr);

  Sqlite3PersistenceStore* store_;

  // Pending writes that failed a previous insert attempt.
  std::deque<std::shared_ptr<DownloadResult>> pendingHistoryWrites_;

  static constexpr size_t kMaxPendingWrites = 1024;
};

} // namespace aria2

#endif // HAVE_SQLITE3

#endif // D_SQLITE3_DOWNLOAD_RESULT_REPOSITORY_H
