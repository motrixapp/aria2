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
// Windows implementation of the private legacy import file primitives.
// Included inside LegacyCheckpointImport.cc's anonymous namespace, after Identity.
// No payload or metadata handle requests write access.
struct Handle {
  HANDLE fd;
  std::vector<Handle> parents;
  explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : fd(value) {}
  ~Handle() { if (fd != INVALID_HANDLE_VALUE) CloseHandle(fd); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& o) : fd(o.fd), parents(std::move(o.parents))
  { o.fd = INVALID_HANDLE_VALUE; }
  Handle& operator=(Handle&& o)
  {
    if (fd != INVALID_HANDLE_VALUE) CloseHandle(fd);
    fd = o.fd;
    o.fd = INVALID_HANDLE_VALUE;
    parents = std::move(o.parents);
    return *this;
  }
};
struct LocalSecurity {
  PSECURITY_DESCRIPTOR value = nullptr;
  ~LocalSecurity() { if (value) LocalFree(value); }
};
void checkFileAcl(HANDLE file)
{
  Handle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.fd))
    throw DL_ABORT_EX("Legacy checkpoint cannot query Windows owner");
  DWORD length = 0;
  GetTokenInformation(token.fd, TokenUser, nullptr, 0, &length);
  if (!length) throw DL_ABORT_EX("Legacy checkpoint cannot query Windows owner");
  std::vector<unsigned char> user(length);
  if (!GetTokenInformation(token.fd, TokenUser, user.data(), length, &length))
    throw DL_ABORT_EX("Legacy checkpoint cannot query Windows owner");
  auto sid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
  LocalSecurity security;
  PSID owner = nullptr;
  PACL acl = nullptr;
  if (GetSecurityInfo(file, SE_FILE_OBJECT,
                      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                      &owner, nullptr, &acl, nullptr, &security.value) != ERROR_SUCCESS ||
      !owner || !EqualSid(owner, sid) || !acl || !IsValidAcl(acl))
    throw DL_ABORT_EX("Legacy checkpoint payload is not exclusively owned (Windows ACL)");
  // SYSTEM and Administrators are the Windows equivalent of privileged root.
  // Reject grants to every other principal, even when a deny ACE might mask
  // them. Unknown/conditional ACE forms fail closed rather than being guessed.
  unsigned char system[SECURITY_MAX_SID_SIZE], admins[SECURITY_MAX_SID_SIZE];
  DWORD systemLength = sizeof(system), adminsLength = sizeof(admins);
  if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &systemLength) ||
      !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins, &adminsLength))
    throw DL_ABORT_EX("Legacy checkpoint cannot query Windows privileged SIDs");
  constexpr DWORD writes = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
      FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER |
      GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED;
  for (DWORD i = 0; i < acl->AceCount; ++i) {
    void* value = nullptr;
    if (!GetAce(acl, i, &value))
      throw DL_ABORT_EX("Legacy checkpoint invalid Windows ACL");
    auto header = static_cast<ACE_HEADER*>(value);
    if (header->AceFlags & INHERIT_ONLY_ACE) continue;
    if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
      throw DL_ABORT_EX("Legacy checkpoint unsupported Windows ACL");
    auto ace = static_cast<ACCESS_ALLOWED_ACE*>(value);
    PSID principal = &ace->SidStart;
    if (!IsValidSid(principal) ||
        ((ace->Mask & writes) && !EqualSid(principal, sid) &&
         !EqualSid(principal, system) && !EqualSid(principal, admins)))
      throw DL_ABORT_EX("Legacy checkpoint payload is not exclusively owned (Windows ACL)");
  }
}
std::string windowsNs(LONGLONG ticks)
{
  constexpr int64_t epoch = 116444736000000000LL;
  if (ticks < epoch || ticks - epoch > INT64_MAX / 100)
    throw DL_ABORT_EX("Legacy checkpoint payload timestamp out of range");
  return util::itos((ticks - epoch) * 100);
}
Identity statHandle(HANDLE fd)
{
  BY_HANDLE_FILE_INFORMATION info;
  FILE_BASIC_INFO basic;
  if (GetFileType(fd) != FILE_TYPE_DISK ||
      !GetFileInformationByHandle(fd, &info) ||
      !GetFileInformationByHandleEx(fd, FileBasicInfo, &basic, sizeof(basic)) ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
      info.nNumberOfLinks != 1 || info.nFileSizeHigh > INT32_MAX)
    throw DL_ABORT_EX("Legacy checkpoint payload is not an exclusively owned regular file");
  checkFileAcl(fd);
  // These fields match Node/libuv bigint stat on NTFS. ChangeTime is metadata
  // change time, not CreationTime (which older CRT stat exposes as ctime).
  return {util::uitos(info.dwVolumeSerialNumber),
          util::uitos((uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow),
          util::uitos((uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow),
          windowsNs(basic.LastWriteTime.QuadPart), windowsNs(basic.ChangeTime.QuadPart)};
}
Handle openSafe(const std::string& path)
{
  checkPath(path);
  auto wide = utf8ToWChar(path);
  if (wCharToUtf8(wide) != path)
    throw DL_ABORT_EX("Legacy checkpoint invalid UTF-8 path");
  std::replace(wide.begin(), wide.end(), L'/', L'\\');
  auto drive = wide.substr(0, 3);
  wchar_t filesystem[32];
  DWORD flags = 0;
  if (GetDriveTypeW(drive.c_str()) != DRIVE_FIXED ||
      !GetVolumeInformationW(drive.c_str(), nullptr, 0, nullptr, nullptr,
                            &flags, filesystem, 32) ||
      wcscmp(filesystem, L"NTFS") || !(flags & FILE_PERSISTENT_ACLS))
    throw DL_ABORT_EX("Legacy checkpoint requires a local NTFS volume with ACLs");
  std::vector<Handle> parents;
  // Retain every ancestor without write/delete sharing. A checked directory
  // cannot be renamed or converted to a reparse point while opening descendants.
  size_t end = 3;
  while (true) {
    bool final = end == wide.size();
    auto prefix = L"\\\\?\\" + wide.substr(0, end);
    Handle handle(CreateFileW(prefix.c_str(),
        final ? (GENERIC_READ | READ_CONTROL) : FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    BY_HANDLE_FILE_INFORMATION info;
    if (handle.fd == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(handle.fd, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        (!final && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)))
      throw DL_ABORT_EX("Legacy checkpoint safe path open failed");
    // Refuse DOS short names, alternate casing, substituted drives and other
    // aliases: the SQLite key must be the actual native path, including case.
    std::vector<wchar_t> actual(32768);
    auto count = GetFinalPathNameByHandleW(handle.fd, actual.data(), actual.size(),
                                         FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!count || count >= actual.size() || std::wstring(actual.data(), count) != prefix)
      throw DL_ABORT_EX("Legacy checkpoint safe path is not canonical");
    if (final) {
      statHandle(handle.fd);
      handle.parents = std::move(parents);
      return handle;
    }
    parents.push_back(std::move(handle));
    end = wide.find(L'\\', end == 3 ? 3 : end + 1);
    if (end == std::wstring::npos) end = wide.size();
  }
}
int64_t readHandle(HANDLE fd, char* buffer, size_t length, size_t offset)
{
  LARGE_INTEGER position;
  position.QuadPart = offset;
  DWORD read = 0;
  if (!SetFilePointerEx(fd, position, nullptr, FILE_BEGIN) ||
      !ReadFile(fd, buffer, static_cast<DWORD>(length), &read, nullptr))
    throw DL_ABORT_EX("Legacy torrent metadata read failed");
  return read;
}
