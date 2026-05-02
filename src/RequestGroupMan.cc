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
#include "RequestGroupMan.h"

#include <unistd.h>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <numeric>
#include <algorithm>
#include <utility>

#include "BtProgressInfoFile.h"
#include "RecoverableException.h"
#include "RequestGroup.h"
#include "LogFactory.h"
#include "Logger.h"
#include "DownloadEngine.h"
#include "message.h"
#include "a2functional.h"
#include "DownloadResult.h"
#include "DownloadContext.h"
#include "ServerStatMan.h"
#include "ServerStat.h"
#include "SegmentMan.h"
#include "FeedbackURISelector.h"
#include "InorderURISelector.h"
#include "AdaptiveURISelector.h"
#include "Option.h"
#include "prefs.h"
#include "File.h"
#include "util.h"
#include "Command.h"
#include "FileEntry.h"
#include "fmt.h"
#include "FileAllocationEntry.h"
#include "CheckIntegrityEntry.h"
#include "Segment.h"
#include "DlAbortEx.h"
#include "uri.h"
#include "Signature.h"
#include "OutputFile.h"
#include "download_helper.h"
#include "UriListParser.h"
#include "SingletonHolder.h"
#include "Notifier.h"
#include "PeerStat.h"
#include "WrDiskCache.h"
#include "PieceStorage.h"
#include "DiskAdaptor.h"
#include "SimpleRandomizer.h"
#include "array_fun.h"
#include "OpenedFileCounter.h"
#include "wallclock.h"
#include "RpcMethodImpl.h"
#ifdef ENABLE_BITTORRENT
#  include "bittorrent_helper.h"
#endif // ENABLE_BITTORRENT
#ifdef HAVE_SQLITE3
#  include "Sqlite3DownloadResultRepository.h"
#  include "Sqlite3SessionStore.h"
#endif // HAVE_SQLITE3

namespace aria2 {

namespace {
template <typename InputIterator>
void appendReservedGroup(RequestGroupList& list, InputIterator first,
                         InputIterator last)
{
  for (; first != last; ++first) {
    list.push_back((*first)->getGID(), *first);
  }
}
} // namespace

RequestGroupMan::RequestGroupMan(
    std::vector<std::shared_ptr<RequestGroup>> requestGroups,
    int maxConcurrentDownloads, const Option* option)
    : maxConcurrentDownloads_(maxConcurrentDownloads),
      optimizeConcurrentDownloads_(false),
      optimizeConcurrentDownloadsCoeffA_(5.),
      optimizeConcurrentDownloadsCoeffB_(25.),
      optimizationSpeed_(0),
      numActive_(0),
      option_(option),
      serverStatMan_(std::make_shared<ServerStatMan>()),
      maxOverallDownloadSpeedLimit_(
          option->getAsInt(PREF_MAX_OVERALL_DOWNLOAD_LIMIT)),
      maxOverallUploadSpeedLimit_(
          option->getAsInt(PREF_MAX_OVERALL_UPLOAD_LIMIT)),
      keepRunning_(option->getAsBool(PREF_ENABLE_RPC)),
      queueCheck_(true),
      removedErrorResult_(0),
      removedLastErrorResult_(error_code::FINISHED),
      maxDownloadResult_(option->getAsInt(PREF_MAX_DOWNLOAD_RESULT)),
      openedFileCounter_(std::make_shared<OpenedFileCounter>(
          this, option->getAsInt(PREF_BT_MAX_OPEN_FILES))),
      numStoppedTotal_(0)
{
  setupOptimizeConcurrentDownloads();
  appendReservedGroup(reservedGroups_, requestGroups.begin(),
                      requestGroups.end());
}

RequestGroupMan::~RequestGroupMan() { openedFileCounter_->deactivate(); }

bool RequestGroupMan::setupOptimizeConcurrentDownloads(void)
{
  optimizeConcurrentDownloads_ =
      option_->getAsBool(PREF_OPTIMIZE_CONCURRENT_DOWNLOADS);
  if (optimizeConcurrentDownloads_) {
    if (option_->defined(PREF_OPTIMIZE_CONCURRENT_DOWNLOADS_COEFFA)) {
      optimizeConcurrentDownloadsCoeffA_ = strtod(
          option_->get(PREF_OPTIMIZE_CONCURRENT_DOWNLOADS_COEFFA).c_str(),
          nullptr);
      optimizeConcurrentDownloadsCoeffB_ = strtod(
          option_->get(PREF_OPTIMIZE_CONCURRENT_DOWNLOADS_COEFFB).c_str(),
          nullptr);
    }
  }
  return optimizeConcurrentDownloads_;
}

bool RequestGroupMan::downloadFinished()
{
  if (keepRunning_) {
    return false;
  }
  return requestGroups_.empty() && reservedGroups_.empty();
}

void RequestGroupMan::addRequestGroup(
    const std::shared_ptr<RequestGroup>& group)
{
  ++numActive_;
  requestGroups_.push_back(group->getGID(), group);
}

void RequestGroupMan::addReservedGroup(
    const std::vector<std::shared_ptr<RequestGroup>>& groups)
{
  requestQueueCheck();
  appendReservedGroup(reservedGroups_, groups.begin(), groups.end());
}

void RequestGroupMan::addReservedGroup(
    const std::shared_ptr<RequestGroup>& group)
{
  requestQueueCheck();
  reservedGroups_.push_back(group->getGID(), group);
}

namespace {
struct RequestGroupKeyFunc {
  a2_gid_t operator()(const std::shared_ptr<RequestGroup>& rg) const
  {
    return rg->getGID();
  }
};
} // namespace

void RequestGroupMan::insertReservedGroup(
    size_t pos, const std::vector<std::shared_ptr<RequestGroup>>& groups)
{
  requestQueueCheck();
  pos = std::min(reservedGroups_.size(), pos);
  reservedGroups_.insert(pos, RequestGroupKeyFunc(), groups.begin(),
                         groups.end());
}

void RequestGroupMan::insertReservedGroup(
    size_t pos, const std::shared_ptr<RequestGroup>& group)
{
  requestQueueCheck();
  pos = std::min(reservedGroups_.size(), pos);
  reservedGroups_.insert(pos, group->getGID(), group);
}

size_t RequestGroupMan::countRequestGroup() const
{
  return requestGroups_.size();
}

std::shared_ptr<RequestGroup> RequestGroupMan::findGroup(a2_gid_t gid) const
{
  std::shared_ptr<RequestGroup> rg = requestGroups_.get(gid);
  if (!rg) {
    rg = reservedGroups_.get(gid);
  }
  return rg;
}

size_t RequestGroupMan::changeReservedGroupPosition(a2_gid_t gid, int pos,
                                                    OffsetMode how)
{
  ssize_t dest = reservedGroups_.move(gid, pos, how);
  if (dest == -1) {
    throw DL_ABORT_EX(fmt("GID#%s not found in the waiting queue.",
                          GroupId::toHex(gid).c_str()));
  }
  else {
    return dest;
  }
}

bool RequestGroupMan::removeReservedGroup(a2_gid_t gid)
{
  bool memSuccess = reservedGroups_.remove(gid);
#ifdef HAVE_SQLITE3
  // The reserved RG was upserted into the `task` table by the previous
  // saveAllTasks pass. Removing it from memory here without also
  // deleting the persisted row leaves a phantom that aria2 will
  // resurrect on next restart — visible to motrix-turbo as an Error /
  // ghost task with no matching sidecar. Clean both atomically.
  if (sessionStore_) {
    try {
      sessionStore_->deleteTask(GroupId::toHex(gid));
    }
    catch (RecoverableException& ex) {
      A2_LOG_ERROR_EX(
          "sqlite3-persistence: deleteTask in removeReservedGroup failed",
          ex);
    }
  }
#endif // HAVE_SQLITE3
  return memSuccess;
}

namespace {

void notifyDownloadEvent(DownloadEvent event,
                         const std::shared_ptr<RequestGroup>& group)
{
  // Check NULL to make unit test easier.
  if (SingletonHolder<Notifier>::instance()) {
    SingletonHolder<Notifier>::instance()->notifyDownloadEvent(event, group);
  }
}

} // namespace

namespace {

void executeStopHook(const std::shared_ptr<RequestGroup>& group,
                     const Option* option, error_code::Value result)
{
  PrefPtr hookPref = nullptr;
  if (!option->blank(PREF_ON_DOWNLOAD_STOP)) {
    hookPref = PREF_ON_DOWNLOAD_STOP;
  }
  if (result == error_code::FINISHED) {
    if (!option->blank(PREF_ON_DOWNLOAD_COMPLETE)) {
      hookPref = PREF_ON_DOWNLOAD_COMPLETE;
    }
  }
  else if (result != error_code::IN_PROGRESS && result != error_code::REMOVED) {
    if (!option->blank(PREF_ON_DOWNLOAD_ERROR)) {
      hookPref = PREF_ON_DOWNLOAD_ERROR;
    }
  }
  if (hookPref) {
    util::executeHookByOptName(group, option, hookPref);
  }

  if (result == error_code::FINISHED) {
    notifyDownloadEvent(EVENT_ON_DOWNLOAD_COMPLETE, group);
  }
  else if (result != error_code::IN_PROGRESS && result != error_code::REMOVED) {
    notifyDownloadEvent(EVENT_ON_DOWNLOAD_ERROR, group);
  }
  else {
    notifyDownloadEvent(EVENT_ON_DOWNLOAD_STOP, group);
  }
}

} // namespace

namespace {
class ProcessStoppedRequestGroup {
private:
  DownloadEngine* e_;
  RequestGroupList& reservedGroups_;

