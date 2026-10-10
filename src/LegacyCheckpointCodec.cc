/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2026 Tatsuhiro Tsujikawa
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
#include "LegacyCheckpointCodec.h"
#include <algorithm>
#include <climits>
#include <set>
#include "DlAbortEx.h"
#include "MessageDigest.h"
#include "Piece.h"
#include "util.h"
namespace aria2 {
namespace {
constexpr uint64_t MAX_PIECES = 16777216;
constexpr size_t MAX_RANGES = 100000;
void put(std::string& out, uint64_t n, size_t size)
{
  for (size_t i = size; i; --i)
    out.push_back(static_cast<char>(n >> ((i - 1) * 8)));
}
struct Reader {
  const std::string& bytes;
  size_t pos = 0;
  uint64_t number(size_t n)
  {
    if (n > bytes.size() - pos)
      throw DL_ABORT_EX("Legacy checkpoint truncated");
    uint64_t value = 0;
    while (n--)
      value = (value << 8) | static_cast<unsigned char>(bytes[pos++]);
    return value;
  }
  std::string data(size_t n)
  {
    if (n > bytes.size() - pos)
      throw DL_ABORT_EX("Legacy checkpoint truncated");
    auto out = bytes.substr(pos, n);
    pos += n;
    return out;
  }
};
bool bit(const std::string& b, uint64_t i)
{
  return static_cast<unsigned char>(b[i / 8]) & (0x80 >> (i % 8));
}
void checkBits(const std::string& b, uint64_t count)
{
  if (b.size() != (count / 8 + (count % 8 != 0)))
    throw DL_ABORT_EX("Legacy checkpoint bitfield length mismatch");
  if (count % 8 &&
      (static_cast<unsigned char>(b.back()) & ((1 << (8 - count % 8)) - 1)))
    throw DL_ABORT_EX("Legacy checkpoint nonzero padding bits");
}
void range(LegacyCheckpoint& c, int64_t offset, int64_t length)
{
  if (!length)
    return;
  if (!c.ranges.empty() &&
      c.ranges.back().offset + c.ranges.back().length == offset)
    c.ranges.back().length += length;
  else {
    if (c.ranges.size() >= MAX_RANGES)
      throw DL_ABORT_EX("Legacy checkpoint too many ranges");
    c.ranges.push_back({offset, length});
  }
  c.completedLength += length;
}
std::string hash(const char* type, const std::string& bytes)
{
  auto md = MessageDigest::create(type);
  md->update(bytes.data(), bytes.size());
  return md->digest();
}
} // namespace
std::string encodeLegacyCheckpointV1(const LegacyCheckpoint& c)
{
  std::string out;
  put(out, 1, 2);
  put(out, c.torrent ? 1 : 0, 4);
  put(out, c.infoHash.size(), 4);
  out += c.infoHash;
  put(out, c.pieceLength, 4);
  put(out, c.totalLength, 8);
  put(out, c.uploadLength, 8);
  put(out, c.bitfield.size(), 4);
  out += c.bitfield;
  put(out, c.pieces.size(), 4);
  for (const auto& p : c.pieces) {
    put(out, p.index, 4);
    put(out, p.length, 4);
    put(out, p.bitfield.size(), 4);
    out += p.bitfield;
  }
  return out;
}
LegacyCheckpoint decodeLegacyCheckpointV1(const std::string& bytes)
{
  if (bytes.size() > LEGACY_CHECKPOINT_MAX_BYTES)
    throw DL_ABORT_EX("Legacy checkpoint too large");
  Reader r{bytes};
  LegacyCheckpoint c;
  if (r.number(2) != 1)
    throw DL_ABORT_EX("Legacy checkpoint requires network-order version 1");
  auto extension = r.number(4);
  if (extension > 1)
    throw DL_ABORT_EX("Legacy checkpoint unknown extension");
  c.torrent = extension == 1;
  auto hashLength = r.number(4);
  if (hashLength != (c.torrent ? 20u : 0u))
    throw DL_ABORT_EX("Legacy checkpoint invalid info hash");
  c.infoHash = r.data(hashLength);
  c.pieceLength = r.number(4);
  auto total = r.number(8), upload = r.number(8);
  if (!c.pieceLength || c.pieceLength > INT32_MAX || total > INT64_MAX ||
      upload > INT64_MAX)
    throw DL_ABORT_EX("Legacy checkpoint numeric range invalid");
  if (!c.torrent && upload)
    throw DL_ABORT_EX("Legacy HTTP checkpoint upload length invalid");
  c.totalLength = total;
  c.uploadLength = upload;
  auto count = total / c.pieceLength + (total % c.pieceLength != 0);
  if (count > MAX_PIECES)
    throw DL_ABORT_EX("Legacy checkpoint too many pieces");
  auto bfLen = r.number(4);
  if (bfLen != count / 8 + (count % 8 != 0))
    throw DL_ABORT_EX("Legacy checkpoint bitfield length mismatch");
  c.bitfield = r.data(bfLen);
  checkBits(c.bitfield, count);
  auto trailer = r.pos;
  auto inFlight = r.number(4);
  if (inFlight > 100000 || inFlight > count ||
      inFlight > (bytes.size() - r.pos) / 13)
    throw DL_ABORT_EX("Legacy checkpoint invalid in-flight count");
  std::set<uint32_t> indices;
  for (uint64_t i = 0; i < inFlight; ++i) {
    uint32_t index = r.number(4), length = r.number(4), len = r.number(4);
    if (index >= count || !indices.insert(index).second ||
        bit(c.bitfield, index))
      throw DL_ABORT_EX("Legacy checkpoint invalid in-flight index");
    auto expectedLength = std::min<uint64_t>(
        c.pieceLength, total - uint64_t(index) * c.pieceLength);
    if (length != expectedLength)
      throw DL_ABORT_EX("Legacy checkpoint invalid in-flight piece length");
    auto blocks =
        length / Piece::BLOCK_LENGTH + (length % Piece::BLOCK_LENGTH != 0);
    if (len != blocks / 8 + (blocks % 8 != 0))
      throw DL_ABORT_EX("Legacy checkpoint invalid in-flight bitfield length");
    auto b = r.data(len);
    checkBits(b, blocks);
    c.pieces.push_back({index, length, std::move(b)});
  }
  if (r.pos != bytes.size())
    throw DL_ABORT_EX("Legacy checkpoint trailing bytes");
  c.inFlightBlob = bytes.substr(trailer);
  std::sort(c.pieces.begin(), c.pieces.end(),
            [](const LegacyCheckpointPiece& a, const LegacyCheckpointPiece& b) {
              return a.index < b.index;
            });
  size_t partial = 0;
  for (uint64_t i = 0; i < count; ++i) {
    auto offset = i * c.pieceLength;
    auto length = std::min<uint64_t>(c.pieceLength, total - offset);
    if (bit(c.bitfield, i))
      range(c, offset, length);
    else if (partial < c.pieces.size() && c.pieces[partial].index == i) {
      const auto& p = c.pieces[partial++];
      auto blocks =
          length / Piece::BLOCK_LENGTH + (length % Piece::BLOCK_LENGTH != 0);
      for (uint64_t j = 0; j < blocks; ++j)
        if (bit(p.bitfield, j)) {
          auto start = j * Piece::BLOCK_LENGTH;
          range(c, offset + start,
                std::min<uint64_t>(Piece::BLOCK_LENGTH, length - start));
        }
    }
  }
  c.controlDigest = util::toHex(hash("sha-256", bytes));
  return c;
}
std::string legacyCheckpointProgressDigest(const LegacyCheckpoint& c)
{
  std::string bytes;
  put(bytes, c.pieceLength, 4);
  put(bytes, c.totalLength, 8);
  put(bytes, c.uploadLength, 8);
  bytes += c.bitfield;
  bytes += c.inFlightBlob;
  return hash("sha-1", bytes);
}
} // namespace aria2
