// SPDX-License-Identifier: MIT

#include "core/transferQueue.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <unordered_set>

namespace havremote
{
  namespace
  {
    [[nodiscard]] RemoteError QueueError(RemoteErrorCode code, std::string message)
    {
      return RemoteError{.code = code, .message = std::move(message)};
    }

    [[nodiscard]] bool ValidProtocol(const ProtocolKind protocol) noexcept
    {
      switch (protocol)
      {
      case ProtocolKind::Ftp:
      case ProtocolKind::FtpsExplicit:
      case ProtocolKind::FtpsImplicit:
      case ProtocolKind::Sftp:
        return true;
      }

      return false;
    }

    [[nodiscard]] bool ValidDirection(const TransferDirection direction) noexcept
    {
      return direction == TransferDirection::Upload ||
             direction == TransferDirection::Download;
    }

    [[nodiscard]] bool ValidConflictPolicy(const ConflictPolicy policy) noexcept
    {
      switch (policy)
      {
      case ConflictPolicy::Ask:
      case ConflictPolicy::Overwrite:
      case ConflictPolicy::Skip:
      case ConflictPolicy::Rename:
      case ConflictPolicy::Resume:
        return true;
      }

      return false;
    }

    [[nodiscard]] bool ValidRemoteEntryKind(const RemoteEntryKind kind) noexcept
    {
      switch (kind)
      {
      case RemoteEntryKind::File:
      case RemoteEntryKind::Directory:
      case RemoteEntryKind::Symlink:
      case RemoteEntryKind::Other:
        return true;
      }

      return false;
    }

    bool SafeLocalIdentity(const std::filesystem::path &path)
    {
      if (path.empty() || path.native().find(
                              std::filesystem::path::value_type{}) !=
                              std::filesystem::path::string_type::npos)
      {
        return false;
      }

      return std::ranges::none_of(path, [](const auto &component)
                                  { return component == ".."; });
    }

    bool SafeRemoteText(const std::string_view bytes)
    {
      if (bytes.empty() ||
          std::ranges::any_of(bytes, [](const unsigned char character)
                              { return character < 0x20U || character == 0x7fU; }))
      {
        return false;
      }

      for (std::size_t begin = 0; begin <= bytes.size();)
      {
        const auto slash = bytes.find('/', begin);
        const auto end = slash == std::string_view::npos ? bytes.size() : slash;
        const auto component = bytes.substr(begin, end - begin);

        if (component == "." || component == "..")
        {
          return false;
        }

        if (slash == std::string_view::npos)
        {
          break;
        }

        begin = slash + 1;
      }

      return true;
    }

    bool SafeRemoteIdentity(const RemotePath &path)
    {
      return SafeRemoteText(path.Bytes()) &&
             SafeRemoteText(path.DisplayUtf8()) &&
             (path.Bytes().front() == '/') == (path.DisplayUtf8().front() == '/');
    }

    auto LocalIdentityKey(const std::filesystem::path &path)
    {
      // Metadata preserves native path spellings. Destination filesystem
      // equivalence is checked separately before transfer writes.
      return path.lexically_normal().native();
    }

    bool LocalPathWithin(const std::filesystem::path &root,
                         const std::filesystem::path &candidate)
    {
      const auto relative = candidate.lexically_relative(root);

      return !relative.empty() && !relative.is_absolute() &&
             SafeLocalIdentity(relative);
    }

    bool RemoteTextWithin(std::string_view prefix, const std::string_view bytes)
    {
      while (prefix.size() > 1 && prefix.back() == '/')
      {
        prefix.remove_suffix(1);
      }

      return bytes == prefix ||
             (prefix == "/" && bytes.starts_with('/')) ||
             (bytes.size() > prefix.size() && bytes.starts_with(prefix) &&
              bytes[prefix.size()] == '/');
    }

    bool RemotePathWithin(const RemotePath &root, const RemotePath &candidate)
    {
      return RemoteTextWithin(root.Bytes(), candidate.Bytes()) &&
             RemoteTextWithin(root.DisplayUtf8(), candidate.DisplayUtf8());
    }

