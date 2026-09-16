// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UI_REMOTE_CONTROLLER_HPP
#define HAVREMOTE_INCLUDE_UI_REMOTE_CONTROLLER_HPP

#include "config/queueTypes.hpp"
#include "core/transferRuntime.hpp"
#include "ui/controllerEvents.hpp"

#include <wx/event.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace havremote::ui
{
  enum class ConflictReason
  {
    DestinationExists,
    RemoteChanged,
  };

  struct ConflictChallenge final
  {
    TransferJob job;
    std::optional<std::uint64_t> existingSize;
    bool safeResumeAvailable{};
    bool applyToRemainingAvailable{true};
    ConflictReason reason{ConflictReason::DestinationExists};
  };

  struct ConflictResolution final
  {
    ConflictPolicy policy{ConflictPolicy::Skip};
    bool applyToRemainingQueue{};
  };

  struct UiInteractions final
  {
    // RemoteController marshals each function to the wx event thread and waits
    // without blocking cancellation. Implementations may safely show dialogs.
    std::function<Result<std::string>(const CredentialRequest &)> requestCredential;
    std::function<Result<TrustDecision>(const TrustChallenge &)> verifyTrust;
    std::function<Result<ConflictResolution>(const ConflictChallenge &)> resolveConflict;
    // Called by Connect() on the wx thread. The returned endpoint-scoped PIN
    // is copied into each FTPS worker's immutable session callbacks.
    std::function<std::optional<std::string>(std::string_view, std::uint16_t)>
        tlsPinnedPublicKey;
    // Invoked on the wx event thread. Worker-side callers wait for this
    // result before starting an operation that requires durable recovery
    // metadata.
    std::function<Result<void>(const std::string &)> persistQueue;
  };

  wxDECLARE_EVENT(EVT_HAVREMOTE_CONTROLLER, wxThreadEvent);

  class RemoteController final
  {
  public:
    RemoteController(wxEvtHandler &eventTarget,
                     std::string connectionId,
                     std::shared_ptr<TransferRuntime> transferRuntime,
                     std::filesystem::path knownHostsFile,
                     UiInteractions interactions,
                     std::chrono::seconds connectionTimeout = std::chrono::seconds{20},
                     std::chrono::seconds commandIdleTimeout = std::chrono::seconds{60},
                     std::vector<config::PersistentQueueItem> restoredItems = {});
    ~RemoteController();

    RemoteController(const RemoteController &) = delete;
    RemoteController &operator=(const RemoteController &) = delete;

    [[nodiscard]] std::uint64_t Connect(SiteProfile site);
    void Disconnect();
    void Browse(RemotePath directory);
    void Refresh();
    void Inspect(RemotePath path, std::string requestId);
    void CreateRemoteDirectory(RemotePath directory, bool recursive = false);
    void CreateRemoteFile(RemotePath path);
    void Rename(RemotePath source, RemotePath destination, bool overwrite = false);
    void Remove(RemotePath path, bool recursive);
    void SetPermissions(std::vector<RemotePath> paths, std::uint32_t permissions);

    [[nodiscard]] Result<std::string> Enqueue(TransferJob job);
    [[nodiscard]] Result<void> Pause(std::string_view jobId);
    [[nodiscard]] Result<void> Cancel(std::string_view jobId);
    [[nodiscard]] Result<void> Retry(std::string_view jobId);
    [[nodiscard]] Result<void> RemoveTerminalTransfer(
        std::string_view jobId);
    [[nodiscard]] Result<void> RemoveTerminalTransfers(
        std::span<const std::string> jobIds);
    // Updates defaults used when a protocol session is next created. Existing
    // browser and transfer sessions keep the immutable timeouts with which
    // they connected, so callers do not interrupt active work.
    [[nodiscard]] Result<void> UpdateTimeouts(
        std::chrono::seconds connectionTimeout,
        std::chrono::seconds commandIdleTimeout);
    // Stops active work as paused, waits for final checkpoints, and leaves
    // failed work intact so MainFrame can perform its final atomic save.
    void PrepareForShutdown();
    [[nodiscard]] std::vector<QueuedTransfer> Transfers() const;
    [[nodiscard]] std::vector<config::PersistentQueueItem>
    PersistentQueueItems() const;
    [[nodiscard]] TransferActionAvailability TransferActions(
        std::string_view jobId) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
  };
} // namespace havremote::ui

#endif // HAVREMOTE_INCLUDE_UI_REMOTE_CONTROLLER_HPP
