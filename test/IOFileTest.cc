#include "IOFile.h"

#include <array>
#include <string>

#include <cppunit/extensions/HelperMacros.h>

#include "BufferedFile.h"
#if HAVE_ZLIB
#  include "GZipFile.h"
#endif

namespace aria2 {

namespace {
template <typename FileType> void checkLineReading(const std::string& path)
{
  const std::array<size_t, 4> lengths{{4094, 4095, 4096, 8192}};
  std::string content = "\n";
  for (auto length : lengths) {
    content += std::string(length, 'x') + "\n";
  }
  content += "last";
  {
    FileType out(path.c_str(), IOFile::WRITE);
    CPPUNIT_ASSERT(out);
    CPPUNIT_ASSERT_EQUAL(content.size(),
                         out.write(content.data(), content.size()));
  }
  FileType in(path.c_str(), IOFile::READ);
  CPPUNIT_ASSERT(in);
  CPPUNIT_ASSERT(in.getLine().empty());
  for (auto length : lengths) {
    CPPUNIT_ASSERT_EQUAL(std::string(length, 'x'), in.getLine());
  }
  CPPUNIT_ASSERT_EQUAL(std::string("last"), in.getLine());
  CPPUNIT_ASSERT(in.getLine().empty());
  CPPUNIT_ASSERT(in.eof());
  in.close();
  CPPUNIT_ASSERT(in.getLine().empty());
}

template <typename FileType> void checkNullLine(const std::string& path)
{
  // fgets/gzgets can succeed with a leading NUL, making strlen return zero.
  const std::string content("\0\nnext\n\0", 8);
  {
    FileType out(path.c_str(), IOFile::WRITE);
    CPPUNIT_ASSERT(out);
    CPPUNIT_ASSERT_EQUAL(content.size(),
                         out.write(content.data(), content.size()));
  }
  FileType in(path.c_str(), IOFile::READ);
  CPPUNIT_ASSERT(in);
  CPPUNIT_ASSERT_EQUAL(std::string("next"), in.getLine());
  CPPUNIT_ASSERT(in.getLine().empty());
  CPPUNIT_ASSERT(in.eof());
}

template <typename FileType> void checkGetsn(const std::string& path)
{
  const std::string content("\0\n\ntext\nlast", 12);
  {
    FileType out(path.c_str(), IOFile::WRITE);
    CPPUNIT_ASSERT(out);
    CPPUNIT_ASSERT_EQUAL(content.size(),
                         out.write(content.data(), content.size()));
  }
  FileType in(path.c_str(), IOFile::READ);
  CPPUNIT_ASSERT(in);
  std::array<char, 32> buf;
  CPPUNIT_ASSERT(in.getsn(buf.data(), buf.size()));
  CPPUNIT_ASSERT_EQUAL('\0', buf[0]);
  CPPUNIT_ASSERT(in.getsn(buf.data(), buf.size()));
  CPPUNIT_ASSERT_EQUAL(std::string(), std::string(buf.data()));
  // fgets accepts a one-byte destination; gzgets may reject it. Neither
  // backend should consume the next line or access outside the buffer.
  auto result = in.getsn(buf.data(), 1);
  if (result) {
    CPPUNIT_ASSERT_EQUAL('\0', buf[0]);
  }
  CPPUNIT_ASSERT(in.getsn(buf.data(), buf.size()));
  CPPUNIT_ASSERT_EQUAL(std::string("text"), std::string(buf.data()));
  CPPUNIT_ASSERT(in.getsn(buf.data(), buf.size()));
  CPPUNIT_ASSERT_EQUAL(std::string("last"), std::string(buf.data()));
  CPPUNIT_ASSERT(!in.getsn(buf.data(), buf.size()));
}

template <typename FileType> void checkEmptyFile(const std::string& path)
{
  {
    FileType out(path.c_str(), IOFile::WRITE);
    CPPUNIT_ASSERT(out);
  }
  FileType in(path.c_str(), IOFile::READ);
  CPPUNIT_ASSERT(in);
  CPPUNIT_ASSERT(in.getLine().empty());
  CPPUNIT_ASSERT(in.eof());
}
} // namespace

class IOFileTest : public CppUnit::TestFixture {
  CPPUNIT_TEST_SUITE(IOFileTest);
  CPPUNIT_TEST(testBufferedLineReading);
  CPPUNIT_TEST(testBufferedNullLine);
  CPPUNIT_TEST(testBufferedGetsn);
  CPPUNIT_TEST(testBufferedEmptyFile);
#if HAVE_ZLIB
  CPPUNIT_TEST(testGZipLineReading);
  CPPUNIT_TEST(testGZipNullLine);
  CPPUNIT_TEST(testGZipGetsn);
  CPPUNIT_TEST(testGZipEmptyFile);
#endif
  CPPUNIT_TEST_SUITE_END();

public:
  void testBufferedLineReading()
  {
    checkLineReading<BufferedFile>(A2_TEST_OUT_DIR "/io-lines.txt");
  }
  void testBufferedNullLine()
  {
    checkNullLine<BufferedFile>(A2_TEST_OUT_DIR "/io-null.txt");
  }
  void testBufferedGetsn()
  {
    checkGetsn<BufferedFile>(A2_TEST_OUT_DIR "/io-getsn.txt");
  }
  void testBufferedEmptyFile()
  {
    checkEmptyFile<BufferedFile>(A2_TEST_OUT_DIR "/io-empty.txt");
  }
#if HAVE_ZLIB
  void testGZipLineReading()
  {
    checkLineReading<GZipFile>(A2_TEST_OUT_DIR "/io-lines.gz");
  }
  void testGZipNullLine()
  {
    checkNullLine<GZipFile>(A2_TEST_OUT_DIR "/io-null.gz");
  }
  void testGZipGetsn() { checkGetsn<GZipFile>(A2_TEST_OUT_DIR "/io-getsn.gz"); }
  void testGZipEmptyFile()
  {
    checkEmptyFile<GZipFile>(A2_TEST_OUT_DIR "/io-empty.gz");
  }
#endif
};

CPPUNIT_TEST_SUITE_REGISTRATION(IOFileTest);

} // namespace aria2
