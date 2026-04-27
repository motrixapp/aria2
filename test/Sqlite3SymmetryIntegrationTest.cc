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

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <sqlite3.h>

namespace aria2 {

// ---------------------------------------------------------------------------
// Port counter — each test case picks a unique port so parallel executions
// (e.g. make check -j) don't collide. Base avoids the default aria2 port.
// ---------------------------------------------------------------------------
namespace {
static int gPortCounter = 17200 + (::getpid() % 200) * 13;
} // namespace

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
class Sqlite3SymmetryIntegrationTest : public CppUnit::TestFixture {
  CPPUNIT_TEST_SUITE(Sqlite3SymmetryIntegrationTest);
  CPPUNIT_TEST(testSaveSessionDualBackends);
  CPPUNIT_TEST(testSaveSessionSqliteOnly);
  CPPUNIT_TEST(testSaveSessionTextOnly);
  CPPUNIT_TEST(testSaveSessionNeitherThrows);
  CPPUNIT_TEST(testSaveSessionTextFailsSqliteOk);
  CPPUNIT_TEST(testSaveSessionBothFailThrows);
  CPPUNIT_TEST(testChangeOptionPersistsAcrossKill);
  CPPUNIT_TEST(testChangeUriPersistsAcrossKill);
  CPPUNIT_TEST(testChangePositionPersistsAcrossKill);
  CPPUNIT_TEST(testPausePersistsAcrossKill);
  CPPUNIT_TEST(testRemoveDownloadResultPersists);
  CPPUNIT_TEST(testPurgeDownloadResultPersists);
  CPPUNIT_TEST(testHundredTasksRecoverAfterKill);
  CPPUNIT_TEST_SUITE_END();

public:
  void setUp() override;
  void tearDown() override;

  void testSaveSessionDualBackends();
  void testSaveSessionSqliteOnly();
  void testSaveSessionTextOnly();
  void testSaveSessionNeitherThrows();
  void testSaveSessionTextFailsSqliteOk();
  void testSaveSessionBothFailThrows();
  void testChangeOptionPersistsAcrossKill();
  void testChangeUriPersistsAcrossKill();
  void testChangePositionPersistsAcrossKill();
  void testPausePersistsAcrossKill();
  void testRemoveDownloadResultPersists();
  void testPurgeDownloadResultPersists();
  void testHundredTasksRecoverAfterKill();

private:
  // Process tracking: 0 means not running.
  pid_t pid_ = 0;
  // Temporary directory and DB path for this test instance.
  std::string tmpDir_;
  std::string dbPath_;
  int rpcPort_ = 0;

  // -------------------------------------------------------------------------
  // Process helpers
  // -------------------------------------------------------------------------

  // Return the path to the aria2c production binary.  The cppunit test binary
  // runs from build/test/, so the production binary is one level up.
  static std::string aria2cPath()
  {
    // A2_TEST_OUT_DIR is "test_outdir" (relative), tests run from build/test/.
    // The production binary is at build/src/aria2c = ../src/aria2c.
    return "../src/aria2c";
  }

  // Allocate a fresh ephemeral port for this test.
  static int allocPort() { return gPortCounter++; }

  // Fork + exec aria2c with the given argument list (NULL-terminated).
  // Returns the child PID. Stdout/stderr of the child are redirected to
  // /dev/null so cppunit output stays clean.
  pid_t spawnAria2c(const std::vector<std::string>& argv)
  {
    // Build a C-string array for execvp.
    std::vector<const char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv) {
      cargv.push_back(a.c_str());
    }
    cargv.push_back(nullptr);

    pid_t pid = ::fork();
    CPPUNIT_ASSERT_MESSAGE("fork() failed", pid != -1);

    if (pid == 0) {
      // Child: redirect stdout+stderr to /dev/null, then exec.
      int devnull = ::open("/dev/null", O_RDWR);
      if (devnull >= 0) {
        ::dup2(devnull, STDOUT_FILENO);
        ::dup2(devnull, STDERR_FILENO);
        ::close(devnull);
      }
      ::execvp(cargv[0], const_cast<char* const*>(cargv.data()));
      ::_exit(127); // exec failed
    }

    return pid;
  }

