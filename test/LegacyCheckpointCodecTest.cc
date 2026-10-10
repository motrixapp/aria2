#include "LegacyCheckpointCodec.h"
#include "DlAbortEx.h"
#include <cppunit/extensions/HelperMacros.h>
namespace aria2 {
class LegacyCheckpointCodecTest : public CppUnit::TestFixture {
  CPPUNIT_TEST_SUITE(LegacyCheckpointCodecTest);
  CPPUNIT_TEST(testNativeRoundTrip);
  CPPUNIT_TEST(testPartialBlocks);
  CPPUNIT_TEST(testRejectMalformed);
  CPPUNIT_TEST_SUITE_END();

public:
  LegacyCheckpoint fixture()
  {
    LegacyCheckpoint c;
    c.pieceLength = 32768;
    c.totalLength = 98305;
    c.bitfield = std::string(1, static_cast<char>(0x80));
    c.pieces.push_back({1, 32768, std::string(1, static_cast<char>(0x80))});
    return c;
  }
  void testNativeRoundTrip()
  {
    auto input = fixture();
    input.torrent = true;
    input.infoHash = std::string(20, 'h');
    input.uploadLength = 1234;
    auto bytes = encodeLegacyCheckpointV1(input);
    auto c = decodeLegacyCheckpointV1(bytes);
    CPPUNIT_ASSERT_EQUAL(bytes, encodeLegacyCheckpointV1(c));
    CPPUNIT_ASSERT_EQUAL(input.infoHash, c.infoHash);
    CPPUNIT_ASSERT_EQUAL(int64_t(1234), c.uploadLength);
    CPPUNIT_ASSERT_EQUAL(size_t(64), c.controlDigest.size());
    CPPUNIT_ASSERT_EQUAL(size_t(20), legacyCheckpointProgressDigest(c).size());
  }
  void testPartialBlocks()
  {
    auto c = decodeLegacyCheckpointV1(encodeLegacyCheckpointV1(fixture()));
    CPPUNIT_ASSERT_EQUAL(int64_t(49152), c.completedLength);
    CPPUNIT_ASSERT_EQUAL(size_t(1), c.ranges.size());
    CPPUNIT_ASSERT_EQUAL(int64_t(0), c.ranges[0].offset);
    CPPUNIT_ASSERT_EQUAL(int64_t(49152), c.ranges[0].length);
    auto input = fixture();
    input.bitfield = std::string(1, static_cast<char>(0x50));
    input.pieces.clear();
    c = decodeLegacyCheckpointV1(encodeLegacyCheckpointV1(input));
    CPPUNIT_ASSERT_EQUAL(int64_t(32769), c.completedLength);
    CPPUNIT_ASSERT_EQUAL(size_t(2), c.ranges.size());
    CPPUNIT_ASSERT_EQUAL(int64_t(98304), c.ranges[1].offset);
    CPPUNIT_ASSERT_EQUAL(int64_t(1), c.ranges[1].length);
  }
  void testRejectMalformed()
  {
    const auto good = encodeLegacyCheckpointV1(fixture());
    for (size_t n = 0; n < good.size(); ++n)
      CPPUNIT_ASSERT_THROW(decodeLegacyCheckpointV1(good.substr(0, n)),
                           DlAbortEx);
    CPPUNIT_ASSERT_THROW(decodeLegacyCheckpointV1(good + "x"), DlAbortEx);
    auto bytes = good;
    bytes[1] = 0;
    CPPUNIT_ASSERT_THROW(decodeLegacyCheckpointV1(bytes), DlAbortEx);
    bytes = good;
    bytes[5] = 2;
    CPPUNIT_ASSERT_THROW(decodeLegacyCheckpointV1(bytes), DlAbortEx);
    bytes = good;
    bytes[9] = 20;
    CPPUNIT_ASSERT_THROW(decodeLegacyCheckpointV1(bytes), DlAbortEx);
    bytes = good;
    bytes[14] = static_cast<char>(0x80);
    CPPUNIT_ASSERT_THROW(decodeLegacyCheckpointV1(bytes), DlAbortEx);
    auto input = fixture();
    input.bitfield[0] |= 1;
    CPPUNIT_ASSERT_THROW(
        decodeLegacyCheckpointV1(encodeLegacyCheckpointV1(input)), DlAbortEx);
    input = fixture();
    input.pieces.push_back(input.pieces[0]);
    CPPUNIT_ASSERT_THROW(
        decodeLegacyCheckpointV1(encodeLegacyCheckpointV1(input)), DlAbortEx);
    input = fixture();
    input.pieces[0].index = 0;
    CPPUNIT_ASSERT_THROW(
        decodeLegacyCheckpointV1(encodeLegacyCheckpointV1(input)), DlAbortEx);
    input = fixture();
    input.pieces[0].length = 1;
    CPPUNIT_ASSERT_THROW(
        decodeLegacyCheckpointV1(encodeLegacyCheckpointV1(input)), DlAbortEx);
    input = fixture();
    input.pieces[0].bitfield[0] |= 1;
    CPPUNIT_ASSERT_THROW(
        decodeLegacyCheckpointV1(encodeLegacyCheckpointV1(input)), DlAbortEx);
    input = fixture();
    input.totalLength = INT64_MAX;
    CPPUNIT_ASSERT_THROW(
        decodeLegacyCheckpointV1(encodeLegacyCheckpointV1(input)), DlAbortEx);
  }
};
CPPUNIT_TEST_SUITE_REGISTRATION(LegacyCheckpointCodecTest);
} // namespace aria2