  void saveSignature(const std::shared_ptr<RequestGroup>& group)
  {
    auto& sig = group->getDownloadContext()->getSignature();
    if (sig && !sig->getBody().empty()) {
      // filename of signature file is the path to download file followed by
      // ".sig".
      std::string signatureFile = group->getFirstFilePath() + ".sig";
      if (sig->save(signatureFile)) {
        A2_LOG_NOTICE(fmt(MSG_SIGNATURE_SAVED, signatureFile.c_str()));
      }
      else {
        A2_LOG_NOTICE(fmt(MSG_SIGNATURE_NOT_SAVED, signatureFile.c_str()));
      }
    }
  }

  // Collect statistics during download in PeerStats and update/register
  // ServerStatMan
  void collectStat(const std::shared_ptr<RequestGroup>& group)
  {
    if (group->getSegmentMan()) {
      bool singleConnection =
          group->getSegmentMan()->getPeerStats().size() == 1;
      const std::vector<std::shared_ptr<PeerStat>>& peerStats =
          group->getSegmentMan()->getFastestPeerStats();
      for (auto& stat : peerStats) {
        if (stat->getHostname().empty() || stat->getProtocol().empty()) {
          continue;
        }
        int speed = stat->getAvgDownloadSpeed();
        if (speed == 0)
          continue;

        std::shared_ptr<ServerStat> ss =
            e_->getRequestGroupMan()->getOrCreateServerStat(
                stat->getHostname(), stat->getProtocol());
        ss->increaseCounter();
        ss->updateDownloadSpeed(speed);
        if (singleConnection) {
          ss->updateSingleConnectionAvgSpeed(speed);
        }
        else {
          ss->updateMultiConnectionAvgSpeed(speed);
        }
      }
    }
  }

public:
  ProcessStoppedRequestGroup(DownloadEngine* e,
                             RequestGroupList& reservedGroups)
      : e_(e), reservedGroups_(reservedGroups)
  {
  }