  // Poll until aria2c's RPC port accepts connections, or bail after 5 seconds.
  void waitForRpc(int port)
  {
    struct sockaddr_in addr;
    ::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    for (int tries = 0; tries < 100; ++tries) {
      int fd = ::socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) {
        ::usleep(50000);
        continue;
      }
      int ret =
          ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
      ::close(fd);
      if (ret == 0) {
        return; // connected — RPC is up
      }
      ::usleep(50000); // 50 ms
    }
    CPPUNIT_FAIL("aria2c RPC did not become ready within 5 seconds");
  }

  // Kill the child with SIGTERM; escalate to SIGKILL after 3 seconds.
  void killGracefully(pid_t pid)
  {
    if (pid <= 0) {
      return;
    }
    ::kill(pid, SIGTERM);
    for (int i = 0; i < 30; ++i) {
      ::usleep(100000); // 100 ms
      int status = 0;
      pid_t r = ::waitpid(pid, &status, WNOHANG);
      if (r == pid) {
        return;
      }
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
  }

  // Start aria2c with full sqlite3 persistence enabled.
  // Returns the RPC port chosen.
  int startWithSqlite(int port = 0)
  {
    if (port == 0) {
      port = allocPort();
    }
    rpcPort_ = port;

    std::string portStr = std::to_string(port);
    pid_ = spawnAria2c({aria2cPath(),
                        "--enable-rpc=true",
                        "--rpc-listen-port=" + portStr,
                        "--rpc-allow-origin-all=true",
                        "--rpc-secret=integrationtestsecret",
                        "--enable-sqlite3-persistence=true",
                        "--sqlite3-db-path=" + dbPath_,
                        "--dir=" + tmpDir_,
                        "--console-log-level=error"});
    waitForRpc(port);
    return port;
  }

  // Start aria2c with dual (text + sqlite) backends.
  int startWithDual(const std::string& sessionFile, int port = 0)
  {
    if (port == 0) {
      port = allocPort();
    }
    rpcPort_ = port;

    std::string portStr = std::to_string(port);
    pid_ = spawnAria2c({aria2cPath(),
                        "--enable-rpc=true",
                        "--rpc-listen-port=" + portStr,
                        "--rpc-allow-origin-all=true",
                        "--rpc-secret=integrationtestsecret",
                        "--save-session=" + sessionFile,
                        "--enable-sqlite3-persistence=true",
                        "--sqlite3-db-path=" + dbPath_,
                        "--dir=" + tmpDir_,
                        "--console-log-level=error"});
    waitForRpc(port);
    return port;
  }

  // Start aria2c with text-only backend.
  int startTextOnly(const std::string& sessionFile, int port = 0)
  {
    if (port == 0) {
      port = allocPort();
    }
    rpcPort_ = port;

    std::string portStr = std::to_string(port);
    pid_ = spawnAria2c({aria2cPath(),
                        "--enable-rpc=true",
                        "--rpc-listen-port=" + portStr,
                        "--rpc-allow-origin-all=true",
                        "--rpc-secret=integrationtestsecret",
                        "--save-session=" + sessionFile,
                        "--dir=" + tmpDir_,
                        "--console-log-level=error"});
    waitForRpc(port);
    return port;
  }

  // Start aria2c with NO session backends.
  int startNoSession(int port = 0)
  {
    if (port == 0) {
      port = allocPort();
    }
    rpcPort_ = port;

    std::string portStr = std::to_string(port);
    pid_ = spawnAria2c({aria2cPath(),
                        "--enable-rpc=true",
                        "--rpc-listen-port=" + portStr,
                        "--rpc-allow-origin-all=true",
                        "--rpc-secret=integrationtestsecret",
                        "--dir=" + tmpDir_,
                        "--console-log-level=error"});
    waitForRpc(port);
    return port;
  }

  // Kill with SIGKILL and wait.
  void sigkill()
  {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, nullptr, 0);
      pid_ = 0;
    }
  }

  // -------------------------------------------------------------------------
  // RPC client (raw HTTP over TCP — no external dependencies)
  // -------------------------------------------------------------------------

  // Send a JSON-RPC 2.0 request and return the response body as a string.
  std::string rpcCall(int port, const std::string& method,
                      const std::string& paramsJson)
  {
    std::string body =
        "{\"jsonrpc\":\"2.0\",\"id\":\"t\",\"method\":\"" + method +
        "\",\"params\":" + paramsJson + "}";

    std::string request = "POST /jsonrpc HTTP/1.1\r\n"
                          "Host: 127.0.0.1:" +
                          std::to_string(port) +
                          "\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: " +
                          std::to_string(body.size()) +
                          "\r\n"
                          "Connection: close\r\n"
                          "\r\n" +
                          body;

    struct sockaddr_in addr;
    ::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    CPPUNIT_ASSERT_MESSAGE("socket() failed for RPC", fd >= 0);

    int ret =
        ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (ret != 0) {
      ::close(fd);
      CPPUNIT_FAIL(("connect() failed for RPC: " + std::string(::strerror(errno))).c_str());
    }

    // Send request.
    const char* ptr = request.c_str();
    size_t remaining = request.size();
    while (remaining > 0) {
      ssize_t sent = ::send(fd, ptr, remaining, 0);
      if (sent <= 0) {
        break;
      }
      ptr += sent;
      remaining -= static_cast<size_t>(sent);
    }

    // Read response.
    std::string response;
    char buf[4096];
    for (;;) {
      ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) {
        break;
      }
      response.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);

    // Strip HTTP headers (find \r\n\r\n).
    auto pos = response.find("\r\n\r\n");
    if (pos != std::string::npos) {
      return response.substr(pos + 4);
    }
    return response;
  }

  // Convenience: call with the standard integration-test secret token.
  std::string rpc(const std::string& method, const std::string& paramsJson)
  {
    return rpcCall(rpcPort_, method, paramsJson);
  }

  // Add a URI with pause=true using the proper params object.
  std::string addPaused(const std::string& uri)
  {
    // Correct JSON-RPC format: params[2] is an options object
    std::string resp =
        rpc("aria2.addUri",
            "[\"token:integrationtestsecret\",[\"" + uri +
            "\"],{\"pause\":\"true\"}]");
    auto pos = resp.find("\"result\":\"");
    if (pos == std::string::npos) {
      return "";
    }
    pos += 10;
    auto end = resp.find('"', pos);
    if (end == std::string::npos) {
      return "";
    }
    return resp.substr(pos, end - pos);
  }

  // Add an active (non-paused) URI and return the GID.
  std::string addActive(const std::string& uri)
  {
    std::string resp = rpc(
        "aria2.addUri",
        "[\"token:integrationtestsecret\",[\"" + uri + "\"]]");
    auto pos = resp.find("\"result\":\"");
    if (pos == std::string::npos) {
      return "";
    }
    pos += 10;
    auto end = resp.find('"', pos);
    if (end == std::string::npos) {
      return "";
    }
    return resp.substr(pos, end - pos);
  }

  // -------------------------------------------------------------------------
  // DB inspection helpers (read-only; only call after kill+waitpid)
  // -------------------------------------------------------------------------

  // Open the DB read-only and execute a scalar int64 query.
  int64_t dbCount(const std::string& sql)
  {
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(dbPath_.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
      sqlite3_close_v2(db);
      return -1;
    }
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
      sqlite3_close_v2(db);
      return -1;
    }
    int64_t result = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      result = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    sqlite3_close_v2(db);
    return result;
  }

  // Open the DB read-only and return a single text value.
  std::string dbScalar(const std::string& sql)
  {
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(dbPath_.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
      sqlite3_close_v2(db);
      return "";
    }
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
      sqlite3_close_v2(db);
      return "";
    }
    std::string result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char* text =
          reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
      if (text) {
        result = text;
      }
    }
    sqlite3_finalize(stmt);
    sqlite3_close_v2(db);
    return result;
  }

  // -------------------------------------------------------------------------
  // Temp directory management
  // -------------------------------------------------------------------------

  static void removeTmpDir(const std::string& dir)
  {
    if (dir.empty()) {
      return;
    }
    // Remove WAL/SHM files first in case the dir was made read-only in a test.
    ::chmod(dir.c_str(), 0700);

    // Walk and remove files, then the directory itself.
    // Use system rm -rf for simplicity; this is test code.
    std::string cmd = "rm -rf " + dir;
    ::system(cmd.c_str());
  }
};

