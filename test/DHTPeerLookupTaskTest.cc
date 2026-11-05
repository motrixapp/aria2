#include "DHTPeerLookupTask.h"

#include <cppunit/extensions/HelperMacros.h>

#include "DHTAnnouncePeerMessage.h"
#include "DHTGetPeersMessage.h"
#include "DHTGetPeersReplyMessage.h"
#include "DHTMessageFactoryImpl.h"
#include "DHTNode.h"
#include "DHTRoutingTable.h"
#include "DefaultPeerStorage.h"
#include "DownloadContext.h"
#include "MockDHTMessageDispatcher.h"
#include "TorrentAttribute.h"
#include "bencode2.h"
#include "bittorrent_helper.h"

namespace aria2 {

class DHTPeerLookupTaskTest : public CppUnit::TestFixture {
  CPPUNIT_TEST_SUITE(DHTPeerLookupTaskTest);
  CPPUNIT_TEST(testAnnounceRequiresToken);
  CPPUNIT_TEST(testTokenlessReplyContinuesLookup);
  CPPUNIT_TEST_SUITE_END();

public:
  void testAnnounceRequiresToken();
  void testTokenlessReplyContinuesLookup();
};

CPPUNIT_TEST_SUITE_REGISTRATION(DHTPeerLookupTaskTest);

void DHTPeerLookupTaskTest::testAnnounceRequiresToken()
{
  for (auto family : {AF_INET, AF_INET6}) {
    for (auto token : {std::string(), std::string("write-token")}) {
      auto local = std::make_shared<DHTNode>();
      auto remote = std::make_shared<DHTNode>();
      remote->setIPAddress(family == AF_INET ? "192.0.2.1" : "2001:db8::1");
      remote->setPort(6881);
      DHTRoutingTable table(local);
      table.addNode(remote);
      DHTMessageFactoryImpl factory(family);
      factory.setLocalNode(local);
      factory.setRoutingTable(&table);
      MockDHTMessageDispatcher dispatcher;
      auto attrs = std::make_shared<TorrentAttribute>();
      attrs->infoHash = std::string(DHT_ID_LENGTH, 'a');
      auto context = std::make_shared<DownloadContext>();
      context->setAttribute(CTX_ATTR_BT, attrs);
      DHTPeerLookupTask task(context, 6881);
      task.setLocalNode(local);
      task.setRoutingTable(&table);
      task.setMessageFactory(&factory);
      task.setMessageDispatcher(&dispatcher);
      task.setPeerStorage(std::make_shared<DefaultPeerStorage>());
      task.startup();
      CPPUNIT_ASSERT_EQUAL(size_t(1), dispatcher.messageQueue_.size());
      CPPUNIT_ASSERT(dynamic_cast<DHTGetPeersMessage*>(
          dispatcher.messageQueue_.front().message_.get()));
      dispatcher.messageQueue_.clear();

      auto reply = factory.createGetPeersReplyMessage(remote, {}, {}, token, "tx");
      task.onReceived(reply.get());
      CPPUNIT_ASSERT(task.finished());
      CPPUNIT_ASSERT_EQUAL(token.empty() ? size_t(0) : size_t(1),
                           dispatcher.messageQueue_.size());
      if (!token.empty()) {
        auto announce = dynamic_cast<DHTAnnouncePeerMessage*>(
            dispatcher.messageQueue_.front().message_.get());
        CPPUNIT_ASSERT(announce);
        auto wire = bencode2::decode(announce->getBencodedMessage());
        auto args = downcast<Dict>(downcast<Dict>(wire)->get("a"));
        CPPUNIT_ASSERT_EQUAL(token, downcast<String>(args->get("token"))->s());
        CPPUNIT_ASSERT(*remote == *announce->getRemoteNode());
      }
    }
  }
}

void DHTPeerLookupTaskTest::testTokenlessReplyContinuesLookup()
{
  for (auto family : {AF_INET, AF_INET6}) {
    auto local = std::make_shared<DHTNode>();
    auto router = std::make_shared<DHTNode>();
    router->setIPAddress(family == AF_INET ? "192.0.2.1" : "2001:db8::1");
    router->setPort(6881);
    auto next = std::make_shared<DHTNode>();
    next->setIPAddress(family == AF_INET ? "192.0.2.2" : "2001:db8::2");
    next->setPort(6882);
    DHTRoutingTable table(local);
    table.addNode(router);
    DHTMessageFactoryImpl factory(family);
    factory.setLocalNode(local);
    factory.setRoutingTable(&table);
    MockDHTMessageDispatcher dispatcher;
    auto attrs = std::make_shared<TorrentAttribute>();
    attrs->infoHash = std::string(DHT_ID_LENGTH, 'a');
    auto context = std::make_shared<DownloadContext>();
    context->setAttribute(CTX_ATTR_BT, attrs);
    DHTPeerLookupTask task(context, 6881);
    task.setLocalNode(local);
    task.setRoutingTable(&table);
    task.setMessageFactory(&factory);
    task.setMessageDispatcher(&dispatcher);
    task.setPeerStorage(std::make_shared<DefaultPeerStorage>());
    task.startup();
    CPPUNIT_ASSERT_EQUAL(size_t(1), dispatcher.messageQueue_.size());
    dispatcher.messageQueue_.clear();

    unsigned char compact[COMPACT_LEN_IPV6];
    auto len = bittorrent::packcompact(compact, next->getIPAddress(),
                                       next->getPort());
    Dict dict;
    dict.put("t", "tx");
    dict.put("y", "r");
    auto response = Dict::g();
    response->put("id", String::g(router->getID(), DHT_ID_LENGTH));
    response->put(family == AF_INET ? "nodes" : "nodes6",
                  std::string(reinterpret_cast<const char*>(next->getID()),
                              DHT_ID_LENGTH) +
                      std::string(reinterpret_cast<const char*>(compact), len));
    dict.put("r", std::move(response));
    auto reply = factory.createResponseMessage(
        "get_peers", &dict, router->getIPAddress(), router->getPort());
    auto getPeers = dynamic_cast<DHTGetPeersReplyMessage*>(reply.get());
    CPPUNIT_ASSERT(getPeers);
    task.onReceived(getPeers);
    CPPUNIT_ASSERT(!task.finished());
    CPPUNIT_ASSERT_EQUAL(size_t(1), dispatcher.messageQueue_.size());
    auto query = dynamic_cast<DHTGetPeersMessage*>(
        dispatcher.messageQueue_.front().message_.get());
    CPPUNIT_ASSERT(query);
    CPPUNIT_ASSERT(*next == *query->getRemoteNode());
    dispatcher.messageQueue_.clear();

    auto nextReply = factory.createGetPeersReplyMessage(next, {}, {}, "token", "tx");
    task.onReceived(nextReply.get());
    CPPUNIT_ASSERT(task.finished());
    CPPUNIT_ASSERT_EQUAL(size_t(1), dispatcher.messageQueue_.size());
    auto announce = dynamic_cast<DHTAnnouncePeerMessage*>(
        dispatcher.messageQueue_.front().message_.get());
    CPPUNIT_ASSERT(announce);
    CPPUNIT_ASSERT(*next == *announce->getRemoteNode());
    CPPUNIT_ASSERT_EQUAL(std::string("token"), announce->getToken());
  }
}

} // namespace aria2
