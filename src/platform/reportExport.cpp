// SPDX-License-Identifier: MIT

#include "platform/reportExport.hpp"

#include "core/types.hpp"
#include "platform/pathSafety.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <system_error>
#include <utility>

namespace havremote::platform
{
  namespace
  {
    PlatformError ReportError(const PlatformErrorCode code, std::string message,
                              const unsigned long nativeCode = 0)
    {
      return {code, std::move(message), nativeCode};
    }

    PlatformError IoError(std::string message, const int nativeCode)
    {
      return ReportError(PlatformErrorCode::Io,
                         std::move(message) + ": " +
                             std::error_code{nativeCode, std::system_category()}.message(),
                         static_cast<unsigned long>(nativeCode));
    }

    bool HasSafeExportSpelling(const std::filesystem::path &path)
    {
#if defined(_WIN32)
      try
      {
        auto preferred = path;
        preferred.make_preferred();

        const auto &native = preferred.native();

        // Device/extended namespaces bypass ordinary Win32 name handling.
        // Accept ordinary drive/UNC paths with the application manifest
        // providing long-path support, but never interpret device syntax.
        if (native.starts_with(L"\\\\?\\") || native.starts_with(L"\\\\.\\"))
        {
          return false;
        }

        for (const auto &component : preferred.relative_path())
        {
          if (component.empty() || component == "." || component == "..")
          {
            continue;
          }

          const auto utf8 = component.u8string();
          if (!IsLocalSafeFilename(std::string_view{
                  reinterpret_cast<const char *>(utf8.data()), utf8.size()}))
          {
            return false;
          }
        }
      }
      catch (const std::filesystem::filesystem_error &)
      {
        return false;
      }
#else
      (void)path;
#endif

      return true;
    }

    Result<void> CheckDestination(const std::filesystem::path &path)
    {
      std::error_code error;

      // Check ancestors as well: a symbolic link or directory redirection must
      // not send the report to an unrelated location.
      for (auto current = path; !current.empty(); current = current.parent_path())
      {
        const auto status = std::filesystem::symlink_status(current, error);

        if (current == path &&
            (error == std::errc::no_such_file_or_directory ||
             (!error && status.type() == std::filesystem::file_type::not_found)))
        {
          error.clear();

          continue;
        }

        if (error)
        {
          return std::unexpected(IoError("Could not inspect the report destination", error.value()));
        }

        if (std::filesystem::is_symlink(status))
        {
          return std::unexpected(ReportError(PlatformErrorCode::UnsafeFileType,
                                             "The report destination must not contain symbolic links"));
        }

#if defined(_WIN32)
        const auto attributes = GetFileAttributesW(current.c_str());

        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
          return std::unexpected(IoError("Could not inspect the report destination",
                                         static_cast<int>(GetLastError())));
        }

        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
          return std::unexpected(ReportError(PlatformErrorCode::UnsafeFileType,
                                             "The report destination must not contain reparse points"));
        }
#endif

        if (current == path ? !std::filesystem::is_regular_file(status)
                            : !std::filesystem::is_directory(status))
        {
          return std::unexpected(ReportError(PlatformErrorCode::UnsafeFileType,
                                             "The report requires a regular file in an existing directory"));
        }

        if (current == current.parent_path())
        {
          break;
        }
      }

