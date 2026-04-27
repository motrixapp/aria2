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
#include "common.h"

#ifdef HAVE_SQLITE3

#include <cppunit/extensions/HelperMacros.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <sqlite3.h>

#include "RecoverableException.h"
#include "Sqlite3PersistenceStore.h"

namespace aria2 {

namespace {

void removeFile(const std::string& path)
{
  std::remove(path.c_str());
}

void removeDb(const std::string& path)
{
  std::remove(path.c_str());
  std::remove((path + "-wal").c_str());
  std::remove((path + "-shm").c_str());
}

void removeCorruptFiles(const std::string& path)
{
  std::string dir = ".";
  std::string prefix;
  auto slash = path.rfind('/');
  if (slash != std::string::npos) {
    dir = path.substr(0, slash);
    prefix = path.substr(slash + 1) + ".corrupt.";
  }
  else {
    prefix = path + ".corrupt.";
  }

  DIR* d = opendir(dir.c_str());
  if (!d) {
    return;
  }
  struct dirent* ent;
  while ((ent = readdir(d)) != nullptr) {
    std::string name = ent->d_name;
    if (name.rfind(prefix, 0) == 0) {
      std::string full = dir + "/" + name;
      std::remove(full.c_str());
      std::remove((full + "-wal").c_str());
      std::remove((full + "-shm").c_str());
    }
  }
  closedir(d);
}

int insertTask(sqlite3* db, const std::string& gid, int queuePos)
{
  const char* sql =
      "INSERT INTO task (gid, state, serialized, queue_position, "
      "                  digest, created_at, updated_at) "
      "VALUES (?, 'active', 'data', ?, X'', 0, 0);";
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return rc;
  }
  sqlite3_bind_text(stmt, 1, gid.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, queuePos);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc;
}

int64_t countRows(sqlite3* db, const std::string& table)
{
  std::string sql = "SELECT COUNT(*) FROM " + table + ";";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    return -1;
  }
  int64_t result = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    result = sqlite3_column_int64(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return result;
}

bool quickCheckOk(sqlite3* db)
{
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "PRAGMA quick_check;", -1, &stmt, nullptr) !=
      SQLITE_OK) {
    return false;
  }
  bool ok = false;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char* r =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    if (r && std::string(r) == "ok") {
      ok = true;
    }
  }
  sqlite3_finalize(stmt);
  return ok;
}

bool integrityCheckOk(sqlite3* db)
{
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &stmt, nullptr) !=
      SQLITE_OK) {
    return false;
  }
  bool ok = false;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char* r =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    if (r && std::string(r) == "ok") {
      ok = true;
    }
  }
  sqlite3_finalize(stmt);
  return ok;
}

bool corruptFileExists(const std::string& path)
{
  std::string dir = ".";
  std::string prefix;
  auto slash = path.rfind('/');
  if (slash != std::string::npos) {
    dir = path.substr(0, slash);
    prefix = path.substr(slash + 1) + ".corrupt.";
  }
  else {
    prefix = path + ".corrupt.";
  }

  DIR* d = opendir(dir.c_str());
  if (!d) {
    return false;
  }
  bool found = false;
  struct dirent* ent;
  while ((ent = readdir(d)) != nullptr) {
    std::string name = ent->d_name;
    if (name.rfind(prefix, 0) == 0) {
      found = true;
      break;
    }
  }
  closedir(d);
  return found;
}

} // namespace

class Sqlite3CrashRecoveryTest : public CppUnit::TestFixture {
  CPPUNIT_TEST_SUITE(Sqlite3CrashRecoveryTest);
  CPPUNIT_TEST(testLeakedHandleWalRecovery);
  CPPUNIT_TEST(testMidMigrationThrowPreservesUserVersion);
  CPPUNIT_TEST(testForkedMidUpsertAbort);
  CPPUNIT_TEST(testCorruptByteMangleRebuild);
  CPPUNIT_TEST(testForkedHundredKInsertsKilledIntegrityOk);
  CPPUNIT_TEST_SUITE_END();

public:
  void testLeakedHandleWalRecovery();
  void testMidMigrationThrowPreservesUserVersion();
  void testForkedMidUpsertAbort();
  void testCorruptByteMangleRebuild();
  void testForkedHundredKInsertsKilledIntegrityOk();
};

CPPUNIT_TEST_SUITE_REGISTRATION(Sqlite3CrashRecoveryTest);

