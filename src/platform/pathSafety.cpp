// SPDX-License-Identifier: MIT

#include "platform/pathSafety.hpp"
#include "destinationNames.hpp"

#include <wx/filename.h>
#include <wx/string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <iterator>
#include <utility>
#include <vector>

namespace havremote::platform
{
  namespace
  {
    PlatformError InvalidPath(std::string message)
    {
      return {PlatformErrorCode::InvalidArgument, std::move(message)};
    }

    bool ContainsNul(const std::filesystem::path &path)
    {
      return path.native().find(std::filesystem::path::value_type{}) !=
             std::filesystem::path::string_type::npos;
    }

    std::vector<std::string_view> Components(const std::string_view path)
    {
      std::vector<std::string_view> result;
      std::size_t position{};

      do
      {
        const auto slash = path.find('/', position);
        const auto end = slash == std::string_view::npos ? path.size() : slash;

        result.push_back(path.substr(position, end - position));

        if (slash == std::string_view::npos)
        {
          break;
        }

        position = slash + 1U;
      } while (position <= path.size());

      return result;
    }

    Result<std::filesystem::path> AbsolutePath(const std::filesystem::path &path)
    {
      if (path.empty() || ContainsNul(path))
      {
        return std::unexpected(InvalidPath("The local path is empty or contains NUL"));
      }

      std::error_code error;

      auto absolute = std::filesystem::absolute(path, error);

      if (error)
      {
        return std::unexpected(InvalidPath("Could not resolve the local path: " + error.message()));
      }

      absolute = absolute.lexically_normal();

      // A terminal separator is not a child component of the destination
      if (!absolute.has_filename() && absolute != absolute.root_path())
      {
        absolute = absolute.parent_path();
      }

      return absolute;
    }

    Result<void> RejectLink(const std::filesystem::path &path)
    {
      std::error_code error;

      const auto status = std::filesystem::symlink_status(path, error);

      if (error == std::errc::no_such_file_or_directory ||
          status.type() == std::filesystem::file_type::not_found)
      {
        return {};
      }

      if (error)
      {
        return std::unexpected(InvalidPath("Could not inspect the local destination: " + error.message()));
      }

      if (std::filesystem::is_symlink(status))
      {
        return std::unexpected(InvalidPath("A symbolic link in the local destination path is not allowed"));
      }

#if defined(_WIN32)
      const auto attributes = GetFileAttributesW(path.c_str());

      if (attributes == INVALID_FILE_ATTRIBUTES)
      {
        return std::unexpected(InvalidPath("Could not inspect the local destination attributes"));
      }

      if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
      {
        return std::unexpected(InvalidPath("A junction or reparse point in the local destination path is not allowed"));
      }
#endif

      return {};
    }

    Result<bool> ExistingEquivalent(const std::filesystem::path &left,
                                    const std::filesystem::path &right)
    {
      std::error_code error;

      const bool leftExists = std::filesystem::exists(left, error);

      if (error)
      {
        return std::unexpected(InvalidPath("Could not inspect a local path: " + error.message()));
      }

      const bool rightExists = std::filesystem::exists(right, error);

      if (error)
      {
        return std::unexpected(InvalidPath("Could not inspect a local path: " + error.message()));
      }

      if (!leftExists || !rightExists)
      {
        return false;
      }

      const bool same = std::filesystem::equivalent(left, right, error);

      if (error)
      {
        return std::unexpected(InvalidPath("Could not compare local paths: " + error.message()));
      }

      return same;
    }
  }

  bool IsLocalSafeFilename(const std::string_view name) noexcept
  {
    if (name.empty() || name == "." || name == ".." ||
        name.find('\0') != std::string_view::npos || name.find('/') != std::string_view::npos)
    {
      return false;
    }

    const auto decoded = wxString::FromUTF8(name.data(), name.size());

    if (decoded.empty())
    {
      return false;
    }

#if !wxUSE_UNICODE_UTF16
    // UTF-32 conversion can retain non-scalar values. Reject them before
    // re-encoding. UTF-16 builds validate surrogate pairs during the round trip.
    for (const auto character : decoded)
    {
      const auto value = character.GetValue();

      if ((value >= 0xd800U && value <= 0xdfffU) || value > 0x10ffffU)
      {
        return false;
      }
    }
#endif

    if (decoded.ToStdString(wxConvUTF8) != name)
    {
      return false;
    }

#if defined(_WIN32)
    if (name.back() == ' ' || name.back() == '.')
    {
      return false;
    }

    const auto forbidden = wxFileName::GetForbiddenChars(wxPATH_DOS);

    for (const auto character : decoded)
    {
      if (character < 0x20 || forbidden.find(character) != wxString::npos)
      {
        return false;
      }
    }

    // wxFileName's punctuation list does not cover reserved device names
    const auto stem = decoded.BeforeFirst('.').Lower();

    for (const auto reserved : {"con", "prn", "aux", "nul", "conin$", "conout$", "clock$"})
    {
      if (stem == reserved)
      {
        return false;
      }
    }

    if (stem.StartsWith("com") || stem.StartsWith("lpt"))
    {
      const auto suffix = stem.Mid(3);

      if ((suffix.length() == 1U && suffix[0] >= '1' && suffix[0] <= '9') ||
          suffix == wxString::FromUTF8("¹") || suffix == wxString::FromUTF8("²") ||
          suffix == wxString::FromUTF8("³"))
      {
        return false;
      }
    }
#endif

    return true;
  }

