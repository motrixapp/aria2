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
#include "MemBufferIOFile.h"

#include <cassert>
#include <cstdarg>
#include <cstdio>

namespace aria2 {

MemBufferIOFile::MemBufferIOFile() {}

size_t MemBufferIOFile::onRead(void* ptr, size_t count)
{
  assert(0);
  return 0;
}

size_t MemBufferIOFile::onWrite(const void* ptr, size_t count)
{
  buf_.append(static_cast<const char*>(ptr), count);
  return count;
}

char* MemBufferIOFile::onGets(char* s, int size)
{
  assert(0);
  return nullptr;
}

int MemBufferIOFile::onVprintf(const char* format, va_list va)
{
  char stackBuf[1024];
  va_list vaCopy;
  va_copy(vaCopy, va);
  int n = vsnprintf(stackBuf, sizeof(stackBuf), format, vaCopy);
  va_end(vaCopy);
  if (n < 0) {
    return n;
  }
  if (static_cast<size_t>(n) < sizeof(stackBuf)) {
    buf_.append(stackBuf, static_cast<size_t>(n));
  }
  else {
    // Allocate exact size and retry.
    std::string tmp(static_cast<size_t>(n + 1), '\0');
    va_copy(vaCopy, va);
    n = vsnprintf(&tmp[0], tmp.size(), format, vaCopy);
    va_end(vaCopy);
    if (n >= 0) {
      buf_.append(tmp.data(), static_cast<size_t>(n));
    }
  }
  return n;
}

int MemBufferIOFile::onFlush() { return 0; }

int MemBufferIOFile::onClose() { return 0; }

bool MemBufferIOFile::onSupportsColor() { return false; }

bool MemBufferIOFile::isError() const { return false; }

bool MemBufferIOFile::isEOF() const { return false; }

bool MemBufferIOFile::isOpen() const { return true; }

} // namespace aria2
