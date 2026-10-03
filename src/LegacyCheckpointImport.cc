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
#include "LegacyCheckpointImport.h"
#include "LegacyCheckpointCodec.h"
#include "RpcMethodImpl.h"
#include "DownloadEngine.h"
#include "DownloadContext.h"
#include "FileEntry.h"
#include "MessageDigest.h"
#ifdef ENABLE_BITTORRENT
#  include "bittorrent_helper.h"
#endif
#include "Option.h"
#include "base64.h"
#include "prefs.h"
#include "util.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <set>
#include <map>
#include <vector>
#ifdef HAVE_SQLITE3
#  include "Sqlite3PersistenceStore.h"
#endif
#ifndef __MINGW32__
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif
#if defined(HAVE_SQLITE3) && !defined(__MINGW32__) && defined(O_NOFOLLOW) &&   \
    defined(O_DIRECTORY)
#  define LEGACY_SAFE_POSIX 1
#endif

namespace aria2 {
bool legacyCheckpointImportAvailable(DownloadEngine* e)
{
#ifdef LEGACY_SAFE_POSIX
  return e && e->getSqlite3Store() && e->getSqlite3Store()->raw() &&
         !e->getOption()->get(PREF_RPC_SECRET).empty() &&
         MessageDigest::supports("sha-256");
#else
  return false;
#endif
}
namespace {
const std::string& stringField(const Dict* d, const char* key)
{
  auto s = d ? downcast<String>(d->get(key)) : nullptr;
  if (!s)
    throw DL_ABORT_EX("Legacy checkpoint missing or invalid string field");
  return s->s();
}
int64_t decimal(const Dict* d, const char* key)
{
  const auto& s = stringField(d, key);
  if (s.empty() || s.size() > 19 || (s.size() > 1 && s[0] == '0'))
    throw DL_ABORT_EX("Legacy checkpoint invalid decimal field");
  int64_t result = 0;
  for (auto c : s) {
    if (c < '0' || c > '9' || result > (INT64_MAX - (c - '0')) / 10)
      throw DL_ABORT_EX("Legacy checkpoint decimal overflow");
    result = result * 10 + c - '0';
  }
  return result;
}
void checkToken(const std::string& s)
{
  if (s.size() < 16 || s.size() > 128)
    throw DL_ABORT_EX("Legacy checkpoint invalid token");
  for (auto c : s)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-'))
      throw DL_ABORT_EX("Legacy checkpoint invalid token");
}
void checkPath(const std::string& p)
{
  if (p.size() < 2 || p.size() > 4096 || p[0] != '/' || p.back() == '/')
    throw DL_ABORT_EX("Legacy checkpoint requires canonical absolute path");
  size_t start = 1;
  for (size_t i = 1; i <= p.size(); ++i) {
    if (i < p.size() && (static_cast<unsigned char>(p[i]) < 32 || p[i] == 127))
      throw DL_ABORT_EX("Legacy checkpoint invalid path character");
    if (i == p.size() || p[i] == '/') {
      auto part = p.substr(start, i - start);
      if (part.empty() || part == "." || part == "..")
        throw DL_ABORT_EX("Legacy checkpoint invalid path component");
      start = i + 1;
    }
  }
}
bool overlaps(const std::string& a, const std::string& b)
{
  return a == b ||
         (a.size() > b.size() && a.compare(0, b.size(), b) == 0 &&
          a[b.size()] == '/') ||
         (b.size() > a.size() && b.compare(0, a.size(), a) == 0 &&
          b[a.size()] == '/');
}
LegacyCheckpoint snapshot(const Dict* d)
{
  const auto& input = stringField(d, "controlFile");
  if (input.size() > 4 * ((LEGACY_CHECKPOINT_MAX_BYTES + 2) / 3) ||
      input.size() % 4)
    throw DL_ABORT_EX("Legacy checkpoint base64 size invalid");
  auto bytes = base64::decode(input.begin(), input.end());
  if (base64::encode(bytes.begin(), bytes.end()) != input)
    throw DL_ABORT_EX("Legacy checkpoint requires canonical base64");
  return decodeLegacyCheckpointV1(bytes);
}
void requireAvailable(DownloadEngine* e)
{
  if (!legacyCheckpointImportAvailable(e))
    throw DL_ABORT_EX("LegacyCheckpointImportV1 unavailable");
}
std::unique_ptr<Dict> inspection(const LegacyCheckpoint& c)
{
  auto result = Dict::g();
  result->put("version", "1");
  result->put("format", "aria2-v1");
  result->put("kind", c.torrent ? "bittorrent" : "http");
  result->put("totalLength", util::itos(c.totalLength));
  result->put("pieceLength", util::uitos(c.pieceLength));
  result->put("infoHash", util::toHex(c.infoHash));
  result->put("uploadLength", util::itos(c.uploadLength));
  result->put("completedLength", util::itos(c.completedLength));
  result->put("controlDigest", c.controlDigest);
  auto ranges = List::g();
  for (const auto& range : c.ranges) {
    auto d = Dict::g();
    d->put("offset", util::itos(range.offset));
    d->put("length", util::itos(range.length));
    ranges->append(std::move(d));
  }
  result->put("ranges", std::move(ranges));
  return result;
}
#ifdef LEGACY_SAFE_POSIX
struct Identity {
  std::string device, inode, size, mtime, ctime;
  bool operator==(const Identity& o) const
  {
    return device == o.device && inode == o.inode && size == o.size &&
           mtime == o.mtime && ctime == o.ctime;
  }
};
Identity identity(const struct stat& st)
{
  if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 ||
      (st.st_mode & 022) || st.st_size < 0)
    throw DL_ABORT_EX(
        "Legacy checkpoint payload is not an exclusively owned regular file");
#  ifdef __APPLE__
  auto mt = st.st_mtimespec, ct = st.st_ctimespec;
#  else
  auto mt = st.st_mtim, ct = st.st_ctim;
#  endif
  if (mt.tv_sec < 0 || ct.tv_sec < 0 ||
      mt.tv_sec > INT64_MAX / 1000000000 - 1 ||
      ct.tv_sec > INT64_MAX / 1000000000 - 1)
    throw DL_ABORT_EX("Legacy checkpoint payload timestamp out of range");
  return {util::uitos(st.st_dev), util::uitos(st.st_ino),
          util::itos(st.st_size),
          util::itos(int64_t(mt.tv_sec) * 1000000000 + mt.tv_nsec),
          util::itos(int64_t(ct.tv_sec) * 1000000000 + ct.tv_nsec)};
}
struct Handle {
  int fd;
  explicit Handle(int value = -1) : fd(value) {}
  ~Handle()
  {
    if (fd >= 0)
      close(fd);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& o) : fd(o.fd) { o.fd = -1; }
  Handle& operator=(Handle&& o)
  {
    if (fd >= 0)
      close(fd);
    fd = o.fd;
    o.fd = -1;
    return *this;
  }
};
Handle openSafe(const std::string& path)
{
  checkPath(path);
  Handle dir(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (dir.fd < 0)
    throw DL_ABORT_EX("Legacy checkpoint safe path open failed");
  size_t start = 1;
  while (true) {
    auto end = path.find('/', start);
    bool final = end == std::string::npos;
    auto part = path.substr(start, final ? path.size() - start : end - start);
    Handle next(openat(dir.fd, part.c_str(),
                       O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
                           (final ? 0 : O_DIRECTORY)));
    if (next.fd < 0)
      throw DL_ABORT_EX("Legacy checkpoint safe path open failed");
    dir = std::move(next);
    if (final)
      return dir;
    start = end + 1;
  }
}
Identity statHandle(int fd)
{
  struct stat st;
  if (fstat(fd, &st))
    throw DL_ABORT_EX("Legacy checkpoint payload stat failed");
  return identity(st);
}
struct FileSpec {
  std::string path;
  int64_t offset, length, size;
  Identity expected;
  Handle handle;
};
void verifyFiles(const std::vector<FileSpec>& files)
{
  for (const auto& f : files) {
    auto reopened = openSafe(f.path);
    if (!(statHandle(reopened.fd) == f.expected) ||
        !(statHandle(f.handle.fd) == f.expected))
      throw DL_ABORT_EX("Legacy checkpoint payload identity changed");
  }
}
std::vector<FileSpec> fileSpecs(const Dict* request, const LegacyCheckpoint& c,
                                const std::string& target, bool openFiles)
{
  auto list = downcast<List>(request->get("files"));
  if (!list || list->empty() || list->size() > 10000 ||
      (!c.torrent && list->size() != 1))
    throw DL_ABORT_EX("Legacy checkpoint invalid file map");
  std::vector<FileSpec> files;
  std::set<std::string> paths, identities;
  int64_t offset = 0;
  size_t rangeIndex = 0;
  for (const auto& value : *list) {
    auto d = downcast<Dict>(value);
    if (!d || d->size() != 4)
      throw DL_ABORT_EX("Legacy checkpoint invalid file map entry");
    auto path = stringField(d, "path");
    checkPath(path);
    if (path != target &&
        (!c.torrent || path.compare(0, target.size() + 1, target + "/") != 0))
      throw DL_ABORT_EX("Legacy checkpoint file outside confirmed target");
    auto o = decimal(d, "offset"), length = decimal(d, "length");
    if (o != offset || length > c.totalLength - offset ||
        !paths.insert(path).second)
      throw DL_ABORT_EX("Legacy checkpoint invalid logical file map");
    offset += length;
    auto i = downcast<Dict>(d->get("identity"));
    if (!i || i->size() != 5)
      throw DL_ABORT_EX("Legacy checkpoint invalid file identity");
    // Canonical, signed 64-bit decimal values throughout the wire contract.
    for (auto key : {"device", "inode", "size", "mtimeNs", "ctimeNs"})
      decimal(i, key);
    Identity expected{stringField(i, "device"), stringField(i, "inode"),
                      stringField(i, "size"), stringField(i, "mtimeNs"),
                      stringField(i, "ctimeNs")};
    auto size = decimal(i, "size");
    if (size > length ||
        !identities.insert(expected.device + ":" + expected.inode).second)
      throw DL_ABORT_EX(
          "Legacy checkpoint invalid payload extent or duplicate inode");
    Handle handle;
    if (openFiles) {
      handle = openSafe(path);
      if (!(statHandle(handle.fd) == expected))
        throw DL_ABORT_EX("Legacy checkpoint payload identity changed");
    }
    while (rangeIndex < c.ranges.size() &&
           c.ranges[rangeIndex].offset + c.ranges[rangeIndex].length <= o)
      ++rangeIndex;
    size_t index = rangeIndex;
    while (index < c.ranges.size() && c.ranges[index].offset < o + length) {
      const auto& r = c.ranges[index];
      auto end = std::min<int64_t>(r.offset + r.length, o + length);
      if (end > std::max<int64_t>(r.offset, o) && end - o > size)
        throw DL_ABORT_EX("Legacy checkpoint ranges exceed existing payload");
      if (r.offset + r.length > o + length)
        break;
      ++index;
    }
    rangeIndex = index;
    files.push_back({std::move(path), o, length, size, std::move(expected),
                     std::move(handle)});
  }
  if (offset != c.totalLength)
    throw DL_ABORT_EX("Legacy checkpoint file map length mismatch");
  if (openFiles)
    verifyFiles(files);
  return files;
}
struct Statement {
  sqlite3_stmt* stmt = nullptr;
  Statement(sqlite3* db, const char* sql)
  {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
      throw DL_ABORT_EX("Legacy checkpoint database prepare failed");
  }
  ~Statement() { sqlite3_finalize(stmt); }
  void text(int n, const std::string& s)
  {
    if (sqlite3_bind_text(stmt, n, s.data(), s.size(), SQLITE_TRANSIENT) !=
        SQLITE_OK)
      throw DL_ABORT_EX("Legacy checkpoint bind failed");
  }
  void blob(int n, const std::string& s)
  {
    if (sqlite3_bind_blob(stmt, n, s.data(), s.size(), SQLITE_TRANSIENT) !=
        SQLITE_OK)
      throw DL_ABORT_EX("Legacy checkpoint bind failed");
  }
  void number(int n, int64_t value)
  {
    if (sqlite3_bind_int64(stmt, n, value) != SQLITE_OK)
      throw DL_ABORT_EX("Legacy checkpoint bind failed");
  }
  bool row()
  {
    auto rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
      return true;
    if (rc == SQLITE_DONE)
      return false;
    throw DL_ABORT_EX("Legacy checkpoint database query failed");
  }
  void done()
  {
    if (sqlite3_step(stmt) != SQLITE_DONE)
      throw DL_ABORT_EX("Legacy checkpoint database write failed");
  }
  std::string column(int n)
  {
    auto p = sqlite3_column_blob(stmt, n);
    auto len = sqlite3_column_bytes(stmt, n);
    return p ? std::string(static_cast<const char*>(p), len) : std::string();
  }
};
bool matchesCheckpoint(Statement& p, const std::string& gid,
                       const std::string& progressDigest,
                       const std::string& controlDigest)
{
  if (!p.row() || p.column(0) != gid || p.column(1) != progressDigest ||
      sqlite3_column_int(p.stmt, 2) != 1)
    return false;
  LegacyCheckpoint c;
  auto torrent = sqlite3_column_int(p.stmt, 3);
  auto pl = sqlite3_column_int64(p.stmt, 5);
  c.torrent = torrent == 1;
  c.infoHash = p.column(4);
  c.totalLength = sqlite3_column_int64(p.stmt, 6);
  c.uploadLength = sqlite3_column_int64(p.stmt, 7);
  if ((torrent != 0 && torrent != 1) || pl <= 0 || pl > INT32_MAX ||
      c.totalLength < 0 || c.uploadLength < 0 ||
      c.infoHash.size() != (c.torrent ? 20u : 0u))
    return false;
  c.pieceLength = pl;
  c.bitfield = p.column(8);
  c.inFlightBlob = p.column(9);
  if (c.bitfield.size() + c.inFlightBlob.size() > LEGACY_CHECKPOINT_MAX_BYTES)
    return false;
  auto bytes = encodeLegacyCheckpointV1(c);
  bytes.resize(bytes.size() - 4);
  bytes += c.inFlightBlob;
  auto md = MessageDigest::create("sha-256");
  md->update(bytes.data(), bytes.size());
  return util::toHex(md->digest()) == controlDigest;
}
const char* CHECKPOINT_PROOF_SQL =
    "SELECT "
    "gid,digest,ctrl_version,is_torrent,info_hash,piece_length,total_length,"
    "upload_length,bitfield,in_flight_blob FROM task_progress WHERE out_path=?";
bool receiptFilesIntact(Sqlite3PersistenceStore& store,
                        const std::string& token)
{
  Statement files(store.raw(),
                  "SELECT path,device,inode,size,mtime_ns,ctime_ns FROM "
                  "legacy_checkpoint_file WHERE token=?");
  files.text(1, token);
  size_t count = 0;
  while (files.row()) {
    ++count;
    try {
      auto handle = openSafe(files.column(0));
      Identity expected{files.column(1), files.column(2), files.column(3),
                        files.column(4), files.column(5)};
      if (!(statHandle(handle.fd) == expected))
        return false;
    }
    catch (const RecoverableException&) {
      return false;
    }
  }
  return count != 0;
}
void fingerprintField(MessageDigest& hash, const std::string& value)
{
  std::string size = util::uitos(value.size()) + ":";
  hash.update(size.data(), size.size());
  hash.update(value.data(), value.size());
}
std::string fingerprint(const std::string& gid, const std::string& target,
                        const LegacyCheckpoint& c,
                        const std::vector<FileSpec>& files)
{
  auto hash = MessageDigest::create("sha-256");
  for (const auto& value : {gid, target, c.controlDigest})
    fingerprintField(*hash, value);
  for (const auto& f : files)
    for (const auto& value :
         {f.path, util::itos(f.offset), util::itos(f.length), f.expected.device,
          f.expected.inode, f.expected.size, f.expected.mtime,
          f.expected.ctime})
      fingerprintField(*hash, value);
  return util::toHex(hash->digest());
}
std::unique_ptr<Dict> receipt(Sqlite3PersistenceStore& store,
                              const std::string& token,
                              const std::string& target,
                              const std::string& expectedFingerprint = "")
{
  auto result = Dict::g();
  result->put("version", "1");
  result->put("token", token);
  result->put("targetPath", target);
  Statement s(store.raw(),
              "SELECT "
              "fingerprint,gid,target_path,control_digest,progress_digest,"
              "consumed FROM legacy_checkpoint_import WHERE token=?");
  s.text(1, token);
  if (!s.row()) {
    result->put("status", "absent");
    return result;
  }
  if (s.column(2) != target ||
      (!expectedFingerprint.empty() && s.column(0) != expectedFingerprint))
    throw DL_ABORT_EX("Legacy checkpoint token input conflict");
  result->put("gid", s.column(1));
  result->put("controlDigest", s.column(3));
  Statement p(store.raw(), CHECKPOINT_PROOF_SQL);
  p.text(1, target);
  bool intact = sqlite3_column_int(s.stmt, 5) == 0 &&
                matchesCheckpoint(p, s.column(1), s.column(4), s.column(3)) &&
                receiptFilesIntact(store, token);
  if (!intact && sqlite3_column_int(s.stmt, 5) == 0) {
    // Terminal invalidation is durable: restoring the old row or payload must
    // never turn a consumed token back into a created token. Keep the original
    // checkpoint for diagnosis, but refuse to load it while it is invalidated.
    store.withTransaction([&]() {
      Statement update(store.raw(), "UPDATE legacy_checkpoint_import SET "
                                    "consumed=2 WHERE token=? AND consumed=0");
      update.text(1, token);
      update.done();
    });
  }
  result->put("status", intact ? "created" : "consumed");
  return result;
}
void checkConflicts(DownloadEngine* e, const std::string& gid,
                    const std::string& target,
                    const std::vector<FileSpec>& files)
{
  auto& man = e->getRequestGroupMan();
  auto ownedInode = [&](const std::string& path) {
    struct stat st;
    if (!stat(path.c_str(), &st))
      for (const auto& f : files)
        if (util::uitos(st.st_dev) == f.expected.device &&
            util::uitos(st.st_ino) == f.expected.inode)
          return true;
    return false;
  };
  auto checkPaths =
      [&](const std::vector<std::shared_ptr<FileEntry>>& entries) {
        for (const auto& entry : entries)
          if (!entry->getPath().empty()) {
            if (overlaps(target, entry->getPath()) ||
                ownedInode(entry->getPath()))
              throw DL_ABORT_EX(
                  "Legacy checkpoint target already owned by engine task");
            for (const auto& file : files)
              if (overlaps(file.path, entry->getPath()))
                throw DL_ABORT_EX(
                    "Legacy checkpoint payload already owned by engine task");
          }
      };
  auto groups = [&](const RequestGroupList& list) {
    for (const auto& group : list) {
      if (GroupId::toHex(group->getGID()) == gid)
        throw DL_ABORT_EX("Legacy checkpoint GID already owned by engine task");
      auto dctx = group->getDownloadContext();
      if (dctx) {
        if (overlaps(target, dctx->getBasePath()))
          throw DL_ABORT_EX(
              "Legacy checkpoint target already owned by engine task");
        checkPaths(dctx->getFileEntries());
      }
    }
  };
  groups(man->getRequestGroups());
  groups(man->getReservedGroups());
  for (const auto& dr : man->getDownloadResults()) {
    if (GroupId::toHex(dr->gid->getNumericId()) == gid)
      throw DL_ABORT_EX("Legacy checkpoint GID already owned by engine result");
    checkPaths(dr->fileEntries);
  }
  for (const auto& dr : man->getUnfinishedDownloadResult()) {
    if (dr->gid && GroupId::toHex(dr->gid->getNumericId()) == gid)
      throw DL_ABORT_EX(
          "Legacy checkpoint GID already owned by unfinished result");
    checkPaths(dr->fileEntries);
  }
  auto db = e->getSqlite3Store()->raw();
  Statement gids(db, "SELECT gid FROM task WHERE gid=? UNION ALL SELECT gid "
                     "FROM download_history WHERE gid=? UNION ALL SELECT gid "
                     "FROM task_progress WHERE gid=? UNION ALL SELECT gid FROM "
                     "legacy_checkpoint_import WHERE gid=?");
  for (int i = 1; i <= 4; ++i)
    gids.text(i, gid);
  if (gids.row())
    throw DL_ABORT_EX("Legacy checkpoint GID database conflict");
  Statement paths(db,
                  "SELECT out_path FROM task_progress WHERE out_path IS NOT "
                  "NULL UNION ALL SELECT path FROM download_history_files "
                  "UNION ALL SELECT path FROM legacy_checkpoint_file UNION ALL "
                  "SELECT target_path FROM legacy_checkpoint_import");
  while (paths.row()) {
    auto path = paths.column(0);
    if (overlaps(target, path) || ownedInode(path))
      throw DL_ABORT_EX("Legacy checkpoint target database conflict");
    for (const auto& file : files)
      if (overlaps(file.path, path))
        throw DL_ABORT_EX("Legacy checkpoint payload database conflict");
  }
  for (const auto& f : files) {
    Statement claims(db, "SELECT 1 FROM legacy_checkpoint_file WHERE device=? "
                         "AND inode=? LIMIT 1");
    claims.text(1, f.expected.device);
    claims.text(2, f.expected.inode);
    if (claims.row())
      throw DL_ABORT_EX("Legacy checkpoint inode database conflict");
  }
}
#endif // LEGACY_SAFE_POSIX
} // namespace

#ifdef HAVE_SQLITE3
bool legacyCheckpointPending(Sqlite3PersistenceStore& store,
                             const std::string& path)
{
#  ifdef LEGACY_SAFE_POSIX
  Statement s(
      store.raw(),
      "SELECT gid,progress_digest,control_digest FROM "
      "legacy_checkpoint_import WHERE target_path=? AND consumed IN(0,2)");
  s.text(1, path);
  if (!s.row())
    return false;
  Statement p(store.raw(), CHECKPOINT_PROOF_SQL);
  p.text(1, path);
  return matchesCheckpoint(p, s.column(0), s.column(1), s.column(2));
#  else
  sqlite3_stmt* s = nullptr;
  if (sqlite3_prepare_v2(
          store.raw(),
          "SELECT 1 FROM legacy_checkpoint_import i JOIN "
          "task_progress p ON i.target_path=p.out_path AND "
          "i.gid=p.gid WHERE i.target_path=? AND i.consumed IN(0,2)",
          -1, &s, nullptr) != SQLITE_OK)
    throw DL_ABORT_EX("Legacy checkpoint receipt read failed");
  sqlite3_bind_text(s, 1, path.data(), path.size(), SQLITE_TRANSIENT);
  auto rc = sqlite3_step(s);
  sqlite3_finalize(s);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    throw DL_ABORT_EX("Legacy checkpoint receipt read failed");
  return rc == SQLITE_ROW;
#  endif
}
void consumeLegacyCheckpoint(Sqlite3PersistenceStore& store,
                             const std::string& path, DownloadContext& context)
{
#  ifdef LEGACY_SAFE_POSIX
  Statement s(
      store.raw(),
      "SELECT token,progress_digest,gid,control_digest,consumed FROM "
      "legacy_checkpoint_import WHERE target_path=? AND consumed IN(0,2)");
  s.text(1, path);
  if (!s.row())
    return;
  auto token = s.column(0), digest = s.column(1), gid = s.column(2),
       controlDigest = s.column(3);
  store.withTransaction([&]() {
    Statement p(store.raw(), CHECKPOINT_PROOF_SQL);
    p.text(1, path);
    bool invalidated = sqlite3_column_int(s.stmt, 4) == 2;
    if (!matchesCheckpoint(p, gid, digest, controlDigest)) {
      if (invalidated)
        return; // The original checkpoint was already removed.
      throw DL_ABORT_EX("Legacy checkpoint changed before restoration");
    }
    if (invalidated)
      throw DL_ABORT_EX("Legacy checkpoint import was invalidated");
    auto owner = context.getOwnerRequestGroup();
    bool torrent = false;
    std::string infoHash;
#    ifdef ENABLE_BITTORRENT
    torrent = context.hasAttribute(CTX_ATTR_BT);
    if (torrent)
      infoHash.assign(
          reinterpret_cast<const char*>(bittorrent::getInfoHash(&context)), 20);
#    endif
    if (!owner || GroupId::toHex(owner->getGID()) != gid ||
        context.getPieceLength() != sqlite3_column_int64(p.stmt, 5) ||
        context.getTotalLength() != sqlite3_column_int64(p.stmt, 6) ||
        torrent != (sqlite3_column_int(p.stmt, 3) != 0) ||
        infoHash != p.column(4))
      throw DL_ABORT_EX(
          "Legacy checkpoint native task metadata or GID mismatch");
    Statement files(store.raw(),
                    "SELECT "
                    "path,device,inode,size,mtime_ns,ctime_ns,logical_offset,"
                    "logical_length FROM legacy_checkpoint_file WHERE token=? "
                    "ORDER BY logical_offset,path");
    files.text(1, token);
    size_t matched = 0;
    std::map<std::string, std::pair<int64_t, int64_t>> nativeFiles;
    for (const auto& entry : context.getFileEntries())
      if (!nativeFiles
               .emplace(entry->getPath(),
                        std::make_pair(entry->getOffset(), entry->getLength()))
               .second)
        throw DL_ABORT_EX("Legacy checkpoint duplicate native file map path");
    while (files.row()) {
      auto entry = nativeFiles.find(files.column(0));
      if (entry == nativeFiles.end() ||
          entry->second.first != sqlite3_column_int64(files.stmt, 6) ||
          entry->second.second != sqlite3_column_int64(files.stmt, 7))
        throw DL_ABORT_EX("Legacy checkpoint native task file map mismatch");
      ++matched;
      auto handle = openSafe(files.column(0));
      Identity expected{files.column(1), files.column(2), files.column(3),
                        files.column(4), files.column(5)};
      if (!(statHandle(handle.fd) == expected))
        throw DL_ABORT_EX(
            "Legacy checkpoint payload changed before restoration");
    }
    if (matched != context.getFileEntries().size())
      throw DL_ABORT_EX("Legacy checkpoint native task file map mismatch");
    Statement update(
        store.raw(),
        "UPDATE legacy_checkpoint_import SET consumed=1 WHERE token=?");
    update.text(1, token);
    update.done();
  });
#  else
  if (legacyCheckpointPending(store, path))
    throw DL_ABORT_EX("Legacy checkpoint restoration unavailable on this host");
#  endif
}
#endif
namespace rpc {
std::unique_ptr<ValueBase>
InspectLegacyCheckpointV1RpcMethod::process(const RpcRequest& req,
                                            DownloadEngine* e)
{
  requireAvailable(e);
  auto d = checkRequiredParam<Dict>(req, 0);
  if (req.params->size() != 1 || d->size() != 1)
    throw DL_ABORT_EX("Legacy checkpoint invalid inspection request");
  return inspection(snapshot(d));
}
std::unique_ptr<ValueBase>
ReconcileLegacyCheckpointV1RpcMethod::process(const RpcRequest& req,
                                              DownloadEngine* e)
{
  requireAvailable(e);
#ifdef LEGACY_SAFE_POSIX
  auto d = checkRequiredParam<Dict>(req, 0);
  if (req.params->size() != 1 || d->size() != 2)
    throw DL_ABORT_EX("Legacy checkpoint invalid reconciliation request");
  auto token = stringField(d, "token"), target = stringField(d, "targetPath");
  checkToken(token);
  checkPath(target);
  return receipt(*e->getSqlite3Store(), token, target);
#else
  throw DL_ABORT_EX("LegacyCheckpointImportV1 unavailable");
#endif
}
std::unique_ptr<ValueBase>
ImportLegacyCheckpointV1RpcMethod::process(const RpcRequest& req,
                                           DownloadEngine* e)
{
  requireAvailable(e);
#ifdef LEGACY_SAFE_POSIX
  auto d = checkRequiredParam<Dict>(req, 0);
  if (req.params->size() != 1 || d->size() != 7)
    throw DL_ABORT_EX("Legacy checkpoint invalid import request");
  auto token = stringField(d, "token"), gid = stringField(d, "gid"),
       target = stringField(d, "targetPath");
  checkToken(token);
  checkPath(target);
  if (gid.size() != 16 || gid == "0000000000000000" ||
      gid.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw DL_ABORT_EX("Legacy checkpoint invalid GID");
  auto c = snapshot(d);
  if (c.controlDigest != stringField(d, "controlDigest"))
    throw DL_ABORT_EX("Legacy checkpoint snapshot digest mismatch");
  auto expected = downcast<Dict>(d->get("expected"));
  if (!expected || expected->size() != 4 ||
      stringField(expected, "kind") != (c.torrent ? "bittorrent" : "http") ||
      decimal(expected, "totalLength") != c.totalLength ||
      decimal(expected, "pieceLength") != c.pieceLength ||
      stringField(expected, "infoHash") != util::toHex(c.infoHash))
    throw DL_ABORT_EX("Legacy checkpoint expected metadata mismatch");
  auto files = fileSpecs(d, c, target, false);
  auto inputFingerprint = fingerprint(gid, target, c, files);
  auto store = e->getSqlite3Store();
  auto existing = receipt(*store, token, target, inputFingerprint);
  if (stringField(existing.get(), "status") != "absent")
    return existing;
  files = fileSpecs(d, c, target, true);
  store->withTransaction([&]() {
    checkConflicts(e, gid, target, files);
    verifyFiles(files);
    auto progressDigest = legacyCheckpointProgressDigest(c);
    Statement insert(
        store->raw(),
        "INSERT INTO "
        "task_progress(gid,out_path,ctrl_version,is_torrent,info_hash,piece_"
        "length,total_length,upload_length,bitfield,in_flight_blob,digest,"
        "updated_at) VALUES(?,?,1,?,?,?,?,?,?,?,?,?)");
    insert.text(1, gid);
    insert.text(2, target);
    insert.number(3, c.torrent ? 1 : 0);
    if (c.torrent)
      insert.blob(4, c.infoHash);
    else
      sqlite3_bind_null(insert.stmt, 4);
    insert.number(5, c.pieceLength);
    insert.number(6, c.totalLength);
    insert.number(7, c.uploadLength);
    insert.blob(8, c.bitfield);
    insert.blob(9, c.inFlightBlob);
    insert.blob(10, progressDigest);
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    insert.number(11, now);
    insert.done();
    Statement r(
        store->raw(),
        "INSERT INTO "
        "legacy_checkpoint_import(token,fingerprint,gid,target_path,control_"
        "digest,progress_digest,created_at) VALUES(?,?,?,?,?,?,?)");
    r.text(1, token);
    r.text(2, inputFingerprint);
    r.text(3, gid);
    r.text(4, target);
    r.text(5, c.controlDigest);
    r.blob(6, progressDigest);
    r.number(7, now);
    r.done();
    for (const auto& f : files) {
      Statement row(
          store->raw(),
          "INSERT INTO "
          "legacy_checkpoint_file(token,path,device,inode,size,mtime_ns,ctime_"
          "ns,logical_offset,logical_length) VALUES(?,?,?,?,?,?,?,?,?)");
      row.text(1, token);
      row.text(2, f.path);
      row.text(3, f.expected.device);
      row.text(4, f.expected.inode);
      row.text(5, f.expected.size);
      row.text(6, f.expected.mtime);
      row.text(7, f.expected.ctime);
      row.number(8, f.offset);
      row.number(9, f.length);
      row.done();
    }
    verifyFiles(files);
  });
  return receipt(*store, token, target, inputFingerprint);
#else
  throw DL_ABORT_EX("LegacyCheckpointImportV1 unavailable");
#endif
}
} // namespace rpc
} // namespace aria2
