#include "AbstractCommand.h"
#include "BackupIPv4ConnectCommand.h"
#include "ConnectCommand.h"
#include "DownloadContext.h"
#include "DownloadEngine.h"
#include "OptionParser.h"
#include "Request.h"
#include "RequestGroup.h"
#include "RequestGroupMan.h"
#include "SelectEventPoll.h"
#include "TestUtil.h"

#include <iostream>
#include <cppunit/extensions/HelperMacros.h>
#ifndef __MINGW32__
#  include <unistd.h>
#endif

#include "Option.h"
#include "prefs.h"
#include "SocketCore.h"
#include "SocketRecvBuffer.h"
#include "wallclock.h"

namespace aria2 {

class AbstractCommandTest : public CppUnit::TestFixture {

  CPPUNIT_TEST_SUITE(AbstractCommandTest);
  CPPUNIT_TEST(testGetProxyUri);
  CPPUNIT_TEST(testConnectExceptionTriesRemainingAddress);
  CPPUNIT_TEST(testConnectExceptionExhaustsAddresses);
  CPPUNIT_TEST(testSilentBackupExpires);
  CPPUNIT_TEST(testBackupConnectException);
  CPPUNIT_TEST_SUITE_END();

public:
  void setUp() {}

  void tearDown() {}

  void testGetProxyUri();
  void testConnectExceptionTriesRemainingAddress();
  void testConnectExceptionExhaustsAddresses();
  void testSilentBackupExpires();
  void testBackupConnectException();
};

CPPUNIT_TEST_SUITE_REGISTRATION(AbstractCommandTest);

namespace {
// Reset a connected socket without releasing the listening port or racing
// another process for its ownership. Do not read SO_ERROR: the command under
// test must consume the real socket error itself.
void resetPeer(SocketCore& listener, sock_t client)
{
  CPPUNIT_ASSERT(listener.isReadable(5));
  const auto peer = accept(listener.getSockfd(), nullptr, nullptr);
  CPPUNIT_ASSERT(peer != static_cast<sock_t>(-1));
  linger abortive{1, 0};
  const int rv = setsockopt(peer, SOL_SOCKET, SO_LINGER,
                            reinterpret_cast<const char*>(&abortive),
                            sizeof(abortive));
#ifdef __MINGW32__
  closesocket(peer);
#else
  close(peer);
#endif
  CPPUNIT_ASSERT_EQUAL(0, rv);
  fd_set reads;
  FD_ZERO(&reads);
  FD_SET(client, &reads);
  timeval wait{5, 0};
  CPPUNIT_ASSERT(select(client + 1, &reads, nullptr, nullptr, &wait) > 0);
}

class UnexpectedChain : public ControlChain<ConnectCommand*> {
public:
  int run(ConnectCommand*, DownloadEngine*) override
  {
    CPPUNIT_FAIL("failed connection advanced to the protocol handshake");
    return 0;
  }
};

void checkConnectException(bool hasAlternate)
{
  auto option = std::make_shared<Option>();
  OptionParser::getInstance()->parseDefaultValues(*option);
  option->put(PREF_MAX_TRIES, "1");
  const std::string uri = "http://connect.test/file";
  auto group = createRequestGroup(1024, 0, "file", uri, option);
  DownloadEngine engine(make_unique<SelectEventPoll>());
  engine.setOption(option.get());
  engine.setRequestGroupMan(make_unique<RequestGroupMan>(
      std::vector<std::shared_ptr<RequestGroup>>{group}, 1, option.get()));

  // Model Windows' exceptfds-only notification on any test host, independently
  // of its IPv6 routes or the readiness flags its own select() would report.
  SocketCore listener;
  listener.bind("127.0.0.1", 0, AF_INET);
  listener.beginListen();
  const auto port = listener.getAddrInfo().port;
  auto socket = std::make_shared<SocketCore>();
  socket->establishConnection("127.0.0.1", port);
  resetPeer(listener, socket->getSockfd());

  auto req = std::make_shared<Request>();
  CPPUNIT_ASSERT(req->setUri(uri));
  req->setConnectedAddrInfo("connect.test", "::1", port);
  engine.cacheIPAddress("connect.test", "::1", port);
  if (hasAlternate) {
    engine.cacheIPAddress("connect.test", "127.0.0.1", port);
  }
  auto command = make_unique<ConnectCommand>(
      1, req, nullptr, group->getDownloadContext()->getFirstFileEntry(),
      group.get(), &engine, socket);
  command->setControlChain(std::make_shared<UnexpectedChain>());
  command->errorEventReceived();
  const bool done = command->execute();
  if (!done) {
    command.release(); // requeued by execute()
  }
  CPPUNIT_ASSERT(done);
  CPPUNIT_ASSERT_EQUAL(hasAlternate ? std::string("127.0.0.1") : std::string(),
                       engine.findCachedIPAddress("connect.test", port));
  if (hasAlternate) {
    CPPUNIT_ASSERT_EQUAL(0, req->getTryCount());
  }
  else {
    CPPUNIT_ASSERT_EQUAL(1, req->getTryCount());
    CPPUNIT_ASSERT_EQUAL(error_code::NETWORK_PROBLEM, group->getLastErrorCode());
  }
}
} // namespace

void AbstractCommandTest::testConnectExceptionTriesRemainingAddress()
{
  checkConnectException(true);
}

void AbstractCommandTest::testConnectExceptionExhaustsAddresses()
{
  checkConnectException(false);
}

namespace {
void checkBackup(bool failed)
{
  class BackupPoll : public SelectEventPoll {
  public:
    explicit BackupPoll(SocketCore* resetter) : resetter_(resetter) {}
    bool addEvents(sock_t fd, Command* cmd, EventPoll::EventType events) override
    {
      fd_ = fd;
      command_ = cmd;
      return SelectEventPoll::addEvents(fd, cmd, events);
    }
    void poll(const timeval&) override
    {
      if (resetter_) {
        CPPUNIT_ASSERT(command_);
        resetPeer(*resetter_, fd_);
        command_->errorEventReceived();
        command_->setStatusActive();
      }
    }
  private:
    SocketCore* resetter_;
    sock_t fd_ = -1;
    Command* command_ = nullptr;
  };
  auto option = std::make_shared<Option>();
  OptionParser::getInstance()->parseDefaultValues(*option);
  option->put(PREF_CONNECT_TIMEOUT, failed ? "60" : "1");
  auto group = createRequestGroup(1024, 0, "file", "http://connect.test/file",
                                  option);
  SocketCore peer;
  peer.bind("127.0.0.1", 0, AF_INET);
  peer.beginListen();
  const auto port = peer.getAddrInfo().port;
  DownloadEngine engine(make_unique<BackupPoll>(failed ? &peer : nullptr));
  engine.setOption(option.get());
  engine.setRequestGroupMan(make_unique<RequestGroupMan>(
      std::vector<std::shared_ptr<RequestGroup>>{group}, 1, option.get()));
  class Main : public Command {
  public:
    Main() : Command(2) {}
    bool execute() override { return true; }
  } main;
  auto info = std::make_shared<BackupConnectInfo>();

  // Start the connection in the past, past its initial delay. run(true)
  // refreshes the loop clock. The silent socket's deadline is due; the
  // refused socket's deadline is still far away and only exceptfds is set.
  global::wallclock().reset();
  global::wallclock().sub(std::chrono::seconds(2));
  auto backup = make_unique<BackupIPv4ConnectCommand>(
      1, "127.0.0.1", port, info, &main, group.get(), &engine);
  global::wallclock().advance(std::chrono::milliseconds(400));
  const bool done = backup->execute();
  if (!done) {
    backup.release(); // requeued by execute()
  }
  global::wallclock().reset();
  CPPUNIT_ASSERT(!done);
  engine.run(true);
  CPPUNIT_ASSERT_EQUAL(0, group->getNumCommand());
  CPPUNIT_ASSERT(!info->socket);
}
} // namespace

void AbstractCommandTest::testSilentBackupExpires() { checkBackup(false); }

void AbstractCommandTest::testBackupConnectException() { checkBackup(true); }

void AbstractCommandTest::testGetProxyUri()
{
  Option op;
  CPPUNIT_ASSERT_EQUAL(std::string(), getProxyUri("http", &op));

  op.put(PREF_HTTP_PROXY, "http://hu:hp@httpproxy/");
  op.put(PREF_FTP_PROXY, "ftp://fu:fp@ftpproxy/");
  CPPUNIT_ASSERT_EQUAL(std::string("http://hu:hp@httpproxy/"),
                       getProxyUri("http", &op));
  CPPUNIT_ASSERT_EQUAL(std::string("ftp://fu:fp@ftpproxy/"),
                       getProxyUri("ftp", &op));

  op.put(PREF_ALL_PROXY, "http://au:ap@allproxy/");
  CPPUNIT_ASSERT_EQUAL(std::string("http://au:ap@allproxy/"),
                       getProxyUri("https", &op));

  op.put(PREF_ALL_PROXY_USER, "aunew");
  op.put(PREF_ALL_PROXY_PASSWD, "apnew");
  CPPUNIT_ASSERT_EQUAL(std::string("http://aunew:apnew@allproxy/"),
                       getProxyUri("https", &op));

  op.put(PREF_HTTPS_PROXY, "http://hsu:hsp@httpsproxy/");
  CPPUNIT_ASSERT_EQUAL(std::string("http://hsu:hsp@httpsproxy/"),
                       getProxyUri("https", &op));

  CPPUNIT_ASSERT_EQUAL(std::string(), getProxyUri("unknown", &op));

  op.put(PREF_HTTP_PROXY_USER, "hunew");
  CPPUNIT_ASSERT_EQUAL(std::string("http://hunew:hp@httpproxy/"),
                       getProxyUri("http", &op));

  op.put(PREF_HTTP_PROXY_PASSWD, "hpnew");
  CPPUNIT_ASSERT_EQUAL(std::string("http://hunew:hpnew@httpproxy/"),
                       getProxyUri("http", &op));

  op.put(PREF_HTTP_PROXY_USER, "");
  CPPUNIT_ASSERT_EQUAL(std::string("http://httpproxy/"),
                       getProxyUri("http", &op));
}

} // namespace aria2
