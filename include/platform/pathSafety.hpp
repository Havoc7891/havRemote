// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_PLATFORM_PATH_SAFETY_HPP
#define HAVREMOTE_INCLUDE_PLATFORM_PATH_SAFETY_HPP

#include "core/types.hpp"
#include "platform/platformError.hpp"

#include <filesystem>
#include <memory>
#include <string_view>

namespace havremote::platform
{
  namespace detail
  {
    class DestinationNames;
  }

  // Checks relative remote paths and destination-native name collisions.
  // Temporary name probes are removed when the mapper is destroyed.
  class SafeDownloadMapper final
  {
  public:
    explicit SafeDownloadMapper(std::filesystem::path destinationRoot);
    ~SafeDownloadMapper();
    SafeDownloadMapper(SafeDownloadMapper &&) noexcept;
    SafeDownloadMapper &operator=(SafeDownloadMapper &&) noexcept;
    SafeDownloadMapper(const SafeDownloadMapper &) = delete;
    SafeDownloadMapper &operator=(const SafeDownloadMapper &) = delete;

    [[nodiscard]] Result<std::filesystem::path> Map(const RemotePath &relativePath);

  private:
    std::filesystem::path mDestinationRoot;
    std::unique_ptr<detail::DestinationNames> mNames;
  };

  // Basic host syntax and UTF-8 validation. The filesystem enforces its limits.
  [[nodiscard]] bool IsLocalSafeFilename(std::string_view utf8Name) noexcept;
  [[nodiscard]] bool IsRemotePathWithin(const RemotePath &root,
                                        const RemotePath &candidate) noexcept;

  // Verifies lexical containment and rejects every existing symlink, junction,
  // or other reparse component from the selected root through the write target.
  // This is a pre-write check, not a lock against concurrent directory changes.
  [[nodiscard]] Result<void> ValidateLocalWriteTarget(
      const std::filesystem::path &destinationRoot,
      const std::filesystem::path &target);

  [[nodiscard]] havremote::Result<bool> CheckLocalPathConflict(
      const std::filesystem::path &left, const std::filesystem::path &right);
} // namespace havremote::platform

#endif // HAVREMOTE_INCLUDE_PLATFORM_PATH_SAFETY_HPP