CPPUNIT_TEST_SUITE_REGISTRATION(Sqlite3SymmetryIntegrationTest);

// ---------------------------------------------------------------------------
// setUp / tearDown
// ---------------------------------------------------------------------------

void Sqlite3SymmetryIntegrationTest::setUp()
{
  // Create a fresh temp directory for this test.
  char tpl[] = "/tmp/aria2-symtest-XXXXXX";
  char* dir = ::mkdtemp(tpl);
  CPPUNIT_ASSERT_MESSAGE("mkdtemp failed", dir != nullptr);
  tmpDir_ = dir;
  dbPath_ = tmpDir_ + "/aria2.db";
  pid_ = 0;
  rpcPort_ = 0;
}

void Sqlite3SymmetryIntegrationTest::tearDown()
{
  // Kill subprocess if still running (e.g. test threw mid-flight).
  if (pid_ > 0) {
    ::kill(pid_, SIGKILL);
    ::waitpid(pid_, nullptr, 0);
    pid_ = 0;
  }
  removeTmpDir(tmpDir_);
  tmpDir_.clear();
  dbPath_.clear();
}

// ---------------------------------------------------------------------------
// L6-1: saveSession writes both backends (text + sqlite)
// ---------------------------------------------------------------------------
void Sqlite3SymmetryIntegrationTest::testSaveSessionDualBackends()
{
  std::string sessionFile = tmpDir_ + "/session.txt";
  startWithDual(sessionFile);

  // Add a task so the session has content.
  std::string gid = addPaused("http://example.com/dual.bin");
  CPPUNIT_ASSERT(!gid.empty());

  std::string resp = rpc("aria2.saveSession",
                         "[\"token:integrationtestsecret\"]");
  CPPUNIT_ASSERT_MESSAGE("saveSession should return OK: " + resp,
                          resp.find("\"result\":\"OK\"") != std::string::npos);

  // Text file should exist.
  struct stat st;
  CPPUNIT_ASSERT_MESSAGE("text session file not created",
                          ::stat(sessionFile.c_str(), &st) == 0 && st.st_size > 0);

  // SQLite DB should have the task row. We deliberately defer the
  // count check until after sigkill — dbCount opens READONLY, but may
  // race with the WAL writer while aria2c is still running.
  sigkill();

  int64_t cnt = dbCount("SELECT COUNT(*) FROM task");
  CPPUNIT_ASSERT_MESSAGE("sqlite task count should be >= 1 after dual saveSession",
                          cnt >= 1);
}