    Result<void> ValidateDestinationResolution(
        const TransferJob &job,
        const TransferDestinationOverride &resolution)
    {
      if (!ValidDirection(job.direction) ||
          !SafeLocalIdentity(resolution.originalLocalPath) ||
          !SafeLocalIdentity(resolution.localPath) ||
          resolution.originalLocalPath.filename().empty() ||
          resolution.localPath.filename().empty() ||
          resolution.localPath.filename() == "." ||
          !SafeRemoteIdentity(resolution.originalRemotePath) ||
          !SafeRemoteIdentity(resolution.remotePath) ||
          resolution.originalRemotePath.Bytes().back() == '/' ||
          resolution.originalRemotePath.DisplayUtf8().back() == '/' ||
          resolution.remotePath.Bytes().back() == '/' ||
          resolution.remotePath.DisplayUtf8().back() == '/')
      {
        return std::unexpected(QueueError(
            RemoteErrorCode::InvalidArgument,
            "A resolved transfer requires safe local and remote file paths"));
      }

      if ((job.direction == TransferDirection::Upload &&
           resolution.originalLocalPath != resolution.localPath) ||
          (job.direction == TransferDirection::Download &&
           resolution.originalRemotePath != resolution.remotePath))
      {
        return std::unexpected(QueueError(
            RemoteErrorCode::InvalidArgument,
            "Resolving a transfer destination cannot change its source"));
      }

      if (job.direction == TransferDirection::Upload
              ? (resolution.originalRemotePath.IsAbsolute() !=
                     resolution.remotePath.IsAbsolute() ||
                 resolution.originalRemotePath.Parent() !=
                     resolution.remotePath.Parent())
              : LocalIdentityKey(resolution.originalLocalPath.parent_path()) !=
                    LocalIdentityKey(resolution.localPath.parent_path()))
      {
        return std::unexpected(QueueError(
            RemoteErrorCode::InvalidArgument,
            "A renamed transfer destination must remain in the same directory"));
      }

      if (job.recursive)
      {
        if (!SafeLocalIdentity(job.localPath) ||
            !SafeRemoteIdentity(job.remotePath) ||
            !LocalPathWithin(job.localPath, resolution.originalLocalPath) ||
            !LocalPathWithin(job.localPath, resolution.localPath) ||
            !RemotePathWithin(job.remotePath, resolution.originalRemotePath) ||
            !RemotePathWithin(job.remotePath, resolution.remotePath))
        {
          return std::unexpected(QueueError(
              RemoteErrorCode::InvalidArgument,
              "A resolved transfer path escapes its recursive job roots"));
        }
      }
      else if (resolution.originalLocalPath != job.localPath ||
               resolution.originalRemotePath != job.remotePath)
      {
        return std::unexpected(QueueError(
            RemoteErrorCode::InvalidArgument,
            "A resolved transfer does not match its single-file job"));
      }

      return {};
    }

