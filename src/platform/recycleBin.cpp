// SPDX-License-Identifier: MIT

#include "platform/recycleBin.hpp"
#include "recycleBinInternal.hpp"

#include <string>
#include <utility>
#include <vector>

namespace havremote::platform
{
  namespace
  {
    Result<std::filesystem::path> PreparePath(const std::filesystem::path &input)
    {
      if (input.empty() || input.native().find(std::filesystem::path::value_type{}) !=
                               std::filesystem::path::string_type::npos)
      {
        return std::unexpected(PlatformError{
            PlatformErrorCode::InvalidArgument, "The selected path is invalid"});
      }

      auto selected = input;

      while (selected.has_relative_path() && selected.filename().empty())
      {
        selected = selected.parent_path();
      }

      if (selected.filename() == "." || selected.filename() == "..")
      {
        return std::unexpected(PlatformError{
            PlatformErrorCode::InvalidArgument, "Select an item, not a dot entry"});
      }

      std::error_code error;

      auto path = std::filesystem::absolute(selected, error);

      if (error)
      {
        return std::unexpected(PlatformError{
            PlatformErrorCode::InvalidArgument,
            "Could not resolve the selected path: " + error.message(),
            static_cast<unsigned long>(error.value())});
      }

      while (path.has_relative_path() && path.filename().empty())
      {
        path = path.parent_path();
      }

      if (!path.lexically_normal().has_relative_path() ||
          path.filename() == "." || path.filename() == "..")
      {
        return std::unexpected(PlatformError{
            PlatformErrorCode::InvalidArgument,
            "Select a file or directory, not a filesystem root or a dot entry"});
      }

      // Resolve only the parent. Native APIs may otherwise collapse ".."
      // differently from filesystem traversal through a directory link.
      const auto parent = std::filesystem::canonical(path.parent_path(), error);

      if (error)
      {
        const auto code = error == std::errc::permission_denied
                              ? PlatformErrorCode::AccessDenied
                          : error == std::errc::no_such_file_or_directory ||
                                  error == std::errc::not_a_directory
                              ? PlatformErrorCode::NotFound
                              : PlatformErrorCode::OperatingSystem;

        return std::unexpected(PlatformError{
            code, "Could not resolve the selected item's directory: " + error.message(),
            static_cast<unsigned long>(error.value())});
      }

      path = parent / path.filename();

      // Inspect the entry itself so dangling links can be trashed too
      const auto status = std::filesystem::symlink_status(path, error);

      if (status.type() == std::filesystem::file_type::not_found ||
          error == std::errc::no_such_file_or_directory)
      {
        return std::unexpected(PlatformError{
            PlatformErrorCode::NotFound, "The selected item no longer exists",
            static_cast<unsigned long>(error.value())});
      }

      if (error)
      {
        return std::unexpected(PlatformError{
            error == std::errc::permission_denied ? PlatformErrorCode::AccessDenied
                                                  : PlatformErrorCode::OperatingSystem,
            "Could not inspect the selected item: " + error.message(),
            static_cast<unsigned long>(error.value())});
      }

      if (!std::filesystem::is_regular_file(status) &&
          !std::filesystem::is_directory(status) &&
          !std::filesystem::is_symlink(status))
      {
        return std::unexpected(PlatformError{
            PlatformErrorCode::UnsafeFileType,
            "Only files, directories, and symbolic links can be moved to Trash"});
      }

      return path;
    }

    class RecycleBin final : public IRecycleBin
    {
    public:
      Result<void> MoveToRecycleBin(
          const std::span<const std::filesystem::path> paths) override
      {
        std::vector<std::filesystem::path> prepared;
        prepared.reserve(paths.size());

        for (const auto &path : paths)
        {
          auto validated = PreparePath(path);
          if (!validated)
          {
            return std::unexpected(validated.error());
          }

          prepared.push_back(std::move(*validated));
        }

        if (prepared.empty())
        {
          return {};
        }

        return detail::MoveToSystemTrash(prepared);
      }
    };
  }

  std::unique_ptr<IRecycleBin> MakeRecycleBin()
  {
    return std::make_unique<RecycleBin>();
  }
}
