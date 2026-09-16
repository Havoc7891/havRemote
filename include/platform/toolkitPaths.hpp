// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_PLATFORM_TOOLKIT_PATHS_HPP
#define HAVREMOTE_INCLUDE_PLATFORM_TOOLKIT_PATHS_HPP

#include <wx/string.h>
#include <filesystem>

namespace havremote::platform
{
  inline std::filesystem::path FromToolkitPath(const wxString &value)
  {
#ifdef _WIN32
    return std::filesystem::path{value.ToStdWstring()};
#else
    return std::filesystem::path{value.ToStdString(wxConvUTF8)};
#endif
  }

  inline wxString ToToolkitPath(const std::filesystem::path &value)
  {
#ifdef _WIN32
    return wxString{value.native()};
#else
    return wxString::FromUTF8(value.native());
#endif
  }
}

#endif // HAVREMOTE_INCLUDE_PLATFORM_TOOLKIT_PATHS_HPP
