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
#ifndef D_SQLITE3_PERSISTENCE_STORE_H
#define D_SQLITE3_PERSISTENCE_STORE_H

#include "common.h"

#ifdef HAVE_SQLITE3

#include <sqlite3.h>

#include <cstdint>
#include <functional>
#include <string>

namespace aria2 {

class Sqlite3PersistenceStore {
public:
  explicit Sqlite3PersistenceStore(std::string dbPath);
  ~Sqlite3PersistenceStore();

  Sqlite3PersistenceStore(const Sqlite3PersistenceStore&) = delete;
  Sqlite3PersistenceStore& operator=(const Sqlite3PersistenceStore&) = delete;

  void open();
  void finalCheckpointAndClose();

  std::string queryPragma(const std::string& name) const;
  void withTransaction(const std::function<void()>& fn);

  sqlite3* raw() const { return db_; }
  const std::string& path() const { return dbPath_; }

private:
  void applyPragmas();
  void maybeCheckpoint();

  std::string dbPath_;
  sqlite3* db_;
  uint64_t commitCounter_;
};

} // namespace aria2

#endif // HAVE_SQLITE3

#endif // D_SQLITE3_PERSISTENCE_STORE_H