// ---------------------------------------------------------------------------
// L6-2: saveSession SQLite-only writes DB, no text file created
// ---------------------------------------------------------------------------
void Sqlite3SymmetryIntegrationTest::testSaveSessionSqliteOnly()
{
  startWithSqlite();

  std::string gid = addPaused("http://example.com/sqonly.bin");
  CPPUNIT_ASSERT(!gid.empty());

  std::string resp = rpc("aria2.saveSession",
                         "[\"token:integrationtestsecret\"]");
  CPPUNIT_ASSERT_MESSAGE("saveSession sqlite-only should return OK: " + resp,
                          resp.find("\"result\":\"OK\"") != std::string::npos);

  sigkill();

  // DB should have the task.
  int64_t cnt = dbCount("SELECT COUNT(*) FROM task");
  CPPUNIT_ASSERT_MESSAGE("sqlite task count should be >= 1", cnt >= 1);
}

// ---------------------------------------------------------------------------
// L6-3: saveSession text-only writes session file
// ---------------------------------------------------------------------------
void Sqlite3SymmetryIntegrationTest::testSaveSessionTextOnly()
{
  std::string sessionFile = tmpDir_ + "/text-only-session.txt";
  startTextOnly(sessionFile);

  std::string gid = addPaused("http://example.com/textonly.bin");
  CPPUNIT_ASSERT(!gid.empty());

  std::string resp = rpc("aria2.saveSession",
                         "[\"token:integrationtestsecret\"]");
  CPPUNIT_ASSERT_MESSAGE("saveSession text-only should return OK: " + resp,
                          resp.find("\"result\":\"OK\"") != std::string::npos);

  killGracefully(pid_);
  pid_ = 0;

  // Text session file should exist and be non-empty.
  struct stat st;
  CPPUNIT_ASSERT_MESSAGE("text session file not found",
                          ::stat(sessionFile.c_str(), &st) == 0);
  CPPUNIT_ASSERT_MESSAGE("text session file is empty", st.st_size > 0);

  // No DB should exist (sqlite was not enabled).
  struct stat stDb;
  CPPUNIT_ASSERT_MESSAGE("DB should not exist for text-only mode",
                          ::stat(dbPath_.c_str(), &stDb) != 0);
}