    [[nodiscard]] Result<void> NormalizeRestoredTransfer(QueuedTransfer &transfer)
    {
      if (transfer.job.id.empty() || transfer.job.siteId.empty() ||
          transfer.job.localPath.empty() || transfer.job.remotePath.Empty())
      {
        return std::unexpected(QueueError(
            RemoteErrorCode::InvalidArgument,
            "A restored transfer requires job, site, local path, and remote path identities"));
      }

      if (!transfer.job.siteEndpoint ||
          !ValidProtocol(transfer.job.siteEndpoint->protocol) ||
          !IsValidEndpointHost(transfer.job.siteEndpoint->host) ||
          transfer.job.siteEndpoint->port == 0)
      {
        return std::unexpected(QueueError(
            RemoteErrorCode::InvalidArgument,
            "A restored transfer requires a valid immutable endpoint identity"));
      }

      if (!ValidDirection(transfer.job.direction) ||
          !ValidConflictPolicy(transfer.job.conflictPolicy))
      {
        return std::unexpected(QueueError(
            RemoteErrorCode::InvalidArgument,
            "A restored transfer contains an invalid transfer enum value"));
      }

      if (auto valid = ValidateTransferDestinations(transfer.job); !valid)
      {
        return valid;
      }

      if (transfer.job.expectedRemoteRevision)
      {
        if (transfer.job.recursive)
        {
          return std::unexpected(QueueError(
              RemoteErrorCode::InvalidArgument,
              "A recursive transfer cannot carry an expected remote revision"));
        }

        if (!ValidRemoteEntryKind(transfer.job.expectedRemoteRevision->kind))
        {
          return std::unexpected(QueueError(
              RemoteErrorCode::InvalidArgument,
              "A restored transfer contains an invalid remote revision kind"));
        }
      }

      if (!transfer.progress.jobId.empty() &&
          transfer.progress.jobId != transfer.job.id)
      {
        return std::unexpected(QueueError(
            RemoteErrorCode::InvalidArgument,
            "Restored transfer progress belongs to a different job"));
      }

      if (transfer.progress.totalBytes &&
          transfer.progress.bytesTransferred > *transfer.progress.totalBytes)
      {
        return std::unexpected(QueueError(
            RemoteErrorCode::InvalidArgument,
            "Restored transfer progress exceeds the total size"));
      }

      switch (transfer.state)
      {
      case TransferState::Enumerating:
      case TransferState::Running:
        transfer.state = TransferState::Paused;
        break;

      case TransferState::Queued:
      case TransferState::Paused:
      case TransferState::Failed:
        break;

      case TransferState::Completed:
      case TransferState::Cancelled:
        return std::unexpected(QueueError(
            RemoteErrorCode::InvalidArgument,
            "Completed and canceled transfers cannot be restored"));

      default:
        return std::unexpected(QueueError(
            RemoteErrorCode::InvalidArgument,
            "A restored transfer contains an invalid state"));
      }

      if (transfer.state == TransferState::Failed)
      {
        if (!transfer.error)
        {
          return std::unexpected(QueueError(
              RemoteErrorCode::InvalidArgument,
              "A restored failed transfer requires its error"));
        }
      }
      else if (transfer.error)
      {
        return std::unexpected(QueueError(
            RemoteErrorCode::InvalidArgument,
            "Only failed restored transfers may contain an error"));
      }

      transfer.progress.jobId = transfer.job.id;
      transfer.progress.activeBytesTransferred = 0U;
      transfer.progress.elapsed = {};

      return {};
    }
  } // namespace

  Result<void> ValidateTransferDestinations(const TransferJob &job)
  {
    if (!job.recursive && !job.destinationOverrides.empty())
    {
      return std::unexpected(QueueError(
          RemoteErrorCode::InvalidArgument,
          "Only recursive transfers may contain destination overrides"));
    }

    std::unordered_set<std::filesystem::path::string_type> originalLocalPaths;
    std::unordered_set<std::string> originalRemotePaths;
    std::unordered_set<std::filesystem::path::string_type> localDestinations;
    std::unordered_set<std::string> remoteDestinations;

    for (const auto &resolution : job.destinationOverrides)
    {
      if (auto valid = ValidateDestinationResolution(job, resolution); !valid)
      {
        return valid;
      }

      if (!originalLocalPaths.insert(LocalIdentityKey(resolution.originalLocalPath)).second ||
          !originalRemotePaths.insert(resolution.originalRemotePath.Bytes()).second)
      {
        return std::unexpected(QueueError(
            RemoteErrorCode::InvalidArgument,
            "A recursive transfer contains duplicate original path identities"));
      }

      const bool uniqueDestination = job.direction == TransferDirection::Upload
                                         ? remoteDestinations.insert(resolution.remotePath.Bytes()).second
                                         : localDestinations.insert(LocalIdentityKey(resolution.localPath)).second;

      if (!uniqueDestination)
      {
        return std::unexpected(QueueError(
            RemoteErrorCode::InvalidArgument,
            "Recursive transfer destinations must not collide"));
      }
    }

    return {};
  }

