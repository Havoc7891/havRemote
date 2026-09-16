// SPDX-License-Identifier: MIT

#include "destinationNames.hpp"

#include "core/types.hpp"

#include <iterator>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace havremote::platform::detail
{
  namespace
  {
    std::string PathText(const std::filesystem::path &path)
    {
      const auto utf8 = path.u8string();
      return {reinterpret_cast<const char *>(utf8.data()), utf8.size()};
    }

    PlatformError PathError(std::string message, const std::filesystem::path &path,
                            const std::error_code &error = {})
    {
      const auto code = error == std::errc::permission_denied
                            ? PlatformErrorCode::AccessDenied
                        : error == std::errc::invalid_argument ||
                                error == std::errc::filename_too_long ||
                                error == std::errc::not_a_directory
                            ? PlatformErrorCode::InvalidArgument
                            : PlatformErrorCode::Io;

      message += ": " + PathText(path);

      if (error)
      {
        message += ": " + error.message();
      }

      return {code, std::move(message),
              error.value() >= 0 ? static_cast<unsigned long>(error.value()) : 0UL};
    }

    PlatformError UnsafeDirectory(const std::filesystem::path &path)
    {
      return {PlatformErrorCode::UnsafeFileType,
              "Filename checks require a directory without symbolic links: " + PathText(path)};
    }

    PlatformError NameConflict(const std::filesystem::path &path)
    {
      return {PlatformErrorCode::Conflict,
              "Two paths refer to the same destination name: " + PathText(path)};
    }

    Result<std::filesystem::file_status> Inspect(const std::filesystem::path &path)
    {
      std::error_code error;

      const auto status = std::filesystem::symlink_status(path, error);

      if (error == std::errc::no_such_file_or_directory)
      {
        return std::filesystem::file_status{std::filesystem::file_type::not_found};
      }

      if (error)
      {
        return std::unexpected(PathError("Could not inspect a destination name", path, error));
      }

      return status;
    }

    Result<bool> IsAlias(const std::filesystem::path &original,
                         const std::filesystem::path &variant)
    {
      const auto status = Inspect(variant);
      if (!status)
      {
        return std::unexpected(status.error());
      }
      if (status->type() == std::filesystem::file_type::not_found)
      {
        return false;
      }
      if (!std::filesystem::is_directory(*status) || std::filesystem::is_symlink(*status))
      {
        return std::unexpected(UnsafeDirectory(variant));
      }

      std::error_code error;
      const bool equivalent = std::filesystem::equivalent(original, variant, error);

      if (error)
      {
        return std::unexpected(PathError("Could not compare destination names", variant, error));
      }

      if (!equivalent)
      {
        return std::unexpected(PathError("A filename-check entry was replaced", variant));
      }

      return true;
    }

    struct Probe final
    {
      std::filesystem::path mParent;
      std::filesystem::path mPath;
      std::vector<std::filesystem::path> mDirectories;
      bool mOwned{};

      ~Probe();
    };

    Result<void> CheckProbeDirectory(const Probe &probe, const std::filesystem::path &path)
    {
      auto current = probe.mPath;

      const auto Check = [](const std::filesystem::path &directory) -> Result<void>
      {
        const auto status = Inspect(directory);

        if (!status)
        {
          return std::unexpected(status.error());
        }

        if (!std::filesystem::is_directory(*status) || std::filesystem::is_symlink(*status))
        {
          return std::unexpected(UnsafeDirectory(directory));
        }

        return {};
      };

      if (auto checked = Check(current); !checked)
      {
        return checked;
      }

      const auto relative = path.lexically_relative(probe.mPath);
      if (relative.empty())
      {
        return std::unexpected(UnsafeDirectory(path));
      }

      for (const auto &component : relative)
      {
        if (component == ".")
        {
          continue;
        }

        if (component.empty() || component == "..")
        {
          return std::unexpected(UnsafeDirectory(path));
        }

        current /= component;

        if (auto checked = Check(current); !checked)
        {
          return checked;
        }
      }

      return {};
    }

    Result<void> RemoveOwnedDirectory(const std::filesystem::path &root,
                                      const std::filesystem::path &directory)
    {
      const auto status = Inspect(directory);
      if (!status)
      {
        return std::unexpected(status.error());
      }
      if (status->type() == std::filesystem::file_type::not_found)
      {
        return {};
      }
      if (!std::filesystem::is_directory(*status) && !std::filesystem::is_symlink(*status))
      {
        return std::unexpected(UnsafeDirectory(directory));
      }

      if (directory != root)
      {
        // Only inspect beneath the owned root. Never traverse a replaced link
        // to remove names in its target directory during cleanup.
        Probe boundary;
        boundary.mPath = root;

        if (auto checked = CheckProbeDirectory(boundary, directory.parent_path()); !checked)
        {
          return checked;
        }
      }

      std::error_code error;
      std::filesystem::remove(directory, error);
      if (error)
      {
        return std::unexpected(PathError("Could not remove a filename-check directory", directory, error));
      }

      return {};
    }

    Probe::~Probe()
    {
      if (!mOwned)
      {
        return;
      }

      for (auto iterator = mDirectories.rbegin(); iterator != mDirectories.rend(); ++iterator)
      {
        (void)RemoveOwnedDirectory(mPath, *iterator);
      }

      (void)RemoveOwnedDirectory(mPath, mPath);
    }

    Result<std::unique_ptr<Probe>> CreateProbe(const std::filesystem::path &parent)
    {
      for (unsigned attempt = 0; attempt < 16U; ++attempt)
      {
        const auto id = GenerateId();

        auto probe = std::make_unique<Probe>();
        probe->mParent = parent;
        probe->mPath = parent / (".havremote-name-check-" + id);

        std::error_code error;
        if (!std::filesystem::create_directory(probe->mPath, error))
        {
          if (!error || error == std::errc::file_exists)
          {
            continue;
          }

          return std::unexpected(PathError("Could not create a private filename-check directory",
                                           parent, error));
        }

        probe->mOwned = true;

#if !defined(_WIN32)
        std::filesystem::permissions(probe->mPath, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, error);

        if (error && error != std::errc::operation_not_supported)
        {
          return std::unexpected(PathError("Could not protect the filename-check directory",
                                           probe->mPath, error));
        }
#endif
        // A child directory can inherit different case rules from its parent.
        // Compare filesystem lookups of fixed ASCII variants without folding
        // any user names. Unicode aliases are checked by the marker entries.
        const auto parentAlias = IsAlias(probe->mPath,
                                         parent / (".HAVREMOTE-NAME-CHECK-" + id));
        if (!parentAlias)
        {
          return std::unexpected(parentAlias.error());
        }

        const auto sample = probe->mPath / ".havremote-case-check";

        error.clear();

        if (!std::filesystem::create_directory(sample, error))
        {
          return std::unexpected(PathError("Could not check inherited filename rules", sample, error));
        }

        probe->mDirectories.push_back(sample);

        const auto childAlias = IsAlias(sample, probe->mPath / ".HAVREMOTE-CASE-CHECK");
        if (!childAlias)
        {
          return std::unexpected(childAlias.error());
        }
        if (*parentAlias != *childAlias)
        {
          return std::unexpected(PlatformError{
              PlatformErrorCode::Unavailable,
              "The destination does not inherit filename rules consistently: " + PathText(parent)});
        }

        if (auto removed = RemoveOwnedDirectory(probe->mPath, sample); !removed)
        {
          return std::unexpected(removed.error());
        }

        return probe;
      }

      return std::unexpected(PathError("Could not allocate a unique filename-check directory", parent));
    }
  } // namespace

  struct DestinationNames::Impl final
  {
    struct Marker final
    {
      std::filesystem::path mRoot;
      std::filesystem::path mPath;
    };

    struct Node final
    {
      std::filesystem::path mActualPath;
      std::filesystem::path mMarkerPath;
      Probe *mMarkerProbe{};
      std::filesystem::path mNamespace;
      Probe *mNamespaceProbe{};
      bool mExistingDirectory{};
      bool mInitialized{};
      bool mTerminal{};
      std::map<std::filesystem::path, std::unique_ptr<Node>> mChildren;
    };

    struct Transaction final
    {
      explicit Transaction(Impl &owner) : mOwner{owner}, mProbeCount{owner.mProbes.size()} {}

      ~Transaction()
      {
        if (mCommitted)
        {
          return;
        }

        for (auto *node : mInitialized)
        {
          node->mNamespace.clear();
          node->mNamespaceProbe = nullptr;
          node->mInitialized = false;
        }

        for (auto iterator = mInserted.rbegin(); iterator != mInserted.rend(); ++iterator)
        {
          iterator->first->mChildren.erase(iterator->second);
        }

        for (auto iterator = mMarkers.rbegin(); iterator != mMarkers.rend(); ++iterator)
        {
          if (!RemoveOwnedDirectory(iterator->mRoot, iterator->mPath))
          {
            mOwner.mPendingCleanup.push_back(*iterator);
          }
        }

        for (auto index = mProbeCount; index < mOwner.mProbes.size(); ++index)
        {
          mOwner.mProbeByParent.erase(mOwner.mProbes[index]->mParent);
        }

        mOwner.mProbes.resize(mProbeCount);
      }

      Impl &mOwner;
      std::size_t mProbeCount;
      bool mCommitted{};
      std::vector<Node *> mInitialized;
      std::vector<std::pair<Node *, std::filesystem::path>> mInserted;
      std::vector<Marker> mMarkers;
    };

    explicit Impl(std::filesystem::path root) { mRoot.mActualPath = std::move(root); }

    Result<Probe *> ProbeFor(const std::filesystem::path &directory)
    {
      const auto found = mProbeByParent.find(directory);
      if (found != mProbeByParent.end())
      {
        return found->second;
      }

      auto created = CreateProbe(directory);
      if (!created)
      {
        return std::unexpected(created.error());
      }

      auto *const probe = created->get();
      mProbes.push_back(std::move(*created));
      mProbeByParent.emplace(directory, probe);

      return probe;
    }

    Result<Node *> FindAliasPrefix(Node &parent, const std::filesystem::path &marker)
    {
      if (auto checked = CheckProbeDirectory(*parent.mNamespaceProbe, marker); !checked)
      {
        return std::unexpected(checked.error());
      }

      for (const auto &[name, child] : parent.mChildren)
      {
        (void)name;

        if (auto checked = CheckProbeDirectory(*child->mMarkerProbe, child->mMarkerPath); !checked)
        {
          return std::unexpected(checked.error());
        }

        std::error_code error;
        const bool equivalent = std::filesystem::equivalent(marker, child->mMarkerPath, error);
        if (error)
        {
          return std::unexpected(PathError("Could not compare destination prefixes", marker, error));
        }

        if (equivalent)
        {
          return child.get();
        }
      }

      return std::unexpected(NameConflict(parent.mActualPath / marker.filename()));
    }

    Result<void> CreateMarker(Probe &probe, const std::filesystem::path &marker,
                              const std::filesystem::path &actual,
                              Transaction &transaction)
    {
      std::error_code error;
      if (!std::filesystem::create_directory(marker, error))
      {
        if (!error || error == std::errc::file_exists)
        {
          return std::unexpected(NameConflict(actual));
        }

        return std::unexpected(PathError("Could not check a destination name", actual, error));
      }

      probe.mDirectories.push_back(marker);

      transaction.mMarkers.push_back(Marker{probe.mPath, marker});

      return {};
    }

    Result<void> Initialize(Node &node, Transaction &transaction)
    {
      const auto status = Inspect(node.mActualPath);
      if (!status)
      {
        return std::unexpected(status.error());
      }

      const bool missing = status->type() == std::filesystem::file_type::not_found;
      if (!missing && (!std::filesystem::is_directory(*status) ||
                       std::filesystem::is_symlink(*status)))
      {
        return std::unexpected(UnsafeDirectory(node.mActualPath));
      }

      if (node.mInitialized)
      {
        if (node.mExistingDirectory == missing)
        {
          return std::unexpected(PathError("The destination changed during filename checks",
                                           node.mActualPath));
        }

        return CheckProbeDirectory(*node.mNamespaceProbe, node.mNamespace);
      }

      Probe *probe{};
      std::filesystem::path directory;

      if (!missing)
      {
        const auto existing = ProbeFor(node.mActualPath);
        if (!existing)
        {
          return std::unexpected(existing.error());
        }

        probe = *existing;
        directory = probe->mPath;
      }
      else if (node.mMarkerProbe)
      {
        probe = node.mMarkerProbe;
        directory = node.mMarkerPath;
      }
      else
      {
        // The selected root itself may not exist. Mirror its missing suffix
        // below a private directory on the nearest existing ancestor.
        auto ancestor = node.mActualPath;
        std::vector<std::filesystem::path> suffix;

        while (true)
        {
          const auto entry = Inspect(ancestor);
          if (!entry)
          {
            return std::unexpected(entry.error());
          }
          if (entry->type() != std::filesystem::file_type::not_found)
          {
            if (!std::filesystem::is_directory(*entry) || std::filesystem::is_symlink(*entry))
            {
              return std::unexpected(UnsafeDirectory(ancestor));
            }

            break;
          }

          if (ancestor.empty() || ancestor == ancestor.parent_path())
          {
            return std::unexpected(PathError("Could not find an existing destination ancestor",
                                             node.mActualPath));
          }

          suffix.push_back(ancestor.filename());

          ancestor = ancestor.parent_path();
        }

        const auto existing = ProbeFor(ancestor);
        if (!existing)
        {
          return std::unexpected(existing.error());
        }

        probe = *existing;
        directory = probe->mPath;

        for (auto iterator = suffix.rbegin(); iterator != suffix.rend(); ++iterator)
        {
          directory /= *iterator;

          if (auto created = CreateMarker(*probe, directory, node.mActualPath, transaction); !created)
          {
            return created;
          }
        }
      }

      if (auto checked = CheckProbeDirectory(*probe, directory); !checked)
      {
        return checked;
      }

      transaction.mInitialized.push_back(&node);

      node.mNamespace = std::move(directory);
      node.mNamespaceProbe = probe;
      node.mExistingDirectory = !missing;
      node.mInitialized = true;

      return {};
    }

    Result<void> Reserve(const std::filesystem::path &relative, const bool allowAliasPrefixes)
    {
      // If storage temporarily prevented rollback, finish removing those exact
      // owned markers before checking more names in the same namespace.
      while (!mPendingCleanup.empty())
      {
        const auto &marker = mPendingCleanup.front();

        if (auto removed = RemoveOwnedDirectory(marker.mRoot, marker.mPath); !removed)
        {
          return removed;
        }

        mPendingCleanup.erase(mPendingCleanup.begin());
      }

      Transaction transaction{*this};

      auto *node = &mRoot;

      for (auto iterator = relative.begin(); iterator != relative.end(); ++iterator)
      {
        const auto &component = *iterator;

        if (auto initialized = Initialize(*node, transaction); !initialized)
        {
          return initialized;
        }

        const auto found = node->mChildren.find(component);
        if (found != node->mChildren.end())
        {
          node = found->second.get();

          if (auto checked = CheckProbeDirectory(*node->mMarkerProbe, node->mMarkerPath); !checked)
          {
            return checked;
          }

          continue;
        }

        auto child = std::make_unique<Node>();
        child->mActualPath = node->mActualPath / component;
        child->mMarkerPath = node->mNamespace / component;
        child->mMarkerProbe = node->mNamespaceProbe;

        if (auto created = CreateMarker(*child->mMarkerProbe, child->mMarkerPath,
                                        child->mActualPath, transaction);
            !created)
        {
          // Runtime comparisons may share an aliased parent while reserving
          // different leaves. Download mapping keeps its stricter default.
          if (!allowAliasPrefixes || std::next(iterator) == relative.end() ||
              created.error().code != PlatformErrorCode::Conflict)
          {
            return created;
          }

          const auto existing = FindAliasPrefix(*node, child->mMarkerPath);
          if (!existing)
          {
            return std::unexpected(existing.error());
          }

          node = *existing;

          continue;
        }

        transaction.mInserted.emplace_back(node, component);

        auto *const next = child.get();
        node->mChildren.emplace(component, std::move(child));
        node = next;
      }

      if (node->mTerminal)
      {
        return std::unexpected(NameConflict(node->mActualPath));
      }

      node->mTerminal = true;
      transaction.mCommitted = true;

      return {};
    }

    Node mRoot;
    std::vector<std::unique_ptr<Probe>> mProbes;
    std::map<std::filesystem::path, Probe *> mProbeByParent;
    std::vector<Marker> mPendingCleanup;
  };

  DestinationNames::DestinationNames(std::filesystem::path absoluteDestinationRoot)
      : mImpl{std::make_unique<Impl>(std::move(absoluteDestinationRoot))} {}

  DestinationNames::~DestinationNames() = default;
  DestinationNames::DestinationNames(DestinationNames &&) noexcept = default;
  DestinationNames &DestinationNames::operator=(DestinationNames &&) noexcept = default;

  Result<void> DestinationNames::Reserve(const std::filesystem::path &relative,
                                         const bool allowAliasPrefixes)
  {
    if (!mImpl || relative.empty() || relative.is_absolute())
    {
      return std::unexpected(PlatformError{
          PlatformErrorCode::InvalidArgument, "A relative destination name is required"});
    }

    return mImpl->Reserve(relative, allowAliasPrefixes);
  }
} // namespace havremote::platform::detail
