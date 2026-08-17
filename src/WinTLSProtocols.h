/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2013 Tatsuhiro Tsujikawa
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
#ifndef D_WIN_TLS_PROTOCOLS_H
#define D_WIN_TLS_PROTOCOLS_H

#include "common.h"

#include <cstdint>

#include "TLSContext.h"

// Schannel protocol-version bit constants. The Windows SDK defines these in
// <schannel.h>; provide fallbacks matching those values so the pure mask
// logic below compiles and is unit-testable on non-Windows hosts.
#ifndef SP_PROT_TLS1_0_CLIENT
#  define SP_PROT_TLS1_0_CLIENT 0x00000080
#endif
#ifndef SP_PROT_TLS1_0_SERVER
#  define SP_PROT_TLS1_0_SERVER 0x00000040
#endif
#ifndef SP_PROT_TLS1_1_CLIENT
#  define SP_PROT_TLS1_1_CLIENT 0x00000200
#endif
#ifndef SP_PROT_TLS1_1_SERVER
#  define SP_PROT_TLS1_1_SERVER 0x00000100
#endif
#ifndef SP_PROT_TLS1_2_CLIENT
#  define SP_PROT_TLS1_2_CLIENT 0x00000800
#endif
#ifndef SP_PROT_TLS1_2_SERVER
#  define SP_PROT_TLS1_2_SERVER 0x00000400
#endif

namespace aria2 {

// Compute the SCH_CREDENTIALS grbitDisabledProtocols black-list that
// enforces |ver| as the MINIMUM TLS version: every known protocol strictly
// below |ver| is disabled, while |ver| and above are left to Schannel's
// defaults. |client| selects the *_CLIENT vs *_SERVER bit set.
//
// grbitDisabledProtocols is a black-list, the inverse of the legacy
// SCHANNEL_CRED grbitEnabledProtocols white-list. The upstream-fork port
// kept the white-list's fall-through shape but AND-ed ~PROTO into a mask
// that starts at 0, so it stayed 0 for every version -- meaning "disable
// nothing / use system default" and silently reducing --min-tls-version to
// a no-op (upstream #1407). Building the disabled set explicitly restores
// the version floor.
inline uint32_t winTLSDisabledProtocols(bool client, TLSVersion ver)
{
  const uint32_t tls10 =
      client ? SP_PROT_TLS1_0_CLIENT : SP_PROT_TLS1_0_SERVER;
  const uint32_t tls11 =
      client ? SP_PROT_TLS1_1_CLIENT : SP_PROT_TLS1_1_SERVER;
  const uint32_t tls12 =
      client ? SP_PROT_TLS1_2_CLIENT : SP_PROT_TLS1_2_SERVER;
  uint32_t disabled = 0;
  switch (ver) {
  case TLS_PROTO_TLS13:
    disabled |= tls12;
    // fall through
  case TLS_PROTO_TLS12:
    disabled |= tls11;
    // fall through
  case TLS_PROTO_TLS11:
    disabled |= tls10;
    break;
  default:
    break;
  }
  return disabled;
}

} // namespace aria2

#endif // D_WIN_TLS_PROTOCOLS_H