  Result<TransferDestinationOverride> ResolveTransferPaths(
      const TransferJob &job,
      const std::filesystem::path &originalLocalPath,
      const RemotePath &originalRemotePath)
  {
    if (!job.recursive && !job.destinationOverrides.empty())
    {
      return std::unexpected(QueueError(
          RemoteErrorCode::InvalidArgument,
          "Only recursive transfers may contain destination overrides"));
    }

    TransferDestinationOverride identity{originalLocalPath, originalRemotePath,
                                         originalLocalPath, originalRemotePath};

    if (auto valid = ValidateDestinationResolution(job, identity); !valid)
    {
      return std::unexpected(valid.error());
    }

    const auto found = std::ranges::find_if(job.destinationOverrides,
                                            [&](const auto &resolution)
                                            {
                                              return resolution.originalLocalPath == originalLocalPath &&
                                                     resolution.originalRemotePath == originalRemotePath;
                                            });

    if (found == job.destinationOverrides.end())
    {
      return identity;
    }

    if (auto valid = ValidateDestinationResolution(job, *found); !valid)
    {
      return std::unexpected(valid.error());
    }

    return *found;
  }

  bool IsTransferTransitionAllowed(TransferState from, TransferState to) noexcept
  {
    switch (from)
    {
    case TransferState::Queued:
      return to == TransferState::Enumerating || to == TransferState::Paused ||
             to == TransferState::Cancelled;

    case TransferState::Enumerating:
      return to == TransferState::Running || to == TransferState::Paused ||
             to == TransferState::Failed || to == TransferState::Cancelled;

    case TransferState::Running:
      return to == TransferState::Paused || to == TransferState::Completed ||
             to == TransferState::Failed || to == TransferState::Cancelled;

    case TransferState::Paused:
      return to == TransferState::Queued || to == TransferState::Cancelled;

    case TransferState::Failed:
      return to == TransferState::Queued || to == TransferState::Cancelled;

    case TransferState::Completed:
    case TransferState::Cancelled:
      return false;
    }

    return false;
  }

  TransferActionAvailability AvailableTransferActions(
      const TransferState state,
      const TransferControl requestedControl) noexcept
  {
    TransferActionAvailability result{
        .pause = IsTransferTransitionAllowed(state, TransferState::Paused),
        .cancel = IsTransferTransitionAllowed(state, TransferState::Cancelled),
        .retry = IsTransferTransitionAllowed(state, TransferState::Queued),
    };

    if (state != TransferState::Queued &&
        state != TransferState::Enumerating &&
        state != TransferState::Running)
    {
      return result;
    }

    if (requestedControl == TransferControl::Cancel)
    {
      return {};
    }

    if (requestedControl == TransferControl::Pause)
    {
      result.pause = false;
    }

    return result;
  }

  Result<std::string> TransferQueue::Enqueue(TransferJob job)
  {
    return EnqueueWithState(std::move(job), TransferState::Queued);
  }

  Result<std::string> TransferQueue::EnqueuePaused(TransferJob job)
  {
    return EnqueueWithState(std::move(job), TransferState::Paused);
  }

  Result<std::string> TransferQueue::EnqueueWithState(
      TransferJob job,
      const TransferState initialState)
  {
    if (job.localPath.empty() || job.remotePath.Empty())
    {
      return std::unexpected(QueueError(RemoteErrorCode::InvalidArgument,
                                        "A transfer requires local and remote paths"));
    }

    if (auto valid = ValidateTransferDestinations(job); !valid)
    {
      return std::unexpected(valid.error());
    }

    if (job.id.empty())
    {
      job.id = GenerateId();
    }

    std::scoped_lock lock{mMutex};

    const auto duplicate = std::ranges::find(mJobs, job.id, [](const auto &item)
                                             { return item.job.id; });
    if (duplicate != mJobs.end())
    {
      return std::unexpected(QueueError(RemoteErrorCode::Conflict,
                                        "A transfer with that id already exists"));
    }

    const auto id = job.id;

    QueuedTransfer queued;
    queued.job = std::move(job);
    queued.state = initialState;
    queued.progress.jobId = id;

    mJobs.push_back(std::move(queued));

    return id;
  }

