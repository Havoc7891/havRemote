// SPDX-License-Identifier: MIT

#include "recycleBinInternal.hpp"

#include <wx/filefn.h>
#include <wx/log.h>
#include <wx/strconv.h>

#include <utility>

#ifndef wxHAS_MOVE_TO_TRASH
#error "havRemote requires wxWidgets with wxMoveToTrash support"
#endif

namespace havremote::platform::detail
{
  Result<void> MoveToSystemTrash(
      const std::span<const std::filesystem::path> paths)
  {
    for (const auto &path : paths)
    {
#if defined(_WIN32)
      const wxString nativePath{path.native()};
#else
      const wxString nativePath{path.c_str(), wxConvFile};
#endif

#if defined(__WXGTK__)
      // wxMoveToTrash passes UTF-8 to GIO, even with a legacy filename encoding.
      if (nativePath.ToStdString(wxConvUTF8) != path.native())
      {
        return std::unexpected(PlatformError{
            PlatformErrorCode::InvalidArgument,
            "The selected filename cannot be passed unchanged to the Trash"});
      }
#endif

      if (nativePath.empty())
      {
        return std::unexpected(PlatformError{
            PlatformErrorCode::InvalidArgument,
            "The selected path could not be converted for the filesystem"});
      }

      // The caller shows failures. Collect wxWidgets errors to avoid a second dialog.
      wxLogCollector errors;

      if (!wxMoveToTrash(nativePath))
      {
        auto message = errors.GetMessages().ToStdString(wxConvUTF8);

        if (message.empty())
        {
          message = "Could not move '" + nativePath.ToStdString(wxConvUTF8) +
                    "' to the Trash or Recycle Bin";
        }

        return std::unexpected(PlatformError{
            PlatformErrorCode::OperatingSystem, std::move(message)});
      }
    }

    return {};
  }
} // namespace havremote::platform::detail
