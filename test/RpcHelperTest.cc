#include "rpc_helper.h"

#include <cppunit/extensions/HelperMacros.h>

#include "RpcRequest.h"
#include "RpcResponse.h"
#include "RecoverableException.h"
#include "DownloadEngine.h"
#include "SelectEventPoll.h"
#include "Option.h"
#include "prefs.h"
#include "ValueBase.h"
#include "a2functional.h"
#ifdef ENABLE_XML_RPC
#  include "XmlRpcRequestParserStateMachine.h"
#endif // ENABLE_XML_RPC

namespace aria2 {

namespace rpc {

class RpcHelperTest : public CppUnit::TestFixture {

  CPPUNIT_TEST_SUITE(RpcHelperTest);
#ifdef ENABLE_XML_RPC
  CPPUNIT_TEST(testParseMemory);
  CPPUNIT_TEST(testParseMemory_shouldFail);
  CPPUNIT_TEST(testParseMemory_withoutStringTag);
#endif // ENABLE_XML_RPC
  CPPUNIT_TEST(testCreateJsonRpcErrorResponseIsNotAuthorized);
  CPPUNIT_TEST(testProcessRequestInvalidRequestIsNotAuthorized);
  CPPUNIT_TEST(testProcessRequestNoTokenIsNotAuthorized);
  CPPUNIT_TEST(testProcessRequestValidTokenIsAuthorized);
  CPPUNIT_TEST(testAllAuthorizedEmptyBatchIsNotAuthorized);
  CPPUNIT_TEST(testAllAuthorizedRejectsMixedBatch);
  CPPUNIT_TEST(testAllAuthorizedAcceptsFullyAuthorizedBatch);
  CPPUNIT_TEST_SUITE_END();

private:
  std::shared_ptr<Option> option_;
  std::unique_ptr<DownloadEngine> e_;

public:
  void setUp()
  {
    option_ = std::make_shared<Option>();
    e_ = make_unique<DownloadEngine>(make_unique<SelectEventPoll>());
    e_->setOption(option_.get());
  }