  Result<void> TransferQueue::Restore(std::vector<QueuedTransfer> transfers)
  {
    std::unordered_set<std::string> restoredIds;
    restoredIds.reserve(transfers.size());

    for (auto &transfer : transfers)
    {
      if (auto normalized = NormalizeRestoredTransfer(transfer); !normalized)
      {
        return std::unexpected(normalized.error());
      }

      if (!restoredIds.insert(transfer.job.id).second)
      {
        return std::unexpected(QueueError(
            RemoteErrorCode::Conflict,
            "A restored transfer snapshot contains duplicate job ids"));
      }
    }

    std::scoped_lock lock{mMutex};

    for (const auto &transfer : transfers)
    {
      const auto duplicate = std::ranges::find(
          mJobs, transfer.job.id, [](const auto &item)
          { return std::string_view{item.job.id}; });

      if (duplicate != mJobs.end())
      {
        return std::unexpected(QueueError(
            RemoteErrorCode::Conflict,
            "A transfer with that id already exists"));
      }
    }

    mJobs.insert(mJobs.end(),
                 std::make_move_iterator(transfers.begin()),
                 std::make_move_iterator(transfers.end()));

    return {};
  }

  std::optional<QueuedTransfer> TransferQueue::Get(std::string_view id) const
  {
    std::scoped_lock lock{mMutex};

    const auto found = std::ranges::find(mJobs, id, [](const auto &item)
                                         { return std::string_view{item.job.id}; });

    return found == mJobs.end() ? std::nullopt : std::optional<QueuedTransfer>{*found};
  }

  std::vector<QueuedTransfer> TransferQueue::Snapshot() const
  {
    std::scoped_lock lock{mMutex};
    return mJobs;
  }

  std::optional<TransferJob> TransferQueue::TakeNext(
      const std::string_view siteId,
      const std::optional<SiteEndpointIdentity> endpoint)
  {
    std::scoped_lock lock{mMutex};

    const auto found = std::ranges::find_if(mJobs, [siteId, &endpoint](const auto &item)
                                            { return item.state == TransferState::Queued &&
                                                     (siteId.empty() || item.job.siteId == siteId) &&
                                                     (!endpoint || item.job.siteEndpoint == endpoint); });

    if (found == mJobs.end())
    {
      return std::nullopt;
    }

    found->state = TransferState::Enumerating;

    ++found->attempt;

    found->error.reset();

    return found->job;
  }

  Result<TransferJob> TransferQueue::ResolveDestination(
      const std::string_view jobId,
      const TransferDestinationOverride &resolution)
  {
    std::scoped_lock lock{mMutex};

    const auto found = std::ranges::find(mJobs, jobId, [](const auto &item)
                                         { return std::string_view{item.job.id}; });

    if (found == mJobs.end())
    {
      return std::unexpected(QueueError(RemoteErrorCode::NotFound, "Transfer not found"));
    }

    if (found->state != TransferState::Enumerating &&
        found->state != TransferState::Running)
    {
      const auto code = found->state == TransferState::Paused
                            ? RemoteErrorCode::Paused
                        : found->state == TransferState::Cancelled
                            ? RemoteErrorCode::Cancelled
                            : RemoteErrorCode::Conflict;

      return std::unexpected(QueueError(
          code, "Only an active transfer may resolve its destination"));
    }

    if (auto valid = ValidateDestinationResolution(found->job, resolution); !valid)
    {
      return std::unexpected(valid.error());
    }

    auto updated = found->job;
    if (updated.recursive)
    {
      const auto previous = std::ranges::find_if(updated.destinationOverrides,
                                                 [&](const auto &item)
                                                 {
                                                   return item.originalLocalPath == resolution.originalLocalPath &&
                                                          item.originalRemotePath == resolution.originalRemotePath;
                                                 });

      if (previous == updated.destinationOverrides.end())
      {
        updated.destinationOverrides.push_back(resolution);
      }
      else
      {
        *previous = resolution;
      }
    }
    else
    {
      updated.localPath = resolution.localPath;
      updated.remotePath = resolution.remotePath;
    }

    if (auto valid = ValidateTransferDestinations(updated); !valid)
    {
      return std::unexpected(valid.error());
    }

    found->job = updated;

    return updated;
  }