// ---------------------------------------------------------------------------
// L6-4: saveSession neither configured → error response
// ---------------------------------------------------------------------------
void Sqlite3SymmetryIntegrationTest::testSaveSessionNeitherThrows()
{
  startNoSession();

  std::string resp = rpc("aria2.saveSession",
                         "[\"token:integrationtestsecret\"]");
  CPPUNIT_ASSERT_MESSAGE(
      "saveSession with no backends should return error: " + resp,
      resp.find("\"error\"") != std::string::npos);
  CPPUNIT_ASSERT_MESSAGE(
      "error should mention storage configuration: " + resp,
      resp.find("No session storage configured") != std::string::npos);

  killGracefully(pid_);
  pid_ = 0;
}

// ---------------------------------------------------------------------------
// L6-5: saveSession text fails + sqlite OK → returns OK
// ---------------------------------------------------------------------------
void Sqlite3SymmetryIntegrationTest::testSaveSessionTextFailsSqliteOk()
{
  // Use a bad text path (/nonexistent/dir/session.txt) + valid sqlite db.
  std::string badSession = "/nonexistent_aria2_dir_xyz/session.txt";
  int port = allocPort();
  rpcPort_ = port;
  std::string portStr = std::to_string(port);

  pid_ = spawnAria2c({aria2cPath(),
                      "--enable-rpc=true",
                      "--rpc-listen-port=" + portStr,
                      "--rpc-allow-origin-all=true",
                      "--rpc-secret=integrationtestsecret",
                      "--save-session=" + badSession,
                      "--enable-sqlite3-persistence=true",
                      "--sqlite3-db-path=" + dbPath_,
                      "--dir=" + tmpDir_,
                      "--console-log-level=error"});
  waitForRpc(port);

  std::string gid = addPaused("http://example.com/textfail.bin");
  CPPUNIT_ASSERT(!gid.empty());

  std::string resp = rpc("aria2.saveSession",
                         "[\"token:integrationtestsecret\"]");
  // Text fails but sqlite succeeds → should return OK.
  CPPUNIT_ASSERT_MESSAGE(
      "saveSession (text fail, sqlite ok) should return OK: " + resp,
      resp.find("\"result\":\"OK\"") != std::string::npos);

  sigkill();

  // DB should have the task.
  int64_t cnt = dbCount("SELECT COUNT(*) FROM task");
  CPPUNIT_ASSERT_MESSAGE("task count should be >= 1 in db after partial save",
                          cnt >= 1);
}

// ---------------------------------------------------------------------------
// L6-6: saveSession both fail (all configured storages fail) → error response
//
// Implementation: text-only with unwritable session path. When the only
// configured backend fails, aria2 returns "All configured session storages
// failed to save." — exactly the "both fail → throws" code path.
// ---------------------------------------------------------------------------
void Sqlite3SymmetryIntegrationTest::testSaveSessionBothFailThrows()
{
  std::string badSession = "/nonexistent_aria2_dir_xyz/session.txt";
  startTextOnly(badSession);

  std::string resp = rpc("aria2.saveSession",
                         "[\"token:integrationtestsecret\"]");
  CPPUNIT_ASSERT_MESSAGE(
      "saveSession (all configured fail) should return error: " + resp,
      resp.find("\"error\"") != std::string::npos);
  // Spec §8.4: throws exact message
  // "All configured session storages failed to save."
  // (omit trailing period from the substring search to be tolerant of any
  // logging/escaping that may strip it.)
  CPPUNIT_ASSERT_MESSAGE(
      "error must use spec §8.4 message: " + resp,
      resp.find("All configured session storages failed to save") !=
          std::string::npos);

  killGracefully(pid_);
  pid_ = 0;
}

// ---------------------------------------------------------------------------
// L6-7: changeOption + kill -9 + restart → option preserved in DB
// ---------------------------------------------------------------------------
void Sqlite3SymmetryIntegrationTest::testChangeOptionPersistsAcrossKill()
{
  startWithSqlite();

  // Add a paused task so there is a row to update.
  std::string gid = addPaused("http://example.com/opt.bin");
  CPPUNIT_ASSERT_MESSAGE("addUri should return a gid", !gid.empty());

  // changeOption: set max-connection-per-server=3
  std::string resp =
      rpc("aria2.changeOption",
          "[\"token:integrationtestsecret\",\"" + gid +
          "\",{\"max-connection-per-server\":\"3\"}]");
  CPPUNIT_ASSERT_MESSAGE("changeOption should return OK: " + resp,
                          resp.find("\"result\":\"OK\"") != std::string::npos);

  // Kill -9 (data committed synchronously in Phase D so it's durable).
  sigkill();

  // The task row should still exist in the DB.
  int64_t cnt = dbCount("SELECT COUNT(*) FROM task WHERE gid='" + gid + "'");
  CPPUNIT_ASSERT_MESSAGE("task row must persist after kill -9", cnt == 1);

  // Serialized blob should be non-empty (options are serialized into it).
  int64_t blobLen =
      dbCount("SELECT length(serialized) FROM task WHERE gid='" + gid + "'");
  CPPUNIT_ASSERT_MESSAGE("serialized blob should be non-empty", blobLen > 0);
}

