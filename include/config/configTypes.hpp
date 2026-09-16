// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_CONFIG_CONFIG_TYPES_HPP
#define HAVREMOTE_INCLUDE_CONFIG_CONFIG_TYPES_HPP

#include "core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace havremote::config
{
  inline constexpr std::uint32_t CurrentFormatVersion = 1;
  inline constexpr std::size_t MaxQuickConnectHistoryEntries = 20;
  inline constexpr std::uint32_t DefaultMainWindowWidth = 1410;
  inline constexpr std::uint32_t DefaultMainWindowHeight = 930;

  // Quick Connect history deliberately contains endpoint metadata only. Passwords
  // and other authentication material remain transient or in Credential Manager.
  using QuickConnectHistoryEntry = SiteEndpointIdentity;

  enum class AppearanceTheme
  {
    Light,
    Dark,
  };

  enum class ExternalEditorMode
  {
    SystemDefault,
    Custom,
  };

  struct ExternalEditorSettings final
  {
    ExternalEditorMode mode{ExternalEditorMode::SystemDefault};
    std::filesystem::path executable;
    std::string arguments{"{file}"};

    friend bool operator==(const ExternalEditorSettings &,
                           const ExternalEditorSettings &) = default;
  };

  enum class FileListSortColumn
  {
    Name,
    Size,
    Type,
    Modified,
    Permissions,
    Owner,
  };

  struct FileListSortSettings final
  {
    FileListSortColumn sortColumn{FileListSortColumn::Name};
    bool sortAscending{true};
    // Empty means use the UI defaults. Otherwise values are stored in logical
    // column order as device-independent pixels (4 local, 6 remote).
    std::vector<std::uint32_t> columnWidths;

    friend bool operator==(const FileListSortSettings &,
                           const FileListSortSettings &) = default;
  };

  struct FileListSettings final
  {
    FileListSortSettings local;
    FileListSortSettings remote;

    friend bool operator==(const FileListSettings &,
                           const FileListSettings &) = default;
  };

  struct UpdateSettings final
  {
    bool checkAutomatically{true};
    // Exact timestamps remain decimal strings because havCSON numbers use
    // double. Empty means that no update check has completed yet.
    std::string lastCheckUnixSeconds;
    // A normalized MAJOR.MINOR.PATCH version without a leading "v". Empty
    // means that no release has been skipped.
    std::string skippedVersion;

    friend bool operator==(const UpdateSettings &,
                           const UpdateSettings &) = default;
  };

  struct AppSettings final
  {
    std::uint32_t transferConcurrency{2};
    std::uint32_t connectionTimeoutSeconds{20};
    std::uint32_t commandIdleTimeoutSeconds{60};
    ConflictPolicy defaultConflictPolicy{ConflictPolicy::Ask};
    AppearanceTheme theme{AppearanceTheme::Dark};
    std::string language{"en"};
    FileListSettings fileLists;
    UpdateSettings updates;
    ExternalEditorSettings externalEditor;

    friend bool operator==(const AppSettings &, const AppSettings &) = default;
  };

  struct TlsTrustRecord final
  {
    std::string host;
    std::uint16_t port{21};
    // A public-key PIN/fingerprint. Certificate material is deliberately not
    // persisted here.
    std::string publicKeyPin;

    friend bool operator==(const TlsTrustRecord &, const TlsTrustRecord &) = default;
  };

  // An empty parentId denotes the Site Manager root. siteIds record direct
  // membership. ConfigData::siteManagerOrder defines the mixed presentation
  // order of folders and sites.
  struct SiteFolder final
  {
    std::string id;
    std::string name;
    std::string parentId;
    std::vector<std::string> siteIds;

    friend bool operator==(const SiteFolder &, const SiteFolder &) = default;
  };

  struct SavedSiteWorkspaceIdentity final
  {
    std::string siteId;

    friend bool operator==(const SavedSiteWorkspaceIdentity &,
                           const SavedSiteWorkspaceIdentity &) = default;
  };

  struct QuickConnectWorkspaceIdentity final
  {
    SiteEndpointIdentity endpoint;

    friend bool operator==(const QuickConnectWorkspaceIdentity &,
                           const QuickConnectWorkspaceIdentity &) = default;
  };

  // Remembered browser directories belong either to a stable saved-site UUID or
  // to the composite identity of an unsaved Quick Connect endpoint.
  using WorkspaceConnectionIdentity =
      std::variant<SavedSiteWorkspaceIdentity,
                   QuickConnectWorkspaceIdentity>;

  struct RememberedConnectionDirectories final
  {
    WorkspaceConnectionIdentity connection;
    std::filesystem::path localDirectory;
    RemotePath remoteDirectory;

    friend bool operator==(const RememberedConnectionDirectories &,
                           const RememberedConnectionDirectories &) = default;
  };

  // A restart restores these tabs as disconnected browser workspaces. The
  // identity contains endpoint metadata only. Passwords and passphrases are
  // never part of workspace state.
  struct OpenConnectionTabState final
  {
    std::string connectionId;
    std::optional<WorkspaceConnectionIdentity> connection;
    std::filesystem::path localDirectory;
    RemotePath remoteDirectory{RemotePath::Root()};

    friend bool operator==(const OpenConnectionTabState &,
                           const OpenConnectionTabState &) = default;
  };

  struct WindowPosition final
  {
    std::int32_t x{};
    std::int32_t y{};

    friend bool operator==(const WindowPosition &,
                           const WindowPosition &) = default;
  };

  // Width and height describe the restored (non-maximized) window. A missing
  // position means that no usable position has been saved yet, so the UI should
  // center the window on its current display.
  struct MainWindowState final
  {
    std::optional<WindowPosition> position;
    std::uint32_t width{DefaultMainWindowWidth};
    std::uint32_t height{DefaultMainWindowHeight};
    bool maximized{};

    friend bool operator==(const MainWindowState &,
                           const MainWindowState &) = default;
  };

  struct WorkspaceState final
  {
    // Used for a new tab until it acquires a saved-site or Quick Connect
    // identity. Identified tabs restore their own pair of browser directories.
    std::filesystem::path lastLocalDirectory;
    std::vector<RememberedConnectionDirectories> connectionDirectories;
    std::vector<OpenConnectionTabState> openTabs;
    std::string selectedConnectionId;
    MainWindowState mainWindow;

    friend bool operator==(const WorkspaceState &,
                           const WorkspaceState &) = default;
  };

  struct ConfigData final
  {
    std::uint32_t formatVersion{CurrentFormatVersion};
    AppSettings settings;
    WorkspaceState workspace;
    // Most-recent connection first. Composite endpoint identities are unique.
    std::vector<QuickConnectHistoryEntry> quickConnectHistory;
    std::vector<SiteProfile> sites;
    std::vector<SiteFolder> siteFolders;
    // A complete permutation of all saved-site and Site Manager folder IDs.
    // Parent relationships remain in SiteFolder. Filtering this global order
    // by parent yields the freely interleaved sibling order at every level.
    std::vector<std::string> siteManagerOrder;
    // Opaque credential-store identifiers awaiting deletion. These references
    // contain no password or passphrase material.
    std::vector<std::string> pendingCredentialDeletions;
    std::vector<TlsTrustRecord> tlsTrust;
  };

  enum class ConfigErrorKind
  {
    PathUnavailable,
    Io,
    Parse,
    Validation,
    UnsupportedVersion,
    AtomicWrite,
  };

  struct ConfigError final
  {
    ConfigErrorKind kind{ConfigErrorKind::Io};
    std::filesystem::path path;
    std::string message;
    std::optional<std::size_t> line;
    std::optional<std::size_t> column;
  };

  struct ConfigLoadResult final
  {
    ConfigData data;
    bool createdDefaults{};
  };
} // namespace havremote::config

#endif // HAVREMOTE_INCLUDE_CONFIG_CONFIG_TYPES_HPP
