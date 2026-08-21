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
#include "DHTGetPeersCommand.h"
#include "DHTTaskQueue.h"
#include "DHTTaskFactory.h"
#include "DHTTask.h"
#include "DownloadEngine.h"
#include "RequestGroup.h"
#include "DHTNode.h"
#include "DHTNodeLookupEntry.h"
#include "BtRuntime.h"
#include "PeerStorage.h"
#include "Peer.h"
#include "Logger.h"
#include "LogFactory.h"
#include "bittorrent_helper.h"
#include "DownloadContext.h"
#include "wallclock.h"
#include "fmt.h"
#include "BtRegistry.h"

namespace aria2 {

namespace {

constexpr auto GET_PEER_INTERVAL = 15_min;
// Interval when the size of peer list is low.
constexpr auto GET_PEER_INTERVAL_LOW = 5_min;
// Interval when the peer list is empty.
constexpr auto GET_PEER_INTERVAL_ZERO = 1_min;
// Interval for retry.
constexpr auto GET_PEER_INTERVAL_RETRY = 5_s;
// Maximum retries. Try more than 5 to drop bad node.
const int MAX_RETRIES = 10;

constexpr auto ENDPOINT_REFRESH_DEBOUNCE = 5_s;
constexpr auto ENDPOINT_REFRESH_MAX_JITTER = 11;
constexpr auto ENDPOINT_REFRESH_MIN_INTERVAL = 60_s;

} // namespace

DHTGetPeersCommand::DHTGetPeersCommand(cuid_t cuid, RequestGroup* requestGroup,
                                       DownloadEngine* e, int family)
    : Command{cuid},
      requestGroup_{requestGroup},
      e_{e},
      taskQueue_{nullptr},
      taskFactory_{nullptr},
      numRetry_{0},
      lastGetPeerTime_{Timer::zero()},
      lastPeerLookupTime_{Timer::zero()},
      family_{family},
      announcePortRevision_{
          e->getBtRegistry()->getAnnouncePortRevision(family)},
      lastPeerLookupPort_{e->getBtRegistry()->getAnnouncePort(family)},
      pendingAnnouncePort_{lastPeerLookupPort_},
      endpointRefreshPending_{false},
      endpointChangedTimer_{Timer::zero()},
      endpointRefreshDelay_{ENDPOINT_REFRESH_DEBOUNCE}
{
  requestGroup_->increaseNumCommand();
}

DHTGetPeersCommand::~DHTGetPeersCommand()
{
  requestGroup_->decreaseNumCommand();
}

bool DHTGetPeersCommand::execute()
{
  if (btRuntime_->isHalt()) {
    return true;
  }
  const auto announcePortRevision =
      e_->getBtRegistry()->getAnnouncePortRevision(family_);
  if (announcePortRevision_ != announcePortRevision) {
    announcePortRevision_ = announcePortRevision;
    const auto announcePort =
        e_->getBtRegistry()->getAnnouncePort(family_);
    if (announcePort == lastPeerLookupPort_) {
      endpointRefreshPending_ = false;
    }
    else if (!endpointRefreshPending_ ||
             pendingAnnouncePort_ != announcePort) {
      pendingAnnouncePort_ = announcePort;
      endpointRefreshPending_ = true;
      endpointChangedTimer_ = global::wallclock();
      endpointRefreshDelay_ =
          ENDPOINT_REFRESH_DEBOUNCE + std::chrono::seconds(
                                          requestGroup_->getGID() %
                                          ENDPOINT_REFRESH_MAX_JITTER);
    }
  }
  const auto& now = global::wallclock();
  auto elapsed = lastGetPeerTime_.difference(now);
  const auto endpointRefreshReady =
      endpointRefreshPending_ &&
      endpointChangedTimer_.difference(now) >= endpointRefreshDelay_ &&
      lastPeerLookupTime_.difference(now) >= ENDPOINT_REFRESH_MIN_INTERVAL;
  if (!task_ && (endpointRefreshReady || elapsed >= GET_PEER_INTERVAL ||
                 (((btRuntime_->lessThanMinPeers() &&
                    ((numRetry_ && elapsed >= GET_PEER_INTERVAL_RETRY) ||
                     elapsed >= GET_PEER_INTERVAL_LOW)) ||
                   (btRuntime_->getConnections() == 0 &&
                    elapsed >= GET_PEER_INTERVAL_ZERO))))) {
    A2_LOG_DEBUG(
        fmt("Issuing PeerLookup for infoHash=%s",
            bittorrent::getInfoHashString(requestGroup_->getDownloadContext())
                .c_str()));
    task_ = taskFactory_->createPeerLookupTask(
        requestGroup_->getDownloadContext(),
        e_->getBtRegistry()->getAnnouncePortState(), family_, peerStorage_);
    taskQueue_->addPeriodicTask2(task_);
    lastPeerLookupTime_ = now;
    endpointRefreshPending_ = false;
  }
  else if (task_ && task_->finished()) {
    A2_LOG_DEBUG("task finished detected");
    lastGetPeerTime_ = global::wallclock();
    lastPeerLookupPort_ = e_->getBtRegistry()->getAnnouncePort(family_);
    pendingAnnouncePort_ = lastPeerLookupPort_;
    endpointRefreshPending_ = false;
    if (numRetry_ < MAX_RETRIES &&
        (btRuntime_->getMaxPeers() == 0 ||
         btRuntime_->getMaxPeers() >
             static_cast<int>(peerStorage_->countAllPeer()))) {
      ++numRetry_;
      A2_LOG_DEBUG(fmt("Too few peers. peers=%lu, max_peers=%d."
                       " Try again(%d)",
                       static_cast<unsigned long>(peerStorage_->countAllPeer()),
                       btRuntime_->getMaxPeers(), numRetry_));
    }
    else {
      numRetry_ = 0;
    }
    task_.reset();
  }

  e_->addCommand(std::unique_ptr<Command>(this));
  return false;
}

void DHTGetPeersCommand::setTaskQueue(DHTTaskQueue* taskQueue)
{
  taskQueue_ = taskQueue;
}

void DHTGetPeersCommand::setTaskFactory(DHTTaskFactory* taskFactory)
{
  taskFactory_ = taskFactory;
}

void DHTGetPeersCommand::setBtRuntime(
    const std::shared_ptr<BtRuntime>& btRuntime)
{
  btRuntime_ = btRuntime;
}

void DHTGetPeersCommand::setPeerStorage(const std::shared_ptr<PeerStorage>& ps)
{
  peerStorage_ = ps;
}

} // namespace aria2
