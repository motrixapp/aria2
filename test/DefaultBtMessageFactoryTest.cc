#include "DefaultBtMessageFactory.h"

#include <cstring>

#include <iostream>

#include <cppunit/extensions/HelperMacros.h>

#include "Peer.h"
#include "bittorrent_helper.h"
#include "DownloadContext.h"
#include "MockExtensionMessageFactory.h"
#include "BtExtendedMessage.h"
#include "BtPortMessage.h"
#include "Exception.h"
#include "FileEntry.h"
#include "DefaultBtInteractive.h"
#include "MockBtMessageDispatcher.h"
#include "ExtensionMessageRegistry.h"
#include "HandshakeExtensionMessage.h"
#include "wallclock.h"

namespace aria2 {

class DefaultBtMessageFactoryTest : public CppUnit::TestFixture {

  CPPUNIT_TEST_SUITE(DefaultBtMessageFactoryTest);
  CPPUNIT_TEST(testCreateBtMessage_BtExtendedMessage);
  CPPUNIT_TEST(testCreatePortMessage);
  CPPUNIT_TEST(testAdvertisedPortBeforeExtendedHandshake);
  CPPUNIT_TEST(testAdvertisedPortUpdatesAreCoalesced);
  CPPUNIT_TEST_SUITE_END();

private:
  std::unique_ptr<DownloadContext> dctx_;
  std::shared_ptr<Peer> peer_;
  std::shared_ptr<MockExtensionMessageFactory> exmsgFactory_;
  std::unique_ptr<DefaultBtMessageFactory> factory_;

public:
  void setUp()
  {
    dctx_ = make_unique<DownloadContext>();

    peer_ = std::make_shared<Peer>("192.168.0.1", 6969);
    peer_->allocateSessionResource(1_k, 1_m);
    peer_->setExtendedMessagingEnabled(true);

    exmsgFactory_ = std::make_shared<MockExtensionMessageFactory>();

    factory_ = make_unique<DefaultBtMessageFactory>();
    factory_->setDownloadContext(dctx_.get());
    factory_->setPeer(peer_);
    factory_->setExtensionMessageFactory(exmsgFactory_.get());
  }

