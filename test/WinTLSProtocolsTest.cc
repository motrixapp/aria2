#include "WinTLSProtocols.h"

#include <cppunit/extensions/HelperMacros.h>

#include "TLSContext.h"

namespace aria2 {

// The WinTLS min-tls-version logic is Windows-only, but its core is the
// pure bit-mask computed here, so it can be locked down on any host. The
// ported fix AND-ed ~PROTO into a zero grbitDisabledProtocols mask, which
// left it at 0 ("no floor") for every requested version, silently turning
// --min-tls-version into a no-op (upstream #1407). These tests assert the
// mask actually disables the versions below the requested minimum.
class WinTLSProtocolsTest : public CppUnit::TestFixture {

  CPPUNIT_TEST_SUITE(WinTLSProtocolsTest);
  CPPUNIT_TEST(testClientDisabledMaskEnforcesMinimum);
  CPPUNIT_TEST(testServerDisabledMaskEnforcesMinimum);
  CPPUNIT_TEST(testMasksDifferPerVersion);
  CPPUNIT_TEST_SUITE_END();

public:
  void testClientDisabledMaskEnforcesMinimum();
  void testServerDisabledMaskEnforcesMinimum();
  void testMasksDifferPerVersion();
};

CPPUNIT_TEST_SUITE_REGISTRATION(WinTLSProtocolsTest);

void WinTLSProtocolsTest::testClientDisabledMaskEnforcesMinimum()
{
  // Minimum TLS 1.2: 1.0 and 1.1 disabled, 1.2 left enabled.
  auto tls12 = winTLSDisabledProtocols(true, TLS_PROTO_TLS12);
  CPPUNIT_ASSERT((tls12 & SP_PROT_PCT1_CLIENT) != 0);
  CPPUNIT_ASSERT((tls12 & SP_PROT_SSL2_CLIENT) != 0);
  CPPUNIT_ASSERT((tls12 & SP_PROT_SSL3_CLIENT) != 0);
  CPPUNIT_ASSERT((tls12 & SP_PROT_TLS1_0_CLIENT) != 0);
  CPPUNIT_ASSERT((tls12 & SP_PROT_TLS1_1_CLIENT) != 0);
  CPPUNIT_ASSERT((tls12 & SP_PROT_TLS1_2_CLIENT) == 0);
  // The core regression: the mask must not be empty (0 == system default).
  CPPUNIT_ASSERT(tls12 != 0);

  // Minimum TLS 1.3: 1.0, 1.1, 1.2 all disabled.
  auto tls13 = winTLSDisabledProtocols(true, TLS_PROTO_TLS13);
  CPPUNIT_ASSERT((tls13 & SP_PROT_TLS1_0_CLIENT) != 0);
  CPPUNIT_ASSERT((tls13 & SP_PROT_TLS1_1_CLIENT) != 0);
  CPPUNIT_ASSERT((tls13 & SP_PROT_TLS1_2_CLIENT) != 0);

  // Minimum TLS 1.1: 1.0 and all pre-TLS protocols disabled.
  auto tls11 = winTLSDisabledProtocols(true, TLS_PROTO_TLS11);
  CPPUNIT_ASSERT((tls11 & SP_PROT_SSL3_CLIENT) != 0);
  CPPUNIT_ASSERT((tls11 & SP_PROT_TLS1_0_CLIENT) != 0);
  CPPUNIT_ASSERT((tls11 & SP_PROT_TLS1_1_CLIENT) == 0);
}

void WinTLSProtocolsTest::testServerDisabledMaskEnforcesMinimum()
{
  auto tls12 = winTLSDisabledProtocols(false, TLS_PROTO_TLS12);
  CPPUNIT_ASSERT((tls12 & SP_PROT_PCT1_SERVER) != 0);
  CPPUNIT_ASSERT((tls12 & SP_PROT_SSL2_SERVER) != 0);
  CPPUNIT_ASSERT((tls12 & SP_PROT_SSL3_SERVER) != 0);
  CPPUNIT_ASSERT((tls12 & SP_PROT_TLS1_0_SERVER) != 0);
  CPPUNIT_ASSERT((tls12 & SP_PROT_TLS1_1_SERVER) != 0);
  CPPUNIT_ASSERT((tls12 & SP_PROT_TLS1_2_SERVER) == 0);
  CPPUNIT_ASSERT(tls12 != 0);
}

void WinTLSProtocolsTest::testMasksDifferPerVersion()
{
  // The bug made all three requests produce the identical mask (0). A
  // higher minimum must disable strictly more protocols than a lower one.
  auto tls11 = winTLSDisabledProtocols(true, TLS_PROTO_TLS11);
  auto tls12 = winTLSDisabledProtocols(true, TLS_PROTO_TLS12);
  auto tls13 = winTLSDisabledProtocols(true, TLS_PROTO_TLS13);
  CPPUNIT_ASSERT(tls11 != tls12);
  CPPUNIT_ASSERT(tls12 != tls13);
  CPPUNIT_ASSERT((tls11 & tls12) == tls11); // tls12 is a superset of tls11
  CPPUNIT_ASSERT((tls12 & tls13) == tls12); // tls13 is a superset of tls12
}

} // namespace aria2
