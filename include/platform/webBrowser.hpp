// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_PLATFORM_WEB_BROWSER_HPP
#define HAVREMOTE_INCLUDE_PLATFORM_WEB_BROWSER_HPP

#include "platform/platformError.hpp"

#include <functional>
#include <string_view>

namespace havremote::platform
{
  using WebBrowserLaunchFunction = std::function<bool(std::string_view)>;

  // Opens an HTTPS URL without invoking a command shell
  [[nodiscard]] Result<void> LaunchHttpsUrl(
      std::string_view url,
      const WebBrowserLaunchFunction &launch = {});
} // namespace havremote::platform

#endif // HAVREMOTE_INCLUDE_PLATFORM_WEB_BROWSER_HPP
