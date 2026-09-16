// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_PLATFORM_EXTERNAL_EDITOR_HPP
#define HAVREMOTE_INCLUDE_PLATFORM_EXTERNAL_EDITOR_HPP

#include "config/configTypes.hpp"
#include "platform/platformError.hpp"

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace havremote::platform
{
  // Parses the user-facing argument template without involving a command shell.
  // Quotes group arguments and are removed. The file value always remains one
  // argv element even when it contains whitespace or command metacharacters.
  [[nodiscard]] Result<std::vector<std::string>> ExpandExternalEditorArguments(
      std::string_view argumentTemplate,
      std::string_view fileArgument);

  struct ExternalEditorLaunchFunctions final
  {
    std::function<bool(const std::filesystem::path &)> launchDefault;
    std::function<long(const std::filesystem::path &,
                       std::span<const std::string>)>
        launchCustom;
  };

  // Validates both paths before calling the selected asynchronous launcher.
  // Empty launch functions use wxWidgets' native implementations.
  [[nodiscard]] Result<void> LaunchExternalEditor(
      const config::ExternalEditorSettings &settings,
      const std::filesystem::path &file,
      const ExternalEditorLaunchFunctions &launchFunctions = {});
} // namespace havremote::platform

#endif // HAVREMOTE_INCLUDE_PLATFORM_EXTERNAL_EDITOR_HPP