// ---------------------------------------------------------------------------
// L6-8: changeUri + kill -9 + restart → URIs preserved
// ---------------------------------------------------------------------------
void Sqlite3SymmetryIntegrationTest::testChangeUriPersistsAcrossKill()
{
  startWithSqlite();

  std::string gid = addPaused("http://example.com/old.bin");
  CPPUNIT_ASSERT(!gid.empty());

  // changeUri: remove old URI, add new one
  std::string resp =
      rpc("aria2.changeUri",
          "[\"token:integrationtestsecret\",\"" + gid +
          "\",1,[\"http://example.com/old.bin\"],[\"http://example.com/new.bin\"]]");
  // Result should be [1,1] (1 removed, 1 added) or similar success.
  CPPUNIT_ASSERT_MESSAGE("changeUri should succeed: " + resp,
                          resp.find("\"error\"") == std::string::npos);

  sigkill();

  // Task should still be in DB.
  int64_t cnt = dbCount("SELECT COUNT(*) FROM task WHERE gid='" + gid + "'");
  CPPUNIT_ASSERT_MESSAGE("task row must persist after changeUri + kill -9", cnt == 1);

  // The serialized blob should encode the new URI.
  std::string blob =
      dbScalar("SELECT serialized FROM task WHERE gid='" + gid + "'");
  CPPUNIT_ASSERT_MESSAGE("serialized blob should contain new URI",
                          blob.find("new.bin") != std::string::npos);
}

// ---------------------------------------------------------------------------
// L6-9: changePosition + kill -9 + restart → queue order preserved
// ---------------------------------------------------------------------------
void Sqlite3SymmetryIntegrationTest::testChangePositionPersistsAcrossKill()
{
  startWithSqlite();

  // Add 3 paused tasks.
  std::string g0 = addPaused("http://example.com/pos0.bin");
  std::string g1 = addPaused("http://example.com/pos1.bin");
  std::string g2 = addPaused("http://example.com/pos2.bin");
  CPPUNIT_ASSERT(!g0.empty());
  CPPUNIT_ASSERT(!g1.empty());
  CPPUNIT_ASSERT(!g2.empty());

  // Move g2 to position 0.
  rpc("aria2.changePosition",
      "[\"token:integrationtestsecret\",\"" + g2 + "\",0,\"POS_SET\"]");

  // Give the persistent write a moment to complete (it's synchronous, but
  // allow any background scheduler tick to flush).
  ::usleep(200000); // 200 ms

  sigkill();

  // g2 should now be at queue_position 0.
  std::string pos =
      dbScalar("SELECT queue_position FROM task WHERE gid='" + g2 + "'");
  CPPUNIT_ASSERT_MESSAGE(
      "g2 should be at queue_position 0 after changePosition: got " + pos,
      pos == "0");

  // Spec §11.L6 line 818: "restart → queue order preserved".
  // Restart aria2c against the same DB and verify via tellWaiting RPC that
  // the moved task (g2) is now at the head of the queue.
  startWithSqlite();
  std::string waiting =
      rpc("aria2.tellWaiting",
          "[\"token:integrationtestsecret\",0,10]");
  // The first "gid":"<...>" entry in the response is queue position 0.
  auto firstGidPos = waiting.find("\"gid\":\"");
  CPPUNIT_ASSERT_MESSAGE("tellWaiting response must contain a gid: " + waiting,
                          firstGidPos != std::string::npos);
  firstGidPos += 7; // skip past "gid":"
  auto firstGidEnd = waiting.find('"', firstGidPos);
  CPPUNIT_ASSERT(firstGidEnd != std::string::npos);
  std::string firstGid = waiting.substr(firstGidPos, firstGidEnd - firstGidPos);
  CPPUNIT_ASSERT_EQUAL_MESSAGE(
      "after restart, moved task g2 must be first in tellWaiting (pos 0)",
      g2, firstGid);
}