// L7-1: WAL recovery after handle abandon.
//
// Simulate a crash after committing to the WAL but before clean shutdown.
// We heap-allocate the store and intentionally release the unique_ptr without
// running the destructor, so sqlite3_close_v2 is never called. The next open
// must find the committed row via SQLite's WAL replay on startup.
void Sqlite3CrashRecoveryTest::testLeakedHandleWalRecovery()
{
  std::string path =
      std::string(A2_TEST_OUT_DIR) + "/test_crash_l7_1.db";
  removeDb(path);

  {
    auto store = std::make_unique<Sqlite3PersistenceStore>(path);
    store->open();

    store->withTransaction([&]() {
      CPPUNIT_ASSERT_EQUAL(SQLITE_DONE,
                           insertTask(store->raw(), "aaaaaaaaaaaaaaaa", 0));
    });

    // Abandon the unique_ptr: the destructor never runs, so the sqlite3
    // handle is not closed and the WAL is left for the next open to recover.
    // This deliberately leaks one Sqlite3PersistenceStore instance per test
    // invocation; cppunit's single-shot harness tolerates the leak.
    (void)store.release();
  }

  {
    Sqlite3PersistenceStore store2(path);
    store2.open();

    CPPUNIT_ASSERT_MESSAGE("quick_check must return ok after second open",
                           quickCheckOk(store2.raw()));

    int64_t cnt = countRows(store2.raw(), "task");
    CPPUNIT_ASSERT_EQUAL_MESSAGE("committed row must be visible after reopen",
                                 (int64_t)1, cnt);
  }

  removeDb(path);
}

// L7-2: mid-migration throw preserves user_version=0.
void Sqlite3CrashRecoveryTest::testMidMigrationThrowPreservesUserVersion()
{
  std::string path =
      std::string(A2_TEST_OUT_DIR) + "/test_crash_l7_2.db";
  removeDb(path);

  {
    sqlite3* db = nullptr;
    CPPUNIT_ASSERT_EQUAL(SQLITE_OK,
                         sqlite3_open_v2(path.c_str(), &db,
                                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                                        nullptr));
    CPPUNIT_ASSERT_EQUAL(SQLITE_OK,
                         sqlite3_exec(db, "CREATE TABLE task (bogus TEXT);",
                                      nullptr, nullptr, nullptr));
    sqlite3_close_v2(db);
  }

  {
    Sqlite3PersistenceStore store(path);
    CPPUNIT_ASSERT_THROW(store.open(), RecoverableException);
  }

  {
    sqlite3* db = nullptr;
    CPPUNIT_ASSERT_EQUAL(SQLITE_OK,
                         sqlite3_open_v2(path.c_str(), &db,
                                        SQLITE_OPEN_READONLY, nullptr));
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr);
    sqlite3_step(stmt);
    int v = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close_v2(db);
    CPPUNIT_ASSERT_EQUAL_MESSAGE(
        "user_version must remain 0 after failed migration",
        0, v);
  }

  removeDb(path);
}

// L7-3: forked mid-UPSERT abort: committed row survives, uncommitted absent.
void Sqlite3CrashRecoveryTest::testForkedMidUpsertAbort()
{
  std::string path =
      std::string(A2_TEST_OUT_DIR) + "/test_crash_l7_3.db";
  removeDb(path);

  {
    Sqlite3PersistenceStore store(path);
    store.open();
    store.withTransaction([&]() {
      CPPUNIT_ASSERT_EQUAL(SQLITE_DONE,
                           insertTask(store.raw(), "bbbbbbbbbbbbbbbb", 0));
    });
  }

  pid_t child = ::fork();
  CPPUNIT_ASSERT_MESSAGE("fork() failed for L7-3", child != -1);

  if (child == 0) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(path.c_str(), &db,
                             SQLITE_OPEN_READWRITE, nullptr);
    if (rc != SQLITE_OK) {
      ::_exit(1);
    }
    sqlite3_exec(db, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
    insertTask(db, "cccccccccccccccc", 1);
    // Exit WITHOUT COMMIT — OS closes fd, SQLite rolls back.
    ::_exit(0);
  }

  int status = 0;
  ::waitpid(child, &status, 0);

  {
    Sqlite3PersistenceStore store(path);
    store.open();

    sqlite3_stmt* stmt = nullptr;

    sqlite3_prepare_v2(store.raw(),
                       "SELECT COUNT(*) FROM task WHERE gid='bbbbbbbbbbbbbbbb';",
                       -1, &stmt, nullptr);
    sqlite3_step(stmt);
    int r1cnt = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    CPPUNIT_ASSERT_EQUAL_MESSAGE("committed row R1 must be present", 1, r1cnt);

    sqlite3_prepare_v2(store.raw(),
                       "SELECT COUNT(*) FROM task WHERE gid='cccccccccccccccc';",
                       -1, &stmt, nullptr);
    sqlite3_step(stmt);
    int r2cnt = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    CPPUNIT_ASSERT_EQUAL_MESSAGE(
        "uncommitted row R2 must be absent after child _exit without COMMIT",
        0, r2cnt);
  }

  removeDb(path);
}

