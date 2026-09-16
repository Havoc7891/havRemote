// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_CONFIG_QUEUE_TYPES_HPP
#define HAVREMOTE_INCLUDE_CONFIG_QUEUE_TYPES_HPP

#include "core/transferQueue.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace havremote::config
{
  inline constexpr std::uint32_t CurrentQueueFormatVersion = 1;

  enum class QueueResumeKind
  {
    Download,
    Upload,
  };

  // A single transfer job can contain many files, so resume information is
  // stored per partially transferred file rather than on TransferJob itself.
  // Fields which do not apply to the selected kind retain their default value.
  struct QueueResumeRecord final
  {
    QueueResumeKind kind{QueueResumeKind::Download};
    std::filesystem::path localPath;
    RemotePath remotePath;

    // Download source and sibling .havremote.part metadata
    std::uint64_t remoteSize{};
    std::optional<std::chrono::system_clock::time_point> remoteModifiedAt;
    std::uint64_t partSize{};
    std::optional<std::filesystem::file_time_type> partModifiedAt;

    // Upload source and remote temporary destination metadata
    std::uint64_t localSize{};
    std::optional<std::filesystem::file_time_type> localModifiedAt;
    std::optional<RemotePath> temporaryRemotePath;
  };

  struct PersistentQueueItem final
  {
    // Owning controller/tab identity used for UI routing after recovery
    std::string connectionId;
    QueuedTransfer transfer;
    std::vector<QueueResumeRecord> resumeRecords;
  };

  struct QueueLoadResult final
  {
    std::vector<PersistentQueueItem> items;
  };
} // namespace havremote::config

#endif // HAVREMOTE_INCLUDE_CONFIG_QUEUE_TYPES_HPP