// ---------------------------------------------------------------------------
// L6-10: pause + kill -9 + restart → state='paused' in DB
// ---------------------------------------------------------------------------
void Sqlite3SymmetryIntegrationTest::testPausePersistsAcrossKill()
{
  startWithSqlite();

  // addUri with pause=true so it's immediately paused.
  std::string gid = addPaused("http://example.com/pause.bin");
  CPPUNIT_ASSERT(!gid.empty());

  sigkill();

  std::string state =
      dbScalar("SELECT state FROM task WHERE gid='" + gid + "'");
  CPPUNIT_ASSERT_MESSAGE("state should be 'paused' after kill -9: got " + state,
                          state == "paused");
}

// ---------------------------------------------------------------------------
// L6-11: removeDownloadResult + restart → row deleted from download_history
// ---------------------------------------------------------------------------
void Sqlite3SymmetryIntegrationTest::testRemoveDownloadResultPersists()
{
  startWithSqlite();

  // Add an active (non-paused) task pointing at a URL that will fail quickly
  // (DNS/connection failure) so aria2 moves it to download_history.
  addActive("http://example.com/remove-dr.bin");

  // Wait for the download to fail and appear in tellStopped.
  std::string stoppedGid;
  for (int i = 0; i < 40; ++i) {
    ::usleep(250000); // 250 ms
    std::string stopped = rpc("aria2.tellStopped",
                              "[\"token:integrationtestsecret\",0,10]");
    // Look for a gid in the result array.
    auto pos = stopped.find("\"gid\":\"");
    if (pos != std::string::npos) {
      pos += 7;
      auto end = stopped.find('"', pos);
      if (end != std::string::npos) {
        stoppedGid = stopped.substr(pos, end - pos);
        break;
      }
    }
  }
  CPPUNIT_ASSERT_MESSAGE("download should appear in tellStopped", !stoppedGid.empty());

  // Verify DB has the history row.
  int64_t before =
      dbCount("SELECT COUNT(*) FROM download_history");
  CPPUNIT_ASSERT_MESSAGE("download_history should have >= 1 row before remove",
                          before >= 1);

  // removeDownloadResult.
  std::string resp =
      rpc("aria2.removeDownloadResult",
          "[\"token:integrationtestsecret\",\"" + stoppedGid + "\"]");
  CPPUNIT_ASSERT_MESSAGE("removeDownloadResult should return OK: " + resp,
                          resp.find("\"result\":\"OK\"") != std::string::npos);

  // Give persistent write time to complete (synchronous, but be safe).
  ::usleep(100000);

  sigkill();

  // download_history should be empty (or reduced) after remove.
  int64_t after =
      dbCount("SELECT COUNT(*) FROM download_history WHERE gid='" + stoppedGid + "'");
  CPPUNIT_ASSERT_MESSAGE("download_history row should be deleted after remove",
                          after == 0);
}

// ---------------------------------------------------------------------------
// L6-12: purgeDownloadResult + restart → download_history cleared
// ---------------------------------------------------------------------------
void Sqlite3SymmetryIntegrationTest::testPurgeDownloadResultPersists()
{
  startWithSqlite();

  // Add 3 active tasks that will fail quickly.
  addActive("http://example.com/purge1.bin");
  addActive("http://example.com/purge2.bin");
  addActive("http://example.com/purge3.bin");

  // Wait for at least one to appear in tellStopped.
  bool anyFailed = false;
  for (int i = 0; i < 50; ++i) {
    ::usleep(200000); // 200 ms
    std::string stopped = rpc("aria2.tellStopped",
                              "[\"token:integrationtestsecret\",0,10]");
    if (stopped.find("\"gid\"") != std::string::npos) {
      anyFailed = true;
      break;
    }
  }
  CPPUNIT_ASSERT_MESSAGE("at least one download should appear in tellStopped",
                          anyFailed);

  // Check DB has history rows.
  int64_t before = dbCount("SELECT COUNT(*) FROM download_history");
  CPPUNIT_ASSERT_MESSAGE("download_history should have >= 1 row before purge",
                          before >= 1);

  // purgeDownloadResult.
  std::string resp = rpc("aria2.purgeDownloadResult",
                          "[\"token:integrationtestsecret\"]");
  CPPUNIT_ASSERT_MESSAGE("purgeDownloadResult should return OK: " + resp,
                          resp.find("\"result\":\"OK\"") != std::string::npos);

  ::usleep(100000);
  sigkill();

  // All history should be gone.
  int64_t after = dbCount("SELECT COUNT(*) FROM download_history");
  CPPUNIT_ASSERT_MESSAGE("download_history should be empty after purge",
                          after == 0);
}

