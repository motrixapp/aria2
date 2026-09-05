// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef D_RPC_COOKIE_H
#define D_RPC_COOKIE_H

#include <memory>

namespace aria2 {
class CookieStorage;
class List;

// Validates the entire input before exposing a task's in-memory cookie store.
// The RPC contract uses integer Unix milliseconds, not Netscape seconds.
std::shared_ptr<CookieStorage> parseRpcCookies(const List& cookies);
} // namespace aria2

#endif // D_RPC_COOKIE_H
