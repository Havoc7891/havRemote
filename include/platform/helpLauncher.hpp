// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_PLATFORM_HELP_LAUNCHER_HPP
#define HAVREMOTE_INCLUDE_PLATFORM_HELP_LAUNCHER_HPP

#include "platform/platformError.hpp"

#include <filesystem>
#include <functional>
#include <string_view>
#include <system_error>

namespace havremote::platform
{
  struct HelpLaunchFunctions final
  {
    std::function<bool(const std::filesystem::path &, std::error_code &)>
        isRegularFile;
    std::function<bool(const std::filesystem::path &)> launchBrowser;
  };

  // Selects the localized help entry point without launching it. Language codes
  // use the same safe, case-insensitive format as translation catalogs: 1-35
  // ASCII alphanumeric characters separated by single hyphens. If the requested
  // language is not installed, English is used as the fallback.
  [[nodiscard]] Result<std::filesystem::path> ResolveHelpIndex(
      const std::filesystem::path &helpRoot,
      std::string_view selectedLanguage,
      const HelpLaunchFunctions &functions = {});

  // Resolves the localized help file and opens its file URL in the default
  // browser. Empty function objects use the production filesystem and platform
  // browser launcher.
  [[nodiscard]] Result<void> LaunchOfflineHelp(
      const std::filesystem::path &helpRoot,
      std::string_view selectedLanguage,
      const HelpLaunchFunctions &functions = {});
} // namespace havremote::platform

#endif // HAVREMOTE_INCLUDE_PLATFORM_HELP_LAUNCHER_HPP
