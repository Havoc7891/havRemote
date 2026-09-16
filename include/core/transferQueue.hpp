// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_CORE_TRANSFER_QUEUE_HPP
#define HAVREMOTE_INCLUDE_CORE_TRANSFER_QUEUE_HPP

#include "core/types.hpp"

#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace havremote
{
  struct QueuedTransfer final
  {
    TransferJob job;
    TransferState state{TransferState::Queued};
    TransferProgress progress;
    std::optional<RemoteError> error;
    std::uint32_t attempt{};
  };

  struct TransferActionAvailability final
  {
    bool pause{};
    bool cancel{};
    bool retry{};

    friend bool operator==(const TransferActionAvailability &,
                           const TransferActionAvailability &) = default;
  };

  // Thread-safe queue state model. Controllers schedule the workers.
  class TransferQueue final
  {
  public:
    [[nodiscard]] Result<std::string> Enqueue(TransferJob job);
    // Inserts work inertly so a caller can durably record it before making it
    // eligible for a worker. Retry() performs the later activation.
    [[nodiscard]] Result<std::string> EnqueuePaused(TransferJob job);
    // Restores one atomic persistence snapshot. Active work is made paused so
    // no interrupted operation can resume without an explicit retry.
    [[nodiscard]] Result<void> Restore(std::vector<QueuedTransfer> transfers);
    [[nodiscard]] std::optional<QueuedTransfer> Get(std::string_view id) const;
    [[nodiscard]] std::vector<QueuedTransfer> Snapshot() const;
    // An empty site ID disables site filtering
    [[nodiscard]] std::optional<TransferJob> TakeNext(
        std::string_view siteId = {},
        std::optional<SiteEndpointIdentity> endpoint = std::nullopt);

    // Commits a conflict rename to an active job, preserving its source and
    // ownership. Single-file jobs use the resolved destination as their
    // canonical path. Recursive jobs retain an exact per-file mapping.
    [[nodiscard]] Result<TransferJob> ResolveDestination(
        std::string_view jobId,
        const TransferDestinationOverride &resolution);

    Result<void> MarkEnumerating(std::string_view id);
    Result<void> MarkRunning(std::string_view id);
    Result<void> UpdateProgress(const TransferProgress &progress);
    Result<void> Pause(std::string_view id);
    [[nodiscard]] Result<bool> PauseIfQueued(std::string_view id);
    Result<void> Complete(std::string_view id);
    Result<void> Fail(std::string_view id, RemoteError error);
    Result<void> Cancel(std::string_view id);
    [[nodiscard]] Result<bool> CancelIfInactive(std::string_view id);
    Result<void> Retry(std::string_view id);
    // Removes only completed, failed, or cancelled attempts. The bulk form is
    // atomic: if any ID is missing, duplicated, or still actionable, no job is
    // removed.
    Result<void> RemoveTerminal(std::string_view id);
    Result<void> RemoveTerminal(std::span<const std::string> ids);

  private:
    [[nodiscard]] Result<std::string> EnqueueWithState(
        TransferJob job,
        TransferState initialState);
    [[nodiscard]] Result<void> Transition(std::string_view id, TransferState target);

    mutable std::mutex mMutex;
    std::vector<QueuedTransfer> mJobs;
  };

  [[nodiscard]] bool IsTransferTransitionAllowed(TransferState from,
                                                 TransferState to) noexcept;
  [[nodiscard]] TransferActionAvailability AvailableTransferActions(
      TransferState state,
      TransferControl requestedControl = TransferControl::Continue) noexcept;

  // Pure lexical checks shared by queue mutation and persistence. Filesystem
  // link checks and destination reservations remain worker-owned.
  [[nodiscard]] Result<void> ValidateTransferDestinations(const TransferJob &job);
  // Resolves one file in an already validated job. Only that file's mapping is
  // rechecked here, avoiding repeated whole-tree validation during enumeration.
  [[nodiscard]] Result<TransferDestinationOverride> ResolveTransferPaths(
      const TransferJob &job,
      const std::filesystem::path &originalLocalPath,
      const RemotePath &originalRemotePath);
} // namespace havremote

#endif // HAVREMOTE_INCLUDE_CORE_TRANSFER_QUEUE_HPP
