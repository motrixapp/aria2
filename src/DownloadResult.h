/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2006 Tatsuhiro Tsujikawa
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
#ifndef D_DOWNLOAD_RESULT_H
#define D_DOWNLOAD_RESULT_H

#include "common.h"

#include <stdint.h>

#include <string>
#include <vector>
#include <memory>

#include "error_code.h"
#include "RequestGroup.h"
#include "ContextAttribute.h"

namespace aria2 {

class Option;
class FileEntry;
class MetadataInfo;

struct DownloadResult {
  // This field contains GID. See comment in
  // RequestGroup.cc::belongsToGID_.
  a2_gid_t belongsTo;

  uint64_t sessionDownloadLength;

  std::chrono::milliseconds sessionTime;

  int64_t totalLength;

  int64_t completedLength;

  int64_t uploadLength;

  std::shared_ptr<GroupId> gid;

  std::shared_ptr<Option> option;

  std::shared_ptr<MetadataInfo> metadataInfo;

  std::vector<std::shared_ptr<ContextAttribute>> attrs;

  std::vector<std::shared_ptr<FileEntry>> fileEntries;

  // This field contains GIDs. See comment in
  // RequestGroup.cc::followedByGIDs_.
  std::vector<a2_gid_t> followedBy;

  // The reverse link for followedBy.
  a2_gid_t following;

  std::string bitfield;

  std::string infoHash;

  std::string dir;

  // Serialized session text (URI list + option lines) produced by
  // SessionSerializer::renderResult at insert time.  Used by
  // requeueDownloadResult (priority 4) to recreate the RequestGroup.
  std::string serialized;

  // URI of the metadata source (magnet, .torrent URL, .metalink URL).
  // Stored in download_history.metadata_uri.
  std::string metadataUri;

  // BitTorrent display name (bt_name column).
  std::string btName;

  // BitTorrent announce list serialized as newline-separated tiers
  // where trackers within a tier are separated by ';'.
  // Stored in download_history.bt_announce_list.
  std::string btAnnounceList;

  // Path to a locally saved .torrent or .metalink file.
  // Stored in download_history.bt_local_path.
  std::string btLocalPath;

  size_t numPieces;

  int32_t pieceLength;

  error_code::Value result;

  std::string resultMessage;

  bool inMemoryDownload;

  DownloadResult();
  ~DownloadResult();

  // Don't allow copying
  DownloadResult(const DownloadResult& c) = delete;
  DownloadResult& operator=(const DownloadResult& c) = delete;
};

} // namespace aria2

#endif // D_DOWNLOAD_RESULT_H
