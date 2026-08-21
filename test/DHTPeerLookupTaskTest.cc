#include "DHTMessageCallback.h"
#include "DHTPeerLookupTask.h"

#include <cppunit/extensions/HelperMacros.h>

#include "BtRegistry.h"
#include "DownloadContext.h"
#include "bittorrent_helper.h"

namespace aria2 {

namespace {
class DHTPeerLookupTaskProbe : public DHTPeerLookupTask {
public:
  DHTPeerLookupTaskProbe(
      const std::shared_ptr<DownloadContext>& downloadContext,
      const std::shared_ptr<const BtAnnouncePortState>& announcePortState,
      int family)
      : DHTPeerLookupTask(downloadContext, announcePortState, family)
  {
  }

  uint16_t announcePort() const { return getAnnouncePort(); }
};
} // namespace

class DHTPeerLookupTaskTest : public CppUnit::TestFixture {
  CPPUNIT_TEST_SUITE(DHTPeerLookupTaskTest);
  CPPUNIT_TEST(testAnnouncePortTracksRegistry);
  CPPUNIT_TEST_SUITE_END();

public:
  void testAnnouncePortTracksRegistry();
};

CPPUNIT_TEST_SUITE_REGISTRATION(DHTPeerLookupTaskTest);

void DHTPeerLookupTaskTest::testAnnouncePortTracksRegistry()
{
  auto dctx = std::make_shared<DownloadContext>();
  auto attrs = make_unique<TorrentAttribute>();
  attrs->infoHash.assign(INFO_HASH_LENGTH, '\0');
  dctx->setAttribute(CTX_ATTR_BT, std::move(attrs));

  auto registry = make_unique<BtRegistry>();
  registry->setTcpPort(6881);
  DHTPeerLookupTaskProbe task4(dctx, registry->getAnnouncePortState(), AF_INET);
  DHTPeerLookupTaskProbe task6(dctx, registry->getAnnouncePortState(),
                               AF_INET6);
  CPPUNIT_ASSERT_EQUAL((uint16_t)6881, task4.announcePort());
  CPPUNIT_ASSERT_EQUAL((uint16_t)6881, task6.announcePort());

  registry->setExternalEndpoint("", 62000);
  CPPUNIT_ASSERT_EQUAL((uint16_t)62000, task4.announcePort());
  CPPUNIT_ASSERT_EQUAL((uint16_t)6881, task6.announcePort());

  registry.reset();
  CPPUNIT_ASSERT_EQUAL((uint16_t)62000, task4.announcePort());
  CPPUNIT_ASSERT_EQUAL((uint16_t)6881, task6.announcePort());
}

} // namespace aria2