  void tearDown() {}

#ifdef ENABLE_XML_RPC
  void testParseMemory();
  void testParseMemory_shouldFail();
  void testParseMemory_withoutParams();
  void testParseMemory_withoutStringTag();
#endif // ENABLE_XML_RPC
  void testCreateJsonRpcErrorResponseIsNotAuthorized();
  void testProcessRequestInvalidRequestIsNotAuthorized();
  void testProcessRequestNoTokenIsNotAuthorized();
  void testProcessRequestValidTokenIsAuthorized();
  void testAllAuthorizedEmptyBatchIsNotAuthorized();
  void testAllAuthorizedRejectsMixedBatch();
  void testAllAuthorizedAcceptsFullyAuthorizedBatch();
};

CPPUNIT_TEST_SUITE_REGISTRATION(RpcHelperTest);

namespace {
// Builds a JSON-RPC request dict the way the parser hands it to
// processJsonRpcRequest(): a Dict carrying "method", "id" and "params".
std::unique_ptr<Dict> createRequestDict(const std::string& method,
                                        std::unique_ptr<List> params)
{
  auto dict = Dict::g();
  dict->put("jsonrpc", "2.0");
  dict->put("id", "1");
  dict->put("method", method);
  dict->put("params", std::move(params));
  return dict;
}

RpcResponse authorizedResponse()
{
  return RpcResponse(0, RpcResponse::AUTHORIZED, Dict::g(), Null::g());
}
} // namespace

// A protocol-level error response is produced without ever validating a
// token, so it must never report itself as authorized. When it did, an
// unauthenticated WebSocket client could flip its session to authorized
// just by sending malformed input (upstream issue #1752).
void RpcHelperTest::testCreateJsonRpcErrorResponseIsNotAuthorized()
{
  auto res = createJsonRpcErrorResponse(-32700, "Parse error.", Null::g());
  CPPUNIT_ASSERT(not_authorized(res));
}

void RpcHelperTest::testProcessRequestInvalidRequestIsNotAuthorized()
{
  option_->put(PREF_RPC_SECRET, "secret");
  // A dict with no "method" is an invalid request; it never reaches token
  // validation and must stay unauthorized.
  auto dict = Dict::g();
  dict->put("jsonrpc", "2.0");
  dict->put("id", "1");
  auto res = processJsonRpcRequest(dict.get(), e_.get());
  CPPUNIT_ASSERT(not_authorized(res));
}

void RpcHelperTest::testProcessRequestNoTokenIsNotAuthorized()
{
  option_->put(PREF_RPC_SECRET, "secret");
  auto dict = createRequestDict("aria2.getVersion", List::g());
  auto res = processJsonRpcRequest(dict.get(), e_.get());
  CPPUNIT_ASSERT(not_authorized(res));
}

void RpcHelperTest::testProcessRequestValidTokenIsAuthorized()
{
  option_->put(PREF_RPC_SECRET, "secret");
  auto params = List::g();
  params->append("token:secret");
  auto dict = createRequestDict("aria2.getVersion", std::move(params));
  auto res = processJsonRpcRequest(dict.get(), e_.get());
  CPPUNIT_ASSERT(!not_authorized(res));
}

// An empty batch (a JSON array whose every entry was an invalid,
// non-dict value) yields no responses. any_not_authorized() returns
// false for an empty range, so the callers used to treat it as fully
// authorized. all_authorized() must reject the empty batch instead.
void RpcHelperTest::testAllAuthorizedEmptyBatchIsNotAuthorized()
{
  std::vector<RpcResponse> results;
  CPPUNIT_ASSERT(!all_authorized(results));
}

void RpcHelperTest::testAllAuthorizedRejectsMixedBatch()
{
  std::vector<RpcResponse> results;
  results.push_back(authorizedResponse());
  results.push_back(
      createJsonRpcErrorResponse(-32600, "Invalid Request.", Null::g()));
  CPPUNIT_ASSERT(!all_authorized(results));
}

void RpcHelperTest::testAllAuthorizedAcceptsFullyAuthorizedBatch()
{
  std::vector<RpcResponse> results;
  results.push_back(authorizedResponse());
  results.push_back(authorizedResponse());
  CPPUNIT_ASSERT(all_authorized(results));
}

#ifdef ENABLE_XML_RPC

void RpcHelperTest::testParseMemory()
{
  std::string s =
      "<?xml version=\"1.0\"?>"
      "<methodCall>"
      "  <methodName>aria2.addURI</methodName>"
      "    <params>"
      "      <param>"
      "        <value><i4>100</i4></value>"
      "      </param>"
      "      <param>"
      "       <value>"
      "         <struct>"
      "           <member>"
      "             <name>max-count</name>"
      "             <value><i4>65535</i4></value>"
      "           </member>"
      "           <member>"
      "             <name>seed-ratio</name>"
      "             <value><double>0.99</double></value>"
      "           </member>"
      "         </struct>"
      "       </value>"
      "     </param>"
      "     <param>"
      "       <value>"
      "         <array>"
      "           <data>"
      "             <value><string>pudding</string></value>"
      "             <value><base64>aGVsbG8gd29ybGQ=</base64></value>"
      "           </data>"
      "         </array>"
      "       </value>"
      "     </param>"
      "   </params>"
      "</methodCall>";
  RpcRequest req = xmlParseMemory(s.c_str(), s.size());

  CPPUNIT_ASSERT_EQUAL(std::string("aria2.addURI"), req.methodName);
  CPPUNIT_ASSERT_EQUAL((size_t)3, req.params->size());
  CPPUNIT_ASSERT_EQUAL((Integer::ValueType)100,
                       downcast<Integer>(req.params->get(0))->i());
  const Dict* dict = downcast<Dict>(req.params->get(1));
  CPPUNIT_ASSERT_EQUAL((Integer::ValueType)65535,
                       downcast<Integer>(dict->get("max-count"))->i());
  // Current implementation handles double as string.
  CPPUNIT_ASSERT_EQUAL(std::string("0.99"),
                       downcast<String>(dict->get("seed-ratio"))->s());
  const List* list = downcast<List>(req.params->get(2));
  CPPUNIT_ASSERT_EQUAL(std::string("pudding"),
                       downcast<String>(list->get(0))->s());
  CPPUNIT_ASSERT_EQUAL(std::string("hello world"),
                       downcast<String>(list->get(1))->s());
}

void RpcHelperTest::testParseMemory_shouldFail()
{
  try {
    std::string s = "<methodCall>"
                    "  <methodName>aria2.addURI</methodName>"
                    "    <params>"
                    "      <param>"
                    "        <value><i4>100</i4></value>"
                    "      </param>";
    xmlParseMemory(s.c_str(), s.size());
    CPPUNIT_FAIL("exception must be thrown.");
  }
  catch (RecoverableException& e) {
    // success
  }
}

void RpcHelperTest::testParseMemory_withoutParams()
{
  {
    std::string s = "<methodCall>"
                    "  <methodName>aria2.addURI</methodName>"
                    "    <params>"
                    "    </params>"
                    "</methodCall>";
    RpcRequest req = xmlParseMemory(s.c_str(), s.size());
    CPPUNIT_ASSERT(req.params);
  }
  {
    std::string s = "<methodCall>"
                    "  <methodName>aria2.addURI</methodName>"
                    "</methodCall>";
    RpcRequest req = xmlParseMemory(s.c_str(), s.size());
    CPPUNIT_ASSERT(req.params->size());
  }
}

void RpcHelperTest::testParseMemory_withoutStringTag()
{
  std::string s = "<?xml version=\"1.0\"?>"
                  "<methodCall>"
                  "  <methodName>aria2.addUri</methodName>"
                  "    <params>"
                  "      <param>"
                  "        <value>http://aria2.sourceforge.net</value>"
                  "      </param>"
                  "      <param>"
                  "        <value>http://aria2.<foo/>sourceforge.net</value>"
                  "      </param>"
                  "      <param>"
                  "       <value>"
                  "         <struct>"
                  "           <member>"
                  "             <name>hello</name>"
                  "             <value>world</value>"
                  "           </member>"
                  "         </struct>"
                  "       </value>"
                  "     </param>"
                  "     <param>"
                  "       <value>"
                  "         <array>"
                  "           <data>"
                  "             <value>apple</value>"
                  "             <value>banana</value>"
                  "             <value><string>lemon</string>peanuts</value>"
                  "           </data>"
                  "         </array>"
                  "       </value>"
                  "     </param>"
                  "   </params>"
                  "</methodCall>";
  RpcRequest req = xmlParseMemory(s.c_str(), s.size());

  CPPUNIT_ASSERT_EQUAL((size_t)4, req.params->size());
  CPPUNIT_ASSERT_EQUAL(std::string("http://aria2.sourceforge.net"),
                       downcast<String>(req.params->get(0))->s());
  CPPUNIT_ASSERT_EQUAL(std::string("http://aria2.sourceforge.net"),
                       downcast<String>(req.params->get(1))->s());
  const Dict* dict = downcast<Dict>(req.params->get(2));
  CPPUNIT_ASSERT_EQUAL(std::string("world"),
                       downcast<String>(dict->get("hello"))->s());
  const List* list = downcast<List>(req.params->get(3));
  CPPUNIT_ASSERT_EQUAL(std::string("apple"),
                       downcast<String>(list->get(0))->s());
  CPPUNIT_ASSERT_EQUAL(std::string("banana"),
                       downcast<String>(list->get(1))->s());
  CPPUNIT_ASSERT_EQUAL(std::string("lemon"),
                       downcast<String>(list->get(2))->s());
}

#endif // ENABLE_XML_RPC

} // namespace rpc

} // namespace aria2