  bool IsRemotePathWithin(const RemotePath &root, const RemotePath &candidate) noexcept
  {
    auto rootBytes = root.Bytes();

    const auto &candidateBytes = candidate.Bytes();

    const auto unsafe = [](const std::string_view path)
    {
      if (path.empty() || path.find('\0') != std::string_view::npos)
      {
        return true;
      }

      for (const auto part : Components(path))
      {
        if (part == "." || part == "..")
        {
          return true;
        }
      }

      return false;
    };

    if (unsafe(rootBytes) || unsafe(candidateBytes))
    {
      return false;
    }

    while (rootBytes.size() > 1U && rootBytes.back() == '/')
    {
      rootBytes.pop_back();
    }

    if (rootBytes == "/")
    {
      return candidateBytes.starts_with('/');
    }

    return candidateBytes == rootBytes ||
           (candidateBytes.size() > rootBytes.size() && candidateBytes.starts_with(rootBytes) &&
            candidateBytes[rootBytes.size()] == '/');
  }

  Result<void> ValidateLocalWriteTarget(const std::filesystem::path &destinationRoot,
                                        const std::filesystem::path &target)
  {
    // Do not erase a link/.. traversal before inspecting the path actually used for I/O
    for (const auto &component : target)
    {
      if (component == "..")
      {
        return std::unexpected(InvalidPath("A parent traversal in the local write target is not allowed"));
      }
    }

    const auto root = AbsolutePath(destinationRoot);
    const auto candidate = AbsolutePath(target);

    if (!root)
    {
      return std::unexpected(root.error());
    }

    if (!candidate)
    {
      return std::unexpected(candidate.error());
    }

    auto rootIterator = root->begin();
    auto candidateIterator = candidate->begin();
    std::filesystem::path rootPrefix, candidatePrefix;

    for (; rootIterator != root->end() && candidateIterator != candidate->end();
         ++rootIterator, ++candidateIterator)
    {
      rootPrefix /= *rootIterator;
      candidatePrefix /= *candidateIterator;

      if (*rootIterator != *candidateIterator)
      {
        const auto same = ExistingEquivalent(rootPrefix, candidatePrefix);
        if (!same)
        {
          return std::unexpected(same.error());
        }
        if (!*same)
        {
          return std::unexpected(InvalidPath("The local write target escapes the selected destination"));
        }
      }
    }

    if (rootIterator != root->end())
    {
      return std::unexpected(InvalidPath("The local write target escapes the selected destination"));
    }

    if (auto safe = RejectLink(candidatePrefix); !safe)
    {
      return safe;
    }

    for (; candidateIterator != candidate->end(); ++candidateIterator)
    {
      candidatePrefix /= *candidateIterator;

      if (auto safe = RejectLink(candidatePrefix); !safe)
      {
        return safe;
      }
    }

    return {};
  }

  SafeDownloadMapper::SafeDownloadMapper(std::filesystem::path root)
      : mDestinationRoot(std::move(root)) {}
  SafeDownloadMapper::~SafeDownloadMapper() = default;
  SafeDownloadMapper::SafeDownloadMapper(SafeDownloadMapper &&) noexcept = default;
  SafeDownloadMapper &SafeDownloadMapper::operator=(SafeDownloadMapper &&) noexcept = default;