  Result<void> TransferQueue::Transition(std::string_view id, TransferState target)
  {
    std::scoped_lock lock{mMutex};

    const auto found = std::ranges::find(mJobs, id, [](const auto &item)
                                         { return std::string_view{item.job.id}; });

    if (found == mJobs.end())
    {
      return std::unexpected(QueueError(RemoteErrorCode::NotFound, "Transfer not found"));
    }

    if (found->state == target)
    {
      return {};
    }

    if (!IsTransferTransitionAllowed(found->state, target))
    {
      return std::unexpected(QueueError(RemoteErrorCode::Conflict,
                                        "Invalid transfer state transition"));
    }

    found->state = target;

    return {};
  }

  Result<void> TransferQueue::MarkEnumerating(std::string_view id)
  {
    return Transition(id, TransferState::Enumerating);
  }

  Result<void> TransferQueue::MarkRunning(std::string_view id)
  {
    return Transition(id, TransferState::Running);
  }

  Result<void> TransferQueue::UpdateProgress(const TransferProgress &progress)
  {
    std::scoped_lock lock{mMutex};

    const auto found = std::ranges::find(mJobs, std::string_view{progress.jobId},
                                         [](const auto &item)
                                         {
                                           return std::string_view{item.job.id};
                                         });

    if (found == mJobs.end())
    {
      return std::unexpected(QueueError(RemoteErrorCode::NotFound, "Transfer not found"));
    }

    if (found->state != TransferState::Running && found->state != TransferState::Enumerating)
    {
      return std::unexpected(QueueError(RemoteErrorCode::Conflict,
                                        "Progress is only valid for an active transfer"));
    }

    if (progress.bytesTransferred < found->progress.bytesTransferred)
    {
      return std::unexpected(QueueError(RemoteErrorCode::InvalidArgument,
                                        "Transfer progress cannot move backwards"));
    }

    if (progress.totalBytes && progress.bytesTransferred > *progress.totalBytes)
    {
      return std::unexpected(QueueError(RemoteErrorCode::InvalidArgument,
                                        "Transfer progress exceeds the total size"));
    }

    if (progress.activeBytesTransferred > progress.bytesTransferred)
    {
      return std::unexpected(QueueError(
          RemoteErrorCode::InvalidArgument,
          "Active transfer bytes exceed logical progress"));
    }

    if (progress.activeBytesTransferred <
            found->progress.activeBytesTransferred ||
        progress.elapsed < found->progress.elapsed)
    {
      return std::unexpected(QueueError(
          RemoteErrorCode::InvalidArgument,
          "Active transfer measurements cannot move backwards"));
    }

    found->progress = progress;

    return {};
  }

  Result<void> TransferQueue::Pause(std::string_view id)
  {
    return Transition(id, TransferState::Paused);
  }

  Result<bool> TransferQueue::PauseIfQueued(const std::string_view id)
  {
    std::scoped_lock lock{mMutex};

    const auto found = std::ranges::find(mJobs, id, [](const auto &item)
                                         { return std::string_view{item.job.id}; });

    if (found == mJobs.end())
    {
      return std::unexpected(
          QueueError(RemoteErrorCode::NotFound, "Transfer not found"));
    }

    if (found->state != TransferState::Queued)
    {
      return false;
    }

    found->state = TransferState::Paused;

    return true;
  }

  Result<void> TransferQueue::Complete(std::string_view id)
  {
    return Transition(id, TransferState::Completed);
  }