      return {};
    }

    class TemporaryReport final
    {
    public:
      std::filesystem::path path;
#if defined(_WIN32)
      HANDLE handle{INVALID_HANDLE_VALUE};
#else
      int handle{-1};
#endif

      ~TemporaryReport()
      {
        (void)Close();

        if (!path.empty())
        {
          std::error_code ignored;
          std::filesystem::remove(path, ignored);
        }
      }

      int Close() noexcept
      {
#if defined(_WIN32)
        int error{};

        if (handle != INVALID_HANDLE_VALUE)
        {
          if (!CloseHandle(handle))
          {
            error = static_cast<int>(GetLastError());
          }
        }

        handle = INVALID_HANDLE_VALUE;
#else
        const auto error = handle >= 0 && ::close(handle) != 0 ? errno : 0;
        handle = -1;
#endif

        return error;
      }
    };

    std::filesystem::path NormalizedExportPath(const std::filesystem::path &path,
                                               std::error_code &error)
    {
      if (path.empty() || !HasSafeExportSpelling(path) ||
          path.native().find(std::filesystem::path::value_type{}) !=
              std::filesystem::path::string_type::npos)
      {
        error = std::make_error_code(std::errc::invalid_argument);

        return {};
      }

      const auto absolute = std::filesystem::absolute(path, error);

      if (error)
      {
        return {};
      }

      return std::filesystem::weakly_canonical(absolute, error).lexically_normal();
    }

    bool SameExportPath(const std::filesystem::path &left,
                        const std::filesystem::path &right)
    {
#if defined(_WIN32)
      constexpr auto maximum = static_cast<std::size_t>((std::numeric_limits<int>::max)());

      if (left.native().size() > maximum || right.native().size() > maximum)
      {
        return true;
      }

      const auto comparison = CompareStringOrdinal(
          left.c_str(), static_cast<int>(left.native().size()),
          right.c_str(), static_cast<int>(right.native().size()), TRUE);

      return comparison == 0 || comparison == CSTR_EQUAL;
#else
      return left == right;
#endif
    }
  } // namespace

  bool IsProtectedExportPath(
      const std::filesystem::path &target,
      const std::span<const std::filesystem::path> protectedPaths)
  {
    std::error_code error;

    const auto normalizedTarget = NormalizedExportPath(target, error);

    if (error)
    {
      return true;
    }

    const auto targetExists = std::filesystem::exists(normalizedTarget, error);

    if (error)
    {
      return true;
    }

    for (const auto &protectedPath : protectedPaths)
    {
      const auto normalizedProtected = NormalizedExportPath(protectedPath, error);

      if (error || SameExportPath(normalizedTarget, normalizedProtected))
      {
        return true;
      }

      const auto protectedExists = std::filesystem::exists(normalizedProtected, error);

      if (error)
      {
        return true;
      }

      if (targetExists && protectedExists)
      {
        const auto equivalent = std::filesystem::equivalent(normalizedTarget, normalizedProtected, error);

        if (error || equivalent)
        {
          return true;
        }
      }
    }

    return false;
  }

  Result<void> WriteReportAtomic(const std::filesystem::path &path,
                                 const std::string_view utf8Text)
  {
    if (utf8Text.empty() || path.empty() || !HasSafeExportSpelling(path) || path.filename().empty() ||
        path.filename() == "." || path.filename() == ".." ||
        path.native().find(std::filesystem::path::value_type{}) !=
            std::filesystem::path::string_type::npos)
    {
      return std::unexpected(ReportError(PlatformErrorCode::InvalidArgument,
                                         "Select a filename and a non-empty report"));
    }

    std::error_code error;

    const auto destination = std::filesystem::absolute(path, error);

    if (error)
    {
      return std::unexpected(IoError("Could not resolve the report destination", error.value()));
    }

    if (auto safe = CheckDestination(destination); !safe)
    {
      return safe;
    }

    TemporaryReport temporary;

    const auto candidate = destination.parent_path() /
                           (".havremote-report-" + GenerateId() + ".tmp");

#if defined(_WIN32)
    temporary.handle = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (temporary.handle == INVALID_HANDLE_VALUE)
    {
      return std::unexpected(IoError("Could not create the temporary report",
                                     static_cast<int>(GetLastError())));
    }
#else
    temporary.handle = ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                              S_IRUSR | S_IWUSR);

    if (temporary.handle < 0)
    {
      return std::unexpected(IoError("Could not create the temporary report", errno));
    }
#endif
    temporary.path = candidate;

    std::size_t offset{};

    while (offset < utf8Text.size())
    {
      const auto chunk = (std::min)(utf8Text.size() - offset, std::size_t{1U << 20U});

#if defined(_WIN32)
      DWORD written{};

      if (!WriteFile(temporary.handle, utf8Text.data() + offset,
                     static_cast<DWORD>(chunk), &written, nullptr))
      {
        return std::unexpected(IoError("Could not write the report", static_cast<int>(GetLastError())));
      }
#else
      const auto written = ::write(temporary.handle, utf8Text.data() + offset, chunk);

      if (written < 0)
      {
        if (errno == EINTR)
        {
          continue;
        }

        return std::unexpected(IoError("Could not write the report", errno));
      }
#endif
      if (written == 0)
      {
        return std::unexpected(ReportError(PlatformErrorCode::Io, "Writing the report made no progress"));
      }

      offset += static_cast<std::size_t>(written);
    }

#if defined(_WIN32)
    if (!FlushFileBuffers(temporary.handle))
    {
      return std::unexpected(IoError("Could not flush the report", static_cast<int>(GetLastError())));
    }
#else
    if (::fsync(temporary.handle) != 0)
    {
      return std::unexpected(IoError("Could not flush the report", errno));
    }
#endif

    if (const auto closeError = temporary.Close(); closeError != 0)
    {
      return std::unexpected(IoError("Could not close the report", closeError));
    }

    if (auto safe = CheckDestination(destination); !safe)
    {
      return safe;
    }

#if defined(_WIN32)
    if (!MoveFileExW(temporary.path.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
      return std::unexpected(IoError("Could not replace the report", static_cast<int>(GetLastError())));
    }
#else
    if (::rename(temporary.path.c_str(), destination.c_str()) != 0)
    {
      return std::unexpected(IoError("Could not replace the report", errno));
    }
#endif

    temporary.path.clear();

    return {};
  }
} // namespace havremote::platform