// L7-4: corrupt byte-mangle triggers rename + rebuild.
void Sqlite3CrashRecoveryTest::testCorruptByteMangleRebuild()
{
  std::string path =
      std::string(A2_TEST_OUT_DIR) + "/test_crash_l7_4.db";
  removeDb(path);
  removeCorruptFiles(path);

  {
    Sqlite3PersistenceStore store(path);
    store.open();
    store.withTransaction([&]() {
      CPPUNIT_ASSERT_EQUAL(SQLITE_DONE,
                           insertTask(store.raw(), "dddddddddddddddd", 0));
    });
    store.finalCheckpointAndClose();
  }

  // Mangle the SQLite magic header (bytes 0-15) with garbage.
  // The magic "SQLite format 3\000" occupies bytes 0-15; overwriting it
  // makes the file unrecognizable to SQLite.
  {
    int fd = ::open(path.c_str(), O_WRONLY);
    CPPUNIT_ASSERT_MESSAGE("open for byte-mangle failed", fd >= 0);
    const uint8_t garbage[] = {0xFF, 0xFE, 0x00, 0x01, 0xDE, 0xAD, 0xBE, 0xEF,
                                0xFF, 0xFE, 0x00, 0x01, 0xDE, 0xAD, 0xBE, 0xEF};
    ::lseek(fd, 0, SEEK_SET);
    ssize_t written = ::write(fd, garbage, sizeof(garbage));
    (void)written;
    ::fsync(fd);
    ::close(fd);
  }

  {
    Sqlite3PersistenceStore store(path);
    CPPUNIT_ASSERT_NO_THROW_MESSAGE(
        "open() on mangled DB must succeed (store rebuilds after rename)",
        store.open());

    CPPUNIT_ASSERT_MESSAGE("rebuilt DB must pass quick_check",
                           quickCheckOk(store.raw()));

    CPPUNIT_ASSERT_EQUAL_MESSAGE("rebuilt DB must have user_version=1",
                                 std::string("1"),
                                 store.queryPragma("user_version"));

    int64_t cnt = countRows(store.raw(), "task");
    CPPUNIT_ASSERT_EQUAL_MESSAGE("rebuilt DB task table must be empty",
                                 (int64_t)0, cnt);
  }

  CPPUNIT_ASSERT_MESSAGE(
      "a .corrupt.<ts> backup file must have been created",
      corruptFileExists(path));

  removeDb(path);
  removeCorruptFiles(path);
}

// L7-5: forked 100k inserts, SIGKILL, integrity_check passes.
void Sqlite3CrashRecoveryTest::testForkedHundredKInsertsKilledIntegrityOk()
{
  std::string path =
      std::string(A2_TEST_OUT_DIR) + "/test_crash_l7_5.db";
  removeDb(path);

  {
    Sqlite3PersistenceStore store(path);
    store.open();
    // store closes cleanly; schema is now in place.
  }

  pid_t child = ::fork();
  CPPUNIT_ASSERT_MESSAGE("fork() failed for L7-5", child != -1);

  if (child == 0) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(path.c_str(), &db,
                             SQLITE_OPEN_READWRITE, nullptr);
    if (rc != SQLITE_OK) {
      ::_exit(1);
    }
    sqlite3_exec(db, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);

    const char* insertSql =
        "INSERT INTO download_history "
        "(gid, status, result_code, total_length, completed_length, "
        " finished_at) "
        "VALUES (?, 'complete', 0, 1000, 1000, ?);";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, insertSql, -1, &stmt, nullptr);

    for (int i = 0; i < 100000; ++i) {
      if (i % 1000 == 0) {
        sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
      }

      char gidBuf[17];
      std::snprintf(gidBuf, sizeof(gidBuf), "%016d", i);
      sqlite3_reset(stmt);
      sqlite3_bind_text(stmt, 1, gidBuf, -1, SQLITE_STATIC);
      sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(i));
      sqlite3_step(stmt);

      if (i % 1000 == 999) {
        sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
      }
    }
    sqlite3_finalize(stmt);
    ::_exit(0);
  }

  // Parent: sleep 100ms then SIGKILL the child.
  ::usleep(100000);
  ::kill(child, SIGKILL);
  ::waitpid(child, nullptr, 0);

  {
    Sqlite3PersistenceStore store(path);
    store.open();

    CPPUNIT_ASSERT_MESSAGE(
        "integrity_check must return ok after SIGKILL mid-100k-insert",
        integrityCheckOk(store.raw()));
  }

  removeDb(path);
}

} // namespace aria2

#endif // HAVE_SQLITE3
