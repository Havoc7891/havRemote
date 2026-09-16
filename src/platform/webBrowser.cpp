// SPDX-License-Identifier: MIT

#include "platform/webBrowser.hpp"

#include <wx/defs.h>

#if wxUSE_GUI
#include <wx/log.h>
#include <wx/utils.h>
#endif

#include <algorithm>

namespace havremote::platform
{
  namespace
  {
    constexpr std::string_view HttpsScheme = "https://";

    bool ValidHttpsUrl(const std::string_view url) noexcept
    {
      if (!url.starts_with(HttpsScheme) || url.size() > 4096U)
      {
        return false;
      }

      const auto authorityEnd = url.find_first_of("/?#", HttpsScheme.size());
      const auto authority = url.substr(
          HttpsScheme.size(),
          authorityEnd == std::string_view::npos
              ? std::string_view::npos
              : authorityEnd - HttpsScheme.size());

      if (authority.empty() || authority.contains('@'))
      {
        return false;
      }

      return std::ranges::none_of(
          url,
          [](const unsigned char character)
          {
            return character <= 0x20U || character == 0x7fU ||
                   character == '\\';
          });
    }

    bool LaunchWithDefaultBrowser(const std::string_view url)
    {
#if wxUSE_GUI
      wxLogNull suppressLaunchErrors;

      return wxLaunchDefaultBrowser(
          wxString::FromUTF8(url.data(), url.size()));
#else
      (void)url;

      return false;
#endif
    }
  } // namespace

  Result<void> LaunchHttpsUrl(
      const std::string_view url,
      const WebBrowserLaunchFunction &launch)
  {
    if (!ValidHttpsUrl(url))
    {
      return std::unexpected(PlatformError{
          PlatformErrorCode::InvalidArgument,
          "The browser URL must be a safe HTTPS URL",
          0});
    }

    if (launch ? launch(url) : LaunchWithDefaultBrowser(url))
    {
      return {};
    }

    return std::unexpected(PlatformError{
        PlatformErrorCode::Unavailable,
        "The link could not be opened in the default browser",
        0});
  }
} // namespace havremote::platform
