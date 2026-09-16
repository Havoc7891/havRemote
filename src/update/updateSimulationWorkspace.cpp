// SPDX-License-Identifier: MIT

#include "update/updateSimulationWorkspace.hpp"

#if defined(HAVREMOTE_UPDATE_SIMULATION)

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <string_view>
#include <system_error>

namespace havremote::updates
{
  namespace
  {
    using WorkspaceResult = std::expected<void, std::string>;

    std::string WorkspaceError(const std::string_view reason)
    {
      return "Update simulation workspace: " + std::string{reason};
    }

    WorkspaceResult ValidatePath(const std::filesystem::path &path,
                                 const bool directory,
                                 const bool missingAllowed)
    {
      std::error_code error;

      const auto status = std::filesystem::symlink_status(path, error);
      if (status.type() == std::filesystem::file_type::not_found &&
          (!error || error == std::errc::no_such_file_or_directory))
      {
        if (missingAllowed)
        {
          return {};
        }

        return std::unexpected{WorkspaceError("a required directory does not exist")};
      }

      if (error)
      {
        return std::unexpected{WorkspaceError("cannot inspect a state path: " + error.message())};
      }

      if (std::filesystem::is_symlink(status) ||
          (directory ? !std::filesystem::is_directory(status)
                     : !std::filesystem::is_regular_file(status)))
      {
        return std::unexpected{WorkspaceError("state paths must not be links or special files")};
      }

#if defined(_WIN32)
      const auto attributes = GetFileAttributesW(path.c_str());

      if (attributes == INVALID_FILE_ATTRIBUTES ||
          (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
      {
        return std::unexpected{WorkspaceError("cannot use a reparse point or inaccessible path")};
      }

      if (!directory && (attributes & FILE_ATTRIBUTE_READONLY) != 0)
      {
        return std::unexpected{WorkspaceError("an existing state file is read-only")};
      }
#else
      if (!directory && (status.permissions() & (std::filesystem::perms::owner_write |
                                                 std::filesystem::perms::group_write |
                                                 std::filesystem::perms::others_write)) == std::filesystem::perms::none)
      {
        return std::unexpected{WorkspaceError("a state path is not writable")};
      }
#endif
      if (!directory)
      {
        const auto links = std::filesystem::hard_link_count(path, error);
        if (error || links != 1)
        {
          return std::unexpected{WorkspaceError("state files must not have hard-link aliases")};
        }

        // Opening without truncation checks existing-file access without
        // altering any persistent settings or queue contents.
        std::fstream stream{path, std::ios::binary | std::ios::in | std::ios::out};
        if (!stream.is_open())
        {
          return std::unexpected{WorkspaceError("an existing state file is not writable")};
        }
      }

      return {};
    }

    WorkspaceResult EnsureDirectory(const std::filesystem::path &path)
    {
      if (auto checked = ValidatePath(path, true, true); !checked)
      {
        return checked;
      }

      std::error_code error;
      std::filesystem::create_directory(path, error);

      if (error)
      {
        return std::unexpected{WorkspaceError("cannot create the directory: " + error.message())};
      }

      return ValidatePath(path, true, false);
    }

    WorkspaceResult CheckDirectoryWritable(const std::filesystem::path &path)
    {
      static std::atomic<unsigned long> sequence{};
      const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
      const auto probe = path / (".write-probe-" + std::to_string(ticks) + "-" +
                                 std::to_string(sequence.fetch_add(1)));

      // Exclusively create a disposable probe, never open or remove a
      // pre-existing file. No simulation configuration is deleted or reset.
#if defined(_WIN32)
      const auto handle = CreateFileW(
          probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
          FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);

      if (handle == INVALID_HANDLE_VALUE)
      {
        return std::unexpected{WorkspaceError("the directory is not writable")};
      }

      const bool closed = CloseHandle(handle) != FALSE;
      if (!closed)
      {
        return std::unexpected{WorkspaceError("could not close the write probe")};
      }
#else
      const int descriptor = open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
      if (descriptor < 0)
      {
        return std::unexpected{WorkspaceError("the directory is not writable")};
      }

      const bool removed = unlink(probe.c_str()) == 0;
      const bool closed = close(descriptor) == 0;
      if (!removed || !closed)
      {
        return std::unexpected{WorkspaceError("could not remove the temporary write probe")};
      }
#endif

      return {};
    }

    class SimulationCredentialStore final : public platform::ICredentialStore
    {
    public:
      platform::Result<void> Store(std::string_view, std::string_view,
                                   std::span<const std::byte>) override
      {
        return std::unexpected{Unavailable()};
      }

      platform::Result<platform::CredentialPayload> Load(std::string_view) const override
      {
        return std::unexpected{Unavailable()};
      }

      platform::Result<void> Erase(std::string_view) override
      {
        return std::unexpected{Unavailable()};
      }

    private:
      static platform::PlatformError Unavailable()
      {
        return {platform::PlatformErrorCode::Unavailable,
                "Credential storage is unavailable in update simulation mode.", 0};
      }
    };
  } // namespace

  std::expected<std::filesystem::path, std::string> PrepareUpdateSimulationWorkspace(
      const std::filesystem::path &executableDirectory,
      const UpdateSimulationScenario scenario)
  {
    const auto scenarioName = UpdateSimulationScenarioName(scenario);
    const auto parsedScenario = ParseUpdateSimulationScenario(scenarioName);

    if (!parsedScenario || *parsedScenario != scenario || scenarioName.empty() ||
        !std::ranges::all_of(scenarioName, [](const char character)
                             { return (character >= 'a' && character <= 'z') || character == '-'; }))
    {
      return std::unexpected{WorkspaceError("invalid scenario")};
    }

    const auto &native = executableDirectory.native();
    if (executableDirectory.empty() || !executableDirectory.is_absolute() ||
        native.find(std::filesystem::path::value_type{}) != native.npos)
    {
      return std::unexpected{WorkspaceError("an absolute executable directory is required")};
    }

    // Check every ancestor before normalizing, so a symlink followed by '..'
    // cannot silently redirect simulation state into an unrelated directory.
    std::filesystem::path ancestor;

    for (const auto &component : executableDirectory)
    {
      if (component == "..")
      {
        return std::unexpected{WorkspaceError("parent traversal is not allowed")};
      }

      ancestor /= component;

      if (ancestor == executableDirectory.root_name())
      {
        continue;
      }

      if (auto checked = ValidatePath(ancestor, true, false); !checked)
      {
        return std::unexpected{checked.error()};
      }
    }

    const auto root = executableDirectory / "update-simulation";
    if (auto created = EnsureDirectory(root); !created)
    {
      return std::unexpected{created.error()};
    }

    const auto directory = root / std::string{scenarioName};
    if (auto created = EnsureDirectory(directory); !created)
    {
      return std::unexpected{created.error()};
    }

    for (const auto *name : {"havRemote.cson", "queue.cson", "known_hosts"})
    {
      if (auto checked = ValidatePath(directory / name, false, true); !checked)
      {
        return std::unexpected{checked.error()};
      }
    }

    if (auto checked = CheckDirectoryWritable(directory); !checked)
    {
      return std::unexpected{checked.error()};
    }

    return directory;
  }

  std::unique_ptr<platform::ICredentialStore> MakeUpdateSimulationCredentialStore()
  {
    return std::make_unique<SimulationCredentialStore>();
  }
} // namespace havremote::updates

#endif // HAVREMOTE_UPDATE_SIMULATION
