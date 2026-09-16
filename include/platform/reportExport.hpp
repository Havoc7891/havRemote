// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_PLATFORM_REPORT_EXPORT_HPP
#define HAVREMOTE_INCLUDE_PLATFORM_REPORT_EXPORT_HPP

#include "platform/platformError.hpp"

#include <filesystem>
#include <span>
#include <string_view>

namespace havremote::platform
{
  // Protects application state from accidental export overwrites, including
  // relative names, existing aliases/hard links, and platform-specific case folding.
  // An unresolvable path is considered protected rather than guessed safe.
  [[nodiscard]] bool IsProtectedExportPath(
      const std::filesystem::path &target,
      std::span<const std::filesystem::path> protectedPaths);

  // Writes report bytes to an exclusively created sibling temporary file,
  // then atomically replaces the selected regular file. The caller owns any
  // overwrite confirmation. Empty reports and unsafe paths are rejected.
  // Directories are never created and failed writes retain the old report.
  [[nodiscard]] Result<void> WriteReportAtomic(
      const std::filesystem::path &path, std::string_view utf8Text);
} // namespace havremote::platform

#endif // HAVREMOTE_INCLUDE_PLATFORM_REPORT_EXPORT_HPP