  void testCreateBtMessage_BtExtendedMessage();
  void testCreatePortMessage();
  void testAdvertisedPortBeforeExtendedHandshake();
  void testAdvertisedPortUpdatesAreCoalesced();
};

CPPUNIT_TEST_SUITE_REGISTRATION(DefaultBtMessageFactoryTest);

void DefaultBtMessageFactoryTest::testCreateBtMessage_BtExtendedMessage()
{
  // payload:{4:name3:foo}->11bytes
  std::string payload = "4:name3:foo";
  char msg[17]; // 6+11bytes
  bittorrent::createPeerMessageString((unsigned char*)msg, sizeof(msg), 13, 20);
  msg[5] = 1; // Set dummy extended message ID 1
  memcpy(msg + 6, payload.c_str(), payload.size());

  auto m =
      factory_->createBtMessage((const unsigned char*)msg + 4, sizeof(msg) - 4);
  CPPUNIT_ASSERT(BtExtendedMessage::ID == m->getId());
  try {
    // disable extended messaging
    peer_->setExtendedMessagingEnabled(false);
    factory_->createBtMessage((const unsigned char*)msg + 4, sizeof(msg) - 4);
    CPPUNIT_FAIL("exception must be thrown.");
  }
  catch (Exception& e) {
    std::cerr << e.stackTrace() << std::endl;
  }
}

void DefaultBtMessageFactoryTest::testCreatePortMessage()
{
  {
    unsigned char data[7];
    bittorrent::createPeerMessageString(data, sizeof(data), 3, 9);
    bittorrent::setShortIntParam(&data[5], 6881);
    try {
      auto r = factory_->createBtMessage(&data[4], sizeof(data) - 4);
      CPPUNIT_ASSERT(BtPortMessage::ID == r->getId());
      auto m = static_cast<const BtPortMessage*>(r.get());
      CPPUNIT_ASSERT_EQUAL((uint16_t)6881, m->getPort());
    }
    catch (Exception& e) {
      CPPUNIT_FAIL(e.stackTrace());
    }
  }
  {
    auto m = factory_->createPortMessage(6881);
    CPPUNIT_ASSERT_EQUAL((uint16_t)6881, m->getPort());
  }
}

void DefaultBtMessageFactoryTest::testAdvertisedPortBeforeExtendedHandshake()
{
  auto dctx = std::make_shared<DownloadContext>();
  dctx->setAttribute(CTX_ATTR_BT, make_unique<TorrentAttribute>());
  auto peer = std::make_shared<Peer>("192.168.0.1", 6969);
  peer->allocateSessionResource(1_k, 1_m);

  DefaultBtInteractive interactive(dctx, peer);
  interactive.enableMetadataGetMode();

  auto dispatcher = make_unique<MockBtMessageDispatcher>();
  auto dispatcherPtr = dispatcher.get();
  auto factory = make_unique<DefaultBtMessageFactory>();
  factory->setDownloadContext(dctx.get());
  factory->setPeer(peer);
  factory->setBtMessageDispatcher(dispatcherPtr);
  interactive.setDispatcher(std::move(dispatcher));
  interactive.setBtMessageFactory(std::move(factory));
  interactive.setExtensionMessageRegistry(
      make_unique<ExtensionMessageRegistry>());

  interactive.updateAdvertisedPort(62000);
  CPPUNIT_ASSERT(dispatcherPtr->messageQueue.empty());

  peer->setExtendedMessagingEnabled(true);
  interactive.doPostHandshakeProcessing();
  CPPUNIT_ASSERT_EQUAL((size_t)1, dispatcherPtr->messageQueue.size());

  auto extended = dynamic_cast<BtExtendedMessage*>(
      dispatcherPtr->messageQueue.front().get());
  CPPUNIT_ASSERT(extended);
  auto handshake = dynamic_cast<HandshakeExtensionMessage*>(
      extended->getExtensionMessage().get());
  CPPUNIT_ASSERT(handshake);
  CPPUNIT_ASSERT_EQUAL((uint16_t)62000, handshake->getTCPPort());
}

void DefaultBtMessageFactoryTest::testAdvertisedPortUpdatesAreCoalesced()
{
  auto dctx = std::make_shared<DownloadContext>();
  dctx->setAttribute(CTX_ATTR_BT, make_unique<TorrentAttribute>());
  auto peer = std::make_shared<Peer>("192.168.0.1", 6969);
  peer->allocateSessionResource(1_k, 1_m);
  peer->setExtendedMessagingEnabled(true);

  DefaultBtInteractive interactive(dctx, peer);
  auto dispatcher = make_unique<MockBtMessageDispatcher>();
  auto dispatcherPtr = dispatcher.get();
  auto factory = make_unique<DefaultBtMessageFactory>();
  factory->setDownloadContext(dctx.get());
  factory->setPeer(peer);
  factory->setBtMessageDispatcher(dispatcherPtr);
  interactive.setDispatcher(std::move(dispatcher));
  interactive.setBtMessageFactory(std::move(factory));
  interactive.setExtensionMessageRegistry(
      make_unique<ExtensionMessageRegistry>());

  interactive.updateAdvertisedPort(62000);
  CPPUNIT_ASSERT_EQUAL((size_t)1, dispatcherPtr->messageQueue.size());

  interactive.updateAdvertisedPort(62001);
  interactive.updateAdvertisedPort(62000);
  CPPUNIT_ASSERT_EQUAL((size_t)1, dispatcherPtr->messageQueue.size());

  interactive.updateAdvertisedPort(62001);
  interactive.updateAdvertisedPort(62002);
  CPPUNIT_ASSERT_EQUAL((size_t)1, dispatcherPtr->messageQueue.size());

  global::wallclock().advance(5_s);
  interactive.flushAdvertisedPortUpdate();
  global::wallclock().sub(5_s);
  CPPUNIT_ASSERT_EQUAL((size_t)2, dispatcherPtr->messageQueue.size());

  auto extended = dynamic_cast<BtExtendedMessage*>(
      dispatcherPtr->messageQueue.back().get());
  CPPUNIT_ASSERT(extended);
  auto handshake = dynamic_cast<HandshakeExtensionMessage*>(
      extended->getExtensionMessage().get());
  CPPUNIT_ASSERT(handshake);
  CPPUNIT_ASSERT_EQUAL((uint16_t)62002, handshake->getTCPPort());
}

} // namespace aria2
