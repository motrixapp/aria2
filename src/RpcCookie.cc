// SPDX-License-Identifier: GPL-2.0-or-later
#include "RpcCookie.h"

#include <algorithm>
#include <limits>

#include "Cookie.h"
#include "a2functional.h"
#include "CookieStorage.h"
#include "DlAbortEx.h"
#include "TimeA2.h"
#include "ValueBase.h"
#include "cookie_helper.h"
#include "util.h"

namespace aria2 {
namespace {
const String* stringField(const Dict& object, const char* key, bool required)
{
  auto value = object.get(key);
  auto result = downcast<String>(value);
  if ((required && !result) || (value && !result)) {
    throw DL_ABORT_EX("Invalid task cookie string field.");
  }
  return result;
}

bool boolField(const Dict& object, const char* key, bool fallback)
{
  auto value = object.get(key);
  if (!value) {
    return fallback;
  }
  auto result = downcast<Bool>(value);
  if (!result) {
    throw DL_ABORT_EX("Invalid task cookie boolean field.");
  }
  return result->val();
}

bool safeValue(const std::string& value)
{
  return std::none_of(value.begin(), value.end(), [](unsigned char c) {
    return c < 0x20 || c == 0x7f || c == ';';
  });
}
} // namespace

std::shared_ptr<CookieStorage> parseRpcCookies(const List& cookies)
{
  if (cookies.size() > 300) {
    throw DL_ABORT_EX("Too many task cookies.");
  }
  auto storage = std::make_shared<CookieStorage>();
  auto now = Time().getTimeFromEpoch();
  for (const auto& entry : cookies) {
    auto object = downcast<Dict>(entry);
    if (!object) {
      throw DL_ABORT_EX("Task cookie must be an object.");
    }
    for (const auto& field : *object) {
      const auto& key = field.first;
      if (key != "name" && key != "value" && key != "domain" && key != "path" &&
          key != "hostOnly" && key != "secure" && key != "httpOnly" &&
          key != "expiresAt") {
        throw DL_ABORT_EX("Unknown task cookie field.");
      }
    }
    auto name = stringField(*object, "name", true)->s();
    auto value = stringField(*object, "value", true)->s();
    auto domain = stringField(*object, "domain", true)->s();
    auto pathField = stringField(*object, "path", false);
    auto path = pathField ? pathField->s() : "/";
    if (name.empty() || name.size() + value.size() > 4096 ||
        domain.empty() || domain.size() > 254 || path.size() > 4096 ||
        !safeValue(value) || !safeValue(path) ||
        !cookie::goodPath(path.begin(), path.end()) ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) {
          return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') ||
                   std::string("!#$%&'*+-.^_`|~").find(c) != std::string::npos);
        })) {
      throw DL_ABORT_EX("Invalid task cookie syntax.");
    }
    bool hostOnly = boolField(*object, "hostOnly", domain.front() != '.');
    if (domain.front() == '.') {
      domain.erase(0, 1);
    }
    domain = cookie::canonicalizeHost(domain);
    bool numeric = util::isNumericHost(domain);
    if (domain.empty() || domain.find("..") != std::string::npos ||
        domain.front() == '.' || domain.back() == '.' ||
        (!numeric && std::any_of(domain.begin(), domain.end(), [](unsigned char c) {
          return !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '-' || c == '.');
        }))) {
      throw DL_ABORT_EX("Invalid task cookie domain.");
    }
    auto expiresValue = object->get("expiresAt");
    auto expires = downcast<Integer>(expiresValue);
    if (expiresValue && (!expires || expires->i() < 0 ||
                         expires->i() > 9007199254740991LL)) {
      throw DL_ABORT_EX(
          "Task cookie expiry must be nonnegative Unix milliseconds.");
    }
    auto expirySeconds = expires ? expires->i() / 1000 : 0;
    auto expiry = static_cast<time_t>(std::min<int64_t>(
        expirySeconds, std::numeric_limits<time_t>::max()));
    auto c = make_unique<Cookie>(
        name, value, expiry, expires != nullptr, domain, hostOnly || numeric,
        path, boolField(*object, "secure", false),
        boolField(*object, "httpOnly", false), now);
    // Expired records are valid input but must not enter the active store.
    if (!expires || expires->i() > static_cast<int64_t>(now) * 1000) {
      storage->store(std::move(c), now);
    }
  }
  return storage;
}
} // namespace aria2