  Result<void> TransferQueue::Fail(std::string_view id, RemoteError error)
  {
    std::scoped_lock lock{mMutex};

    const auto found = std::ranges::find(mJobs, id, [](const auto &item)
                                         { return std::string_view{item.job.id}; });

    if (found == mJobs.end())
    {
      return std::unexpected(QueueError(RemoteErrorCode::NotFound, "Transfer not found"));
    }

    if (!IsTransferTransitionAllowed(found->state, TransferState::Failed))
    {
      return std::unexpected(QueueError(RemoteErrorCode::Conflict,
                                        "Invalid transfer state transition"));
    }

    found->state = TransferState::Failed;
    found->error = std::move(error);

    return {};
  }

  Result<void> TransferQueue::Cancel(std::string_view id)
  {
    return Transition(id, TransferState::Cancelled);
  }

  Result<bool> TransferQueue::CancelIfInactive(const std::string_view id)
  {
    std::scoped_lock lock{mMutex};

    const auto found = std::ranges::find(mJobs, id, [](const auto &item)
                                         { return std::string_view{item.job.id}; });

    if (found == mJobs.end())
    {
      return std::unexpected(
          QueueError(RemoteErrorCode::NotFound, "Transfer not found"));
    }

    if (found->state != TransferState::Queued &&
        found->state != TransferState::Paused &&
        found->state != TransferState::Failed)
    {
      return false;
    }

    found->state = TransferState::Cancelled;

    return true;
  }

  Result<void> TransferQueue::Retry(std::string_view id)
  {
    std::scoped_lock lock{mMutex};

    const auto found = std::ranges::find(mJobs, id, [](const auto &item)
                                         { return std::string_view{item.job.id}; });

    if (found == mJobs.end())
    {
      return std::unexpected(QueueError(RemoteErrorCode::NotFound, "Transfer not found"));
    }

    if (!IsTransferTransitionAllowed(found->state, TransferState::Queued))
    {
      return std::unexpected(QueueError(RemoteErrorCode::Conflict,
                                        "Invalid transfer state transition"));
    }

    found->state = TransferState::Queued;
    found->error.reset();
    found->progress = TransferProgress{.jobId = found->job.id,
                                       .bytesTransferred = 0,
                                       .totalBytes = std::nullopt,
                                       .elapsed = {}};

    return {};
  }

  Result<void> TransferQueue::RemoveTerminal(const std::string_view id)
  {
    const std::array ids{std::string{id}};
    return RemoveTerminal(std::span<const std::string>{ids});
  }

  Result<void> TransferQueue::RemoveTerminal(
      const std::span<const std::string> ids)
  {
    if (ids.empty())
    {
      return {};
    }

    std::unordered_set<std::string_view> uniqueIds;
    uniqueIds.reserve(ids.size());

    for (const auto &id : ids)
    {
      if (id.empty())
      {
        return std::unexpected(QueueError(
            RemoteErrorCode::InvalidArgument,
            "A transfer id cannot be empty"));
      }

      if (!uniqueIds.insert(id).second)
      {
        return std::unexpected(QueueError(
            RemoteErrorCode::InvalidArgument,
            "A transfer cannot be removed more than once"));
      }
    }

    std::scoped_lock lock{mMutex};

    for (const auto id : uniqueIds)
    {
      const auto found = std::ranges::find(
          mJobs, id, [](const auto &item)
          { return std::string_view{item.job.id}; });

      if (found == mJobs.end())
      {
        return std::unexpected(QueueError(
            RemoteErrorCode::NotFound, "Transfer not found"));
      }

      if (found->state != TransferState::Completed &&
          found->state != TransferState::Failed &&
          found->state != TransferState::Cancelled)
      {
        return std::unexpected(QueueError(
            RemoteErrorCode::Conflict,
            "Only completed, failed, or canceled transfers can be removed"));
      }
    }

    std::erase_if(mJobs, [&](const QueuedTransfer &item)
                  { return uniqueIds.contains(item.job.id); });

    return {};
  }
} // namespace havremote
