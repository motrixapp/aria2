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
#include "Sqlite3SessionStore.h"

#ifdef HAVE_SQLITE3

#include <chrono>

#include <sqlite3.h>

#include "DlAbortEx.h"
#include "GroupId.h"
#include "MessageDigest.h"
#include "RequestGroup.h"
#include "RequestGroupMan.h"
#include "SessionSerializer.h"
#include "Sqlite3PersistenceStore.h"
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

const char* const kInsertTaskSql =
    "INSERT INTO task"
    " (gid, state, serialized, queue_position, digest, created_at, updated_at)"
    " VALUES (?, ?, ?, ?, ?, ?, ?)";

} // namespace

Sqlite3SessionStore::Sqlite3SessionStore(Sqlite3PersistenceStore* store)
    : store_(store)
{
}

Sqlite3SessionStore::~Sqlite3SessionStore() = default;

void Sqlite3SessionStore::saveAllTasks(RequestGroupMan* rgman)
{
  const int64_t now = currentUnixMs();
  SessionSerializer ser(rgman);

  sqlite3* db = store_->raw();

  store_->withTransaction([&]() {
    // 1) DELETE FROM task
    if (sqlite3_exec(db, "DELETE FROM task", nullptr, nullptr, nullptr) !=
        SQLITE_OK) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: DELETE FROM task failed: %s",
              sqlite3_errmsg(db)));
    }

    // 2) Prepare INSERT statement
    StmtGuard stmt;
    if (sqlite3_prepare_v2(db, kInsertTaskSql, -1, &stmt.stmt, nullptr) !=
        SQLITE_OK) {
      throw DL_ABORT_EX(
          fmt("sqlite3-persistence: prepare INSERT task failed: %s",
              sqlite3_errmsg(db)));
    }

    int pos = 0;

    auto handleRG = [&](const std::shared_ptr<RequestGroup>& rg) {
      std::string text = ser.renderOne(rg);
      if (text.empty()) {
        return; // RG was skipped by serializer
      }
      std::string state = rg->isPauseRequested() ? "paused" : "waiting";

      // Compute digest for dirty-skip in future tasks.
      auto md = MessageDigest::sha1();
      md->update(text.data(), text.size());
      md->update(state.data(), state.size());
      std::string digest = md->digest();

      auto gidHex = GroupId::toHex(rg->getGID());

      sqlite3_bind_text(stmt, 1, gidHex.data(),
                        static_cast<int>(gidHex.size()), SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, state.data(),
                        static_cast<int>(state.size()), SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, text.data(),
                        static_cast<int>(text.size()), SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 4, pos++);
      sqlite3_bind_blob(stmt, 5, digest.data(),
                        static_cast<int>(digest.size()), SQLITE_TRANSIENT);
      sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(now));
      sqlite3_bind_int64(stmt, 7, static_cast<sqlite3_int64>(now));

      if (sqlite3_step(stmt) != SQLITE_DONE) {
        throw DL_ABORT_EX(
            fmt("sqlite3-persistence: INSERT task failed: %s",
                sqlite3_errmsg(db)));
      }
      sqlite3_reset(stmt);
      sqlite3_clear_bindings(stmt);
    };

    for (const auto& rg : rgman->getRequestGroups()) {
      handleRG(rg);
    }
    for (const auto& rg : rgman->getReservedGroups()) {
      handleRG(rg);
    }
  });
}

} // namespace aria2

#endif // HAVE_SQLITE3