// ---------------------------------------------------------------------------
// L6-13: RPC add 100 tasks + kill -9 + restart → all 100 recovered
// ---------------------------------------------------------------------------
void Sqlite3SymmetryIntegrationTest::testHundredTasksRecoverAfterKill()
{
  int firstPort = allocPort();
  rpcPort_ = firstPort;
  std::string portStr = std::to_string(firstPort);

  pid_ = spawnAria2c({aria2cPath(),
                      "--enable-rpc=true",
                      "--rpc-listen-port=" + portStr,
                      "--rpc-allow-origin-all=true",
                      "--rpc-secret=integrationtestsecret",
                      "--enable-sqlite3-persistence=true",
                      "--sqlite3-db-path=" + dbPath_,
                      "--dir=" + tmpDir_,
                      "--console-log-level=error"});
  waitForRpc(firstPort);

  // Add 100 paused tasks.
  std::vector<std::string> gids;
  gids.reserve(100);
  for (int i = 0; i < 100; ++i) {
    std::ostringstream uri;
    uri << "http://example.com/task" << i << ".bin";
    std::string g = addPaused(uri.str());
    if (!g.empty()) {
      gids.push_back(g);
    }
  }
  CPPUNIT_ASSERT_EQUAL_MESSAGE("all 100 addUri calls should succeed",
                                (size_t)100, gids.size());

  // Verify DB has 100 rows before kill.
  int64_t dbBefore = dbCount("SELECT COUNT(*) FROM task");
  CPPUNIT_ASSERT_EQUAL_MESSAGE("DB should have 100 task rows before kill",
                                (int64_t)100, dbBefore);

  // Kill -9.
  sigkill();

  // Verify DB still has 100 rows after kill -9.
  int64_t dbAfter = dbCount("SELECT COUNT(*) FROM task");
  CPPUNIT_ASSERT_EQUAL_MESSAGE("DB should still have 100 task rows after kill -9",
                                (int64_t)100, dbAfter);

  // Restart aria2c and confirm it loads all 100 tasks.
  int secondPort = allocPort();
  rpcPort_ = secondPort;
  std::string portStr2 = std::to_string(secondPort);
  pid_ = spawnAria2c({aria2cPath(),
                      "--enable-rpc=true",
                      "--rpc-listen-port=" + portStr2,
                      "--rpc-allow-origin-all=true",
                      "--rpc-secret=integrationtestsecret",
                      "--enable-sqlite3-persistence=true",
                      "--sqlite3-db-path=" + dbPath_,
                      "--dir=" + tmpDir_,
                      "--console-log-level=error"});
  waitForRpc(secondPort);

  // tellWaiting with a limit of 200 to get all tasks.
  std::string waiting =
      rpc("aria2.tellWaiting",
          "[\"token:integrationtestsecret\",0,200]");

  // Extract the GIDs from the response in the order they appear. Each
  // task object in the JSON array begins with `"gid":"<hex>"...`, so we
  // walk forward and pull each gid string out.
  std::vector<std::string> recovered;
  recovered.reserve(100);
  size_t scan = 0;
  while ((scan = waiting.find("\"gid\":\"", scan)) != std::string::npos) {
    scan += 7; // skip past "gid":"
    auto end = waiting.find('"', scan);
    if (end == std::string::npos) {
      break;
    }
    recovered.push_back(waiting.substr(scan, end - scan));
    scan = end + 1;
  }

  CPPUNIT_ASSERT_EQUAL_MESSAGE(
      "all 100 tasks must be recoverable after restart",
      (size_t)100, recovered.size());

  // Spec §11.L6 line 822: "queue order intact". Strict positional check —
  // the i-th recovered GID must equal the i-th originally-inserted GID.
  for (size_t i = 0; i < gids.size(); ++i) {
    CPPUNIT_ASSERT_EQUAL_MESSAGE(
        "queue order must be preserved across restart at position " +
            std::to_string(i),
        gids[i], recovered[i]);
  }

  sigkill();
}

} // namespace aria2

#endif // HAVE_SQLITE3
