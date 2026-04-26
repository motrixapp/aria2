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
#ifndef D_SQLITE3_SESSION_STORE_H
#define D_SQLITE3_SESSION_STORE_H

#include "common.h"

#ifdef HAVE_SQLITE3

#include <memory>
#include <vector>

#include "GroupId.h"

namespace aria2 {

class Option;
class RequestGroup;
class Sqlite3PersistenceStore;
class RequestGroupMan;

class Sqlite3SessionStore {
public:
  explicit Sqlite3SessionStore(Sqlite3PersistenceStore* store);
  ~Sqlite3SessionStore();

  Sqlite3SessionStore(const Sqlite3SessionStore&) = delete;
  Sqlite3SessionStore& operator=(const Sqlite3SessionStore&) = delete;

  // Wholesale rewrite: DELETE FROM task; then INSERT one row per active and
  // reserved RG, in queue-position order, all in a single transaction.
  void saveAllTasks(RequestGroupMan* rgman);

  // Read task rows (ordered by queue_position ASC), concatenate their
  // serialized blobs, and parse them into RequestGroup objects appended to out.
  void loadActiveTasksInto(std::vector<std::shared_ptr<RequestGroup>>& out,
                           const std::shared_ptr<Option>& op);

  // Insert a single task row, or UPDATE if its gid already exists.
  // New rows get queue_position = COALESCE(MAX+1, 0).
  // On conflict: preserves created_at and queue_position; refreshes updated_at.
  void upsertTask(const std::shared_ptr<RequestGroup>& rg);

  // Delete the task row identified by gidHex.
  void deleteTask(const std::string& gidHex);

  // Update only the state column (and updated_at) for the given gid.
  void updateTaskState(const std::string& gidHex, const std::string& state);

  // Move a task to newPos using the 3-statement ranged UPDATE (spec §7.4).
  void moveTaskPosition(const std::string& gidHex, int newPos);

private:
  Sqlite3PersistenceStore* store_;

  void removeOrphanTasks(const std::vector<a2_gid_t>& liveGids);
};

} // namespace aria2

#endif // HAVE_SQLITE3

#endif // D_SQLITE3_SESSION_STORE_H
