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
#ifndef D_LEGACY_CHECKPOINT_CODEC_H
#define D_LEGACY_CHECKPOINT_CODEC_H
#include "common.h"
#include <cstdint>
#include <string>
#include <vector>
namespace aria2 {
struct LegacyCheckpointPiece {
  uint32_t index;
  uint32_t length;
  std::string bitfield;
};
struct LegacyCheckpointRange {
  int64_t offset;
  int64_t length;
};
struct LegacyCheckpoint {
  bool torrent = false;
  std::string infoHash;
  uint32_t pieceLength = 0;
  int64_t totalLength = 0;
  int64_t uploadLength = 0;
  std::string bitfield;
  std::vector<LegacyCheckpointPiece> pieces;
  std::string inFlightBlob;
  std::string controlDigest;
  int64_t completedLength = 0;
  std::vector<LegacyCheckpointRange> ranges;
};
constexpr size_t LEGACY_CHECKPOINT_MAX_BYTES = 64 * 1024 * 1024;
// The serializer is shared with native .aria2 saving. The strict, bounded
// decoder is reserved for untrusted migration snapshots, and refuses v0.
std::string encodeLegacyCheckpointV1(const LegacyCheckpoint& checkpoint);
LegacyCheckpoint decodeLegacyCheckpointV1(const std::string& bytes);
std::string legacyCheckpointProgressDigest(const LegacyCheckpoint& checkpoint);
} // namespace aria2
#endif
