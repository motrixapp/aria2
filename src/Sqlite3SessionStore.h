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
#include <set>
#include <vector>

#include "GroupId.h"

namespace aria2 {

class Option;
class CookieStorage;
class RequestGroup;
class Sqlite3PersistenceStore;
class RequestGroupMan;

class Sqlite3SessionStore {
public:
  explicit Sqlite3SessionStore(Sqlite3PersistenceStore* store);
  ~Sqlite3SessionStore();

  Sqlite3SessionStore(const Sqlite3SessionStore&) = delete;
  Sqlite3SessionStore& operator=(const Sqlite3SessionStore&) = delete;

  // Upsert active/reserved groups and prune orphan rows. Invalidated legacy
  // metadata bindings retain their task rows as durable recovery evidence.
  void saveAllTasks(RequestGroupMan* rgman);

  // Read task rows in queue-position order and parse each serialized blob.
  // Durable legacy torrents verify their metadata grant before RAM-only
  // restoration; invalid grants never fall back to ordinary URI loading.
  void loadActiveTasksInto(std::vector<std::shared_ptr<RequestGroup>>& out,
                           const std::shared_ptr<Option>& op);

  // Insert a single task row, or UPDATE if its gid already exists.
  // New rows get queue_position = COALESCE(MAX+1, 0).
  // On conflict: preserves created_at and queue_position; refreshes updated_at.
  // useExistingTransaction requires an open transaction owned by the caller.
  void upsertTask(const std::shared_ptr<RequestGroup>& rg,
                  bool persistTaskCookieSnapshot = true,
                  bool useExistingTransaction = false);

  // Atomically replace the durable task-scoped cookie jar. The context row is
  // retained for an empty jar so isolation survives an engine restart.
  void replaceTaskCookies(const std::string& gidHex,
                          const std::shared_ptr<CookieStorage>& storage);

  // Retry the latest in-memory snapshot during the next periodic task save.
  void markTaskCookiesDirty(const std::string& gidHex);

  // Remove only the task-scoped cookie context, preserving the task row and
  // its result/session data.
  void deleteTaskCookies(const std::string& gidHex);

  // Delete the task row identified by gidHex.
  void deleteTask(const std::string& gidHex);

  // Update only the state column (and updated_at) for the given gid.
  void updateTaskState(const std::string& gidHex, const std::string& state);

  // Move a task to newPos using the 3-statement ranged UPDATE (spec §7.4).
  void moveTaskPosition(const std::string& gidHex, int newPos);

private:
  Sqlite3PersistenceStore* store_;
  std::set<std::string> dirtyTaskCookieGids_;

  void removeOrphanTasks(const std::vector<a2_gid_t>& liveGids);
  void restoreTaskCookies(const std::shared_ptr<RequestGroup>& rg);
};

} // namespace aria2

#endif // HAVE_SQLITE3

#endif // D_SQLITE3_SESSION_STORE_H