  Result<std::filesystem::path> SafeDownloadMapper::Map(const RemotePath &relativePath)
  {
    const auto root = AbsolutePath(mDestinationRoot);
    if (!root)
    {
      return std::unexpected(root.error());
    }

    const auto &raw = relativePath.DisplayUtf8();
    if (raw.empty() || raw.starts_with('/'))
    {
      return std::unexpected(InvalidPath("The server returned an absolute or empty child path"));
    }

    std::filesystem::path relative;

    for (const auto piece : Components(raw))
    {
      if (!IsLocalSafeFilename(piece))
      {
        return std::unexpected(InvalidPath("The remote name cannot be used safely on the local filesystem"));
      }

      const std::u8string utf8{reinterpret_cast<const char8_t *>(piece.data()), piece.size()};
      const std::filesystem::path component{utf8};

      if (component.has_root_path() || component.has_parent_path())
      {
        return std::unexpected(InvalidPath("The remote name contains a local path separator"));
      }

      relative /= component;
    }

    const auto target = *root / relative;

    if (auto safe = ValidateLocalWriteTarget(*root, target); !safe)
    {
      return std::unexpected(safe.error());
    }

    if (!mNames)
    {
      mNames = std::make_unique<detail::DestinationNames>(*root);
    }

    if (auto reserved = mNames->Reserve(relative); !reserved)
    {
      return std::unexpected(reserved.error());
    }

    // Preserve the caller's relative/absolute spelling for queue identities
    return (mDestinationRoot / relative).lexically_normal();
  }

  havremote::Result<bool> CheckLocalPathConflict(const std::filesystem::path &left,
                                                 const std::filesystem::path &right)
  {
    const auto failure = [](const PlatformError &error)
    {
      return std::unexpected(RemoteError{.code = RemoteErrorCode::LocalIo, .message = error.message});
    };

    const auto a = AbsolutePath(left), b = AbsolutePath(right);

    if (!a)
    {
      return failure(a.error());
    }

    if (!b)
    {
      return failure(b.error());
    }

    if (*a == *b)
    {
      return true;
    }

    const auto same = ExistingEquivalent(*a, *b);
    if (!same)
    {
      return failure(same.error());
    }
    if (*same)
    {
      return true;
    }

    // Resolve existing parent aliases using the filesystem. Missing suffixes stay literal.
    std::error_code error;

    const auto parentA = std::filesystem::weakly_canonical(a->parent_path(), error);

    if (error)
    {
      return failure(InvalidPath("Could not resolve a destination parent: " + error.message()));
    }

    const auto parentB = std::filesystem::weakly_canonical(b->parent_path(), error);

    if (error)
    {
      return failure(InvalidPath("Could not resolve a destination parent: " + error.message()));
    }

    const auto ExistingParent = [](std::filesystem::path parent) -> Result<std::filesystem::path>
    {
      std::error_code parentError;

      while (!std::filesystem::exists(parent, parentError))
      {
        if (parentError)
        {
          return std::unexpected(InvalidPath("Could not inspect a destination parent: " + parentError.message()));
        }

        if (parent.empty() || parent == parent.parent_path())
        {
          return std::unexpected(InvalidPath("The destination has no accessible existing parent"));
        }

        parent = parent.parent_path();
      }

      if (!std::filesystem::is_directory(parent, parentError) || parentError)
      {
        return std::unexpected(InvalidPath("The destination parent is not an accessible directory"));
      }

      return parent;
    };

    const auto anchorA = ExistingParent(parentA), anchorB = ExistingParent(parentB);

    if (!anchorA)
    {
      return failure(anchorA.error());
    }

    if (!anchorB)
    {
      return failure(anchorB.error());
    }

    const auto sharedParent = ExistingEquivalent(*anchorA, *anchorB);

    if (!sharedParent)
    {
      return failure(sharedParent.error());
    }

    if (!*sharedParent)
    {
      return false;
    }

    // Anchor by filesystem identity, not drive-letter, share, or directory
    // spelling. Only the not-yet-created suffixes need filename probes.
    const auto relativeA = (parentA.lexically_relative(*anchorA) / a->filename()).lexically_normal();
    const auto relativeB = (parentB.lexically_relative(*anchorB) / b->filename()).lexically_normal();

    if (relativeA == relativeB)
    {
      return true;
    }

    if (std::distance(relativeA.begin(), relativeA.end()) !=
        std::distance(relativeB.begin(), relativeB.end()))
    {
      return false; // An ancestor and its child are different final destinations
    }

    detail::DestinationNames names{*anchorA};

    auto reserved = names.Reserve(relativeA, true);

    if (!reserved)
    {
      return failure(reserved.error());
    }

    reserved = names.Reserve(relativeB, true);

    if (reserved)
    {
      return false;
    }

    if (reserved.error().code == PlatformErrorCode::Conflict)
    {
      return true;
    }

    return failure(reserved.error());
  }
} // namespace havremote::platform
