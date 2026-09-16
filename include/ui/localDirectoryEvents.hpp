// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UI_LOCAL_DIRECTORY_EVENTS_HPP
#define HAVREMOTE_INCLUDE_UI_LOCAL_DIRECTORY_EVENTS_HPP

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace havremote::ui
{
  enum class LocalEntryKind
  {
    File,
    Directory,
    Symlink,
    Other,
  };

  enum class LocalDirectoryOperation
  {
    OpenDirectory,
    AdvanceIterator,
    ReadStatus,
    ReadSize,
    ReadModifiedTime,
    Enumerate,
  };

  struct LocalDirectoryIssue final
  {
    std::filesystem::path path;
    LocalDirectoryOperation operation{LocalDirectoryOperation::Enumerate};
    std::error_code error;
    std::string message;
  };

  struct LocalDirectoryEntrySnapshot final
  {
    std::filesystem::path path;
    // Captured with symlink_status(). Consumers must not use status() to
    // classify this entry because that would follow a symbolic link.
    std::filesystem::file_status symlinkStatus;
    LocalEntryKind kind{LocalEntryKind::Other};
    std::optional<std::uint64_t> exactSize;
    std::optional<std::chrono::system_clock::time_point> modifiedAt;
  };

  struct LocalDirectoryResult final
  {
    // Runtime identity of the connection tab that owns the loader. This is
    // required in addition to generation: separate loaders may legitimately
    // produce the same generation value.
    std::string connectionId;
    std::uint64_t generation{};
    std::filesystem::path directory;
    std::vector<LocalDirectoryEntrySnapshot> entries;
    std::vector<LocalDirectoryIssue> warnings;
    // Set only when the directory could not be enumerated as a whole. Errors
    // affecting one entry are retained in warnings instead.
    std::optional<LocalDirectoryIssue> error;
  };

  // The const pointer makes a posted result immutable while wxWidgets transfers
  // ownership of its lightweight shared payload to the event thread.
  using LocalDirectoryResultPtr = std::shared_ptr<const LocalDirectoryResult>;
} // namespace havremote::ui

#endif // HAVREMOTE_INCLUDE_UI_LOCAL_DIRECTORY_EVENTS_HPP
