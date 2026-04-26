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
#ifndef D_SQLITE3_BT_PROGRESS_INFO_FILE_H
#define D_SQLITE3_BT_PROGRESS_INFO_FILE_H

#include "common.h"

#ifdef HAVE_SQLITE3

#include "BtProgressInfoFile.h"

#include <memory>
#include <string>

namespace aria2 {

class DownloadContext;
class PieceStorage;
class Option;
#ifdef ENABLE_BITTORRENT
class BtRuntime;
class PeerStorage;
#endif
class Sqlite3PersistenceStore;

class Sqlite3BtProgressInfoFile : public BtProgressInfoFile {
public:
  Sqlite3BtProgressInfoFile(const std::shared_ptr<DownloadContext>& dctx,
                             const std::shared_ptr<PieceStorage>& pieceStorage,
                             const Option* option,
                             Sqlite3PersistenceStore* store);
  ~Sqlite3BtProgressInfoFile() CXX11_OVERRIDE;

  std::string getFilename() CXX11_OVERRIDE { return filename_; }
  bool exists() CXX11_OVERRIDE;
  void save() CXX11_OVERRIDE;
  void load() CXX11_OVERRIDE;
  void removeFile() CXX11_OVERRIDE;
  void updateFilename() CXX11_OVERRIDE;

#ifdef ENABLE_BITTORRENT
  void setBtRuntime(
      const std::shared_ptr<BtRuntime>& btRuntime) CXX11_OVERRIDE;
  void setPeerStorage(
      const std::shared_ptr<PeerStorage>& peerStorage) CXX11_OVERRIDE;
#endif

private:
  std::shared_ptr<DownloadContext> dctx_;
  std::shared_ptr<PieceStorage> pieceStorage_;
#ifdef ENABLE_BITTORRENT
  std::shared_ptr<PeerStorage> peerStorage_;
  std::shared_ptr<BtRuntime> btRuntime_;
#endif
  const Option* option_;
  Sqlite3PersistenceStore* store_; // non-owning
  std::string gidHex_;
  std::string filename_;
  std::string lastDigest_;
};

} // namespace aria2

#endif // HAVE_SQLITE3
#endif // D_SQLITE3_BT_PROGRESS_INFO_FILE_H