  bool operator()(const RequestGroupList::value_type& group)
  {
    if (group->getNumCommand() == 0) {
      collectStat(group);
      const std::shared_ptr<DownloadContext>& dctx =
          group->getDownloadContext();

      if (!group->isSeedOnlyEnabled()) {
        e_->getRequestGroupMan()->decreaseNumActive();
      }

      // DownloadContext::resetDownloadStopTime() is only called when
      // download completed. If
      // DownloadContext::getDownloadStopTime().isZero() is true, then
      // there is a possibility that the download is error or
      // in-progress and resetDownloadStopTime() is not called. So
      // call it here.
      if (dctx->getDownloadStopTime().isZero()) {
        dctx->resetDownloadStopTime();
      }
      try {
        group->closeFile();
        if (group->isPauseRequested()) {
          if (!group->isRestartRequested()) {
            A2_LOG_NOTICE(fmt(_("Download GID#%s paused"),
                              GroupId::toHex(group->getGID()).c_str()));
          }
          group->saveControlFile();
        }
        else if (group->downloadFinished() &&
                 !group->getDownloadContext()->isChecksumVerificationNeeded()) {
          group->applyLastModifiedTimeToLocalFiles();
          group->reportDownloadFinished();
          if (group->allDownloadFinished() &&
              !group->getOption()->getAsBool(PREF_FORCE_SAVE)) {
            group->removeControlFile();
            saveSignature(group);
          }
          else {
            group->saveControlFile();
          }
          std::vector<std::shared_ptr<RequestGroup>> nextGroups;
          group->postDownloadProcessing(nextGroups);
          if (!nextGroups.empty()) {
            A2_LOG_DEBUG(fmt("Adding %lu RequestGroups as a result of"
                             " PostDownloadHandler.",
                             static_cast<unsigned long>(nextGroups.size())));
            e_->getRequestGroupMan()->insertReservedGroup(0, nextGroups);
          }
#ifdef ENABLE_BITTORRENT
          // For in-memory download (e.g., Magnet URI), the
          // FileEntry::getPath() does not return actual file path, so
          // we don't remove it.
          if (group->getOption()->getAsBool(PREF_BT_REMOVE_UNSELECTED_FILE) &&
              !group->inMemoryDownload() && dctx->hasAttribute(CTX_ATTR_BT)) {
            A2_LOG_INFO(fmt(MSG_REMOVING_UNSELECTED_FILE,
                            GroupId::toHex(group->getGID()).c_str()));
            const std::vector<std::shared_ptr<FileEntry>>& files =
                dctx->getFileEntries();
            for (auto& file : files) {
              if (!file->isRequested()) {
                if (File(file->getPath()).remove()) {
                  A2_LOG_INFO(fmt(MSG_FILE_REMOVED, file->getPath().c_str()));
                }
                else {
                  A2_LOG_INFO(
                      fmt(MSG_FILE_COULD_NOT_REMOVED, file->getPath().c_str()));
                }
              }
            }
          }
#endif // ENABLE_BITTORRENT
        }
        else {
          A2_LOG_NOTICE(
              fmt(_("Download GID#%s not complete: %s"),
                  GroupId::toHex(group->getGID()).c_str(),
                  group->getDownloadContext()->getBasePath().c_str()));
          group->saveControlFile();
        }
      }
      catch (RecoverableException& ex) {
        A2_LOG_ERROR_EX(EX_EXCEPTION_CAUGHT, ex);
      }
      if (group->isPauseRequested()) {
        group->setState(RequestGroup::STATE_WAITING);
        reservedGroups_.push_front(group->getGID(), group);
        group->releaseRuntimeResource(e_);
        group->setForceHaltRequested(false);

        auto pendingOption = group->getPendingOption();
        if (pendingOption) {
          changeOption(group, *pendingOption, e_);
        }

        if (group->isRestartRequested()) {
          group->setPauseRequested(false);
        }
        else {
          util::executeHookByOptName(group, e_->getOption(),
                                     PREF_ON_DOWNLOAD_PAUSE);
          notifyDownloadEvent(EVENT_ON_DOWNLOAD_PAUSE, group);
        }
        // TODO Should we have to prepend spend uris to remaining uris
        // in case PREF_REUSE_URI is disabled?
      }
      else {
        std::shared_ptr<DownloadResult> dr = group->createDownloadResult();
        e_->getRequestGroupMan()->addDownloadResult(dr);
#ifdef HAVE_SQLITE3
        if (auto* ss = e_->getSqlite3SessionStore()) {
          try {
            // Two cases delete the persisted `task` row at this
            // active→stopped transition:
            //   • `--force-save=false`: the original semantic — never
            //     keep stopped rows.
            //   • `--force-save=true` AND the halt was user-requested
            //     (RPC remove / forceRemove). The user explicitly asked
            //     to drop this gid; force-save's "preserve in-flight
            //     across crashes" intent does NOT extend to that. Without
            //     this, motrix-turbo's finalizeBt path leaves a phantom
            //     row whenever its removeDownloadResult call lands
            //     before the transition has run (the deferred transition
            //     re-creates / preserves the row that motrix's later
            //     deleteTask had already removed).
            const bool forceSave =
                group->getOption()->getAsBool(PREF_FORCE_SAVE);
            const bool userRemoved =
                group->getHaltReason() == RequestGroup::USER_REQUEST;
            if (!forceSave || userRemoved) {
              ss->deleteTask(GroupId::toHex(group->getGID()));
            }
            else {
              // Force-save preserves the `task` row across this
              // transition; we must also flush the FINAL BT progress
              // (notably `upload_length`) into `task_progress` here.
              // RequestGroupMan::save() only iterates `requestGroups_`,
              // and this RG has just left it — so without this call
              // the persisted upload_length sticks at whatever the
              // last periodic save captured (typically a few hundred
              // ms before the SeedCheckCommand decided ratio was met).
              // After restart aria2 reads that stale value, decides
              // ratio is NOT yet satisfied, and starts seeding again
              // — the user-visible "Completed → Seeding → Completed"
              // restart loop. saveControlFile is a no-op for non-BT
              // tasks (HTTP/FTP DefaultBtProgressInfoFile::save just
              // writes the standard .aria2 control file, which the
              // download-already-finished branch removes on next save).
              try {
                group->saveControlFile();
              }
              catch (RecoverableException& ex) {
                A2_LOG_ERROR_EX(
                    "sqlite3-persistence: final saveControlFile failed",
                    ex);
              }
            }
          }
          catch (RecoverableException& ex) {
            A2_LOG_ERROR_EX("sqlite3-persistence: deleteTask failed", ex);
          }
        }
#endif // HAVE_SQLITE3
        executeStopHook(group, e_->getOption(), dr->result);
        group->releaseRuntimeResource(e_);
      }

      group->setRestartRequested(false);
      group->setPendingOption(nullptr);

      return true;
    }
    else {
      return false;
    }
  }
};
} // namespace

void RequestGroupMan::removeStoppedGroup(DownloadEngine* e)
{
  size_t numPrev = requestGroups_.size();
  requestGroups_.remove_if(ProcessStoppedRequestGroup(e, reservedGroups_));
  size_t numRemoved = numPrev - requestGroups_.size();
  if (numRemoved > 0) {
    A2_LOG_DEBUG(fmt("%lu RequestGroup(s) deleted.",
                     static_cast<unsigned long>(numRemoved)));
  }
}

void RequestGroupMan::configureRequestGroup(
    const std::shared_ptr<RequestGroup>& requestGroup) const
{
  const std::string& uriSelectorValue =
      requestGroup->getOption()->get(PREF_URI_SELECTOR);
  if (uriSelectorValue == V_FEEDBACK) {
    requestGroup->setURISelector(
        make_unique<FeedbackURISelector>(serverStatMan_));
  }
  else if (uriSelectorValue == V_INORDER) {
    requestGroup->setURISelector(make_unique<InorderURISelector>());
  }
  else if (uriSelectorValue == V_ADAPTIVE) {
    requestGroup->setURISelector(
        make_unique<AdaptiveURISelector>(serverStatMan_, requestGroup.get()));
  }
}

namespace {
std::vector<std::unique_ptr<Command>>
createInitialCommand(const std::shared_ptr<RequestGroup>& requestGroup,
                     DownloadEngine* e)
{
  std::vector<std::unique_ptr<Command>> res;
  requestGroup->createInitialCommand(res, e);
  return res;
}
} // namespace

void RequestGroupMan::fillRequestGroupFromReserver(DownloadEngine* e)
{
  removeStoppedGroup(e);

  int maxConcurrentDownloads = optimizeConcurrentDownloads_
                                   ? optimizeConcurrentDownloads()
                                   : maxConcurrentDownloads_;

  if (static_cast<size_t>(maxConcurrentDownloads) <= numActive_) {
    return;
  }
  int count = 0;
  int num = maxConcurrentDownloads - numActive_;
  std::vector<std::shared_ptr<RequestGroup>> pending;

  while (count < num && (uriListParser_ || !reservedGroups_.empty())) {
    if (uriListParser_ && reservedGroups_.empty()) {
      std::vector<std::shared_ptr<RequestGroup>> groups;
      // May throw exception
      bool ok = createRequestGroupFromUriListParser(groups, option_,
                                                    uriListParser_.get());
      if (ok) {
        appendReservedGroup(reservedGroups_, groups.begin(), groups.end());
      }
      else {
        uriListParser_.reset();
        if (reservedGroups_.empty()) {
          break;
        }
      }
    }
    std::shared_ptr<RequestGroup> groupToAdd = *reservedGroups_.begin();
    reservedGroups_.pop_front();
    if ((keepRunning_ && groupToAdd->isPauseRequested()) ||
        !groupToAdd->isDependencyResolved()) {
      pending.push_back(groupToAdd);
      continue;
    }
    // Drop pieceStorage here because paused download holds its
    // reference.
    groupToAdd->dropPieceStorage();
    configureRequestGroup(groupToAdd);
    groupToAdd->setRequestGroupMan(this);
    groupToAdd->setState(RequestGroup::STATE_ACTIVE);
    ++numActive_;
    requestGroups_.push_back(groupToAdd->getGID(), groupToAdd);
    try {
      auto res = createInitialCommand(groupToAdd, e);
      ++count;
      if (res.empty()) {
        requestQueueCheck();
      }
      else {
        e->addCommand(std::move(res));
      }
    }
    catch (RecoverableException& ex) {
      A2_LOG_ERROR_EX(EX_EXCEPTION_CAUGHT, ex);
      A2_LOG_DEBUG("Deleting temporal commands.");
      groupToAdd->setLastErrorCode(ex.getErrorCode(), ex.what());
      // We add groupToAdd to e later in order to it is processed in
      // removeStoppedGroup().
      requestQueueCheck();
    }

    util::executeHookByOptName(groupToAdd, e->getOption(),
                               PREF_ON_DOWNLOAD_START);
    notifyDownloadEvent(EVENT_ON_DOWNLOAD_START, groupToAdd);
  }
  if (!pending.empty()) {
    reservedGroups_.insert(reservedGroups_.begin(), RequestGroupKeyFunc(),
                           pending.begin(), pending.end());
  }
  if (count > 0) {
    e->setNoWait(true);
    e->setRefreshInterval(std::chrono::milliseconds(0));
    A2_LOG_DEBUG(fmt("%d RequestGroup(s) added.", count));
  }
}

void RequestGroupMan::save()
{
  for (auto& rg : requestGroups_) {
    if (rg->allDownloadFinished() &&
        !rg->getDownloadContext()->isChecksumVerificationNeeded() &&
        !rg->getOption()->getAsBool(PREF_FORCE_SAVE)) {
      rg->removeControlFile();
    }
    else {
      try {
        rg->saveControlFile();
      }
      catch (RecoverableException& e) {
        A2_LOG_ERROR_EX(EX_EXCEPTION_CAUGHT, e);
      }
    }
  }
}

void RequestGroupMan::closeFile()
{
  for (auto& elem : requestGroups_) {
    elem->closeFile();
  }
}

RequestGroupMan::DownloadStat RequestGroupMan::getDownloadStat() const
{
  int finished = 0;
  int error = removedErrorResult_;
  int inprogress = 0;
  int removed = 0;
  error_code::Value lastError = removedLastErrorResult_;
  for (auto& dr : downloadResults_) {

    if (dr->belongsTo != 0) {
      continue;
    }
    if (dr->result == error_code::FINISHED) {
      ++finished;
    }
    else if (dr->result == error_code::IN_PROGRESS) {
      ++inprogress;
    }
    else if (dr->result == error_code::REMOVED) {
      ++removed;
    }
    else {
      ++error;
      lastError = dr->result;
    }
  }
  return DownloadStat(error, inprogress, reservedGroups_.size(), lastError);
}

enum DownloadResultStatus {
  A2_STATUS_OK,
  A2_STATUS_INPR,
  A2_STATUS_RM,
  A2_STATUS_ERR
};

namespace {
const char* getStatusStr(DownloadResultStatus status, bool useColor)
{
  // status string is formatted in 4 characters wide.
  switch (status) {
  case (A2_STATUS_OK):
    if (useColor) {
      return "\033[1;32mOK\033[0m  ";
    }
    else {
      return "OK  ";
    }
  case (A2_STATUS_INPR):
    if (useColor) {
      return "\033[1;34mINPR\033[0m";
    }
    else {
      return "INPR";
    }
  case (A2_STATUS_RM):
    if (useColor) {
      return "\033[1mRM\033[0m  ";
    }
    else {
      return "RM  ";
    }
  case (A2_STATUS_ERR):
    if (useColor) {
      return "\033[1;31mERR\033[0m ";
    }
    else {
      return "ERR ";
    }
  default:
    return "";
  }
}
} // namespace

void RequestGroupMan::showDownloadResults(OutputFile& o, bool full) const
{
  int pathRowSize = 55;
  // Download Results:
  // idx|stat|path/length
  // ===+====+=======================================================================
  o.printf("\n%s"
           "\ngid   |stat|avg speed  |",
           _("Download Results:"));
  if (full) {
    o.write("  %|path/URI"
            "\n======+====+===========+===+");
    pathRowSize -= 4;
  }
  else {
    o.write("path/URI"
            "\n======+====+===========+");
  }
  std::string line(pathRowSize, '=');
  o.printf("%s\n", line.c_str());
  bool useColor = o.supportsColor() && option_->getAsBool(PREF_ENABLE_COLOR);
  int ok = 0;
  int err = 0;
  int inpr = 0;
  int rm = 0;
  for (auto& dr : downloadResults_) {

    if (dr->belongsTo != 0) {
      continue;
    }
    const char* status;
    switch (dr->result) {
    case error_code::FINISHED:
      status = getStatusStr(A2_STATUS_OK, useColor);
      ++ok;
      break;
    case error_code::IN_PROGRESS:
      status = getStatusStr(A2_STATUS_INPR, useColor);
      ++inpr;
      break;
    case error_code::REMOVED:
      status = getStatusStr(A2_STATUS_RM, useColor);
      ++rm;
      break;
    default:
      status = getStatusStr(A2_STATUS_ERR, useColor);
      ++err;
    }
    if (full) {
      formatDownloadResultFull(o, status, dr);
    }
    else {
      o.write(formatDownloadResult(status, dr).c_str());
      o.write("\n");
    }
  }
  if (ok > 0 || err > 0 || inpr > 0 || rm > 0) {
    o.printf("\n%s\n", _("Status Legend:"));
    if (ok > 0) {
      o.write(_("(OK):download completed."));
    }
    if (err > 0) {
      o.write(_("(ERR):error occurred."));
    }
    if (inpr > 0) {
      o.write(_("(INPR):download in-progress."));
    }
    if (rm > 0) {
      o.write(_("(RM):download removed."));
    }
    o.write("\n");
  }
}

namespace {
void formatDownloadResultCommon(
    std::ostream& o, const char* status,
    const std::shared_ptr<DownloadResult>& downloadResult)
{
  o << std::setw(3) << downloadResult->gid->toAbbrevHex() << "|" << std::setw(4)
    << status << "|";
  if (downloadResult->sessionTime.count() > 0) {
    o << std::setw(8)
      << util::abbrevSize(downloadResult->sessionDownloadLength * 1000 /
                          downloadResult->sessionTime.count())
      << "B/s";
  }
  else {
    o << std::setw(11);
    o << "n/a";
  }
  o << "|";
}
} // namespace

void RequestGroupMan::formatDownloadResultFull(
    OutputFile& out, const char* status,
    const std::shared_ptr<DownloadResult>& downloadResult) const
{
  BitfieldMan bt(downloadResult->pieceLength, downloadResult->totalLength);
  bt.setBitfield(
      reinterpret_cast<const unsigned char*>(downloadResult->bitfield.data()),
      downloadResult->bitfield.size());
  bool head = true;
  const std::vector<std::shared_ptr<FileEntry>>& fileEntries =
      downloadResult->fileEntries;
  for (auto& f : fileEntries) {
    if (!f->isRequested()) {
      continue;
    }
    std::stringstream o;
    if (head) {
      formatDownloadResultCommon(o, status, downloadResult);
      head = false;
    }
    else {
      o << "   |    |           |";
    }
    if (f->getLength() == 0 || downloadResult->bitfield.empty()) {
      o << "  -|";
    }
    else {
      int64_t completedLength =
          bt.getOffsetCompletedLength(f->getOffset(), f->getLength());
      o << std::setw(3) << 100 * completedLength / f->getLength() << "|";
    }
    writeFilePath(o, f, downloadResult->inMemoryDownload);
    o << "\n";
    out.write(o.str().c_str());
  }
  if (head) {
    std::stringstream o;
    formatDownloadResultCommon(o, status, downloadResult);
    o << "  -|n/a\n";
    out.write(o.str().c_str());
  }
}

std::string RequestGroupMan::formatDownloadResult(
    const char* status,
    const std::shared_ptr<DownloadResult>& downloadResult) const
{
  std::stringstream o;
  formatDownloadResultCommon(o, status, downloadResult);
  const std::vector<std::shared_ptr<FileEntry>>& fileEntries =
      downloadResult->fileEntries;
  writeFilePath(fileEntries.begin(), fileEntries.end(), o,
                downloadResult->inMemoryDownload);
  return o.str();
}

namespace {
template <typename StringInputIterator, typename FileEntryInputIterator>
bool sameFilePathExists(StringInputIterator sfirst, StringInputIterator slast,
                        FileEntryInputIterator ffirst,
                        FileEntryInputIterator flast)
{
  for (; ffirst != flast; ++ffirst) {
    if (std::binary_search(sfirst, slast, (*ffirst)->getPath())) {
      return true;
    }
  }
  return false;
}
} // namespace

bool RequestGroupMan::isSameFileBeingDownloaded(
    RequestGroup* requestGroup) const
{
  // TODO it may be good to use dedicated method rather than use
  // isPreLocalFileCheckEnabled
  if (!requestGroup->isPreLocalFileCheckEnabled()) {
    return false;
  }
  std::vector<std::string> files;
  for (auto& rg : requestGroups_) {
    if (rg.get() != requestGroup) {
      const std::vector<std::shared_ptr<FileEntry>>& entries =
          rg->getDownloadContext()->getFileEntries();
      std::transform(entries.begin(), entries.end(), std::back_inserter(files),
                     std::mem_fn(&FileEntry::getPath));
    }
  }
  std::sort(files.begin(), files.end());
  const std::vector<std::shared_ptr<FileEntry>>& entries =
      requestGroup->getDownloadContext()->getFileEntries();
  return sameFilePathExists(files.begin(), files.end(), entries.begin(),
                            entries.end());
}

void RequestGroupMan::halt()
{
  for (auto& elem : requestGroups_) {
    elem->setHaltRequested(true);
  }
}

void RequestGroupMan::forceHalt()
{
  for (auto& elem : requestGroups_) {
    elem->setForceHaltRequested(true);
  }
}

TransferStat RequestGroupMan::calculateStat()
{
  // TODO Currently, all time upload length is not set.
  return netStat_.toTransferStat();
}

std::shared_ptr<DownloadResult>
RequestGroupMan::findDownloadResult(a2_gid_t gid) const
{
  return downloadResults_.get(gid);
}

bool RequestGroupMan::removeDownloadResult(a2_gid_t gid)
{
  bool memSuccess = downloadResults_.remove(gid);
#ifdef HAVE_SQLITE3
  bool dbSuccess = false;
  if (repo_) {
    try {
      dbSuccess = repo_->deleteByGid(gid);
    }
    catch (RecoverableException&) {
      dbSuccess = false;
    }
  }
  // removeDownloadResult is the explicit "purge this gid" RPC. With
  // `--force-save=true` the active→stopped transition site (line 466)
  // intentionally preserves the `task` row so an in-flight download
  // can resume after a crash; that semantic does NOT extend to a user-
  // initiated removal. If we don't delete here, the row outlives the
  // in-memory result and aria2 will load it as a phantom task on the
  // next restart (motrix-turbo then sees an extra Error row whose gid
  // doesn't match any sidecar).
  //
  // Also covers the forceRemove → removeDownloadResult race: if the
  // halt request hadn't transitioned the gid into downloadResults_ yet
  // when this call arrives, memSuccess and dbSuccess will both be false
  // here, but the deferred transition (executed on the next event-loop
  // iteration with force-save=true) will leave a row that no later
  // call cleans up. Issuing deleteTask unconditionally on the explicit-
  // remove path closes that window.
  if (sessionStore_) {
    try {
      sessionStore_->deleteTask(GroupId::toHex(gid));
    }
    catch (RecoverableException& ex) {
      A2_LOG_ERROR_EX(
          "sqlite3-persistence: deleteTask in removeDownloadResult failed",
          ex);
    }
  }
  return memSuccess || dbSuccess;
#else
  return memSuccess;
#endif
}

void RequestGroupMan::addDownloadResult(
    const std::shared_ptr<DownloadResult>& dr)
{
  ++numStoppedTotal_;
  bool rv = downloadResults_.push_back(dr->gid->getNumericId(), dr);
  assert(rv);
  while (downloadResults_.size() > maxDownloadResult_) {
    // Save last encountered error code so that we can report it
    // later.
    const auto& dr = downloadResults_[0];
    if (dr->belongsTo == 0 && dr->result != error_code::FINISHED) {
      removedLastErrorResult_ = dr->result;
      ++removedErrorResult_;

      // Keep unfinished download result, so that we can save them by
      // SessionSerializer.
      if (option_->getAsBool(PREF_KEEP_UNFINISHED_DOWNLOAD_RESULT)) {
        if (dr->result != error_code::REMOVED ||
            dr->option->getAsBool(PREF_FORCE_SAVE)) {
          unfinishedDownloadResults_.push_back(dr);
        }
      }
    }
    downloadResults_.pop_front();
  }
#ifdef HAVE_SQLITE3
  if (repo_) {
    bool inserted = false;
    try {
      repo_->insert(dr);
      inserted = true;
    }
    catch (RecoverableException& e) {
      A2_LOG_ERROR_EX("sqlite3-persistence: history insert failed; queued for retry",
                      e);
      repo_->enqueuePending(dr);
    }
    if (inserted) {
      try {
        repo_->trimToCap(option_->getAsInt(PREF_SQLITE3_HISTORY_LIMIT),
                         option_->getAsBool(PREF_KEEP_UNFINISHED_DOWNLOAD_RESULT));
      }
      catch (RecoverableException& e) {
        A2_LOG_ERROR_EX("sqlite3-persistence: history trim failed", e);
      }
    }
  }
#endif
}

void RequestGroupMan::purgeDownloadResult()
{
#ifdef HAVE_SQLITE3
  // Snapshot every gid that's about to be cleared so we can prune the
  // matching `task` rows. With `--force-save=true` those rows survive
  // the active→stopped transition; purging only download_history but
  // leaving `task` behind would let the gids resurrect on restart.
  std::vector<a2_gid_t> purgedGids;
  if (sessionStore_) {
    purgedGids.reserve(downloadResults_.size() +
                       unfinishedDownloadResults_.size());
    for (const auto& dr : downloadResults_) {
      if (dr && dr->gid) {
        purgedGids.push_back(dr->gid->getNumericId());
      }
    }
    for (const auto& dr : unfinishedDownloadResults_) {
      if (dr && dr->gid) {
        purgedGids.push_back(dr->gid->getNumericId());
      }
    }
  }
#endif // HAVE_SQLITE3

  downloadResults_.clear();
#ifdef HAVE_SQLITE3
  if (repo_) {
    try {
      repo_->purgeAll();
    }
    catch (RecoverableException&) {
    }
  }
  if (sessionStore_) {
    for (auto gid : purgedGids) {
      try {
        sessionStore_->deleteTask(GroupId::toHex(gid));
      }
      catch (RecoverableException& ex) {
        A2_LOG_ERROR_EX(
            "sqlite3-persistence: deleteTask in purgeDownloadResult failed",
            ex);
      }
    }
  }
#endif
}

std::vector<std::shared_ptr<DownloadResult>>
RequestGroupMan::getDownloadResultsRange(int offset, int num, bool desc) const
{
  std::vector<std::shared_ptr<DownloadResult>> result;
  if (num <= 0) {
    return result;
  }

  // Snapshot memory list (oldest-first, as stored).
  std::vector<std::shared_ptr<DownloadResult>> mem;
  mem.reserve(downloadResults_.size());
  for (auto& dr : downloadResults_) {
    mem.push_back(dr);
  }
  const int M = static_cast<int>(mem.size());

#ifdef HAVE_SQLITE3
  int D = 0;
  if (repo_) {
    try {
      D = static_cast<int>(repo_->countAll());
    }
    catch (Exception& ex) {
      A2_LOG_ERROR_EX("sqlite3-persistence: countAll failed in tellStopped; "
                      "degrading to memory-only",
                      ex);
    }
  }
#else
  const int D = 0;
#endif

  if (!desc) {
    // ASC slice (offset >= 0 by contract)
    int off = (offset < 0) ? 0 : offset;
    if (off >= D + M) {
      return result;
    }
    if (off < D) {
#ifdef HAVE_SQLITE3
      if (repo_) {
        try {
          result = repo_->range(off, num, /*desc=*/false);
        }
        catch (RecoverableException& ex) {
          A2_LOG_ERROR_EX("sqlite3-persistence: tellStopped DB read failed; "
                          "degrading to memory-only",
                          ex);
          result.clear();
        }
      }
#endif
      if (static_cast<int>(result.size()) < num) {
        int needed = num - static_cast<int>(result.size());
        for (int i = 0; i < needed && i < M; ++i) {
          result.push_back(mem[i]);
        }
      }
    }
    else {
      // off >= D: slice purely from memory
      int memOff = off - D;
      int end = std::min(memOff + num, M);
      for (int i = memOff; i < end; ++i) {
        result.push_back(mem[i]);
      }
    }
  }
  else {
    // DESC slice (offset < 0)
    int abs_off = -offset - 1;
    if (abs_off < 0 || abs_off >= M + D) {
      return result;
    }
    if (abs_off < M) {
      // Take from tail of mem (newest-first)
      int start = M - 1 - abs_off;
      int end = std::max(start - num + 1, 0);
      for (int i = start; i >= end; --i) {
        result.push_back(mem[i]);
      }
      if (static_cast<int>(result.size()) < num) {
        int needed = num - static_cast<int>(result.size());
#ifdef HAVE_SQLITE3
        if (repo_) {
          try {
            auto more = repo_->range(0, needed, /*desc=*/true);
            for (auto& dr : more) {
              result.push_back(dr);
            }
          }
          catch (RecoverableException& ex) {
            A2_LOG_ERROR_EX("sqlite3-persistence: tellStopped DB read failed; "
                            "degrading to memory-only",
                            ex);
          }
        }
#endif
      }
    }
    else {
#ifdef HAVE_SQLITE3
      if (repo_) {
        try {
          int dbOff = abs_off - M;
          result = repo_->range(dbOff, num, /*desc=*/true);
        }
        catch (RecoverableException& ex) {
          A2_LOG_ERROR_EX(
              "sqlite3-persistence: tellStopped DB read failed", ex);
        }
      }
#endif
    }
  }

  return result;
}

int64_t RequestGroupMan::getDownloadResultsCount() const
{
  int64_t count = static_cast<int64_t>(downloadResults_.size());
#ifdef HAVE_SQLITE3
  if (repo_) {
    try {
      count += repo_->countAll();
    }
    catch (RecoverableException& ex) {
      A2_LOG_ERROR_EX("sqlite3-persistence: countAll failed", ex);
    }
  }
#endif
  return count;
}

std::shared_ptr<ServerStat>
RequestGroupMan::findServerStat(const std::string& hostname,
                                const std::string& protocol) const
{
  return serverStatMan_->find(hostname, protocol);
}

std::shared_ptr<ServerStat>
RequestGroupMan::getOrCreateServerStat(const std::string& hostname,
                                       const std::string& protocol)
{
  std::shared_ptr<ServerStat> ss = findServerStat(hostname, protocol);
  if (!ss) {
    ss = std::make_shared<ServerStat>(hostname, protocol);
    addServerStat(ss);
  }
  return ss;
}

bool RequestGroupMan::addServerStat(
    const std::shared_ptr<ServerStat>& serverStat)
{
  return serverStatMan_->add(serverStat);
}

bool RequestGroupMan::loadServerStat(const std::string& filename)
{
  return serverStatMan_->load(filename);
}

bool RequestGroupMan::saveServerStat(const std::string& filename) const
{
  return serverStatMan_->save(filename);
}

void RequestGroupMan::removeStaleServerStat(const std::chrono::seconds& timeout)
{
  serverStatMan_->removeStaleServerStat(timeout);
}

bool RequestGroupMan::doesOverallDownloadSpeedExceed()
{
  return maxOverallDownloadSpeedLimit_ > 0 &&
         maxOverallDownloadSpeedLimit_ < netStat_.calculateDownloadSpeed();
}

bool RequestGroupMan::doesOverallUploadSpeedExceed()
{
  return maxOverallUploadSpeedLimit_ > 0 &&
         maxOverallUploadSpeedLimit_ < netStat_.calculateUploadSpeed();
}

void RequestGroupMan::getUsedHosts(
    std::vector<std::pair<size_t, std::string>>& usedHosts)
{
  // vector of tuple which consists of use count, -download speed,
  // hostname. We want to sort by least used and faster download
  // speed. We use -download speed so that we can sort them using
  // operator<().
  std::vector<std::tuple<size_t, int, std::string>> tempHosts;
  for (const auto& rg : requestGroups_) {
    const auto& inFlightReqs =
        rg->getDownloadContext()->getFirstFileEntry()->getInFlightRequests();
    for (const auto& req : inFlightReqs) {
      uri_split_result us;
      if (uri_split(&us, req->getUri().c_str()) == 0) {
        std::string host =
            uri::getFieldString(us, USR_HOST, req->getUri().c_str());
        auto k = tempHosts.begin();
        auto eok = tempHosts.end();
        for (; k != eok; ++k) {
          if (std::get<2>(*k) == host) {
            ++std::get<0>(*k);
            break;
          }
        }
        if (k == eok) {
          std::string protocol =
              uri::getFieldString(us, USR_SCHEME, req->getUri().c_str());
          auto ss = findServerStat(host, protocol);
          int invDlSpeed = (ss && ss->isOK())
                               ? -(static_cast<int>(ss->getDownloadSpeed()))
                               : 0;
          tempHosts.emplace_back(1, invDlSpeed, host);
        }
      }
    }
  }
  std::sort(tempHosts.begin(), tempHosts.end());
  std::transform(tempHosts.begin(), tempHosts.end(),
                 std::back_inserter(usedHosts),
                 [](const std::tuple<size_t, int, std::string>& x) {
                   return std::make_pair(std::get<0>(x), std::get<2>(x));
                 });
}

void RequestGroupMan::setUriListParser(
    const std::shared_ptr<UriListParser>& uriListParser)
{
  uriListParser_ = uriListParser;
}

void RequestGroupMan::initWrDiskCache()
{
  assert(!wrDiskCache_);
  size_t limit = option_->getAsInt(PREF_DISK_CACHE);
  if (limit > 0) {
    wrDiskCache_ = make_unique<WrDiskCache>(limit);
  }
}

void RequestGroupMan::decreaseNumActive()
{
  assert(numActive_ > 0);
  --numActive_;
}

int RequestGroupMan::optimizeConcurrentDownloads()
{
  // gauge the current speed
  int currentSpeed = getNetStat().calculateDownloadSpeed();

  const auto& now = global::wallclock();
  if (currentSpeed >= optimizationSpeed_) {
    optimizationSpeed_ = currentSpeed;
    optimizationSpeedTimer_ = now;
  }
  else if (std::chrono::duration_cast<std::chrono::seconds>(
               optimizationSpeedTimer_.difference(now)) >= 5_s) {
    // we keep using the reference speed for minimum 5 seconds so reset the
    // timer
    optimizationSpeedTimer_ = now;

    // keep the reference speed as long as the speed tends to augment or to
    // maintain itself within 10%
    if (currentSpeed >= 1.1 * getNetStat().calculateNewestDownloadSpeed(5)) {
      // else assume a possible congestion and record a new optimization speed
      // by dichotomy
      optimizationSpeed_ = (optimizationSpeed_ + currentSpeed) / 2.;
    }
  }

  if (optimizationSpeed_ <= 0) {
    return optimizeConcurrentDownloadsCoeffA_;
  }

  // apply the rule
  if ((maxOverallDownloadSpeedLimit_ > 0) &&
      (optimizationSpeed_ > maxOverallDownloadSpeedLimit_)) {
    optimizationSpeed_ = maxOverallDownloadSpeedLimit_;
  }
  int maxConcurrentDownloads =
      ceil(optimizeConcurrentDownloadsCoeffA_ +
           optimizeConcurrentDownloadsCoeffB_ *
               log10(optimizationSpeed_ * 8. / 1000000.));

  // bring the value in bound between 1 and the defined maximum
  maxConcurrentDownloads =
      std::min(std::max(1, maxConcurrentDownloads), maxConcurrentDownloads_);

  A2_LOG_DEBUG(
      fmt("Max concurrent downloads optimized at %d (%lu currently active) "
          "[optimization speed %sB/s, current speed %sB/s]",
          maxConcurrentDownloads, static_cast<unsigned long>(numActive_),
          util::abbrevSize(optimizationSpeed_).c_str(),
          util::abbrevSize(currentSpeed).c_str()));

  return maxConcurrentDownloads;
}
} // namespace aria2
