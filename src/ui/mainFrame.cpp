// SPDX-License-Identifier: MIT

#include "ui/mainFrame.hpp"

#include "core/logSanitizer.hpp"
#include "core/transferRuntime.hpp"
#include "havRemoteVersion.hpp"
#include "havRemoteImages.hpp"
#include "platform/toolkitPaths.hpp"
#include "localization/translationCatalog.hpp"
#include "platform/credentialCleanup.hpp"
#include "platform/externalEditor.hpp"
#include "platform/helpLauncher.hpp"
#include "platform/reportExport.hpp"
#include "platform/webBrowser.hpp"
#include "platform/pathSafety.hpp"
#include "ui/aboutDialog.hpp"
#include "ui/buttonBitmap.hpp"
#include "ui/connectionProfileModel.hpp"
#include "ui/fileListCtrl.hpp"
#include "ui/messageLogModel.hpp"
#include "ui/pickerLabel.hpp"
#include "ui/remotePermissionsDialog.hpp"
#include "ui/siteManagerDialog.hpp"
#include "ui/siteManagerModel.hpp"
#include "ui/splitButton.hpp"
#include "ui/transferListModel.hpp"
#include "ui/updateDialog.hpp"
#include "update/updateService.hpp"

#include <wx/button.h>
#include <wx/aui/auibook.h>
#include <wx/bmpbuttn.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/datetime.h>
#include <wx/dataobj.h>
#include <wx/dialog.h>
#include <wx/display.h>
#include <wx/dnd.h>
#include <wx/event.h>
#include <wx/filepicker.h>
#include <wx/filedlg.h>
#include <wx/gbsizer.h>
#include <wx/icon.h>
#include <wx/listctrl.h>
#include <wx/log.h>
#include <wx/menu.h>
#include <wx/modalhook.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/scopeguard.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/splitter.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/textdlg.h>
#include <wx/textctrl.h>
#include <wx/thread.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/wupdlock.h>
#include <wx/wrapsizer.h>

#ifdef __WXMSW__
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <ranges>
#include <span>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace havremote::ui
{
  wxDECLARE_EVENT(EVT_HAVREMOTE_UPDATE_CHECK, wxThreadEvent);
  wxDEFINE_EVENT(EVT_HAVREMOTE_UPDATE_CHECK, wxThreadEvent);

  namespace
  {
    constexpr int MainWindowMinimumWidthDips = 1100;
    constexpr int MainWindowMinimumHeightDips = 700;
    constexpr int DefaultWindowWorkAreaPercent = 90;
    constexpr int WindowGeometrySaveDelayMilliseconds = 300;
    constexpr std::uint64_t UpdateCheckIntervalSeconds = 24U * 60U * 60U;

    struct UpdateCheckEventPayload final
    {
      bool manuallyRequested{};
      std::expected<updates::UpdateCheck, updates::UpdateError> result;
    };

    std::unique_ptr<updates::IUpdateService> MakeApplicationUpdateService(
#if defined(HAVREMOTE_UPDATE_SIMULATION)
        const std::optional<updates::UpdateSimulationScenario> simulation
#endif
    )
    {
      const auto version = updates::ParseReleaseVersion(ApplicationVersion);
      if (!version)
      {
        return {};
      }

      auto source = updates::GitHubUpdateSource{
          .owner = std::string{UpdateRepositoryOwner},
          .repository = std::string{UpdateRepositoryName},
          .currentVersion = *version,
      };

#if defined(HAVREMOTE_UPDATE_SIMULATION)
      if (simulation)
      {
        return updates::MakeSimulatedUpdateService(
            *simulation, std::move(source), std::chrono::milliseconds{750});
      }
#endif

      return updates::MakeGitHubUpdateService(std::move(source));
    }

    wxRect PrimaryDisplayClientArea()
    {
      if (wxDisplay::GetCount() != 0U)
      {
        wxDisplay display{0U};

        if (display.IsOk())
        {
          return display.GetClientArea();
        }
      }

      return wxGetClientDisplayRect();
    }

    wxSize LimitedWindowSize(const wxSize desired,
                             const wxSize minimum,
                             const wxSize maximum)
    {
      const auto limit = [](const int value,
                            const int lower,
                            const int upper)
      {
        const int safeUpper = std::max(1, upper);
        return std::clamp(value, std::min(lower, safeUpper), safeUpper);
      };

      return {
          limit(desired.x, minimum.x, maximum.x),
          limit(desired.y, minimum.y, maximum.y),
      };
    }

    wxPoint CenteredWindowPosition(const wxRect &area, const wxSize size)
    {
      return {
          area.x + std::max(0, area.width - size.x) / 2,
          area.y + std::max(0, area.height - size.y) / 2,
      };
    }

    wxPoint VisibleWindowPosition(const wxPoint requested,
                                  const wxRect &area,
                                  const wxSize size)
    {
      return {
          std::clamp(requested.x, area.x,
                     area.x + std::max(0, area.width - size.x)),
          std::clamp(requested.y, area.y,
                     area.y + std::max(0, area.height - size.y)),
      };
    }

    class EnterDirPickerCtrl final : public wxDirPickerCtrl
    {
    public:
      EnterDirPickerCtrl(wxWindow *parent,
                         const wxWindowID id,
                         const wxString &path,
                         const wxString &message,
                         const wxPoint &position,
                         const wxSize &size,
                         const long style)
          : wxDirPickerCtrl()
      {
        // wxDirPickerCtrl::Create() enables SHAutoComplete on Windows, which
        // consumes Return before wxTextCtrl can emit wxEVT_TEXT_ENTER. The
        // main path picker needs Enter to navigate, so create the same
        // composite control without enabling shell auto-completion.
        (void)wxFileDirPickerCtrlBase::CreateBase(
            parent,
            id,
            path,
            message,
            {},
            position,
            size,
            style,
            wxDefaultValidator,
            wxASCII_STR(wxDirPickerCtrlNameStr));
      }

    protected:
      long GetTextCtrlStyle(const long style) const override
      {
        return wxDirPickerCtrl::GetTextCtrlStyle(style) |
               wxTE_PROCESS_ENTER;
      }
    };

    bool EndpointHostEqual(const std::string_view left, const std::string_view right)
    {
      return std::ranges::equal(left, right, [](const unsigned char lhs, const unsigned char rhs)
                                { return std::tolower(lhs) == std::tolower(rhs); });
    }

    bool EndpointMatchesSite(const SiteEndpointIdentity &endpoint,
                             const SiteProfile &site)
    {
      return endpoint.protocol == site.protocol &&
             EndpointHostEqual(endpoint.host, site.host) &&
             endpoint.port == site.port && endpoint.username == site.username;
    }

    bool EndpointIdentitiesEqual(const SiteEndpointIdentity &left,
                                 const SiteEndpointIdentity &right)
    {
      return left.protocol == right.protocol &&
             EndpointHostEqual(left.host, right.host) &&
             left.port == right.port && left.username == right.username;
    }

    const SiteProfile *UniqueSavedSiteForEndpoint(
        const config::ConfigData &configuration,
        const SiteEndpointIdentity &endpoint)
    {
      const SiteProfile *match{};

      for (const auto &site : configuration.sites)
      {
        if (!EndpointMatchesSite(endpoint, site))
        {
          continue;
        }

        if (match != nullptr)
        {
          return nullptr;
        }

        match = &site;
      }

      return match;
    }

    bool UnfinishedTransfer(const QueuedTransfer &transfer)
    {
      return transfer.state == TransferState::Queued ||
             transfer.state == TransferState::Enumerating ||
             transfer.state == TransferState::Running ||
             transfer.state == TransferState::Paused;
    }

    bool PersistentTransfer(const QueuedTransfer &transfer)
    {
      return transfer.state != TransferState::Completed &&
             transfer.state != TransferState::Cancelled;
    }

    void ClearSensitiveString(std::string &value) noexcept
    {
      if (!value.empty())
      {
        wxSecureZeroMemory(value.data(), value.size());

        value.clear();
      }
    }

    bool WorkspaceConnectionsEqual(
        const config::WorkspaceConnectionIdentity &left,
        const config::WorkspaceConnectionIdentity &right)
    {
      if (const auto *leftSite =
              std::get_if<config::SavedSiteWorkspaceIdentity>(&left))
      {
        const auto *rightSite =
            std::get_if<config::SavedSiteWorkspaceIdentity>(&right);

        return rightSite != nullptr && leftSite->siteId == rightSite->siteId;
      }

      const auto *leftQuick =
          std::get_if<config::QuickConnectWorkspaceIdentity>(&left);
      const auto *rightQuick =
          std::get_if<config::QuickConnectWorkspaceIdentity>(&right);

      return leftQuick != nullptr && rightQuick != nullptr &&
             EndpointIdentitiesEqual(leftQuick->endpoint,
                                     rightQuick->endpoint);
    }

    bool QuickHistoryContains(const config::ConfigData &configuration,
                              const SiteEndpointIdentity &endpoint)
    {
      return std::ranges::any_of(
          configuration.quickConnectHistory,
          [&](const auto &entry)
          {
            return EndpointIdentitiesEqual(entry, endpoint);
          });
    }

    std::optional<config::WorkspaceConnectionIdentity> WorkspaceIdentityForSite(
        const config::ConfigData &configuration,
        const SiteProfile &site,
        const bool includeUnrecordedQuick)
    {
      if (site.host.empty())
      {
        return std::nullopt;
      }

      const auto endpoint = EndpointIdentity(site);
      const auto savedById = std::ranges::find(
          configuration.sites, site.id, &SiteProfile::id);

      if (savedById != configuration.sites.end() &&
          EndpointMatchesSite(endpoint, *savedById))
      {
        return config::SavedSiteWorkspaceIdentity{savedById->id};
      }

      if (const auto *saved = UniqueSavedSiteForEndpoint(configuration, endpoint))
      {
        return config::SavedSiteWorkspaceIdentity{saved->id};
      }

      if (includeUnrecordedQuick ||
          QuickHistoryContains(configuration, endpoint))
      {
        return config::QuickConnectWorkspaceIdentity{std::move(endpoint)};
      }

      return std::nullopt;
    }

    bool HasQuickWorkspaceDirectories(const config::ConfigData &configuration)
    {
      return std::ranges::any_of(
          configuration.workspace.connectionDirectories,
          [](const auto &remembered)
          {
            return std::holds_alternative<
                config::QuickConnectWorkspaceIdentity>(
                remembered.connection);
          });
    }

    void RemoveQuickWorkspaceDirectories(config::ConfigData &configuration)
    {
      std::erase_if(
          configuration.workspace.connectionDirectories,
          [](const auto &remembered)
          {
            return std::holds_alternative<
                config::QuickConnectWorkspaceIdentity>(
                remembered.connection);
          });
    }

    void RemoveWorkspaceDirectoriesForDeletedSites(
        config::ConfigData &configuration,
        const std::vector<SiteProfile> &previousSites)
    {
      std::erase_if(
          configuration.workspace.connectionDirectories,
          [&](const auto &remembered)
          {
            const auto *identity =
                std::get_if<config::SavedSiteWorkspaceIdentity>(
                    &remembered.connection);

            return identity != nullptr &&
                   std::ranges::any_of(
                       previousSites,
                       [&](const auto &site)
                       {
                         return site.id == identity->siteId;
                       }) &&
                   std::ranges::none_of(
                       configuration.sites,
                       [&](const auto &site)
                       {
                         return site.id == identity->siteId;
                       });
          });
    }

    const config::RememberedConnectionDirectories *RememberedDirectories(
        const config::ConfigData &configuration,
        const config::WorkspaceConnectionIdentity &identity,
        const SiteEndpointIdentity &endpoint)
    {
      const auto exact = std::ranges::find_if(
          configuration.workspace.connectionDirectories,
          [&](const auto &remembered)
          {
            return WorkspaceConnectionsEqual(remembered.connection,
                                             identity);
          });

      if (exact != configuration.workspace.connectionDirectories.end())
      {
        return &*exact;
      }

      // A Quick Connect endpoint may later be saved as exactly one site. Use its
      // last location once, then migrate it to that site's stable UUID after the
      // next successful listing. Duplicate saved endpoints are ambiguous and
      // never share workspace state implicitly.
      if (!std::holds_alternative<config::SavedSiteWorkspaceIdentity>(identity))
      {
        return nullptr;
      }

      const auto &savedIdentity =
          std::get<config::SavedSiteWorkspaceIdentity>(identity);
      const auto *uniqueSaved =
          UniqueSavedSiteForEndpoint(configuration, endpoint);

      if (uniqueSaved == nullptr || uniqueSaved->id != savedIdentity.siteId)
      {
        return nullptr;
      }

      const auto quick = std::ranges::find_if(
          configuration.workspace.connectionDirectories,
          [&](const auto &remembered)
          {
            const auto *quickIdentity =
                std::get_if<config::QuickConnectWorkspaceIdentity>(
                    &remembered.connection);

            return quickIdentity != nullptr &&
                   EndpointIdentitiesEqual(quickIdentity->endpoint, endpoint);
          });

      return quick == configuration.workspace.connectionDirectories.end()
                 ? nullptr
                 : &*quick;
    }

    std::optional<std::filesystem::path> ConfiguredLocalDirectory(
        const std::filesystem::path &directory)
    {
      return directory.empty()
                 ? std::nullopt
                 : std::optional<std::filesystem::path>{directory};
    }

    void PruneSavedSitesFromQuickConnectHistory(config::ConfigData &configuration)
    {
      std::erase_if(configuration.quickConnectHistory, [&](const auto &entry)
                    { return UniqueSavedSiteForEndpoint(configuration, entry) != nullptr; });
    }

    std::string CurlTlsPin(const std::string_view fingerprint)
    {
      std::string pin{fingerprint};

      if (fingerprint.starts_with("SHA256:"))
      {
        pin = "sha256//" + std::string{fingerprint.substr(7)};
      }

      if (pin.starts_with("sha256//"))
      {
        const auto separator = pin.find(';');
        const auto firstLength = (separator == std::string::npos ? pin.size() : separator) - 8U;

        // OpenSSH displays SHA-256 fingerprints without base64 padding, while
        // libcurl compares the exact padded base64 digest.
        if (firstLength % 4U != 0U)
        {
          pin.insert(8U + firstLength, 4U - firstLength % 4U, '=');
        }
      }

      return pin;
    }

    enum : int
    {
      IdSiteManager = wxID_HIGHEST + 200,
      IdNewConnectionTab,
      IdCloseConnectionTab,
      IdDisconnect,
      IdQuickProtocol,
      IdQuickConnect,
      IdQuickClear,
      IdQuickHistory,
      IdQuickHistoryClear,
      IdLocalPath,
      IdRemotePath,
      IdLocalUp,
      IdLocalRefresh,
      IdRemoteUp,
      IdRemoteRefresh,
      IdLocalList,
      IdRemoteList,
      IdUpload,
      IdDownload,
      IdLocalCreate,
      IdLocalEdit,
      IdLocalRename,
      IdLocalDelete,
      IdRemoteCreateDirectory,
      IdRemoteCreateFile,
      IdRemoteEdit,
      IdRemoteRename,
      IdRemotePermissions,
      IdRemoteDelete,
      IdQueuePause,
      IdQueueCancel,
      IdQueueRetry,
      IdQueueRemove,
      IdQueueClear,
      IdQueueCopy,
      IdQueueCopyAll,
      IdQueueExport,
      IdQueueExportSelected,
      IdQueueExportAll,
      IdMessageLogClear,
      IdCheckForUpdates,
    };

    constexpr int IdQuickHistoryEntryBase = wxID_HIGHEST + 500;

    const wxDataFormat &FileListDragFormat()
    {
      static const wxDataFormat format{"application/x-havremote-file-list"};
      return format;
    }

    class FileListDropTarget final : public wxDropTarget
    {
    public:
      using InternalHandler = std::function<bool(std::string_view)>;
      using ExternalHandler = std::function<bool(const std::vector<std::filesystem::path> &)>;

      FileListDropTarget(InternalHandler internalHandler,
                         ExternalHandler externalHandler)
          : mInternalHandler(std::move(internalHandler)),
            mExternalHandler(std::move(externalHandler))
      {
        auto data = std::make_unique<wxDataObjectComposite>();

        mInternalData = new wxCustomDataObject(FileListDragFormat());
        mFileData = new wxFileDataObject();

        data->Add(mInternalData, true);
        data->Add(mFileData);

        SetDataObject(data.release());
      }

      wxDragResult OnData(wxCoord, wxCoord, wxDragResult) override
      {
        if (!GetData())
        {
          return wxDragNone;
        }

        const auto received = static_cast<wxDataObjectComposite *>(
                                  GetDataObject())
                                  ->GetReceivedFormat();

        if (received == FileListDragFormat())
        {
          const auto size = mInternalData->GetSize();
          const auto *bytes = static_cast<const char *>(mInternalData->GetData());

          return bytes != nullptr &&
                         mInternalHandler(std::string_view{bytes, size})
                     ? wxDragCopy
                     : wxDragNone;
        }

        if (received == wxDataFormat{wxDF_FILENAME})
        {
          std::vector<std::filesystem::path> paths;
          paths.reserve(mFileData->GetFilenames().size());

          for (const auto &path : mFileData->GetFilenames())
          {
            paths.emplace_back(platform::FromToolkitPath(path));
          }

          return !paths.empty() && mExternalHandler(paths)
                     ? wxDragCopy
                     : wxDragNone;
        }

        return wxDragNone;
      }

    private:
      wxCustomDataObject *mInternalData{};
      wxFileDataObject *mFileData{};
      InternalHandler mInternalHandler;
      ExternalHandler mExternalHandler;
    };

    wxString FromUtf8(const std::string_view value)
    {
      return wxString::FromUTF8(value.data(), value.size());
    }

    wxString Translated(const localization::TranslationCatalog &catalog,
                        const std::string_view key)
    {
      return FromUtf8(catalog.Text(key));
    }

    std::string ExternalEditorErrorText(
        const localization::TranslationCatalog &catalog,
        const platform::PlatformError &error)
    {
      return error.code == platform::PlatformErrorCode::UnsafeFileType
                 ? std::string{catalog.Text(
                       "externalEditor.error.systemDefaultUnsafeType")}
                 : error.message;
    }

    wxString EscapedMenuLabel(const std::string_view value)
    {
      // wxMenu interprets ampersands as mnemonic markers and tabs as accelerator
      // separators. Saved-site names are user data, not menu syntax. Replace all
      // C0 controls and DEL as well, not just the common line separators.
      wxString label;

      for (const wxUniChar character : FromUtf8(value))
      {
        const auto scalar = character.GetValue();

        if (scalar < 0x20 || scalar == 0x7f)
        {
          label += ' ';
        }
        else if (character == '&')
        {
          label += "&&";
        }
        else
        {
          label += character;
        }
      }

      return label;
    }

    wxString SavedSiteMenuLabel(const SiteProfile &site)
    {
      auto label = EscapedMenuLabel(site.name);
      auto visible = label;
      visible.Trim(true).Trim(false);

      return visible.empty() ? EscapedMenuLabel(site.host) : label;
    }

    std::string ParentDirectoryIdentity(const std::string_view currentDirectory)
    {
      // Local and remote paths cannot contain NUL, so this presentation-
      // only identity cannot collide with the stable identity of a real entry.
      // Including the displayed directory preserves selection during refreshes
      // without carrying it into a different directory after navigation.
      std::string identity(1, '\0');
      identity.append("parent:");
      identity.append(currentDirectory);

      return identity;
    }

    wxBitmapBundle EmbeddedButtonIcon(const wxString &resourceName)
    {
      return wxBitmapBundle::FromBitmap(LoadUiBitmap(resourceName));
    }

    void SetIconButtonText(wxBitmapButton &button, const wxString &text)
    {
      // Keep a descriptive label and control name for accessibility while
      // wxBU_NOTEXT hides the label on the icon button.
      button.SetLabel(text);
      button.SetToolTip(text);
      button.SetName(text);
    }

    wxBitmapButton *MakeIconButton(wxWindow *parent,
                                   const wxWindowID id,
                                   const wxString &resourceName,
                                   const wxString &text)
    {
      const auto bitmap = EmbeddedButtonIcon(resourceName);

      auto *button = new wxBitmapButton(parent, id, bitmap);

      SetIconButtonBitmap(*button, bitmap);
      SetIconButtonText(*button, text);

      return button;
    }

    FileListColumn ToFileListColumn(
        const config::FileListSortColumn column) noexcept
    {
      switch (column)
      {
      case config::FileListSortColumn::Name:
        return FileListColumn::Name;

      case config::FileListSortColumn::Size:
        return FileListColumn::Size;

      case config::FileListSortColumn::Type:
        return FileListColumn::Type;

      case config::FileListSortColumn::Modified:
        return FileListColumn::Modified;

      case config::FileListSortColumn::Permissions:
        return FileListColumn::Permissions;

      case config::FileListSortColumn::Owner:
        return FileListColumn::Owner;
      }

      return FileListColumn::Name;
    }

    config::FileListSortColumn PersistedFileListColumn(
        const FileListColumn column) noexcept
    {
      switch (column)
      {
      case FileListColumn::Name:
        return config::FileListSortColumn::Name;

      case FileListColumn::Size:
        return config::FileListSortColumn::Size;

      case FileListColumn::Type:
        return config::FileListSortColumn::Type;

      case FileListColumn::Modified:
        return config::FileListSortColumn::Modified;

      case FileListColumn::Permissions:
        return config::FileListSortColumn::Permissions;

      case FileListColumn::Owner:
        return config::FileListSortColumn::Owner;
      }

      return config::FileListSortColumn::Name;
    }

    FileListColumnWidths DisplayedColumnWidths(
        const std::span<const std::uint32_t> widths)
    {
      FileListColumnWidths result;
      result.reserve(widths.size());

      for (std::size_t index = 0; index < widths.size(); ++index)
      {
        result.push_back(FileListColumnWidth{
            .column = static_cast<FileListColumn>(index),
            .widthDips = widths[index],
        });
      }

      return result;
    }

    std::vector<std::uint32_t> PersistedColumnWidths(
        const FileListColumnWidths &widths)
    {
      std::vector<std::uint32_t> result;
      result.reserve(widths.size());

      for (const auto &width : widths)
      {
        result.push_back(width.widthDips);
      }

      return result;
    }

    std::string_view TranslatedOperation(
        const localization::TranslationCatalog &catalog,
        const std::string_view operation)
    {
      if (operation == "Connect")
      {
        return catalog.Text("operation.connect");
      }

      if (operation == "Create directory")
      {
        return catalog.Text("operation.createDirectory");
      }

      if (operation == "Create file")
      {
        return catalog.Text("operation.createFile");
      }

      if (operation == "Rename")
      {
        return catalog.Text("operation.rename");
      }

      if (operation == "Change permissions")
      {
        return catalog.Text("operation.changePermissions");
      }

      if (operation == "Delete")
      {
        return catalog.Text("operation.delete");
      }

      if (operation == "List directory")
      {
        return catalog.Text("operation.listDirectory");
      }

      return operation;
    }

    void LocalizeStandardButtons(
        wxWindow &window,
        const localization::TranslationCatalog &catalog)
    {
      const auto setLabel = [&](const int id, const std::string_view key)
      {
        if (auto *button = wxWindow::FindWindowById(id, &window))
        {
          button->SetLabel(Translated(catalog, key));
        }
      };

      setLabel(wxID_OK, "common.ok");
      setLabel(wxID_CANCEL, "common.cancel");
      setLabel(wxID_YES, "common.yes");
      setLabel(wxID_NO, "common.no");
    }

    int LocalizedMessageBox(const localization::TranslationCatalog &catalog,
                            const wxString &message,
                            const wxString &caption,
                            const long style,
                            wxWindow *parent)
    {
      wxMessageDialog dialog(parent, message, caption, style);

      if ((style & wxYES_NO) != 0)
      {
        if ((style & wxCANCEL) != 0)
        {
          dialog.SetYesNoCancelLabels(
              Translated(catalog, "common.yes"),
              Translated(catalog, "common.no"),
              Translated(catalog, "common.cancel"));
        }
        else
        {
          dialog.SetYesNoLabels(Translated(catalog, "common.yes"),
                                Translated(catalog, "common.no"));
        }
      }
      else if ((style & wxOK) != 0 && (style & wxCANCEL) != 0)
      {
        dialog.SetOKCancelLabels(Translated(catalog, "common.ok"),
                                 Translated(catalog, "common.cancel"));
      }
      else if ((style & wxOK) != 0)
      {
        dialog.SetOKLabel(Translated(catalog, "common.ok"));
      }

      dialog.CentreOnParent();

      return dialog.ShowModal();
    }

    template <typename... Arguments>
    wxString TranslatedFormat(const localization::TranslationCatalog &catalog,
                              const std::string_view key,
                              Arguments &&...arguments)
    {
      std::array<std::string, sizeof...(Arguments)> values{
          std::format("{}", std::forward<Arguments>(arguments))...};

      std::array<std::string_view, sizeof...(Arguments)> views{};

      for (std::size_t index = 0; index < values.size(); ++index)
      {
        views[index] = values[index];
      }

      return FromUtf8(catalog.Format(key, views));
    }

    std::string ToUtf8(const wxString &value)
    {
      const auto buffer = value.ToUTF8();
      return buffer ? std::string(buffer.data(), buffer.length()) : std::string{};
    }

    std::string BytesToString(const std::span<const std::byte> value)
    {
      return {reinterpret_cast<const char *>(value.data()), value.size()};
    }

    std::string GenericUtf8(const std::filesystem::path &path)
    {
      const auto value = path.generic_u8string();
      return {reinterpret_cast<const char *>(value.data()), value.size()};
    }

    std::optional<std::filesystem::path> LocalChildName(const wxString &value)
    {
      const auto utf8 = ToUtf8(value);

      if (!platform::IsLocalSafeFilename(utf8))
      {
        return std::nullopt;
      }

      const std::u8string bytes{reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()};

      auto path = std::filesystem::path{bytes};

      if (path.empty() || path.has_root_path() || path.has_parent_path() ||
          path.filename() != path)
      {
        return std::nullopt;
      }

      return path;
    }

    ProtocolKind ProtocolAt(const int selection)
    {
      switch (selection)
      {
      case 0:
        return ProtocolKind::Ftp;

      case 1:
        return ProtocolKind::FtpsExplicit;

      case 2:
        return ProtocolKind::FtpsImplicit;

      default:
        return ProtocolKind::Sftp;
      }
    }

    int ProtocolIndex(const ProtocolKind protocol)
    {
      switch (protocol)
      {
      case ProtocolKind::Ftp:
        return 0;

      case ProtocolKind::FtpsExplicit:
        return 1;

      case ProtocolKind::FtpsImplicit:
        return 2;

      case ProtocolKind::Sftp:
        return 3;
      }

      return 3;
    }

    std::string_view ProtocolTranslationKey(const ProtocolKind protocol)
    {
      switch (protocol)
      {
      case ProtocolKind::Ftp:
        return "quick.protocol.ftp";

      case ProtocolKind::FtpsExplicit:
        return "quick.protocol.ftpsExplicit";

      case ProtocolKind::FtpsImplicit:
        return "quick.protocol.ftpsImplicit";

      case ProtocolKind::Sftp:
        return "quick.protocol.sftp";
      }

      return "quick.protocol.sftp";
    }

    wxString QuickHistoryLabel(
        const config::QuickConnectHistoryEntry &entry,
        const localization::TranslationCatalog &catalog)
    {
      auto host = FromUtf8(entry.host);
      if (host.Contains(":"))
      {
        host = "[" + host + "]";
      }

      auto endpoint = entry.username.empty()
                          ? host
                          : FromUtf8(entry.username) + "@" + host;

      auto label = Translated(catalog, ProtocolTranslationKey(entry.protocol)) +
                   " - " + endpoint + ":" +
                   wxString::Format("%u", static_cast<unsigned>(entry.port));

      label.Replace("&", "&&");
      label.Replace("\r", " ");
      label.Replace("\n", " ");
      label.Replace("\t", " ");

      return label;
    }

    wxString FormatBytes(const std::uint64_t bytes)
    {
      constexpr std::array<const char *, 5> units{"B", "KiB", "MiB", "GiB", "TiB"};

      double amount = static_cast<double>(bytes);

      std::size_t unit = 0;

      while (amount >= 1024.0 && unit + 1 < units.size())
      {
        amount /= 1024.0;
        ++unit;
      }

      return unit == 0
                 ? wxString::Format("%llu B", static_cast<unsigned long long>(bytes))
                 : wxString::Format("%.1f %s", amount, FromUtf8(units[unit]).c_str());
    }

    wxString FormatEta(const std::chrono::seconds duration)
    {
      const auto total = std::max<std::int64_t>(0, duration.count());
      const auto days = total / 86'400;
      const auto hours = (total / 3'600) % 24;
      const auto minutes = (total / 60) % 60;
      const auto seconds = total % 60;

      return days == 0
                 ? wxString::Format("%02lld:%02lld:%02lld",
                                    static_cast<long long>(hours),
                                    static_cast<long long>(minutes),
                                    static_cast<long long>(seconds))
                 : wxString::Format("%lldd %02lld:%02lld:%02lld",
                                    static_cast<long long>(days),
                                    static_cast<long long>(hours),
                                    static_cast<long long>(minutes),
                                    static_cast<long long>(seconds));
    }

    wxString EntryType(const localization::TranslationCatalog &catalog,
                       const RemoteEntryKind kind,
                       const std::string_view filename)
    {
      switch (kind)
      {
      case RemoteEntryKind::File:
        if (const auto extension = FileListExtension(filename); !extension.empty())
        {
          return TranslatedFormat(catalog, "entry.kind.fileWithExtension",
                                  ToUtf8(FromUtf8(extension).Upper()));
        }
        return Translated(catalog, "entry.kind.file");

      case RemoteEntryKind::Directory:
        return Translated(catalog, "entry.kind.directory");

      case RemoteEntryKind::Symlink:
        return Translated(catalog, "entry.kind.symlink");

      case RemoteEntryKind::Other:
        return Translated(catalog, "entry.kind.other");
      }

      return Translated(catalog, "entry.kind.other");
    }

    FileListRowKind FileListKind(const RemoteEntryKind kind) noexcept
    {
      switch (kind)
      {
      case RemoteEntryKind::Directory:
        return FileListRowKind::Directory;

      case RemoteEntryKind::File:
        return FileListRowKind::File;

      case RemoteEntryKind::Symlink:
        return FileListRowKind::Symlink;

      case RemoteEntryKind::Other:
        return FileListRowKind::Other;
      }

      return FileListRowKind::Other;
    }

    RemoteEntryKind ToRemoteEntryKind(const LocalEntryKind kind) noexcept
    {
      switch (kind)
      {
      case LocalEntryKind::File:
        return RemoteEntryKind::File;

      case LocalEntryKind::Directory:
        return RemoteEntryKind::Directory;

      case LocalEntryKind::Symlink:
        return RemoteEntryKind::Symlink;

      case LocalEntryKind::Other:
        return RemoteEntryKind::Other;
      }

      return RemoteEntryKind::Other;
    }

    std::string FormattedModifiedTime(
        const std::optional<std::chrono::system_clock::time_point> &value)
    {
      if (!value)
      {
        return {};
      }

      const wxDateTime dateTime{std::chrono::system_clock::to_time_t(*value)};

      return dateTime.IsValid() ? ToUtf8(dateTime.Format("%x %X"))
                                : std::string{};
    }

    void SelectFileListIdentity(FileListCtrl &list,
                                const std::string_view identity)
    {
      const auto &rows = list.Rows();

      const auto found = std::ranges::find(rows, identity, &FileListRow::stableIdentity);

      if (found == rows.end())
      {
        return;
      }

      const auto visualIndex = static_cast<long>(std::distance(rows.begin(), found));

      list.SetItemState(-1, 0, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
      list.SetItemState(visualIndex,
                        wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                        wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
      list.EnsureVisible(visualIndex);
    }

    wxString TransferStateText(const localization::TranslationCatalog &catalog,
                               const TransferState state)
    {
      switch (state)
      {
      case TransferState::Queued:
        return Translated(catalog, "transfer.state.queued");

      case TransferState::Enumerating:
        return Translated(catalog, "transfer.state.enumerating");

      case TransferState::Running:
        return Translated(catalog, "transfer.state.running");

      case TransferState::Paused:
        return Translated(catalog, "transfer.state.paused");

      case TransferState::Completed:
        return Translated(catalog, "transfer.state.completed");

      case TransferState::Failed:
        return Translated(catalog, "transfer.state.failed");

      case TransferState::Cancelled:
        return Translated(catalog, "transfer.state.canceled");
      }

      return Translated(catalog, "transfer.state.unknown");
    }

    std::string_view DiagnosticPrefix(const localization::TranslationCatalog &catalog,
                                      const DiagnosticLevel level)
    {
      switch (level)
      {
      case DiagnosticLevel::Debug:
        return catalog.Text("log.level.debug");

      case DiagnosticLevel::Information:
        return catalog.Text("log.level.info");

      case DiagnosticLevel::Warning:
        return catalog.Text("log.level.warning");

      case DiagnosticLevel::Error:
        return catalog.Text("log.level.error");
      }

      return catalog.Text("log.level.info");
    }

    RemoteError UiError(const RemoteErrorCode code, std::string message)
    {
      return RemoteError{.code = code, .message = std::move(message)};
    }

    std::string ConfigErrorMessage(const config::ConfigError &error)
    {
      std::string message = error.path.empty() ? std::string{} : error.path.string() + ": ";

      if (error.line)
      {
        message += std::to_string(*error.line);

        if (error.column)
        {
          message += ':' + std::to_string(*error.column);
        }

        message += ": ";
      }

      message += error.message;

      return message;
    }

    class ConflictDialog final : public wxDialog
    {
    public:
      ConflictDialog(wxWindow *parent,
                     const ConflictChallenge &challenge,
                     const localization::TranslationCatalog &catalog)
          : wxDialog(parent,
                     wxID_ANY,
                     Translated(
                         catalog,
                         challenge.reason == ConflictReason::RemoteChanged
                             ? "externalEditor.remoteChangedTitle"
                             : "conflict.title"),
                     wxDefaultPosition,
                     wxDefaultSize)
      {
        auto *root = new wxBoxSizer(wxVERTICAL);

        const auto item = challenge.job.direction == TransferDirection::Download
                              ? GenericUtf8(challenge.job.localPath)
                              : challenge.job.remotePath.DisplayUtf8();

        root->Add(new wxStaticText(
                      this,
                      wxID_ANY,
                      TranslatedFormat(
                          catalog,
                          challenge.reason == ConflictReason::RemoteChanged
                              ? "externalEditor.remoteChangedMessage"
                              : "conflict.destinationExists",
                          item)),
                  0,
                  wxALL,
                  12);

        mChoice = new wxChoice(this, wxID_ANY);

        const auto appendChoice = [this](const wxString &label,
                                         const ConflictPolicy policy)
        {
          mChoice->Append(label);
          mDisplayedPolicies.push_back(policy);
        };

        appendChoice(Translated(catalog, "conflict.overwrite"),
                     ConflictPolicy::Overwrite);
        appendChoice(Translated(catalog, "conflict.skip"),
                     ConflictPolicy::Skip);

        if (challenge.reason != ConflictReason::RemoteChanged)
        {
          appendChoice(Translated(catalog, "conflict.rename"),
                       ConflictPolicy::Rename);

          if (challenge.safeResumeAvailable)
          {
            appendChoice(Translated(catalog, "conflict.resume"),
                         ConflictPolicy::Resume);
          }
        }

        const auto defaultChoice = std::ranges::find(
            mDisplayedPolicies, ConflictPolicy::Skip);

        mChoice->SetSelection(static_cast<int>(
            defaultChoice - mDisplayedPolicies.begin()));

        root->Add(mChoice, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

        if (challenge.applyToRemainingAvailable)
        {
          mApply = new wxCheckBox(
              this,
              wxID_ANY,
              Translated(catalog, "conflict.applyRemaining"));

          root->Add(mApply, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
        }

        root->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);

        SetSizerAndFit(root);

        LocalizeStandardButtons(*this, catalog);

        CentreOnParent();
      }

      ConflictResolution Resolution() const
      {
        const auto selection = mChoice->GetSelection();

        ConflictPolicy policy{ConflictPolicy::Skip};

        if (selection != wxNOT_FOUND &&
            static_cast<std::size_t>(selection) < mDisplayedPolicies.size())
        {
          policy = mDisplayedPolicies[static_cast<std::size_t>(selection)];
        }

        return ConflictResolution{policy, mApply && mApply->GetValue()};
      }

    private:
      wxChoice *mChoice{};
      wxCheckBox *mApply{};
      std::vector<ConflictPolicy> mDisplayedPolicies;
    };
  } // namespace

  struct MainFrame::ConnectionTab final
  {
    ~ConnectionTab()
    {
      controller.reset();

      ClearSensitiveString(ephemeralPassword);
      ClearSensitiveString(activeCredentialSecret);
      ClearSensitiveString(activePassphraseSecret);
    }

    std::string connectionId;
    wxPanel *panel{};
    std::unique_ptr<LocalDirectoryLoader> localDirectoryLoader;
    std::unique_ptr<RemoteController> controller;
    SiteProfile quickDraft;
    std::string quickSiteId;
    std::string quickRuntimeSiteId;
    std::optional<SiteEndpointIdentity> quickRuntimeEndpoint;
    std::string ephemeralPassword;
    std::string activeCredentialSecret;
    std::string activePassphraseSecret;
    std::string activeConnectionSiteId;
    std::uint64_t activeConnectionGeneration{};
    std::optional<SiteProfile> activeConnectionSite;
    std::optional<config::WorkspaceConnectionIdentity> workspaceIdentity;
    std::filesystem::path localDirectory;
    std::vector<LocalDirectoryEntrySnapshot> localEntries;
    std::optional<std::string> preferredLocalSelectionIdentity;
    std::optional<std::filesystem::path> localDirectoryFallback;
    bool localDirectoryLoading{};
    bool localRefreshPending{};
    RemotePath remoteDirectory{RemotePath::Root()};
    std::vector<RemoteEntry> remoteEntries;
    std::vector<QueuedTransfer> transfers;
    std::optional<std::string> preferredRemoteSelectionIdentity;
    ConnectionState connectionState{ConnectionState::Disconnected};
    bool remoteViewReady{};

    wxStaticText *localHeading{};
    wxDirPickerCtrl *localPath{};
    FileListCtrl *localList{};
    wxBitmapButton *localUpButton{};
    wxBitmapButton *localRefreshButton{};
    wxButton *localCreateButton{};
    wxButton *localEditButton{};
    wxButton *localRenameButton{};
    wxButton *localDeleteButton{};
    wxBitmapButton *uploadButton{};
    wxBitmapButton *downloadButton{};
    wxStaticText *remoteHeading{};
    wxTextCtrl *remotePath{};
    FileListCtrl *remoteList{};
    wxBitmapButton *remoteUpButton{};
    wxBitmapButton *remoteRefreshButton{};
    wxButton *remoteCreateDirectoryButton{};
    wxButton *remoteCreateFileButton{};
    wxButton *remoteEditButton{};
    wxButton *remoteRenameButton{};
    wxButton *remotePermissionsButton{};
    wxButton *remoteDeleteButton{};
  };

  struct MainFrame::PendingFileDrag final
  {
    std::string token;
    std::string sourceConnectionId;
    bool sourceIsLocal{};
    std::vector<std::string> sourceIdentities;
  };

  struct MainFrame::ExternalEditSession final
  {
    struct LocalStamp final
    {
      std::uintmax_t size{};
      std::filesystem::file_time_type modifiedAt;

      friend bool operator==(const LocalStamp &, const LocalStamp &) = default;
    };

    enum class Phase
    {
      Downloading,
      Watching,
      Uploading,
      Inspecting,
      Failed,
      Detached,
    };

    std::string id;
    std::string connectionId;
    std::string siteId;
    std::uint64_t generation{};
    RemotePath remotePath;
    std::filesystem::path localPath;
    RemoteFileRevision remoteRevision;
    std::string downloadJobId;
    std::optional<std::string> uploadJobId;
    std::optional<std::string> inspectionRequestId;
    std::optional<LocalStamp> synchronizedStamp;
    std::optional<LocalStamp> observedStamp;
    std::optional<LocalStamp> queuedStamp;
    std::optional<std::chrono::steady_clock::time_point> stableSince;
    std::optional<std::chrono::steady_clock::time_point> missingSince;
    Phase phase{Phase::Downloading};
    bool failureReported{};

    [[nodiscard]] std::optional<LocalStamp> ReadLocalStamp() const
    {
      std::error_code error;

      const auto status = std::filesystem::symlink_status(localPath, error);

      if (error || !std::filesystem::is_regular_file(status) ||
          std::filesystem::is_symlink(status))
      {
        return std::nullopt;
      }

      const auto size = std::filesystem::file_size(localPath, error);

      if (error)
      {
        return std::nullopt;
      }

      const auto modified = std::filesystem::last_write_time(localPath, error);

      if (error)
      {
        return std::nullopt;
      }

      return LocalStamp{size, modified};
    }
  };

  MainFrame::MainFrame(std::unique_ptr<config::IConfigRepository> configRepository,
                       std::unique_ptr<config::IQueueRepository> queueRepository,
                       std::unique_ptr<platform::ICredentialStore> credentialStore,
                       std::unique_ptr<platform::IRecycleBin> recycleBin,
                       std::filesystem::path knownHostsFile,
                       std::filesystem::path helpRoot,
                       std::shared_ptr<const localization::TranslationCatalog> translations,
                       std::expected<config::ConfigLoadResult, config::ConfigError>
                           initialConfiguration,
                       std::expected<config::QueueLoadResult, config::ConfigError>
                           initialQueue
#if defined(HAVREMOTE_UPDATE_SIMULATION)
                       ,
                       const std::optional<updates::UpdateSimulationScenario> updateSimulation
#endif
                       )
      : wxFrame(nullptr,
                wxID_ANY,
                wxT("havRemote"),
                wxDefaultPosition,
                wxDefaultSize,
                wxDEFAULT_FRAME_STYLE),
        mConfigRepository(std::move(configRepository)),
        mQueueRepository(std::move(queueRepository)),
        mCredentialStore(std::move(credentialStore)),
        mRecycleBin(std::move(recycleBin)),
#if defined(HAVREMOTE_UPDATE_SIMULATION)
        mUpdateService(MakeApplicationUpdateService(updateSimulation)),
        mUpdateSimulation(updateSimulation),
#else
        mUpdateService(MakeApplicationUpdateService()),
#endif
        mKnownHostsFile(std::move(knownHostsFile)),
        mHelpRoot(std::move(helpRoot)),
        mTranslations(std::move(translations))
  {
    const auto applicationIcon = LoadApplicationIcon();

    if (applicationIcon.IsOk())
    {
      SetIcon(applicationIcon);
    }

    BuildMenus();

    BuildLayout();

    ApplyWindowGeometry(config::MainWindowState{});

    InitializeConfiguration(std::move(initialConfiguration));

    CreateStatusBar(3);

    SetStatusWidths(3, std::array<int, 3>{-2, -1, -1}.data());

    SetStatusText(Translated(*mTranslations, "status.notConnected"), 0);
    SetStatusText(Translated(*mTranslations, "status.queueEmpty"), 1);
    SetStatusText(Translated(*mTranslations, "status.trustEnforced"), 2);

    mTransferRuntime = std::make_shared<TransferRuntime>(
        mConfiguration.settings.transferConcurrency, platform::CheckLocalPathConflict);

    mExternalEditTimer = std::make_unique<wxTimer>(this);
    Bind(wxEVT_TIMER,
         &MainFrame::OnExternalEditTimer,
         this,
         mExternalEditTimer->GetId());

    mExternalEditTimer->Start(750);

    mTransferFailureTimer = std::make_unique<wxTimer>(this);
    Bind(wxEVT_TIMER,
         &MainFrame::OnTransferFailureTimer,
         this,
         mTransferFailureTimer->GetId());

    mWindowGeometrySaveTimer = std::make_unique<wxTimer>(this);
    Bind(wxEVT_TIMER,
         &MainFrame::OnWindowGeometrySaveTimer,
         this,
         mWindowGeometrySaveTimer->GetId());

    mRestoringWorkspace = true;

    InitializeTransferQueue(std::move(initialQueue));

    mRestoringWorkspace = false;

    RestoreFileListSortSettings();
    RestoreFileListColumnWidths();

    if (auto *const selected = FindConnectionTab(
            mConfiguration.workspace.selectedConnectionId))
    {
      const int index = mConnectionNotebook->GetPageIndex(selected->panel);

      if (index != wxNOT_FOUND)
      {
        mConnectionNotebook->SetSelection(static_cast<std::size_t>(index));
      }
    }

    PersistOpenConnectionTabs();

    UpdateSelectedConnectionUi();

    mLastPersistedWindowState = mConfiguration.workspace.mainWindow;

    Bind(wxEVT_MOVE, &MainFrame::OnWindowMoved, this);
    Bind(wxEVT_SIZE, &MainFrame::OnWindowSized, this);

#ifdef __WXMSW__
    Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent &event)
         {
           event.Skip();

           // Re-measure after wx has updated the child controls' fonts
           CallAfter([this]
                     {
                       SizeCompactIconButtons();
                       Layout();
                     }); });
#endif

#if defined(HAVREMOTE_UPDATE_SIMULATION)
    if (mUpdateSimulation)
    {
      const auto scenario = updates::UpdateSimulationScenarioName(*mUpdateSimulation);

      SetTitle("havRemote [Update simulation: " + FromUtf8(scenario) + "]");

      AppendLog(DiagnosticLevel::Information,
                "Update simulation: " + std::string{scenario} +
                    ". Update requests and release links stay offline. "
                    "Normal application settings and credentials are not used.");

      if (const auto path = mConfigRepository->ConfigPath(); path)
      {
        AppendLog(DiagnosticLevel::Debug,
                  "Update simulation configuration: " + GenericUtf8(*path));
      }
    }
#endif

    CallAfter([this]
              { EraseReleasedCredentials(); });
  }

  MainFrame::~MainFrame()
  {
    mTransferFailureTimer->Stop();

    if (mUpdateCheckWorker.joinable())
    {
      mUpdateCheckWorker.request_stop();
      mUpdateCheckWorker.join();
    }

    if (mExternalEditTimer)
    {
      mExternalEditTimer->Stop();
    }

    if (mWindowGeometrySaveTimer)
    {
      mWindowGeometrySaveTimer->Stop();
    }

    PrepareTransferQueueForShutdown();

    (void)PersistTransferQueue();

    mConnectionTabs.clear();

    EraseReleasedCredentials();
  }

  void MainFrame::ShowWithRestoredGeometry()
  {
    if (mWindowGeometryInitialized)
    {
      Show();

      return;
    }

    // Apply the saved monitor, DPI, bounds, and maximized state before first paint.
    // wxMSW defers Maximize() for a hidden frame until Show().
    ApplyWindowGeometry(mConfiguration.workspace.mainWindow);

    Show();

    mWindowGeometryInitialized = true;

    mConfiguration.workspace.mainWindow = mNormalWindowState;

    if (mConfiguration.workspace.mainWindow != mLastPersistedWindowState)
    {
      ScheduleWindowGeometrySave();
    }

    ScheduleAutomaticUpdateCheck();
  }

  void MainFrame::BuildMenus()
  {
    auto *file = new wxMenu;
    file->Append(
        IdNewConnectionTab,
        Translated(*mTranslations, "menu.file.newConnectionTab"),
        Translated(*mTranslations, "menu.file.newConnectionTabHelp"));
    file->Append(IdCloseConnectionTab,
                 Translated(*mTranslations, "menu.file.closeConnectionTab"),
                 Translated(*mTranslations, "menu.file.closeConnectionTabHelp"));
    file->AppendSeparator();
    file->Append(IdSiteManager,
                 Translated(*mTranslations, "menu.file.siteManager"),
                 Translated(*mTranslations, "menu.file.siteManagerHelp"));
    file->Append(IdDisconnect,
                 Translated(*mTranslations, "menu.file.disconnect"),
                 Translated(*mTranslations, "menu.file.disconnectHelp"));
    file->AppendSeparator();
    file->Append(wxID_EXIT,
                 Translated(*mTranslations, "menu.file.exit"),
                 Translated(*mTranslations, "menu.file.exitHelp"));

    auto *menuBar = new wxMenuBar;
    menuBar->Append(file, Translated(*mTranslations, "menu.file.title"));

    auto *preferences = new wxMenu;
    preferences->Append(wxID_PREFERENCES,
                        Translated(*mTranslations, "menu.preferences.settings"),
                        Translated(*mTranslations, "menu.preferences.settingsHelp"));

    menuBar->Append(preferences,
                    Translated(*mTranslations, "menu.preferences.title"));

    auto *help = new wxMenu;
    help->Append(wxID_HELP,
                 Translated(*mTranslations, "menu.help.contents"),
                 Translated(*mTranslations, "menu.help.contentsHelp"));
    help->Append(
        IdCheckForUpdates,
        Translated(*mTranslations, "menu.help.checkForUpdates"),
        Translated(*mTranslations, "menu.help.checkForUpdatesHelp"));
    help->AppendSeparator();
    help->Append(wxID_ABOUT,
                 Translated(*mTranslations, "menu.help.about"),
                 Translated(*mTranslations, "menu.help.aboutHelp"));
    menuBar->Append(help, Translated(*mTranslations, "menu.help.title"));

    SetMenuBar(menuBar);

    Bind(wxEVT_MENU, &MainFrame::OnSiteManager, this, IdSiteManager);
    Bind(wxEVT_MENU, &MainFrame::OnNewConnectionTab, this, IdNewConnectionTab);
    Bind(wxEVT_MENU, &MainFrame::OnCloseConnectionTab, this, IdCloseConnectionTab);
    Bind(wxEVT_MENU, &MainFrame::OnSettings, this, wxID_PREFERENCES);
    Bind(wxEVT_MENU, &MainFrame::OnHelp, this, wxID_HELP);
    Bind(wxEVT_MENU,
         &MainFrame::OnCheckForUpdates,
         this,
         IdCheckForUpdates);
    Bind(wxEVT_MENU, &MainFrame::OnAbout, this, wxID_ABOUT);
    Bind(wxEVT_MENU, &MainFrame::OnDisconnect, this, IdDisconnect);
    Bind(wxEVT_MENU, &MainFrame::OnExit, this, wxID_EXIT);
  }

  void MainFrame::BuildLayout()
  {
    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *quick = new wxBoxSizer(wxHORIZONTAL);

    mQuickHeading = new wxStaticText(
        this, wxID_ANY, Translated(*mTranslations, "quick.heading"));

    quick->Add(mQuickHeading,
               0,
               wxALIGN_CENTER_VERTICAL | wxRIGHT,
               8);

    mQuickProtocol = new wxChoice(this, IdQuickProtocol, wxDefaultPosition, wxSize{145, -1});
    mQuickProtocol->Append(Translated(*mTranslations, "quick.protocol.ftp"));
    mQuickProtocol->Append(Translated(*mTranslations, "quick.protocol.ftpsExplicit"));
    mQuickProtocol->Append(Translated(*mTranslations, "quick.protocol.ftpsImplicit"));
    mQuickProtocol->Append(Translated(*mTranslations, "quick.protocol.sftp"));
    mQuickProtocol->SetSelection(3);
    mQuickHost = new wxTextCtrl(this, wxID_ANY, {}, wxDefaultPosition, wxSize{190, -1});
    mQuickHost->SetHint(Translated(*mTranslations, "quick.hostHint"));
    mQuickPort = new wxSpinCtrl(this, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535, 22);
    mQuickUsername = new wxTextCtrl(this, wxID_ANY, {}, wxDefaultPosition, wxSize{145, -1});
    mQuickUsername->SetHint(Translated(*mTranslations, "quick.usernameHint"));
    mQuickPassword = new wxTextCtrl(this,
                                    wxID_ANY,
                                    {},
                                    wxDefaultPosition,
                                    wxSize{145, -1},
                                    wxTE_PASSWORD | wxTE_PROCESS_ENTER);
    mQuickPassword->SetHint(Translated(*mTranslations, "quick.passwordHint"));
    mConnectButton = MakeIconButton(
        this,
        IdQuickConnect,
        "HAVREMOTE_CONNECT_ICON",
        Translated(*mTranslations, "quick.connect"));
    mQuickClearButton = MakeIconButton(
        this,
        IdQuickClear,
        "HAVREMOTE_CLEAR_ICON",
        Translated(*mTranslations, "quick.clear"));
    mNewConnectionButton = MakeIconButton(
        this,
        IdNewConnectionTab,
        "HAVREMOTE_NEW_CONNECTION_ICON",
        Translated(*mTranslations, "connection.tab.new"));
    mQuickHistoryButton = MakeIconButton(
        this,
        IdQuickHistory,
        "HAVREMOTE_HISTORY_ICON",
        Translated(*mTranslations, "quick.history"));
    mSiteManagerButton = new SplitButton(
        this,
        IdSiteManager,
        EmbeddedButtonIcon("HAVREMOTE_SITE_MANAGER_ICON"),
        Translated(*mTranslations, "quick.siteManager"),
        Translated(*mTranslations, "quick.savedSites"),
        [this](wxMenu &menu)
        { PopulateSavedSitesMenu(menu); });
    quick->Add(mQuickProtocol, 0, wxEXPAND | wxRIGHT, 5);
    quick->Add(mQuickHost, 1, wxEXPAND | wxRIGHT, 5);
    quick->Add(mQuickPort, 0, wxEXPAND | wxRIGHT, 5);
    quick->Add(mQuickUsername, 0, wxEXPAND | wxRIGHT, 5);
    quick->Add(mQuickPassword, 0, wxEXPAND | wxRIGHT, 5);
    quick->Add(mConnectButton, 0, wxEXPAND | wxRIGHT, 5);
    quick->Add(mQuickClearButton, 0, wxEXPAND | wxRIGHT, 5);
    quick->Add(mNewConnectionButton, 0, wxEXPAND | wxRIGHT, 5);
    quick->Add(mQuickHistoryButton, 0, wxEXPAND | wxRIGHT, 5);
    quick->Add(mSiteManagerButton, 0, wxEXPAND);
    root->Add(quick, 0, wxEXPAND | wxALL, 8);
    root->Add(new wxStaticLine(this), 0, wxEXPAND);

    mConnectionNotebook = new wxAuiNotebook(
        this,
        wxID_ANY,
        wxDefaultPosition,
        wxDefaultSize,
        wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE | wxAUI_NB_SCROLL_BUTTONS |
            wxAUI_NB_CLOSE_ON_ALL_TABS | wxAUI_NB_MIDDLE_CLICK_CLOSE);

    root->Add(mConnectionNotebook, 3, wxEXPAND | wxALL, 8);

    auto *lower = new wxBoxSizer(wxHORIZONTAL);
    auto *logPanel = new wxPanel(this);
    auto *logSizer = new wxBoxSizer(wxVERTICAL);
    auto *logHeadingSizer = new wxBoxSizer(wxHORIZONTAL);

    mLogHeading = new wxStaticText(
        logPanel, wxID_ANY, Translated(*mTranslations, "log.heading"));
    logHeadingSizer->Add(mLogHeading, 1, wxALIGN_CENTER_VERTICAL);
    mMessageLogClearButton = MakeIconButton(
        logPanel,
        IdMessageLogClear,
        "HAVREMOTE_CLEAR_ICON",
        Translated(*mTranslations, "log.clear"));
    mMessageLogClearButton->Disable();
    logHeadingSizer->Add(mMessageLogClearButton, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
    logSizer->Add(logHeadingSizer, 0, wxEXPAND | wxBOTTOM, 4);
    mMessageLog = new wxTextCtrl(logPanel,
                                 wxID_ANY,
                                 {},
                                 wxDefaultPosition,
                                 wxDefaultSize,
                                 wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
    logSizer->Add(mMessageLog, 1, wxEXPAND);
    logPanel->SetSizer(logSizer);
    lower->Add(logPanel, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    auto *queuePanel = new wxPanel(this);
    auto *queueSizer = new wxBoxSizer(wxVERTICAL);

    mTransferNotebook = new wxNotebook(queuePanel, wxID_ANY);

    const auto makeTransferList = [this](const wxString &title, wxListCtrl *&output)
    {
      output = new wxListCtrl(mTransferNotebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
      output->AppendColumn(Translated(*mTranslations, "column.connection"), wxLIST_FORMAT_LEFT, 130);
      output->AppendColumn(Translated(*mTranslations, "column.direction"), wxLIST_FORMAT_LEFT, 80);
      output->AppendColumn(Translated(*mTranslations, "column.item"), wxLIST_FORMAT_LEFT, 190);
      output->AppendColumn(Translated(*mTranslations, "column.progress"), wxLIST_FORMAT_RIGHT, 90);
      output->AppendColumn(Translated(*mTranslations, "column.speed"), wxLIST_FORMAT_RIGHT, 95);
      output->AppendColumn(Translated(*mTranslations, "column.eta"), wxLIST_FORMAT_RIGHT, 90);
      output->AppendColumn(Translated(*mTranslations, "column.state"), wxLIST_FORMAT_LEFT, 85);
      output->AppendColumn(Translated(*mTranslations, "column.details"), wxLIST_FORMAT_LEFT, 220);
      mTransferNotebook->AddPage(output, title);
    };

    makeTransferList(Translated(*mTranslations, "queue.tab.queue"), mQueuedList);
    makeTransferList(Translated(*mTranslations, "queue.tab.failed"), mFailedList);
    makeTransferList(Translated(*mTranslations, "queue.tab.completed"), mCompletedList);
    queueSizer->Add(mTransferNotebook, 1, wxEXPAND);

    // Keep natural button widths, including the last button on a wrapped row
    auto *queueButtons = new wxWrapSizer(wxHORIZONTAL, wxREMOVE_LEADING_SPACES);
    mQueuePauseButton = new wxButton(
        queuePanel,
        IdQueuePause,
        Translated(*mTranslations, "queue.pause"));
    mQueueCancelButton = new wxButton(
        queuePanel,
        IdQueueCancel,
        Translated(*mTranslations, "queue.cancel"));
    mQueueRetryButton = new wxButton(
        queuePanel,
        IdQueueRetry,
        Translated(*mTranslations, "queue.retry"));
    mQueueRemoveButton = new wxButton(
        queuePanel,
        IdQueueRemove,
        Translated(*mTranslations, "queue.remove"));
    mQueueClearButton = new wxButton(
        queuePanel,
        IdQueueClear,
        Translated(*mTranslations, "queue.clearView"));
    mQueueCopyButton = new wxButton(
        queuePanel,
        IdQueueCopy,
        Translated(*mTranslations, "queue.copy"));
    mQueueExportButton = new wxButton(
        queuePanel,
        IdQueueExport,
        Translated(*mTranslations, "queue.export"));
    mQueuePauseButton->Enable(false);
    mQueueCancelButton->Enable(false);
    mQueueRetryButton->Enable(false);
    mQueueRemoveButton->Enable(false);
    mQueueClearButton->Enable(false);
    mQueueCopyButton->Enable(false);
    mQueueExportButton->Enable(false);
    queueButtons->Add(mQueuePauseButton,
                      0,
                      wxRIGHT,
                      4);
    queueButtons->Add(mQueueCancelButton,
                      0,
                      wxRIGHT,
                      4);
    queueButtons->Add(mQueueRetryButton, 0);
    queueButtons->Add(mQueueRemoveButton, 0, wxLEFT, 4);
    queueButtons->Add(mQueueClearButton, 0, wxLEFT, 4);
    queueButtons->Add(mQueueCopyButton, 0, wxLEFT, 4);
    queueButtons->Add(mQueueExportButton, 0, wxLEFT, 4);
    queueSizer->Add(queueButtons, 0, wxEXPAND | wxTOP, 5);
    queuePanel->SetSizer(queueSizer);
    lower->Add(queuePanel, 1, wxEXPAND | wxRIGHT | wxBOTTOM, 8);
    root->Add(lower, 2, wxEXPAND);

    SetSizer(root);
    SizeCompactIconButtons();

    Bind(EVT_HAVREMOTE_CONTROLLER, &MainFrame::OnControllerEvent, this);
    Bind(EVT_HAVREMOTE_LOCAL_DIRECTORY,
         &MainFrame::OnLocalDirectoryLoaded,
         this);
    Bind(EVT_HAVREMOTE_UPDATE_CHECK,
         &MainFrame::OnUpdateCheckFinished,
         this);
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);

    mConnectionNotebook->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGING,
                              &MainFrame::OnConnectionTabChanging,
                              this);
    mConnectionNotebook->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED,
                              &MainFrame::OnConnectionTabChanged,
                              this);
    mConnectionNotebook->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE,
                              &MainFrame::OnConnectionTabClose,
                              this);
    mConnectionNotebook->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSED,
                              &MainFrame::OnConnectionTabClosed,
                              this);
    mConnectionNotebook->Bind(
        wxEVT_AUINOTEBOOK_DRAG_DONE,
        [this](wxAuiNotebookEvent &event)
        {
          event.Skip();
          CallAfter([this]
                    { PersistOpenConnectionTabs(); });
        });

    Bind(wxEVT_BUTTON, &MainFrame::OnQuickConnectButton, this, IdQuickConnect);
    Bind(wxEVT_TEXT_ENTER, &MainFrame::OnQuickConnect, this, mQuickPassword->GetId());
    Bind(wxEVT_BUTTON, &MainFrame::OnQuickClear, this, IdQuickClear);
    Bind(wxEVT_BUTTON,
         &MainFrame::OnNewConnectionTab,
         this,
         IdNewConnectionTab);
    Bind(wxEVT_BUTTON, &MainFrame::OnQuickHistory, this, IdQuickHistory);
    Bind(wxEVT_MENU, &MainFrame::OnQuickHistoryClear, this, IdQuickHistoryClear);
    Bind(wxEVT_MENU,
         &MainFrame::OnQuickHistorySelection,
         this,
         IdQuickHistoryEntryBase,
         IdQuickHistoryEntryBase +
             static_cast<int>(config::MaxQuickConnectHistoryEntries) - 1);
    Bind(wxEVT_CHOICE, [this](wxCommandEvent &)
         { mQuickPort->SetValue(DefaultPort(ProtocolAt(mQuickProtocol->GetSelection()))); }, IdQuickProtocol);
    Bind(wxEVT_BUTTON, &MainFrame::OnSiteManager, this, IdSiteManager);
    Bind(wxEVT_TEXT_ENTER, &MainFrame::OnRemotePathEnter, this, IdRemotePath);
    Bind(wxEVT_BUTTON, &MainFrame::OnLocalUp, this, IdLocalUp);
    Bind(wxEVT_BUTTON, &MainFrame::OnLocalRefresh, this, IdLocalRefresh);
    Bind(wxEVT_BUTTON, &MainFrame::OnRemoteUp, this, IdRemoteUp);
    Bind(wxEVT_BUTTON, &MainFrame::OnRemoteRefresh, this, IdRemoteRefresh);
    Bind(wxEVT_LIST_ITEM_ACTIVATED, &MainFrame::OnLocalActivated, this, IdLocalList);
    Bind(wxEVT_LIST_ITEM_ACTIVATED, &MainFrame::OnRemoteActivated, this, IdRemoteList);
    Bind(wxEVT_LIST_COL_CLICK,
         &MainFrame::OnFileListColumnClick,
         this,
         IdLocalList,
         IdRemoteList);
    Bind(wxEVT_LIST_COL_END_DRAG,
         &MainFrame::OnFileListColumnResized,
         this,
         IdLocalList,
         IdRemoteList);
    Bind(wxEVT_LIST_ITEM_SELECTED,
         &MainFrame::OnFileSelectionChanged,
         this,
         IdLocalList,
         IdRemoteList);
    Bind(wxEVT_LIST_ITEM_DESELECTED,
         &MainFrame::OnFileSelectionChanged,
         this,
         IdLocalList,
         IdRemoteList);
    Bind(wxEVT_LIST_ITEM_RIGHT_CLICK,
         &MainFrame::OnLocalListRightClick,
         this,
         IdLocalList);
    Bind(wxEVT_LIST_ITEM_RIGHT_CLICK,
         &MainFrame::OnRemoteListRightClick,
         this,
         IdRemoteList);
    Bind(wxEVT_LIST_BEGIN_DRAG,
         &MainFrame::OnFileListBeginDrag,
         this,
         IdLocalList,
         IdRemoteList);
    Bind(wxEVT_LIST_BEGIN_LABEL_EDIT,
         &MainFrame::OnLocalBeginLabelEdit,
         this,
         IdLocalList);
    Bind(wxEVT_LIST_END_LABEL_EDIT,
         &MainFrame::OnLocalEndLabelEdit,
         this,
         IdLocalList);
    Bind(wxEVT_LIST_BEGIN_LABEL_EDIT,
         &MainFrame::OnRemoteBeginLabelEdit,
         this,
         IdRemoteList);
    Bind(wxEVT_LIST_END_LABEL_EDIT,
         &MainFrame::OnRemoteEndLabelEdit,
         this,
         IdRemoteList);
    Bind(wxEVT_BUTTON, &MainFrame::OnUpload, this, IdUpload);
    Bind(wxEVT_BUTTON, &MainFrame::OnDownload, this, IdDownload);
    Bind(wxEVT_BUTTON, &MainFrame::OnLocalCreate, this, IdLocalCreate);
    Bind(wxEVT_BUTTON, &MainFrame::OnLocalEdit, this, IdLocalEdit);
    Bind(wxEVT_BUTTON, &MainFrame::OnLocalRename, this, IdLocalRename);
    Bind(wxEVT_BUTTON, &MainFrame::OnLocalDelete, this, IdLocalDelete);
    Bind(wxEVT_BUTTON,
         &MainFrame::OnRemoteCreateDirectory,
         this,
         IdRemoteCreateDirectory);
    Bind(wxEVT_BUTTON,
         &MainFrame::OnRemoteCreateFile,
         this,
         IdRemoteCreateFile);
    Bind(wxEVT_BUTTON, &MainFrame::OnRemoteEdit, this, IdRemoteEdit);
    Bind(wxEVT_BUTTON, &MainFrame::OnRemoteRename, this, IdRemoteRename);
    Bind(wxEVT_BUTTON,
         &MainFrame::OnRemotePermissions,
         this,
         IdRemotePermissions);
    Bind(wxEVT_BUTTON, &MainFrame::OnRemoteDelete, this, IdRemoteDelete);
    Bind(wxEVT_BUTTON, &MainFrame::OnQueuePause, this, IdQueuePause);
    Bind(wxEVT_BUTTON, &MainFrame::OnQueueCancel, this, IdQueueCancel);
    Bind(wxEVT_BUTTON, &MainFrame::OnQueueRetry, this, IdQueueRetry);
    Bind(wxEVT_BUTTON, &MainFrame::OnQueueRemove, this, IdQueueRemove);
    Bind(wxEVT_BUTTON, &MainFrame::OnQueueClear, this, IdQueueClear);
    Bind(wxEVT_BUTTON, &MainFrame::OnQueueCopy, this, IdQueueCopy);
    Bind(wxEVT_BUTTON, &MainFrame::OnQueueExport, this, IdQueueExport);
    Bind(wxEVT_BUTTON,
         &MainFrame::OnMessageLogClear,
         this,
         IdMessageLogClear);

    for (auto *list : {mQueuedList, mFailedList, mCompletedList})
    {
      list->Bind(wxEVT_LIST_ITEM_SELECTED,
                 &MainFrame::OnQueueSelectionChanged,
                 this);
      list->Bind(wxEVT_LIST_ITEM_DESELECTED,
                 &MainFrame::OnQueueSelectionChanged,
                 this);
      list->Bind(wxEVT_KEY_DOWN, &MainFrame::OnQueueListKeyDown, this);
      list->Bind(wxEVT_CONTEXT_MENU, &MainFrame::OnQueueContextMenu, this);
    }

    mTransferNotebook->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED,
                            &MainFrame::OnTransferViewChanged,
                            this);
  }

  MainFrame::ConnectionTab *MainFrame::SelectedConnectionTab() const
  {
    if (!mConnectionNotebook)
    {
      return nullptr;
    }

    const auto *const page = mConnectionNotebook->GetCurrentPage();

    const auto found = std::ranges::find_if(
        mConnectionTabs,
        [page](const auto &tab)
        { return tab->panel == page; });

    return found == mConnectionTabs.end() ? nullptr : found->get();
  }

  MainFrame::ConnectionTab *MainFrame::FindConnectionTab(
      const std::string_view connectionId) const
  {
    const auto found = std::ranges::find_if(
        mConnectionTabs,
        [connectionId](const auto &tab)
        {
          return tab->connectionId == connectionId;
        });

    return found == mConnectionTabs.end() ? nullptr : found->get();
  }

  MainFrame::ConnectionTab *MainFrame::ConnectionTabForWindow(
      const wxWindow *window) const
  {
    for (auto *current = window; current != nullptr;
         current = current->GetParent())
    {
      const auto found = std::ranges::find_if(
          mConnectionTabs,
          [current](const auto &tab)
          { return tab->panel == current; });

      if (found != mConnectionTabs.end())
      {
        return found->get();
      }
    }

    return nullptr;
  }

  MainFrame::ConnectionTab &MainFrame::CreateConnectionTab(
      const bool select,
      std::optional<std::string> restoredConnectionId,
      std::vector<config::PersistentQueueItem> restoredItems,
      std::optional<config::OpenConnectionTabState> restoredTab)
  {
    std::optional<std::pair<std::string, SiteEndpointIdentity>> restoredSite;

    if (!restoredItems.empty() && restoredItems.front().transfer.job.siteEndpoint)
    {
      restoredSite = std::pair{
          restoredItems.front().transfer.job.siteId,
          *restoredItems.front().transfer.job.siteEndpoint};
    }

    auto tab = std::make_unique<ConnectionTab>();

    if ((!restoredConnectionId || restoredConnectionId->empty()) &&
        restoredTab && !restoredTab->connectionId.empty())
    {
      restoredConnectionId = restoredTab->connectionId;
    }

    tab->connectionId = restoredConnectionId && !restoredConnectionId->empty()
                            ? std::move(*restoredConnectionId)
                            : GenerateId();
    tab->quickDraft.protocol = ProtocolKind::Sftp;
    tab->quickDraft.port = DefaultPort(ProtocolKind::Sftp);
    tab->panel = new wxPanel(mConnectionNotebook);

    auto *panes = new wxBoxSizer(wxHORIZONTAL);

    auto *localPanel = new wxPanel(tab->panel);
    auto *localSizer = new wxBoxSizer(wxVERTICAL);

    tab->localHeading = new wxStaticText(
        localPanel, wxID_ANY, Translated(*mTranslations, "local.heading"));

    localSizer->Add(tab->localHeading, 0, wxBOTTOM, 4);

    auto *localPathRow = new wxBoxSizer(wxHORIZONTAL);
    tab->localUpButton = MakeIconButton(
        localPanel,
        IdLocalUp,
        "HAVREMOTE_UP_ICON",
        Translated(*mTranslations, "local.up"));
    tab->localUpButton->SetToolTip(
        Translated(*mTranslations, "local.upTooltip"));
    tab->localRefreshButton = MakeIconButton(
        localPanel,
        IdLocalRefresh,
        "HAVREMOTE_REFRESH_ICON",
        Translated(*mTranslations, "local.refresh"));
    tab->localRefreshButton->SetToolTip(
        Translated(*mTranslations, "local.refreshTooltip"));
    tab->localPath = new EnterDirPickerCtrl(
        localPanel,
        IdLocalPath,
        {},
        Translated(*mTranslations, "local.chooseDirectory"),
        wxDefaultPosition,
        wxDefaultSize,
        wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST);

    SetPickerButtonLabel(*tab->localPath,
                         Translated(*mTranslations, "common.browse"));

    tab->localPath->SetToolTip(
        Translated(*mTranslations, "local.pathTooltip"));

    localPathRow->Add(tab->localUpButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    localPathRow->Add(tab->localRefreshButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    localPathRow->Add(tab->localPath, 1, wxEXPAND);
    localSizer->Add(localPathRow, 0, wxEXPAND | wxBOTTOM, 4);

    tab->localList = new FileListCtrl(localPanel, IdLocalList);
    tab->localList->SetColumnLabels({
        .name = Translated(*mTranslations, "column.name"),
        .size = Translated(*mTranslations, "column.size"),
        .type = Translated(*mTranslations, "column.type"),
        .modified = Translated(*mTranslations, "column.modified"),
        .permissions = {},
        .owner = {},
    });
    tab->localList->SetIcons({
        .folder = EmbeddedButtonIcon("HAVREMOTE_FILE_LIST_FOLDER_ICON"),
        .file = EmbeddedButtonIcon("HAVREMOTE_FILE_LIST_FILE_ICON"),
        .symlink = EmbeddedButtonIcon("HAVREMOTE_FILE_LIST_SYMLINK_ICON"),
        .other = EmbeddedButtonIcon("HAVREMOTE_FILE_LIST_UNKNOWN_ICON"),
    });
    tab->localList->SortBy(
        ToFileListColumn(mConfiguration.settings.fileLists.local.sortColumn),
        mConfiguration.settings.fileLists.local.sortAscending);
    tab->localList->ApplyColumnWidthsDips(DisplayedColumnWidths(
        mConfiguration.settings.fileLists.local.columnWidths));
    localSizer->Add(tab->localList, 1, wxEXPAND);

    auto *localButtons = new wxBoxSizer(wxHORIZONTAL);

    tab->localCreateButton = new wxButton(
        localPanel,
        IdLocalCreate,
        Translated(*mTranslations, "local.newFolder"));
    tab->localEditButton = new wxButton(
        localPanel,
        IdLocalEdit,
        Translated(*mTranslations, "local.edit"));
    tab->localRenameButton = new wxButton(
        localPanel,
        IdLocalRename,
        Translated(*mTranslations, "local.rename"));
    tab->localDeleteButton = new wxButton(
        localPanel,
        IdLocalDelete,
        Translated(*mTranslations, "local.delete"));

    localButtons->Add(tab->localCreateButton, 0, wxRIGHT, 4);
    localButtons->Add(tab->localEditButton, 0, wxRIGHT, 4);
    localButtons->Add(tab->localRenameButton, 0, wxRIGHT, 4);
    localButtons->Add(tab->localDeleteButton, 0);
    localSizer->Add(localButtons, 0, wxTOP, 5);
    localPanel->SetSizer(localSizer);

    auto *transferButtons = new wxBoxSizer(wxVERTICAL);

    transferButtons->AddStretchSpacer();

    tab->uploadButton = MakeIconButton(
        tab->panel,
        IdUpload,
        "HAVREMOTE_UPLOAD_ICON",
        Translated(*mTranslations, "transfer.uploadButton"));
    tab->downloadButton = MakeIconButton(
        tab->panel,
        IdDownload,
        "HAVREMOTE_DOWNLOAD_ICON",
        Translated(*mTranslations, "transfer.downloadButton"));

    transferButtons->Add(tab->uploadButton, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, 8);
    transferButtons->Add(tab->downloadButton, 0, wxALIGN_CENTER_HORIZONTAL);
    transferButtons->AddStretchSpacer();

    auto *remotePanel = new wxPanel(tab->panel);
    auto *remoteSizer = new wxBoxSizer(wxVERTICAL);

    tab->remoteHeading = new wxStaticText(
        remotePanel, wxID_ANY, Translated(*mTranslations, "remote.heading"));
    remoteSizer->Add(tab->remoteHeading, 0, wxBOTTOM, 4);

    auto *remotePathRow = new wxBoxSizer(wxHORIZONTAL);

    tab->remoteUpButton = MakeIconButton(
        remotePanel,
        IdRemoteUp,
        "HAVREMOTE_UP_ICON",
        Translated(*mTranslations, "remote.up"));
    tab->remoteUpButton->SetToolTip(
        Translated(*mTranslations, "remote.upTooltip"));
    tab->remoteRefreshButton = MakeIconButton(
        remotePanel,
        IdRemoteRefresh,
        "HAVREMOTE_REFRESH_ICON",
        Translated(*mTranslations, "remote.refresh"));
    tab->remoteRefreshButton->SetToolTip(
        Translated(*mTranslations, "remote.refreshTooltip"));
    tab->remotePath = new wxTextCtrl(remotePanel,
                                     IdRemotePath,
                                     "/",
                                     wxDefaultPosition,
                                     wxDefaultSize,
                                     wxTE_PROCESS_ENTER);
    tab->remotePath->SetToolTip(
        Translated(*mTranslations, "remote.pathTooltip"));

    remotePathRow->Add(tab->remoteUpButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    remotePathRow->Add(tab->remoteRefreshButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    remotePathRow->Add(tab->remotePath, 1, wxEXPAND);
    remoteSizer->Add(remotePathRow, 0, wxEXPAND | wxBOTTOM, 4);

    tab->remoteList = new FileListCtrl(
        remotePanel,
        IdRemoteList,
        FileListColumnProfile::Remote);
    tab->remoteList->SetColumnLabels({
        .name = Translated(*mTranslations, "column.name"),
        .size = Translated(*mTranslations, "column.size"),
        .type = Translated(*mTranslations, "column.type"),
        .modified = Translated(*mTranslations, "column.modified"),
        .permissions = Translated(*mTranslations, "column.permissions"),
        .owner = Translated(*mTranslations, "column.owner"),
    });
    tab->remoteList->SetIcons({
        .folder = EmbeddedButtonIcon("HAVREMOTE_FILE_LIST_FOLDER_ICON"),
        .file = EmbeddedButtonIcon("HAVREMOTE_FILE_LIST_FILE_ICON"),
        .symlink = EmbeddedButtonIcon("HAVREMOTE_FILE_LIST_SYMLINK_ICON"),
        .other = EmbeddedButtonIcon("HAVREMOTE_FILE_LIST_UNKNOWN_ICON"),
    });
    tab->remoteList->SortBy(
        ToFileListColumn(mConfiguration.settings.fileLists.remote.sortColumn),
        mConfiguration.settings.fileLists.remote.sortAscending);
    tab->remoteList->ApplyColumnWidthsDips(DisplayedColumnWidths(
        mConfiguration.settings.fileLists.remote.columnWidths));
    remoteSizer->Add(tab->remoteList, 1, wxEXPAND);

    auto *remoteButtons = new wxWrapSizer(wxHORIZONTAL);

    tab->remoteCreateDirectoryButton = new wxButton(
        remotePanel,
        IdRemoteCreateDirectory,
        Translated(*mTranslations, "remote.newFolder"));
    tab->remoteCreateFileButton = new wxButton(
        remotePanel,
        IdRemoteCreateFile,
        Translated(*mTranslations, "remote.newFile"));
    tab->remoteEditButton = new wxButton(
        remotePanel,
        IdRemoteEdit,
        Translated(*mTranslations, "remote.edit"));
    tab->remoteRenameButton = new wxButton(
        remotePanel,
        IdRemoteRename,
        Translated(*mTranslations, "remote.rename"));
    tab->remotePermissionsButton = new wxButton(
        remotePanel,
        IdRemotePermissions,
        Translated(*mTranslations, "remote.permissions"));
    tab->remoteDeleteButton = new wxButton(
        remotePanel,
        IdRemoteDelete,
        Translated(*mTranslations, "remote.deletePermanently"));

    remoteButtons->Add(tab->remoteCreateDirectoryButton, 0, wxRIGHT, 4);
    remoteButtons->Add(tab->remoteCreateFileButton, 0, wxRIGHT, 4);
    remoteButtons->Add(tab->remoteEditButton, 0, wxRIGHT, 4);
    remoteButtons->Add(tab->remoteRenameButton, 0, wxRIGHT, 4);
    remoteButtons->Add(tab->remotePermissionsButton, 0, wxRIGHT, 4);
    remoteButtons->Add(tab->remoteDeleteButton, 0);
    remoteSizer->Add(remoteButtons, 0, wxTOP, 5);
    remotePanel->SetSizer(remoteSizer);

    // Bind to the browsers themselves, not a frame accelerator: Delete must
    // keep its normal text-editing meaning in paths and inline rename fields.
    for (auto *list : {tab->localList, tab->remoteList})
    {
      list->Bind(wxEVT_KEY_DOWN, &MainFrame::OnFileListKeyDown, this);
    }

    panes->Add(localPanel, 1, wxEXPAND | wxALL, 8);
    panes->Add(transferButtons, 0, wxEXPAND | wxTOP | wxBOTTOM, 8);
    panes->Add(remotePanel, 1, wxEXPAND | wxALL, 8);

    tab->panel->SetSizer(panes);

    const auto connectionId = tab->connectionId;

    tab->localDirectoryLoader =
        std::make_unique<LocalDirectoryLoader>(*this, connectionId);

    if (auto *const editor = tab->localPath->GetTextCtrl())
    {
      editor->Bind(wxEVT_TEXT_ENTER,
                   &MainFrame::OnLocalPathEnter,
                   this);
    }

    tab->localPath->GetPickerCtrl()->Bind(
        wxEVT_DIRPICKER_CHANGED,
        &MainFrame::OnLocalDirectoryPicked,
        this);

    UiInteractions interactions;

    interactions.requestCredential =
        [this, connectionId](const CredentialRequest &request)
    {
      auto *const current = FindConnectionTab(connectionId);

      return current
                 ? RequestCredential(*current, request)
                 : Result<std::string>{std::unexpected(UiError(
                       RemoteErrorCode::Cancelled,
                       std::string{mTranslations->Text(
                           "authentication.inactiveRequest")}))};
    };

    interactions.verifyTrust =
        [this, connectionId](const TrustChallenge &challenge)
    {
      auto *const current = FindConnectionTab(connectionId);

      return current ? VerifyTrust(*current, challenge)
                     : Result<TrustDecision>{TrustDecision::Reject};
    };

    interactions.resolveConflict =
        [this, connectionId](const ConflictChallenge &challenge)
    {
      auto *const current = FindConnectionTab(connectionId);

      return current
                 ? ResolveConflict(*current, challenge)
                 : Result<ConflictResolution>{std::unexpected(UiError(
                       RemoteErrorCode::Cancelled,
                       std::string{mTranslations->Text(
                           "conflict.canceled")}))};
    };

    interactions.tlsPinnedPublicKey =
        [this](const std::string_view host,
               const std::uint16_t port) -> std::optional<std::string>
    {
      const auto pin = std::ranges::find_if(
          mConfiguration.tlsTrust,
          [&](const auto &record)
          {
            return record.port == port &&
                   EndpointHostEqual(record.host, host);
          });

      return pin == mConfiguration.tlsTrust.end()
                 ? std::nullopt
                 : std::optional<std::string>{CurlTlsPin(pin->publicKeyPin)};
    };

    interactions.persistQueue =
        [this](const std::string &) -> Result<void>
    {
      // Leave an invalid queue file untouched. Transfers continue without persistence
      if (!mQueuePersistenceWritable)
      {
        return {};
      }

      if (PersistTransferQueue())
      {
        return {};
      }

      return std::unexpected(UiError(
          RemoteErrorCode::LocalIo,
          "The transfer queue could not be saved"));
    };

    tab->controller = std::make_unique<RemoteController>(
        *this,
        connectionId,
        mTransferRuntime,
        mKnownHostsFile,
        std::move(interactions),
        std::chrono::seconds{mConfiguration.settings.connectionTimeoutSeconds},
        std::chrono::seconds{mConfiguration.settings.commandIdleTimeoutSeconds},
        std::move(restoredItems));

    auto *const result = tab.get();

    mConnectionTabs.push_back(std::move(tab));
    mConnectionNotebook->AddPage(
        result->panel,
        Translated(*mTranslations, "connection.tab.new"),
        select);

    result->localList->Bind(wxEVT_CONTEXT_MENU,
                            &MainFrame::OnLocalContextMenu,
                            this);
    result->remoteList->Bind(wxEVT_CONTEXT_MENU,
                             &MainFrame::OnRemoteContextMenu,
                             this);

    result->localList->SetDropTarget(new FileListDropTarget(
        [this, connectionId](const std::string_view token)
        {
          auto *const current = FindConnectionTab(connectionId);
          return current != nullptr &&
                 HandleInternalFileDrop(*current, false, token);
        },
        [this, connectionId](const std::vector<std::filesystem::path> &paths)
        {
          auto *const current = FindConnectionTab(connectionId);
          return current != nullptr &&
                 HandleExternalFileDrop(*current, false, paths);
        }));
    result->remoteList->SetDropTarget(new FileListDropTarget(
        [this, connectionId](const std::string_view token)
        {
          auto *const current = FindConnectionTab(connectionId);
          return current != nullptr &&
                 HandleInternalFileDrop(*current, true, token);
        },
        [this, connectionId](const std::vector<std::filesystem::path> &paths)
        {
          auto *const current = FindConnectionTab(connectionId);
          return current != nullptr &&
                 HandleExternalFileDrop(*current, true, paths);
        }));
    result->transfers = result->controller->Transfers();

    bool restoredWorkspaceIdentity = false;

    if (restoredTab && restoredTab->connection)
    {
      if (const auto *saved = std::get_if<config::SavedSiteWorkspaceIdentity>(
              &*restoredTab->connection))
      {
        const auto site = std::ranges::find(
            mConfiguration.sites, saved->siteId, &SiteProfile::id);

        if (site != mConfiguration.sites.end())
        {
          SetQuickSite(*result, *site, false);
          restoredWorkspaceIdentity = true;
        }
      }
      else if (const auto *quick =
                   std::get_if<config::QuickConnectWorkspaceIdentity>(
                       &*restoredTab->connection);
               quick != nullptr &&
               QuickHistoryContains(mConfiguration, quick->endpoint))
      {
        SiteProfile recovered;
        recovered.name = quick->endpoint.host;
        recovered.protocol = quick->endpoint.protocol;
        recovered.host = quick->endpoint.host;
        recovered.port = quick->endpoint.port;
        recovered.username = quick->endpoint.username;
        recovered.authentication.kind = AuthenticationKind::Password;

        SetQuickSite(*result, recovered, false);

        restoredWorkspaceIdentity = true;

        if (restoredSite &&
            EndpointIdentitiesEqual(restoredSite->second,
                                    quick->endpoint))
        {
          result->quickRuntimeSiteId = restoredSite->first;
          result->quickRuntimeEndpoint = restoredSite->second;
        }
      }
    }

    if (!restoredWorkspaceIdentity && restoredSite)
    {
      const auto &[siteId, endpoint] = *restoredSite;
      const auto saved = std::ranges::find(
          mConfiguration.sites, siteId, &SiteProfile::id);

      if (saved != mConfiguration.sites.end() &&
          EndpointMatchesSite(endpoint, *saved))
      {
        SetQuickSite(*result, *saved, false);
      }
      else
      {
        SiteProfile recovered;
        recovered.id = siteId;
        recovered.name = endpoint.host;
        recovered.protocol = endpoint.protocol;
        recovered.host = endpoint.host;
        recovered.port = endpoint.port;
        recovered.username = endpoint.username;
        recovered.authentication.kind = AuthenticationKind::Password;

        SetQuickSite(*result, recovered, false);

        // SetQuickSite() clears runtime identities for saved-site IDs. Restore
        // the captured endpoint when the saved site now points elsewhere.
        result->quickRuntimeSiteId = siteId;
        result->quickRuntimeEndpoint = endpoint;
      }
    }

    SetConnectionState(*result, ConnectionState::Disconnected);

    if (restoredTab && !restoredTab->remoteDirectory.DisplayUtf8().empty())
    {
      result->remoteDirectory = restoredTab->remoteDirectory;
      result->quickDraft.initialRemoteDirectory = restoredTab->remoteDirectory;
      result->remotePath->ChangeValue(
          FromUtf8(restoredTab->remoteDirectory.DisplayUtf8()));
    }

    std::error_code currentPathError;

    auto currentPath = std::filesystem::current_path(currentPathError);
    auto initialLocal = restoredTab
                            ? ConfiguredLocalDirectory(
                                  restoredTab->localDirectory)
                            : std::nullopt;

    if (!initialLocal)
    {
      initialLocal = ConfiguredLocalDirectory(
          mConfiguration.workspace.lastLocalDirectory);
    }

    if (initialLocal)
    {
      PopulateLocalDirectory(
          *result,
          *initialLocal,
          currentPathError
              ? std::nullopt
              : std::optional<std::filesystem::path>{
                    std::move(currentPath)});
    }
    else if (!currentPathError)
    {
      PopulateLocalDirectory(*result, currentPath);
    }

    if (select)
    {
      ShowQuickConnectDraft(*result);
      UpdateSelectedConnectionUi();
    }

    return *result;
  }

  void MainFrame::DestroyConnectionTab(ConnectionTab &tab)
  {
    const auto found = std::ranges::find_if(
        mConnectionTabs,
        [&tab](const auto &candidate)
        { return candidate.get() == &tab; });

    if (found == mConnectionTabs.end())
    {
      return;
    }

    const auto disconnectLog = IntentionalDisconnectLog(tab);

    MarkExternalEditDetached(tab.connectionId);

    mConnectionTabs.erase(found);

    if (disconnectLog)
    {
      AppendLog(DiagnosticLevel::Information, *disconnectLog);
    }

    (void)PersistTransferQueue();

    EraseReleasedCredentials();

    PopulateTransferLists();

    PersistOpenConnectionTabs();
  }

  bool MainFrame::ConfirmCloseConnectionTab(const ConnectionTab &tab)
  {
    const bool unfinished = tab.controller &&
                            std::ranges::any_of(
                                tab.controller->Transfers(),
                                PersistentTransfer);

    if (unfinished &&
        LocalizedMessageBox(
            *mTranslations,
            Translated(*mTranslations, "connection.closeActiveMessage"),
            Translated(*mTranslations, "connection.closeActiveTitle"),
            wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
            this) != wxID_YES)
    {
      return false;
    }

    return !HasExternalEditSessions(std::string_view{tab.connectionId}) ||
           LocalizedMessageBox(
               *mTranslations,
               Translated(*mTranslations,
                          "externalEditor.closeActiveMessage"),
               Translated(*mTranslations,
                          "externalEditor.closeActiveTitle"),
               wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
               this) == wxID_YES;
  }

  void MainFrame::EnsureConnectionTab()
  {
    if (mConnectionTabs.empty())
    {
      (void)CreateConnectionTab();
    }
    else
    {
      UpdateSelectedConnectionUi();
    }
  }

  void MainFrame::CaptureQuickConnectDraft(ConnectionTab &tab)
  {
    if (tab.connectionState != ConnectionState::Disconnected)
    {
      return;
    }

    tab.quickDraft.protocol = ProtocolAt(mQuickProtocol->GetSelection());
    tab.quickDraft.host = ToUtf8(mQuickHost->GetValue().Trim(true).Trim(false));
    tab.quickDraft.port = static_cast<std::uint16_t>(mQuickPort->GetValue());
    tab.quickDraft.username = ToUtf8(mQuickUsername->GetValue());

    if (tab.quickRuntimeEndpoint &&
        !EndpointIdentitiesEqual(*tab.quickRuntimeEndpoint,
                                 EndpointIdentity(tab.quickDraft)))
    {
      tab.quickRuntimeSiteId.clear();
      tab.quickRuntimeEndpoint.reset();
    }

    ClearSensitiveString(tab.ephemeralPassword);

    tab.ephemeralPassword = ToUtf8(mQuickPassword->GetValue());
    tab.workspaceIdentity = WorkspaceIdentityForSite(mConfiguration, tab.quickDraft, false);

    UpdateConnectionTabTitle(tab);
  }

  void MainFrame::ShowQuickConnectDraft(const ConnectionTab &tab)
  {
    const auto &site = tab.activeConnectionSite
                           ? *tab.activeConnectionSite
                           : tab.quickDraft;

    mQuickProtocol->SetSelection(ProtocolIndex(site.protocol));
    mQuickHost->ChangeValue(FromUtf8(site.host));
    mQuickPort->SetValue(site.port);
    mQuickUsername->ChangeValue(FromUtf8(site.username));
    mQuickPassword->ChangeValue(
        tab.connectionState == ConnectionState::Disconnected
            ? FromUtf8(tab.ephemeralPassword)
            : wxString{});
  }

  wxString MainFrame::ConnectionTabTitle(const ConnectionTab &tab) const
  {
    const auto &site = tab.activeConnectionSite
                           ? *tab.activeConnectionSite
                           : tab.quickDraft;

    wxString title;
    if (!site.name.empty())
    {
      title = FromUtf8(site.name);
    }
    else if (!site.host.empty())
    {
      title = site.username.empty()
                  ? FromUtf8(site.host)
                  : FromUtf8(site.username) + "@" + FromUtf8(site.host);
    }
    else
    {
      title = Translated(*mTranslations, "connection.tab.new");
    }

    if (tab.connectionState == ConnectionState::Connecting)
    {
      title += "...";
    }

    return title;
  }

  std::optional<std::string> MainFrame::IntentionalDisconnectLog(
      const ConnectionTab &tab) const
  {
    if (tab.connectionState == ConnectionState::Disconnected)
    {
      return std::nullopt;
    }

    // Capture the label from the profile: a closed notebook page may already
    // be gone. Worker shutdown invalidates its events, so the UI owns this log.
    return "[" + ToUtf8(ConnectionTabTitle(tab)) + "] " +
           std::string{mTranslations->Text(
               tab.connectionState == ConnectionState::Connected
                   ? "status.disconnected"
                   : "connection.canceledLog")};
  }

  void MainFrame::UpdateConnectionTabTitle(ConnectionTab &tab)
  {
    const auto &site = tab.activeConnectionSite
                           ? *tab.activeConnectionSite
                           : tab.quickDraft;

    const int index = mConnectionNotebook->GetPageIndex(tab.panel);

    if (index != wxNOT_FOUND)
    {
      mConnectionNotebook->SetPageText(static_cast<std::size_t>(index),
                                       ConnectionTabTitle(tab));

      if (!site.host.empty())
      {
        mConnectionNotebook->SetPageToolTip(
            static_cast<std::size_t>(index),
            Translated(*mTranslations,
                       ProtocolTranslationKey(site.protocol)) +
                " - " + FromUtf8(site.host) + ":" +
                wxString::Format("%u", static_cast<unsigned>(site.port)));
      }
      else
      {
        mConnectionNotebook->SetPageToolTip(
            static_cast<std::size_t>(index), {});
      }
    }
  }

  void MainFrame::SizeCompactIconButtons()
  {
#ifdef __WXMSW__
    // Plain single-line MSW edits cannot vertically center stretched text.
    // Keep their native height and match the compact action buttons to it.
    int height = 0;

    for (auto *const input : std::array<wxWindow *, 5>{
             mQuickProtocol, mQuickHost, mQuickPort,
             mQuickUsername, mQuickPassword})
    {
      height = std::max(height, input->GetBestSize().y);
    }

    for (auto *const button : {mConnectButton, mQuickClearButton,
                               mNewConnectionButton, mQuickHistoryButton,
                               mMessageLogClearButton})
    {
      SetSquareIconButtonSize(*button, height);
    }

    mSiteManagerButton->SetButtonHeight(height);
#endif
  }

  void MainFrame::UpdateSelectedConnectionUi()
  {
    auto *const tab = SelectedConnectionTab();

    if (!tab)
    {
      return;
    }

    ShowQuickConnectDraft(*tab);

    const bool disconnected = tab->connectionState == ConnectionState::Disconnected;

    mQuickClearButton->Enable(disconnected);

    for (auto *control : std::array<wxWindow *, 5>{
             mQuickProtocol, mQuickHost, mQuickPort, mQuickUsername, mQuickPassword})
    {
      control->Enable(disconnected);
    }

    SetIconButtonBitmap(*mConnectButton, EmbeddedButtonIcon(
                                             tab->connectionState == ConnectionState::Connected
                                                 ? "HAVREMOTE_DISCONNECT_ICON"
                                                 : "HAVREMOTE_CONNECT_ICON"));

    SetIconButtonText(
        *mConnectButton,
        Translated(*mTranslations,
                   tab->connectionState == ConnectionState::Connected
                       ? "quick.disconnect"
                       : "quick.connect"));

    mConnectButton->Enable(tab->connectionState != ConnectionState::Connecting);

    if (auto *menu = GetMenuBar())
    {
      menu->Enable(IdDisconnect,
                   tab->connectionState != ConnectionState::Disconnected);
      menu->Enable(IdCloseConnectionTab, true);
    }

    if (tab->connectionState == ConnectionState::Disconnected)
    {
      SetStatusText(Translated(*mTranslations, "status.notConnected"), 0);
    }
    else if (tab->connectionState == ConnectionState::Connecting)
    {
      SetStatusText(Translated(*mTranslations, "status.connecting"), 0);
    }
    else
    {
      SetStatusText(Translated(*mTranslations, "status.connected"), 0);
    }

    UpdateFileActionState();

    mConnectButton->InvalidateBestSize();

    SizeCompactIconButtons();

    Layout();
  }

  void MainFrame::ApplyTranslations()
  {
    auto *menuBar = GetMenuBar();

    if (menuBar && menuBar->GetMenuCount() >= 3)
    {
      auto *file = menuBar->GetMenu(0);

      file->SetLabel(
          IdNewConnectionTab,
          Translated(*mTranslations, "menu.file.newConnectionTab"));
      file->SetHelpString(
          IdNewConnectionTab,
          Translated(*mTranslations, "menu.file.newConnectionTabHelp"));
      file->SetLabel(
          IdCloseConnectionTab,
          Translated(*mTranslations, "menu.file.closeConnectionTab"));
      file->SetHelpString(
          IdCloseConnectionTab,
          Translated(*mTranslations, "menu.file.closeConnectionTabHelp"));
      file->SetLabel(IdSiteManager,
                     Translated(*mTranslations, "menu.file.siteManager"));
      file->SetHelpString(IdSiteManager,
                          Translated(*mTranslations, "menu.file.siteManagerHelp"));
      file->SetLabel(IdDisconnect,
                     Translated(*mTranslations, "menu.file.disconnect"));
      file->SetHelpString(IdDisconnect,
                          Translated(*mTranslations, "menu.file.disconnectHelp"));
      file->SetLabel(wxID_EXIT, Translated(*mTranslations, "menu.file.exit"));
      file->SetHelpString(wxID_EXIT,
                          Translated(*mTranslations, "menu.file.exitHelp"));
      menuBar->SetMenuLabel(0, Translated(*mTranslations, "menu.file.title"));

      auto *preferences = menuBar->GetMenu(1);

      preferences->SetLabel(
          wxID_PREFERENCES,
          Translated(*mTranslations, "menu.preferences.settings"));
      preferences->SetHelpString(
          wxID_PREFERENCES,
          Translated(*mTranslations, "menu.preferences.settingsHelp"));
      menuBar->SetMenuLabel(
          1, Translated(*mTranslations, "menu.preferences.title"));

      auto *help = menuBar->GetMenu(2);

      help->SetLabel(wxID_HELP,
                     Translated(*mTranslations, "menu.help.contents"));
      help->SetHelpString(wxID_HELP,
                          Translated(*mTranslations, "menu.help.contentsHelp"));
      help->SetLabel(
          IdCheckForUpdates,
          Translated(*mTranslations, "menu.help.checkForUpdates"));
      help->SetHelpString(
          IdCheckForUpdates,
          Translated(*mTranslations, "menu.help.checkForUpdatesHelp"));
      help->SetLabel(wxID_ABOUT,
                     Translated(*mTranslations, "menu.help.about"));
      help->SetHelpString(wxID_ABOUT,
                          Translated(*mTranslations, "menu.help.aboutHelp"));
      menuBar->SetMenuLabel(2, Translated(*mTranslations, "menu.help.title"));
    }

    const auto setLabel = [this](const int id, const std::string_view key)
    {
      if (auto *window = wxWindow::FindWindowById(id, this))
      {
        window->SetLabel(Translated(*mTranslations, key));
      }
    };

    const auto setIconText = [this](const int id, const std::string_view key)
    {
      if (auto *button = dynamic_cast<wxBitmapButton *>(
              wxWindow::FindWindowById(id, this)))
      {
        SetIconButtonText(*button, Translated(*mTranslations, key));
      }
    };

    mSiteManagerButton->SetButtonText(
        Translated(*mTranslations, "quick.siteManager"),
        Translated(*mTranslations, "quick.savedSites"));

    const auto *const selectedTab = SelectedConnectionTab();

    setIconText(IdQuickConnect,
                selectedTab &&
                        selectedTab->connectionState == ConnectionState::Connected
                    ? "quick.disconnect"
                    : "quick.connect");
    setIconText(IdQuickClear, "quick.clear");
    setIconText(IdNewConnectionTab, "connection.tab.new");
    setIconText(IdQuickHistory, "quick.history");
    setIconText(IdMessageLogClear, "log.clear");
    setLabel(IdQueuePause, "queue.pause");
    setLabel(IdQueueCancel, "queue.cancel");
    setLabel(IdQueueRetry, "queue.retry");
    setLabel(IdQueueRemove, "queue.remove");
    setLabel(IdQueueClear, "queue.clearView");
    setLabel(IdQueueCopy, "queue.copy");
    setLabel(IdQueueExport, "queue.export");

    mQuickHeading->SetLabel(Translated(*mTranslations, "quick.heading"));
    mLogHeading->SetLabel(Translated(*mTranslations, "log.heading"));

    const int protocolSelection = mQuickProtocol->GetSelection();

    mQuickProtocol->Clear();
    mQuickProtocol->Append(Translated(*mTranslations, "quick.protocol.ftp"));
    mQuickProtocol->Append(Translated(*mTranslations, "quick.protocol.ftpsExplicit"));
    mQuickProtocol->Append(Translated(*mTranslations, "quick.protocol.ftpsImplicit"));
    mQuickProtocol->Append(Translated(*mTranslations, "quick.protocol.sftp"));
    mQuickProtocol->SetSelection(protocolSelection);
    mQuickHost->SetHint(Translated(*mTranslations, "quick.hostHint"));
    mQuickUsername->SetHint(Translated(*mTranslations, "quick.usernameHint"));
    mQuickPassword->SetHint(Translated(*mTranslations, "quick.passwordHint"));

    for (auto &tab : mConnectionTabs)
    {
      tab->localHeading->SetLabel(
          Translated(*mTranslations, "local.heading"));

      SetIconButtonText(*tab->localUpButton,
                        Translated(*mTranslations, "local.up"));
      SetIconButtonText(*tab->localRefreshButton,
                        Translated(*mTranslations, "local.refresh"));

      tab->localCreateButton->SetLabel(
          Translated(*mTranslations, "local.newFolder"));
      tab->localEditButton->SetLabel(
          Translated(*mTranslations, "local.edit"));
      tab->localRenameButton->SetLabel(
          Translated(*mTranslations, "local.rename"));
      tab->localDeleteButton->SetLabel(
          Translated(*mTranslations, "local.delete"));

      SetIconButtonText(*tab->uploadButton,
                        Translated(*mTranslations, "transfer.uploadButton"));
      SetIconButtonText(*tab->downloadButton,
                        Translated(*mTranslations, "transfer.downloadButton"));

      tab->localUpButton->SetToolTip(
          Translated(*mTranslations, "local.upTooltip"));
      tab->localRefreshButton->SetToolTip(
          Translated(*mTranslations, "local.refreshTooltip"));
      tab->localPath->SetToolTip(
          Translated(*mTranslations, "local.pathTooltip"));

      SetPickerButtonLabel(*tab->localPath,
                           Translated(*mTranslations, "common.browse"));

      tab->localList->SetColumnLabels({
          .name = Translated(*mTranslations, "column.name"),
          .size = Translated(*mTranslations, "column.size"),
          .type = Translated(*mTranslations, "column.type"),
          .modified = Translated(*mTranslations, "column.modified"),
          .permissions = {},
          .owner = {},
      });
      tab->remoteHeading->SetLabel(
          Translated(*mTranslations, "remote.heading"));

      SetIconButtonText(*tab->remoteUpButton,
                        Translated(*mTranslations, "remote.up"));
      SetIconButtonText(*tab->remoteRefreshButton,
                        Translated(*mTranslations, "remote.refresh"));

      tab->remoteCreateDirectoryButton->SetLabel(
          Translated(*mTranslations, "remote.newFolder"));
      tab->remoteCreateFileButton->SetLabel(
          Translated(*mTranslations, "remote.newFile"));
      tab->remoteEditButton->SetLabel(
          Translated(*mTranslations, "remote.edit"));
      tab->remoteRenameButton->SetLabel(
          Translated(*mTranslations, "remote.rename"));
      tab->remotePermissionsButton->SetLabel(
          Translated(*mTranslations, "remote.permissions"));
      tab->remoteDeleteButton->SetLabel(
          Translated(*mTranslations, "remote.deletePermanently"));
      tab->remoteUpButton->SetToolTip(
          Translated(*mTranslations, "remote.upTooltip"));
      tab->remoteRefreshButton->SetToolTip(
          Translated(*mTranslations, "remote.refreshTooltip"));
      tab->remotePath->SetToolTip(
          Translated(*mTranslations, "remote.pathTooltip"));
      tab->remoteList->SetColumnLabels({
          .name = Translated(*mTranslations, "column.name"),
          .size = Translated(*mTranslations, "column.size"),
          .type = Translated(*mTranslations, "column.type"),
          .modified = Translated(*mTranslations, "column.modified"),
          .permissions = Translated(*mTranslations, "column.permissions"),
          .owner = Translated(*mTranslations, "column.owner"),
      });

      RefreshLocalFileRows(*tab);

      RefreshRemoteFileRows(*tab);

      UpdateConnectionTabTitle(*tab);

      tab->panel->Layout();
    }

    const auto setColumn = [](wxListCtrl *list,
                              const int column,
                              const wxString &label)
    {
      wxListItem item;
      item.SetMask(wxLIST_MASK_TEXT);
      item.SetText(label);

      list->SetColumn(column, item);
    };

    for (auto *list : {mQueuedList, mFailedList, mCompletedList})
    {
      setColumn(list, 0, Translated(*mTranslations, "column.connection"));
      setColumn(list, 1, Translated(*mTranslations, "column.direction"));
      setColumn(list, 2, Translated(*mTranslations, "column.item"));
      setColumn(list, 3, Translated(*mTranslations, "column.progress"));
      setColumn(list, 4, Translated(*mTranslations, "column.speed"));
      setColumn(list, 5, Translated(*mTranslations, "column.eta"));
      setColumn(list, 6, Translated(*mTranslations, "column.state"));
      setColumn(list, 7, Translated(*mTranslations, "column.details"));
    }

    mTransferNotebook->SetPageText(
        0, Translated(*mTranslations, "queue.tab.queue"));
    mTransferNotebook->SetPageText(
        1, Translated(*mTranslations, "queue.tab.failed"));
    mTransferNotebook->SetPageText(
        2, Translated(*mTranslations, "queue.tab.completed"));

    PopulateTransferLists();

    SetStatusText(Translated(*mTranslations, "status.trustEnforced"), 2);

    UpdateSelectedConnectionUi();

    Layout();
  }

  void MainFrame::RestoreFileListSortSettings()
  {
    const auto &fileLists = mConfiguration.settings.fileLists;

    for (auto &tab : mConnectionTabs)
    {
      tab->localList->SortBy(ToFileListColumn(fileLists.local.sortColumn),
                             fileLists.local.sortAscending);
      tab->remoteList->SortBy(ToFileListColumn(fileLists.remote.sortColumn),
                              fileLists.remote.sortAscending);
    }
  }

  void MainFrame::RestoreFileListColumnWidths()
  {
    const auto local = DisplayedColumnWidths(
        mConfiguration.settings.fileLists.local.columnWidths);

    const auto remote = DisplayedColumnWidths(
        mConfiguration.settings.fileLists.remote.columnWidths);

    for (auto &tab : mConnectionTabs)
    {
      tab->localList->ApplyColumnWidthsDips(local);
      tab->remoteList->ApplyColumnWidthsDips(remote);
    }
  }

  void MainFrame::InitializeConfiguration(
      std::expected<config::ConfigLoadResult, config::ConfigError>
          initialConfiguration)
  {
    auto loaded = std::move(initialConfiguration);

    if (loaded)
    {
      mConfiguration = std::move(loaded->data);

      mActiveAppearanceTheme = mConfiguration.settings.theme;

      PruneSavedSitesFromQuickConnectHistory(mConfiguration);

      mConfigurationWritable = true;

      AppendLog(DiagnosticLevel::Information,
                mTranslations->Text(
                    loaded->createdDefaults
                        ? "config.createdDefaultLog"
                        : "config.loadedLog"));

      if (mTranslations->UsedFallback())
      {
        AppendLog(
            DiagnosticLevel::Warning,
            mTranslations->Format(
                "localization.fallbackLog",
                std::array<std::string_view, 2>{
                    mConfiguration.settings.language,
                    mTranslations->SelectedLanguageCode()}));
      }

      return;
    }

    const auto message = ConfigErrorMessage(loaded.error());

    AppendLog(DiagnosticLevel::Error, message);

    LocalizedMessageBox(
        *mTranslations,
        TranslatedFormat(*mTranslations, "config.loadFailedMessage", message),
        Translated(*mTranslations, "config.loadFailedTitle"),
        wxOK | wxICON_ERROR,
        this);

    mConfiguration = {};

    mConfigurationWritable = false;

    AppendLog(DiagnosticLevel::Warning,
              mTranslations->Text("config.writesDisabledLog"));
  }

  void MainFrame::ApplyWindowGeometry(const config::MainWindowState &state)
  {
#ifdef __WXMSW__
    if (state.position)
    {
      const int requestedWidth = static_cast<int>(state.width);
      const int requestedHeight = static_cast<int>(state.height);

      const RECT titleAnchor{
          .left = static_cast<LONG>(state.position->x),
          .top = static_cast<LONG>(state.position->y),
          .right = static_cast<LONG>(state.position->x +
                                     std::min(requestedWidth, 64)),
          .bottom = static_cast<LONG>(state.position->y +
                                      std::min(requestedHeight, 32)),
      };

      const HMONITOR monitor = ::MonitorFromRect(
          &titleAnchor, MONITOR_DEFAULTTONULL);
      MONITORINFO monitorInfo{};
      monitorInfo.cbSize = sizeof(MONITORINFO);

      const auto window = reinterpret_cast<HWND>(GetHandle());
      if (monitor != nullptr && window != nullptr &&
          ::GetMonitorInfoW(monitor, &monitorInfo) != FALSE)
      {
        if (IsIconized() || IsMaximized())
        {
          if (IsShown())
          {
            Restore();
          }
          else
          {
            // Restore() shows a hidden wxMSW top-level window. Clear the
            // deferred show state without making intermediate geometry
            // visible.
            Maximize(false);
          }
        }

        // wxDisplay reports logical coordinates relative to the window's
        // current monitor. During first show that can be a different-DPI
        // monitor from the saved one. Move once in native physical screen
        // coordinates so wx adopts the target monitor DPI, then establish the
        // target-DPI minimum and apply the authoritative native bounds.
        SetMinSize(wxDefaultSize);

        (void)::SetWindowPos(
            window,
            nullptr,
            static_cast<int>(state.position->x),
            static_cast<int>(state.position->y),
            requestedWidth,
            requestedHeight,
            SWP_NOACTIVATE | SWP_NOZORDER);

        const wxSize requestedMinimum = FromDIP(
            wxSize{MainWindowMinimumWidthDips, MainWindowMinimumHeightDips});

        const wxSize maximumSize{
            monitorInfo.rcWork.right - monitorInfo.rcWork.left,
            monitorInfo.rcWork.bottom - monitorInfo.rcWork.top,
        };

        const wxSize minimumSize{
            std::min(requestedMinimum.x, std::max(1, maximumSize.x)),
            std::min(requestedMinimum.y, std::max(1, maximumSize.y)),
        };

        const wxSize windowSize = LimitedWindowSize(
            wxSize{requestedWidth, requestedHeight}, minimumSize, maximumSize);

        SetMinSize(minimumSize);

        (void)::SetWindowPos(
            window,
            nullptr,
            static_cast<int>(state.position->x),
            static_cast<int>(state.position->y),
            windowSize.x,
            windowSize.y,
            SWP_NOACTIVATE | SWP_NOZORDER);

        RECT appliedRect{};

        const bool haveAppliedRect =
            ::GetWindowRect(window, &appliedRect) != FALSE;

        mNormalWindowState = config::MainWindowState{
            .position = config::WindowPosition{
                .x = haveAppliedRect
                         ? static_cast<std::int32_t>(appliedRect.left)
                         : state.position->x,
                .y = haveAppliedRect
                         ? static_cast<std::int32_t>(appliedRect.top)
                         : state.position->y,
            },
            .width = static_cast<std::uint32_t>(haveAppliedRect ? appliedRect.right - appliedRect.left : windowSize.x),
            .height = static_cast<std::uint32_t>(haveAppliedRect ? appliedRect.bottom - appliedRect.top : windowSize.y),
            .maximized = state.maximized,
        };

        if (state.maximized)
        {
          Maximize();
        }

        return;
      }
    }
#endif

    wxRect workArea = PrimaryDisplayClientArea();

    bool restorePosition = false;

    wxSize desiredSize;

    if (state.position)
    {
      desiredSize = {
          static_cast<int>(state.width),
          static_cast<int>(state.height),
      };

      const wxRect requestedRect{
          wxPoint{static_cast<int>(state.position->x),
                  static_cast<int>(state.position->y)},
          desiredSize,
      };

      const int displayIndex = wxDisplay::GetFromRect(requestedRect);

      if (displayIndex != wxNOT_FOUND)
      {
        wxDisplay display{static_cast<unsigned>(displayIndex)};

        if (display.IsOk())
        {
          workArea = display.GetClientArea();
          restorePosition = true;
        }
      }
    }
    else
    {
      desiredSize = FromDIP(wxSize{
          static_cast<int>(state.width),
          static_cast<int>(state.height),
      });
    }

    const auto requestedMinimum = FromDIP(
        wxSize{MainWindowMinimumWidthDips, MainWindowMinimumHeightDips});

    const wxSize maximumSize{
        restorePosition
            ? workArea.width
            : workArea.width * DefaultWindowWorkAreaPercent / 100,
        restorePosition
            ? workArea.height
            : workArea.height * DefaultWindowWorkAreaPercent / 100,
    };

    const wxSize minimumSize{
        std::min(requestedMinimum.x, std::max(1, maximumSize.x)),
        std::min(requestedMinimum.y, std::max(1, maximumSize.y)),
    };

    const wxSize windowSize =
        LimitedWindowSize(desiredSize, minimumSize, maximumSize);

    const wxPoint windowPosition = restorePosition
                                       ? VisibleWindowPosition(
                                             wxPoint{static_cast<int>(state.position->x),
                                                     static_cast<int>(state.position->y)},
                                             workArea,
                                             windowSize)
                                       : CenteredWindowPosition(workArea, windowSize);

    if (IsIconized() || IsMaximized())
    {
      if (IsShown())
      {
        Restore();
      }
      else
      {
        Maximize(false);
      }
    }

    SetMinSize(minimumSize);

    SetSize(windowPosition.x,
            windowPosition.y,
            windowSize.x,
            windowSize.y,
            wxSIZE_ALLOW_MINUS_ONE);

    mNormalWindowState = config::MainWindowState{
        .position = config::WindowPosition{
            .x = static_cast<std::int32_t>(windowPosition.x),
            .y = static_cast<std::int32_t>(windowPosition.y),
        },
        .width = static_cast<std::uint32_t>(windowSize.x),
        .height = static_cast<std::uint32_t>(windowSize.y),
        .maximized = state.maximized,
    };

    if (state.maximized)
    {
      Maximize();
    }
  }

  config::MainWindowState MainFrame::DefaultWindowStateForCurrentDisplay() const
  {
    wxRect workArea = PrimaryDisplayClientArea();

    int displayIndex = wxDisplay::GetFromWindow(this);

    if (displayIndex != wxNOT_FOUND)
    {
      wxDisplay display{static_cast<unsigned>(displayIndex)};

      if (display.IsOk())
      {
        workArea = display.GetClientArea();
      }
      else
      {
        displayIndex = wxNOT_FOUND;
      }
    }

    const auto requestedMinimum = FromDIP(
        wxSize{MainWindowMinimumWidthDips, MainWindowMinimumHeightDips});

    const wxSize maximumSize{
        workArea.width * DefaultWindowWorkAreaPercent / 100,
        workArea.height * DefaultWindowWorkAreaPercent / 100,
    };

    const wxSize minimumSize{
        std::min(requestedMinimum.x, std::max(1, maximumSize.x)),
        std::min(requestedMinimum.y, std::max(1, maximumSize.y)),
    };

    const wxSize windowSize = LimitedWindowSize(
        FromDIP(wxSize{
            static_cast<int>(config::DefaultMainWindowWidth),
            static_cast<int>(config::DefaultMainWindowHeight),
        }),
        minimumSize,
        maximumSize);

    wxPoint windowPosition = CenteredWindowPosition(workArea, windowSize);

    if (mNormalWindowState.position)
    {
      const wxPoint requestedPosition{
          static_cast<int>(mNormalWindowState.position->x),
          static_cast<int>(mNormalWindowState.position->y),
      };

      const int requestedDisplay = wxDisplay::GetFromRect(
          wxRect{requestedPosition, windowSize});

      if (requestedDisplay != wxNOT_FOUND &&
          (displayIndex == wxNOT_FOUND || requestedDisplay == displayIndex))
      {
        windowPosition = VisibleWindowPosition(
            requestedPosition, workArea, windowSize);
      }
    }

    return config::MainWindowState{
        .position = config::WindowPosition{
            .x = static_cast<std::int32_t>(windowPosition.x),
            .y = static_cast<std::int32_t>(windowPosition.y),
        },
        .width = static_cast<std::uint32_t>(windowSize.x),
        .height = static_cast<std::uint32_t>(windowSize.y),
        .maximized = false,
    };
  }

  void MainFrame::CaptureWindowGeometry()
  {
    if (IsIconized())
    {
      return;
    }

    mNormalWindowState.maximized = IsMaximized();
    if (mNormalWindowState.maximized)
    {
      return;
    }

    const auto rect = GetScreenRect();
    if (rect.width <= 0 || rect.height <= 0)
    {
      return;
    }

    mNormalWindowState.position = config::WindowPosition{
        .x = static_cast<std::int32_t>(rect.x),
        .y = static_cast<std::int32_t>(rect.y),
    };
    mNormalWindowState.width = static_cast<std::uint32_t>(rect.width);
    mNormalWindowState.height = static_cast<std::uint32_t>(rect.height);
  }

  void MainFrame::OnWindowMoved(wxMoveEvent &event)
  {
    if (mWindowGeometryInitialized)
    {
      CaptureWindowGeometry();

      mConfiguration.workspace.mainWindow = mNormalWindowState;

      ScheduleWindowGeometrySave();
    }

    event.Skip();
  }

  void MainFrame::OnWindowSized(wxSizeEvent &event)
  {
    if (mWindowGeometryInitialized)
    {
      CaptureWindowGeometry();

      mConfiguration.workspace.mainWindow = mNormalWindowState;

      ScheduleWindowGeometrySave();
    }

    event.Skip();
  }

  void MainFrame::ScheduleWindowGeometrySave()
  {
    if (!mConfigurationWritable || !mWindowGeometrySaveTimer)
    {
      return;
    }

    mWindowGeometrySaveTimer->StartOnce(
        WindowGeometrySaveDelayMilliseconds);
  }

  void MainFrame::PersistWindowGeometry()
  {
    if (!mConfigurationWritable)
    {
      return;
    }

    CaptureWindowGeometry();

    mConfiguration.workspace.mainWindow = mNormalWindowState;

    if (mConfiguration.workspace.mainWindow == mLastPersistedWindowState)
    {
      return;
    }

    (void)SaveConfiguration();
  }

  void MainFrame::CaptureOpenConnectionTabs()
  {
    if (!mConnectionNotebook)
    {
      return;
    }

    if (auto *const selected = SelectedConnectionTab();
        selected != nullptr &&
        selected->connectionState == ConnectionState::Disconnected)
    {
      CaptureQuickConnectDraft(*selected);
    }

    std::vector<config::OpenConnectionTabState> openTabs;
    openTabs.reserve(mConnectionNotebook->GetPageCount());

    for (std::size_t pageIndex = 0; pageIndex < mConnectionNotebook->GetPageCount(); ++pageIndex)
    {
      auto *const page = mConnectionNotebook->GetPage(pageIndex);

      const auto found = std::ranges::find_if(
          mConnectionTabs,
          [page](const auto &candidate)
          { return candidate->panel == page; });

      if (found == mConnectionTabs.end())
      {
        continue;
      }

      const auto &tab = **found;

      auto identity = tab.workspaceIdentity;
      if (identity)
      {
        if (const auto *saved =
                std::get_if<config::SavedSiteWorkspaceIdentity>(&*identity);
            saved != nullptr &&
            std::ranges::none_of(
                mConfiguration.sites,
                [&](const SiteProfile &site)
                { return site.id == saved->siteId; }))
        {
          identity.reset();
        }
        else if (const auto *quick =
                     std::get_if<config::QuickConnectWorkspaceIdentity>(
                         &*identity);
                 quick != nullptr &&
                 !QuickHistoryContains(mConfiguration, quick->endpoint))
        {
          identity.reset();
        }
      }

      auto localDirectory = tab.localDirectory;
      if (localDirectory.empty() && tab.localPath)
      {
        localDirectory = platform::FromToolkitPath(tab.localPath->GetPath());
      }

      auto remoteDirectory = tab.remoteViewReady
                                 ? tab.remoteDirectory
                                 : tab.quickDraft.initialRemoteDirectory;
      if (remoteDirectory.DisplayUtf8().empty())
      {
        remoteDirectory = RemotePath::Root();
      }

      openTabs.push_back(config::OpenConnectionTabState{
          .connectionId = tab.connectionId,
          .connection = std::move(identity),
          .localDirectory = std::move(localDirectory),
          .remoteDirectory = std::move(remoteDirectory),
      });
    }

    mConfiguration.workspace.openTabs = std::move(openTabs);

    if (auto *const selected = SelectedConnectionTab())
    {
      mConfiguration.workspace.selectedConnectionId = selected->connectionId;
    }
    else
    {
      mConfiguration.workspace.selectedConnectionId.clear();
    }
  }

  void MainFrame::PersistOpenConnectionTabs()
  {
    if (!mConfigurationWritable || !mConnectionNotebook || mRestoringWorkspace)
    {
      return;
    }

    auto previousTabs = mConfiguration.workspace.openTabs;
    auto previousSelection = mConfiguration.workspace.selectedConnectionId;

    CaptureOpenConnectionTabs();

    if (mConfiguration.workspace.openTabs == previousTabs &&
        mConfiguration.workspace.selectedConnectionId == previousSelection)
    {
      return;
    }

    if (!SaveConfiguration())
    {
      mConfiguration.workspace.openTabs = std::move(previousTabs);
      mConfiguration.workspace.selectedConnectionId =
          std::move(previousSelection);
    }
  }

  void MainFrame::OnWindowGeometrySaveTimer(wxTimerEvent &)
  {
    PersistWindowGeometry();
  }

  bool MainFrame::SaveConfiguration()
  {
    if (!mConfigurationWritable)
    {
      LocalizedMessageBox(*mTranslations, Translated(*mTranslations, "config.readOnlyMessage"),
                          Translated(*mTranslations, "config.readOnlyTitle"),
                          wxOK | wxICON_WARNING,
                          this);

      return false;
    }

    CaptureOpenConnectionTabs();

    auto saved = mConfigRepository->Save(mConfiguration);
    if (!saved)
    {
      LocalizedMessageBox(*mTranslations, FromUtf8(ConfigErrorMessage(saved.error())),
                          Translated(*mTranslations, "config.saveFailedTitle"),
                          wxOK | wxICON_ERROR,
                          this);

      AppendLog(DiagnosticLevel::Error, ConfigErrorMessage(saved.error()));

      return false;
    }

    mLastPersistedWindowState = mConfiguration.workspace.mainWindow;

    return true;
  }

  void MainFrame::InitializeTransferQueue(
      std::expected<config::QueueLoadResult, config::ConfigError> initialQueue)
  {
    if (!initialQueue)
    {
      const auto message = ConfigErrorMessage(initialQueue.error());

      AppendLog(DiagnosticLevel::Error, message);

      LocalizedMessageBox(
          *mTranslations,
          TranslatedFormat(*mTranslations, "queue.persistenceLoadFailedMessage", message),
          Translated(*mTranslations, "queue.persistenceLoadFailedTitle"),
          wxOK | wxICON_ERROR,
          this);

      mQueuePersistenceWritable = false;

      for (const auto &restoredTab : mConfiguration.workspace.openTabs)
      {
        (void)CreateConnectionTab(false,
                                  restoredTab.connectionId,
                                  {},
                                  restoredTab);
      }

      EnsureConnectionTab();

      return;
    }

    mQueuePersistenceWritable = mQueueRepository != nullptr;

    struct RestoredGroup final
    {
      std::string connectionId;
      std::string originalConnectionId;
      std::string siteId;
      SiteEndpointIdentity endpoint;
      std::vector<config::PersistentQueueItem> items;
    };

    std::vector<RestoredGroup> groups;

    for (auto &item : initialQueue->items)
    {
      const auto &job = item.transfer.job;
      if (!job.siteEndpoint)
      {
        continue;
      }

      auto group = std::ranges::find_if(
          groups,
          [&](const RestoredGroup &candidate)
          {
            return candidate.originalConnectionId == item.connectionId &&
                   candidate.siteId == job.siteId &&
                   EndpointIdentitiesEqual(candidate.endpoint,
                                           *job.siteEndpoint);
          });

      if (group == groups.end())
      {
        const bool firstForOriginal = !std::ranges::any_of(
            groups,
            [&](const RestoredGroup &candidate)
            { return candidate.originalConnectionId == item.connectionId; });

        auto recoveredId = firstForOriginal ? item.connectionId : GenerateId();

        if (recoveredId.empty() || FindConnectionTab(recoveredId) != nullptr)
        {
          recoveredId = GenerateId();
        }

        groups.push_back(RestoredGroup{
            .connectionId = std::move(recoveredId),
            .originalConnectionId = item.connectionId,
            .siteId = job.siteId,
            .endpoint = *job.siteEndpoint,
            .items = {},
        });

        group = std::prev(groups.end());
      }

      item.connectionId = group->connectionId;

      group->items.push_back(std::move(item));
    }

    std::size_t restoredCount{};

    for (const auto &restoredTab : mConfiguration.workspace.openTabs)
    {
      auto matchingGroup = std::ranges::find_if(
          groups,
          [&](const RestoredGroup &candidate)
          {
            return candidate.originalConnectionId ==
                       restoredTab.connectionId &&
                   restoredTab.connection &&
                   WorkspaceConnectionMatchesQueuedSite(
                       *restoredTab.connection,
                       candidate.siteId,
                       candidate.endpoint,
                       mConfiguration.sites);
          });

      if (matchingGroup == groups.end())
      {
        (void)CreateConnectionTab(
            false,
            restoredTab.connectionId,
            {},
            restoredTab);

        continue;
      }

      matchingGroup->connectionId = restoredTab.connectionId;

      for (auto &item : matchingGroup->items)
      {
        item.connectionId = matchingGroup->connectionId;
      }

      restoredCount += matchingGroup->items.size();

      (void)CreateConnectionTab(
          false,
          restoredTab.connectionId,
          std::move(matchingGroup->items),
          restoredTab);

      groups.erase(matchingGroup);
    }

    for (auto &group : groups)
    {
      restoredCount += group.items.size();

      if (FindConnectionTab(group.connectionId) != nullptr)
      {
        group.connectionId = GenerateId();

        for (auto &item : group.items)
        {
          item.connectionId = group.connectionId;
        }
      }

      (void)CreateConnectionTab(
          false, group.connectionId, std::move(group.items));
    }

    EnsureConnectionTab();

    if (!mConfiguration.workspace.selectedConnectionId.empty())
    {
      if (auto *const selected = FindConnectionTab(
              mConfiguration.workspace.selectedConnectionId))
      {
        const int index = mConnectionNotebook->GetPageIndex(selected->panel);
        if (index != wxNOT_FOUND)
        {
          mConnectionNotebook->SetSelection(static_cast<std::size_t>(index));
        }
      }
    }

    if (restoredCount != 0U)
    {
      PopulateTransferLists();

      const auto restoredCountText = std::to_string(restoredCount);

      AppendLog(
          DiagnosticLevel::Information,
          mTranslations->Format(
              "queue.persistenceRestoredLog",
              std::array<std::string_view, 1>{
                  restoredCountText}));

      // Rewrites only when loading split a formerly multi-endpoint tab. It also
      // canonicalizes interrupted states before any connection can run them.
      (void)PersistTransferQueue();
    }

    PersistOpenConnectionTabs();
  }

  bool MainFrame::PersistTransferQueue()
  {
    if (!mQueuePersistenceWritable || !mQueueRepository)
    {
      return false;
    }

    std::vector<config::PersistentQueueItem> items;

    for (const auto &tab : mConnectionTabs)
    {
      if (!tab->controller)
      {
        continue;
      }

      auto persisted = tab->controller->PersistentQueueItems();

      items.insert(items.end(),
                   std::make_move_iterator(persisted.begin()),
                   std::make_move_iterator(persisted.end()));
    }

    auto saved = mQueueRepository->Save(items);
    if (saved)
    {
      mQueuePersistenceErrorReported = false;

      return true;
    }

    if (!mQueuePersistenceErrorReported)
    {
      mQueuePersistenceErrorReported = true;

      const auto message = ConfigErrorMessage(saved.error());

      AppendLog(DiagnosticLevel::Error, message);

      LocalizedMessageBox(
          *mTranslations,
          TranslatedFormat(*mTranslations, "queue.persistenceSaveFailedMessage", message),
          Translated(*mTranslations, "queue.persistenceSaveFailedTitle"),
          wxOK | wxICON_ERROR,
          this);
    }

    return false;
  }

  void MainFrame::PrepareTransferQueueForShutdown()
  {
    for (const auto &tab : mConnectionTabs)
    {
      if (tab->controller)
      {
        tab->controller->PrepareForShutdown();
      }
    }
  }

  bool MainFrame::HasUnfinishedTransfers() const
  {
    return std::ranges::any_of(mConnectionTabs, [](const auto &tab)
                               { return tab->controller &&
                                        std::ranges::any_of(tab->controller->Transfers(),
                                                            UnfinishedTransfer); });
  }

  void MainFrame::SetConnectionState(ConnectionTab &tab,
                                     const ConnectionState state)
  {
    if (state != ConnectionState::Connected && tab.remoteViewReady)
    {
      tab.quickDraft.initialRemoteDirectory = tab.remoteDirectory;
    }

    tab.connectionState = state;

    if (state != ConnectionState::Connected)
    {
      tab.remoteViewReady = false;

      ClearRemoteView(tab);
    }

    UpdateConnectionTabTitle(tab);

    if (&tab == SelectedConnectionTab())
    {
      UpdateSelectedConnectionUi();
    }

    UpdateQueueActionState();
  }

  void MainFrame::UpdateFileActionState()
  {
    for (auto &tab : mConnectionTabs)
    {
      const bool remoteEnabled = RemoteActionsAvailable(*tab);
      const auto localSelectedRows = tab->localList->SelectedRowCount();
      const auto remoteSelectedRows = tab->remoteList->SelectedRowCount();
      const auto localSelection = SelectedLocalIndices(*tab).size();
      const auto remoteSelection = SelectedRemoteIndices(*tab).size();
      const auto localIndex = SelectedLocalIndex(*tab);
      const auto remoteIndex = SelectedRemoteIndex(*tab);
      const bool localReady = !tab->localDirectory.empty() &&
                              !tab->localDirectoryLoading;

      tab->localList->Enable(localReady);

      const auto localParent = tab->localDirectory.parent_path();

      tab->localUpButton->Enable(localReady && !localParent.empty() &&
                                 localParent != tab->localDirectory);
      tab->localRefreshButton->Enable(localReady);
      tab->localCreateButton->Enable(localReady);
      tab->localEditButton->Enable(
          localReady && localSelectedRows == 1U && localIndex &&
          tab->localEntries[*localIndex].kind == LocalEntryKind::File);
      tab->localRenameButton->Enable(
          localReady && localSelectedRows == 1U && localSelection == 1U);
      tab->localDeleteButton->Enable(localReady && localSelection != 0U);

      // Keep direct navigation available after the connection succeeds even
      // if the configured initial directory could not be listed.
      tab->remotePath->Enable(
          tab->connectionState == ConnectionState::Connected);
      tab->remoteUpButton->Enable(remoteEnabled &&
                                  !tab->remoteDirectory.IsRoot());
      tab->remoteRefreshButton->Enable(remoteEnabled);
      tab->remoteList->Enable(remoteEnabled);
      tab->remoteCreateDirectoryButton->Enable(remoteEnabled);
      tab->remoteCreateFileButton->Enable(remoteEnabled);
      tab->remoteEditButton->Enable(
          remoteEnabled && remoteSelectedRows == 1U && remoteIndex &&
          tab->remoteEntries[*remoteIndex].kind == RemoteEntryKind::File);
      tab->remoteRenameButton->Enable(
          remoteEnabled && remoteSelectedRows == 1U &&
          remoteSelection == 1U);

      const bool permissionSelection =
          remoteSelection != 0U &&
          remoteSelectedRows == remoteSelection &&
          std::ranges::all_of(
              SelectedRemoteIndices(*tab),
              [tab = tab.get()](const std::size_t sourceIndex)
              {
                if (sourceIndex >= tab->remoteEntries.size())
                {
                  return false;
                }

                const auto kind = tab->remoteEntries[sourceIndex].kind;

                return kind == RemoteEntryKind::File ||
                       kind == RemoteEntryKind::Directory;
              });

      tab->remotePermissionsButton->Enable(remoteEnabled &&
                                           permissionSelection);
      tab->remoteDeleteButton->Enable(remoteEnabled &&
                                      remoteSelection != 0U);
      tab->uploadButton->Enable(remoteEnabled && localReady &&
                                localSelection != 0U);
      tab->downloadButton->Enable(remoteEnabled && localReady &&
                                  remoteSelection != 0U);
    }
  }

  void MainFrame::UpdateQueueActionState()
  {
    bool canPause = false;
    bool canCancel = false;
    bool canRetry = false;
    bool canRemove = false;

    if (const auto ref = SelectedQueueJob())
    {
      if (const auto *const tab = FindConnectionTab(ref->connectionId))
      {
        const auto actions = tab->controller->TransferActions(ref->jobId);

        canPause = actions.pause;
        canCancel = actions.cancel;
        canRetry = actions.retry &&
                   tab->connectionState == ConnectionState::Connected;

        const auto transfer = std::ranges::find(
            tab->transfers,
            ref->jobId,
            [](const QueuedTransfer &candidate)
            { return std::string_view{candidate.job.id}; });

        canRemove = transfer != tab->transfers.end() &&
                    (transfer->state == TransferState::Completed ||
                     transfer->state == TransferState::Failed ||
                     transfer->state == TransferState::Cancelled);
      }
    }

    mQueuePauseButton->Enable(canPause);
    mQueueCancelButton->Enable(canCancel);
    mQueueRetryButton->Enable(canRetry);
    mQueueRemoveButton->Enable(canRemove);

    const int view = mTransferNotebook ? mTransferNotebook->GetSelection()
                                       : wxNOT_FOUND;

    mQueueClearButton->Enable(
        (view == 1 && !mFailedJobIds.empty()) ||
        (view == 2 && !mCompletedJobIds.empty()));

    const auto *const list = ActiveTransferList();

    mQueueCopyButton->Enable(list && list->GetSelectedItemCount() != 0);
    mQueueExportButton->Enable(list && list->GetItemCount() != 0);
  }

  void MainFrame::ClearRemoteView(ConnectionTab &tab)
  {
    tab.remoteDirectory = RemotePath::Root();
    tab.remoteEntries.clear();
    tab.preferredRemoteSelectionIdentity.reset();
    tab.remotePath->ChangeValue("/");
    tab.remoteList->ClearRows();
  }

  bool MainFrame::RemoteActionsAvailable(const ConnectionTab &tab) const noexcept
  {
    return tab.connectionState == ConnectionState::Connected &&
           tab.remoteViewReady;
  }

  void MainFrame::RequestLocalDirectoryRefresh(ConnectionTab &tab)
  {
    if (tab.localDirectory.empty())
    {
      tab.localRefreshPending = false;

      return;
    }

    if (tab.localDirectoryLoading)
    {
      tab.localRefreshPending = true;

      return;
    }

    tab.localRefreshPending = false;

    PopulateLocalDirectory(tab, tab.localDirectory);
  }

  void MainFrame::PopulateLocalDirectory(
      ConnectionTab &tab,
      const std::filesystem::path &directory,
      std::optional<std::filesystem::path> fallback)
  {
    std::error_code error;

    auto requested = std::filesystem::absolute(directory, error);

    if (error)
    {
      AppendLog(DiagnosticLevel::Error,
                mTranslations->Format(
                    "local.directoryUnavailable",
                    std::array<std::string_view, 1>{GenericUtf8(directory)}));

      return;
    }

    requested = requested.lexically_normal();

    if (fallback)
    {
      std::error_code fallbackError;

      auto absoluteFallback = std::filesystem::absolute(*fallback, fallbackError);

      if (!fallbackError)
      {
        absoluteFallback = absoluteFallback.lexically_normal();

        if (absoluteFallback != requested)
        {
          fallback = std::move(absoluteFallback);
        }
        else
        {
          fallback.reset();
        }
      }
      else
      {
        fallback.reset();
      }
    }

    tab.localDirectoryFallback = std::move(fallback);
    tab.localDirectoryLoading = true;
    tab.localPath->SetPath(platform::ToToolkitPath(requested));

    UpdateFileActionState();

    try
    {
      (void)tab.localDirectoryLoader->Load(std::move(requested));
    }
    catch (const std::system_error &exception)
    {
      tab.localDirectoryLoading = false;
      tab.localDirectoryFallback.reset();
      tab.localPath->SetPath(platform::ToToolkitPath(tab.localDirectory));

      const bool workersBusy =
          exception.code() == std::make_error_code(
                                  std::errc::resource_unavailable_try_again);

      const auto message = workersBusy
                               ? Translated(*mTranslations,
                                            "local.directoryBusy")
                               : FromUtf8(exception.what());

      AppendLog(DiagnosticLevel::Error, ToUtf8(message));

      LocalizedMessageBox(*mTranslations, message,
                          Translated(*mTranslations,
                                     "local.openFailedTitle"),
                          wxOK | wxICON_ERROR,
                          this);

      UpdateFileActionState();
    }
  }

  void MainFrame::OnLocalDirectoryLoaded(wxThreadEvent &event)
  {
    const auto result = event.GetPayload<LocalDirectoryResultPtr>();
    if (!result)
    {
      return;
    }

    auto *const tab = FindConnectionTab(result->connectionId);
    if (!tab || !tab->localDirectoryLoader ||
        result->generation !=
            tab->localDirectoryLoader->CurrentGeneration())
    {
      return;
    }

    tab->localDirectoryLoading = false;
    if (result->error)
    {
      tab->preferredLocalSelectionIdentity.reset();
      tab->localPath->SetPath(platform::ToToolkitPath(tab->localDirectory));

      auto fallback = std::exchange(tab->localDirectoryFallback,
                                    std::nullopt);

      AppendLog(
          fallback ? DiagnosticLevel::Warning : DiagnosticLevel::Error,
          mTranslations->Format(
              "local.directoryUnavailable",
              std::array<std::string_view, 1>{GenericUtf8(result->directory)}));

      if (fallback && *fallback != result->directory)
      {
        PopulateLocalDirectory(*tab, *fallback);

        return;
      }

      if (tab == SelectedConnectionTab())
      {
        LocalizedMessageBox(*mTranslations,
                            FromUtf8(result->error->message),
                            Translated(*mTranslations,
                                       "local.openFailedTitle"),
                            wxOK | wxICON_WARNING,
                            this);
      }

      UpdateFileActionState();

      if (tab->localRefreshPending)
      {
        RequestLocalDirectoryRefresh(*tab);
      }

      return;
    }

    tab->localDirectoryFallback.reset();
    tab->localDirectory = result->directory;
    tab->localEntries = result->entries;
    tab->localPath->SetPath(platform::ToToolkitPath(tab->localDirectory));

    RefreshLocalFileRows(*tab);

    RememberLocalDirectory(*tab, tab->localDirectory);

    if (tab->preferredLocalSelectionIdentity)
    {
      SelectFileListIdentity(*tab->localList,
                             *tab->preferredLocalSelectionIdentity);

      tab->preferredLocalSelectionIdentity.reset();
    }

    if (!result->warnings.empty())
    {
      AppendLog(
          DiagnosticLevel::Warning,
          mTranslations->Format(
              "local.entriesUnavailableCount",
              std::array<std::string_view, 2>{
                  std::to_string(result->warnings.size()),
                  result->warnings.front().message}));
    }

    UpdateFileActionState();

    if (tab->localRefreshPending)
    {
      RequestLocalDirectoryRefresh(*tab);
    }
  }

  void MainFrame::RefreshLocalFileRows(ConnectionTab &tab)
  {
    std::vector<FileListRow> rows;

    const auto parent = tab.localDirectory.parent_path();
    const bool hasParent = !parent.empty() && parent != tab.localDirectory;

    rows.reserve(tab.localEntries.size() + (hasParent ? 1U : 0U));

    if (hasParent)
    {
      const auto currentPath =
          GenericUtf8(tab.localDirectory.lexically_normal());

      rows.push_back(FileListRow{
          .stableIdentity = ParentDirectoryIdentity(currentPath),
          .sourceIndex = std::nullopt,
          .kind = FileListRowKind::ParentDirectory,
          .name = "..",
          .size = {},
          .type = {},
          .modified = {},
          .permissions = {},
          .owner = {},
          .rawSize = std::nullopt,
          .modifiedAt = std::nullopt,
          .rawPermissions = std::nullopt,
      });
    }

    for (std::size_t index = 0; index < tab.localEntries.size(); ++index)
    {
      const auto &entry = tab.localEntries[index];
      const auto kind = ToRemoteEntryKind(entry.kind);
      const auto filename = GenericUtf8(entry.path.filename());

      rows.push_back(FileListRow{
          .stableIdentity = GenericUtf8(entry.path.lexically_normal()),
          .sourceIndex = index,
          .kind = FileListKind(kind),
          .name = filename,
          .size = entry.exactSize ? ToUtf8(FormatBytes(*entry.exactSize))
                                  : std::string{},
          .type = ToUtf8(EntryType(*mTranslations, kind, filename)),
          .modified = FormattedModifiedTime(entry.modifiedAt),
          .permissions = {},
          .owner = {},
          .rawSize = entry.exactSize,
          .modifiedAt = entry.modifiedAt,
          .rawPermissions = std::nullopt,
      });
    }

    tab.localList->SetRows(std::move(rows));
  }

  void MainFrame::PopulateRemoteDirectory(ConnectionTab &tab,
                                          const DirectoryEvent &directory)
  {
    tab.remoteDirectory = directory.directory;

    // Keep the last successful browser location in the disconnected draft as
    // well. SetConnectionState() clears the live remote view on disconnect,
    // and workspace capture must not regress to the site's original path.
    tab.quickDraft.initialRemoteDirectory = directory.directory;
    tab.remoteEntries = directory.entries;
    tab.remotePath->ChangeValue(
        FromUtf8(tab.remoteDirectory.DisplayUtf8()));
    tab.remoteUpButton->Enable(RemoteActionsAvailable(tab) &&
                               !tab.remoteDirectory.IsRoot());

    RefreshRemoteFileRows(tab);
  }

  void MainFrame::RefreshRemoteFileRows(ConnectionTab &tab)
  {
    std::vector<FileListRow> rows;

    const bool hasParent = !tab.remoteDirectory.IsRoot();

    rows.reserve(tab.remoteEntries.size() + (hasParent ? 1U : 0U));

    if (hasParent)
    {
      rows.push_back(FileListRow{
          .stableIdentity =
              ParentDirectoryIdentity(tab.remoteDirectory.Bytes()),
          .sourceIndex = std::nullopt,
          .kind = FileListRowKind::ParentDirectory,
          .name = "..",
          .size = {},
          .type = {},
          .modified = {},
          .permissions = {},
          .owner = {},
          .rawSize = std::nullopt,
          .modifiedAt = std::nullopt,
          .rawPermissions = std::nullopt,
      });
    }

    for (std::size_t index = 0; index < tab.remoteEntries.size(); ++index)
    {
      const auto &entry = tab.remoteEntries[index];

      const auto size = entry.kind == RemoteEntryKind::File
                            ? std::optional<std::uint64_t>{entry.size}
                            : std::nullopt;

      rows.push_back(FileListRow{
          .stableIdentity = entry.path.Bytes(),
          .sourceIndex = index,
          .kind = FileListKind(entry.kind),
          .name = entry.name.DisplayUtf8(),
          .size = size ? ToUtf8(FormatBytes(*size)) : std::string{},
          .type = ToUtf8(EntryType(*mTranslations, entry.kind,
                                   entry.name.DisplayUtf8())),
          .modified = FormattedModifiedTime(entry.modifiedAt),
          .permissions = FormatFilePermissions(entry.permissions),
          .owner = FormatFileOwnerGroup(
              entry.owner.value_or(std::string{}),
              entry.group.value_or(std::string{})),
          .rawSize = size,
          .modifiedAt = entry.modifiedAt,
          .rawPermissions = entry.permissions,
      });
    }

    tab.remoteList->SetRows(std::move(rows));

    if (tab.preferredRemoteSelectionIdentity)
    {
      const auto found = std::ranges::find(
          tab.remoteList->Rows(),
          *tab.preferredRemoteSelectionIdentity,
          &FileListRow::stableIdentity);

      if (found != tab.remoteList->Rows().end())
      {
        SelectFileListIdentity(*tab.remoteList,
                               *tab.preferredRemoteSelectionIdentity);

        tab.preferredRemoteSelectionIdentity.reset();
      }
    }
  }

  void MainFrame::PopulateTransferLists()
  {
    const auto visibleId = [](const std::vector<QueueJobRef> &ids,
                              const long row)
        -> std::optional<QueueJobRef>
    {
      return row < 0 || static_cast<std::size_t>(row) >= ids.size()
                 ? std::nullopt
                 : std::optional<QueueJobRef>{ids[static_cast<std::size_t>(row)]};
    };

    struct ViewSelection final
    {
      std::unordered_map<std::string, std::unordered_set<std::string>> jobs;
      std::optional<QueueJobRef> focused;
    };

    const auto selectedIds = [&visibleId](
                                 wxListCtrl *list,
                                 const std::vector<QueueJobRef> &ids)
    {
      ViewSelection selection;

      for (long row = list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
           row != -1;
           row = list->GetNextItem(row, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED))
      {
        if (const auto ref = visibleId(ids, row))
        {
          selection.jobs[ref->connectionId].insert(ref->jobId);
        }
      }

      selection.focused = visibleId(
          ids,
          list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_FOCUSED));

      return selection;
    };

    const auto queuedSelection = selectedIds(mQueuedList, mQueuedJobIds);
    const auto failedSelection = selectedIds(mFailedList, mFailedJobIds);
    const auto completedSelection = selectedIds(mCompletedList, mCompletedJobIds);
    const auto queuedTop = visibleId(
        mQueuedJobIds, mQueuedList->GetTopItem());
    const auto failedTop = visibleId(
        mFailedJobIds, mFailedList->GetTopItem());
    const auto completedTop = visibleId(
        mCompletedJobIds, mCompletedList->GetTopItem());

    // Progress can arrive frequently from several workers. Suppress native
    // list redraw until all three views contain one consistent snapshot.
    wxWindowUpdateLocker redrawLock{mTransferNotebook};

    // Rebuilding selection must not publish transient action states, nor run
    // one controller lookup per selected row while progress updates arrive.
    wxEventBlocker queuedEvents{mQueuedList};
    wxEventBlocker failedEvents{mFailedList};
    wxEventBlocker completedEvents{mCompletedList};

    mQueuedList->DeleteAllItems();
    mFailedList->DeleteAllItems();
    mCompletedList->DeleteAllItems();
    mQueuedJobIds.clear();
    mFailedJobIds.clear();
    mCompletedJobIds.clear();

    const auto append = [this](wxListCtrl *list,
                               const ConnectionTab &tab,
                               const QueuedTransfer &transfer)
    {
      const wxString item = transfer.job.direction == TransferDirection::Upload
                                ? platform::ToToolkitPath(transfer.job.localPath.filename())
                                : FromUtf8(transfer.job.remotePath.Filename().DisplayUtf8());

      const int pageIndex = mConnectionNotebook->GetPageIndex(tab.panel);

      const auto connectionLabel = pageIndex == wxNOT_FOUND
                                       ? Translated(*mTranslations, "connection.tab.closed")
                                       : mConnectionNotebook->GetPageText(
                                             static_cast<std::size_t>(pageIndex));

      const long row = list->InsertItem(list->GetItemCount(),
                                        connectionLabel);

      list->SetItem(row,
                    1,
                    transfer.job.direction == TransferDirection::Upload
                        ? Translated(*mTranslations, "transfer.direction.upload")
                        : Translated(*mTranslations, "transfer.direction.download"));

      list->SetItem(row, 2, item);

      if (const auto percent = TransferProgressPercent(transfer.state,
                                                       transfer.progress))
      {
        list->SetItem(row, 3, wxString::Format("%u%%", *percent));
      }
      else
      {
        list->SetItem(row, 3, FormatBytes(transfer.progress.bytesTransferred));
      }

      if (const auto speed = TransferSpeedBytesPerSecond(
              transfer.state, transfer.progress))
      {
        list->SetItem(row, 4, FormatBytes(*speed) + "/s");
      }

      if (const auto eta = TransferEstimatedTimeRemaining(
              transfer.state, transfer.progress))
      {
        list->SetItem(row, 5, FormatEta(*eta));
      }

      list->SetItem(row, 6, TransferStateText(*mTranslations, transfer.state));

      if (transfer.error)
      {
        list->SetItem(row, 7, FromUtf8(SanitizeDiagnosticText(transfer.error->message)));
      }
    };

    for (const auto &tab : mConnectionTabs)
    {
      for (const auto &transfer : tab->transfers)
      {
        QueueJobRef ref{tab->connectionId, transfer.job.id};

        if (transfer.state == TransferState::Completed)
        {
          append(mCompletedList, *tab, transfer);
          mCompletedJobIds.push_back(std::move(ref));
        }
        else if (transfer.state == TransferState::Failed ||
                 transfer.state == TransferState::Cancelled)
        {
          append(mFailedList, *tab, transfer);
          mFailedJobIds.push_back(std::move(ref));
        }
        else
        {
          append(mQueuedList, *tab, transfer);
          mQueuedJobIds.push_back(std::move(ref));
        }
      }
    }

    const auto restoreSelection = [](wxListCtrl *list,
                                     const std::vector<QueueJobRef> &ids,
                                     const ViewSelection &selected)
    {
      for (std::size_t row = 0; row < ids.size(); ++row)
      {
        const auto found = selected.jobs.find(ids[row].connectionId);

        if (found != selected.jobs.end() && found->second.contains(ids[row].jobId))
        {
          list->SetItemState(static_cast<long>(row),
                             wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        }
      }

      // Focus and the Shift-selection anchor belong to one row, independently
      // of the selected set. Never take keyboard focus away from another view.
      if (selected.focused)
      {
        const auto found = std::ranges::find(ids, *selected.focused);

        if (found != ids.end())
        {
          list->SetItemState(static_cast<long>(found - ids.begin()),
                             wxLIST_STATE_FOCUSED, wxLIST_STATE_FOCUSED);
        }
      }
    };

    restoreSelection(mQueuedList, mQueuedJobIds, queuedSelection);
    restoreSelection(mFailedList, mFailedJobIds, failedSelection);
    restoreSelection(mCompletedList, mCompletedJobIds, completedSelection);

    const auto restoreTop = [](wxListCtrl *list,
                               const std::vector<QueueJobRef> &ids,
                               const std::optional<QueueJobRef> &top)
    {
      if (!top)
      {
        return;
      }

      const auto found = std::ranges::find(ids, *top);

      if (found == ids.end())
      {
        return;
      }

      const long row = static_cast<long>(found - ids.begin());

      if (row == 0)
      {
        return;
      }

      // Ensuring the last row first makes the second call scroll upward, so
      // wxListCtrl places the former top item at (or nearest) the top again.
      const long last = list->GetItemCount() - 1;

      if (last >= 0)
      {
        list->EnsureVisible(last);
        list->EnsureVisible(row);
      }
    };

    restoreTop(mQueuedList, mQueuedJobIds, queuedTop);
    restoreTop(mFailedList, mFailedJobIds, failedTop);
    restoreTop(mCompletedList, mCompletedJobIds, completedTop);

    UpdateQueueActionState();

    SetStatusText(mQueuedJobIds.empty()
                      ? Translated(*mTranslations, "status.queueEmpty")
                      : TranslatedFormat(*mTranslations,
                                         "status.queuedActive",
                                         mQueuedJobIds.size()),
                  1);
  }

  void MainFrame::HandleExternalEditTransfers(
      ConnectionTab &tab,
      const std::vector<QueuedTransfer> &transfers)
  {
    const auto findTransfer = [&](const std::string_view jobId)
        -> const QueuedTransfer *
    {
      const auto found = std::ranges::find(
          transfers, jobId, [](const QueuedTransfer &transfer)
          { return std::string_view{transfer.job.id}; });

      return found == transfers.end() ? nullptr : &*found;
    };

    const auto transferError = [this](const QueuedTransfer &transfer)
    {
      return transfer.error
                 ? transfer.error->message
                 : std::string{mTranslations->Text(
                       transfer.state == TransferState::Cancelled
                           ? "transfer.state.canceled"
                           : "transfer.state.failed")};
    };

    const auto reportRecovery = [this](ExternalEditSession &session,
                                       const std::string_view titleKey,
                                       const std::string_view messageKey,
                                       const std::string_view logKey,
                                       const std::string_view errorText)
    {
      if (session.failureReported)
      {
        return;
      }

      session.failureReported = true;

      const auto remoteText = session.remotePath.DisplayUtf8();
      const auto localText = GenericUtf8(session.localPath);

      wxString message = TranslatedFormat(
          *mTranslations, messageKey, remoteText, errorText);
      message += "\n\n";
      message += TranslatedFormat(
          *mTranslations, "externalEditor.recoveryCopyMessage", localText);

      LocalizedMessageBox(
          *mTranslations,
          message,
          Translated(*mTranslations, titleKey),
          wxOK | wxICON_ERROR,
          this);

      AppendLog(
          DiagnosticLevel::Error,
          mTranslations->Format(
              logKey,
              std::array<std::string_view, 2>{remoteText, errorText}));

      AppendLog(
          DiagnosticLevel::Warning,
          mTranslations->Format(
              "externalEditor.recoveryCopyLog",
              std::array<std::string_view, 1>{localText}));
    };

    for (auto &owned : mExternalEditSessions)
    {
      auto &session = *owned;

      if (session.connectionId != tab.connectionId ||
          session.phase == ExternalEditSession::Phase::Detached)
      {
        continue;
      }

      if (const auto *download = findTransfer(session.downloadJobId))
      {
        if (session.phase == ExternalEditSession::Phase::Failed &&
            !session.uploadJobId && UnfinishedTransfer(*download))
        {
          session.phase = ExternalEditSession::Phase::Downloading;
          session.failureReported = false;
        }

        if (session.phase == ExternalEditSession::Phase::Downloading &&
            download->state == TransferState::Completed)
        {
          const auto stamp = session.ReadLocalStamp();
          if (!stamp)
          {
            session.phase = ExternalEditSession::Phase::Failed;
            session.failureReported = true;

            const auto localText = GenericUtf8(session.localPath);
            const std::string errorText{mTranslations->Text(
                "externalEditor.error.downloadedCopyUnavailable")};

            LocalizedMessageBox(
                *mTranslations,
                TranslatedFormat(
                    *mTranslations,
                    "externalEditor.inspectionFailedMessage",
                    localText,
                    errorText),
                Translated(
                    *mTranslations,
                    "externalEditor.inspectionFailedTitle"),
                wxOK | wxICON_ERROR,
                this);

            AppendLog(
                DiagnosticLevel::Error,
                mTranslations->Format(
                    "externalEditor.inspectionFailedLog",
                    std::array<std::string_view, 2>{localText,
                                                    errorText}));

            continue;
          }

          const auto launched = platform::LaunchExternalEditor(
              mConfiguration.settings.externalEditor,
              session.localPath);
          if (!launched)
          {
            session.phase = ExternalEditSession::Phase::Failed;

            reportRecovery(
                session,
                "externalEditor.launchFailedTitle",
                "externalEditor.launchFailedMessage",
                "externalEditor.launchFailedLog",
                ExternalEditorErrorText(*mTranslations,
                                        launched.error()));

            continue;
          }

          session.synchronizedStamp = *stamp;
          session.observedStamp = *stamp;
          session.stableSince.reset();
          session.missingSince.reset();
          session.phase = ExternalEditSession::Phase::Watching;
          session.failureReported = false;

          const auto remoteText = session.remotePath.DisplayUtf8();

          AppendLog(
              DiagnosticLevel::Information,
              mTranslations->Format(
                  "externalEditor.openedLog",
                  std::array<std::string_view, 1>{remoteText}));

          AppendLog(
              DiagnosticLevel::Information,
              mTranslations->Format(
                  "externalEditor.watchingLog",
                  std::array<std::string_view, 1>{remoteText}));
        }
        else if (session.phase == ExternalEditSession::Phase::Downloading &&
                 (download->state == TransferState::Failed ||
                  download->state == TransferState::Cancelled))
        {
          session.phase = ExternalEditSession::Phase::Failed;

          const auto errorText = transferError(*download);

          if (!session.failureReported)
          {
            session.failureReported = true;

            const auto remoteText = session.remotePath.DisplayUtf8();

            LocalizedMessageBox(
                *mTranslations,
                TranslatedFormat(
                    *mTranslations,
                    "externalEditor.downloadFailedMessage",
                    remoteText,
                    errorText),
                Translated(
                    *mTranslations,
                    "externalEditor.downloadFailedTitle"),
                wxOK | wxICON_ERROR,
                this);

            AppendLog(
                DiagnosticLevel::Error,
                mTranslations->Format(
                    "externalEditor.downloadFailedLog",
                    std::array<std::string_view, 2>{remoteText,
                                                    errorText}));
          }
        }
      }

      if (!session.uploadJobId)
      {
        continue;
      }

      const auto *upload = findTransfer(*session.uploadJobId);
      if (!upload)
      {
        continue;
      }

      if (session.phase == ExternalEditSession::Phase::Failed &&
          UnfinishedTransfer(*upload))
      {
        if (const auto stamp = session.ReadLocalStamp())
        {
          session.queuedStamp = *stamp;
          session.observedStamp = *stamp;
          session.stableSince.reset();
          session.missingSince.reset();
        }
        session.phase = ExternalEditSession::Phase::Uploading;
        session.failureReported = false;
      }

      if (session.phase == ExternalEditSession::Phase::Uploading &&
          upload->state == TransferState::Completed)
      {
        session.phase = ExternalEditSession::Phase::Inspecting;
        session.inspectionRequestId = GenerateId();

        tab.controller->Inspect(session.remotePath,
                                *session.inspectionRequestId);
      }
      else if (session.phase == ExternalEditSession::Phase::Uploading &&
               (upload->state == TransferState::Failed ||
                upload->state == TransferState::Cancelled))
      {
        session.phase = ExternalEditSession::Phase::Failed;

        const auto errorText = transferError(*upload);

        reportRecovery(
            session,
            "externalEditor.uploadFailedTitle",
            "externalEditor.uploadFailedMessage",
            "externalEditor.uploadFailedLog",
            errorText);
      }
    }
  }

  void MainFrame::BeginExternalEditUpload(ExternalEditSession &session)
  {
    if (session.phase != ExternalEditSession::Phase::Watching ||
        !session.observedStamp ||
        session.observedStamp == session.synchronizedStamp)
    {
      return;
    }

    auto *const tab = FindConnectionTab(session.connectionId);

    if (!tab || !RemoteActionsAvailable(*tab) ||
        tab->activeConnectionSiteId != session.siteId ||
        tab->activeConnectionGeneration != session.generation)
    {
      session.phase = ExternalEditSession::Phase::Detached;

      const auto localText = GenericUtf8(session.localPath);

      AppendLog(
          DiagnosticLevel::Warning,
          mTranslations->Format(
              "externalEditor.recoveryCopyLog",
              std::array<std::string_view, 1>{localText}));

      return;
    }

    TransferJob job{
        .id = {},
        .siteId = session.siteId,
        .siteEndpoint = std::nullopt,
        .direction = TransferDirection::Upload,
        .localPath = session.localPath,
        .remotePath = session.remotePath,
        .recursive = false,
        .conflictPolicy = ConflictPolicy::Overwrite,
        .expectedRemoteRevision = session.remoteRevision};

    auto queued = tab->controller->Enqueue(std::move(job));
    if (!queued)
    {
      session.phase = ExternalEditSession::Phase::Failed;
      session.failureReported = true;

      const auto remoteText = session.remotePath.DisplayUtf8();
      const auto localText = GenericUtf8(session.localPath);

      wxString message = TranslatedFormat(
          *mTranslations,
          "externalEditor.uploadFailedMessage",
          remoteText,
          queued.error().message);
      message += "\n\n";
      message += TranslatedFormat(
          *mTranslations, "externalEditor.recoveryCopyMessage", localText);

      LocalizedMessageBox(
          *mTranslations,
          message,
          Translated(*mTranslations, "externalEditor.uploadFailedTitle"),
          wxOK | wxICON_ERROR,
          this);

      AppendLog(
          DiagnosticLevel::Error,
          mTranslations->Format(
              "externalEditor.uploadFailedLog",
              std::array<std::string_view, 2>{remoteText,
                                              queued.error().message}));

      AppendLog(
          DiagnosticLevel::Warning,
          mTranslations->Format(
              "externalEditor.recoveryCopyLog",
              std::array<std::string_view, 1>{localText}));

      return;
    }

    session.uploadJobId = std::move(*queued);
    session.queuedStamp = session.observedStamp;
    session.stableSince.reset();
    session.failureReported = false;
    session.phase = ExternalEditSession::Phase::Uploading;
  }

  bool MainFrame::HasExternalEditSessions(
      const std::optional<std::string_view> connectionId) const
  {
    return std::ranges::any_of(
        mExternalEditSessions,
        [&](const auto &session)
        {
          return (!connectionId || session->connectionId == *connectionId) &&
                 (session->phase == ExternalEditSession::Phase::Watching ||
                  session->phase == ExternalEditSession::Phase::Uploading ||
                  session->phase == ExternalEditSession::Phase::Inspecting);
        });
  }

  void MainFrame::MarkExternalEditDetached(const std::string_view connectionId)
  {
    for (auto &session : mExternalEditSessions)
    {
      if (session->connectionId != connectionId ||
          session->phase == ExternalEditSession::Phase::Detached ||
          session->phase == ExternalEditSession::Phase::Failed)
      {
        continue;
      }

      const bool hasEditedCopy =
          session->phase != ExternalEditSession::Phase::Downloading &&
          session->ReadLocalStamp().has_value();

      session->phase = ExternalEditSession::Phase::Detached;

      if (hasEditedCopy)
      {
        const auto localText = GenericUtf8(session->localPath);

        AppendLog(
            DiagnosticLevel::Warning,
            mTranslations->Format(
                "externalEditor.recoveryCopyLog",
                std::array<std::string_view, 1>{localText}));
      }
    }
  }

  void MainFrame::OnExternalEditTimer(wxTimerEvent &)
  {
    const auto now = std::chrono::steady_clock::now();
    constexpr auto MissingFileGrace = std::chrono::seconds{4};
    constexpr auto SaveDebounce = std::chrono::milliseconds{1200};

    for (auto &owned : mExternalEditSessions)
    {
      auto &session = *owned;

      if (session.phase != ExternalEditSession::Phase::Watching &&
          session.phase != ExternalEditSession::Phase::Uploading &&
          session.phase != ExternalEditSession::Phase::Inspecting)
      {
        continue;
      }

      const auto stamp = session.ReadLocalStamp();
      if (!stamp)
      {
        if (session.phase != ExternalEditSession::Phase::Watching)
        {
          continue;
        }

        if (!session.missingSince)
        {
          session.missingSince = now;

          continue;
        }

        if (now - *session.missingSince < MissingFileGrace)
        {
          continue;
        }

        session.phase = ExternalEditSession::Phase::Failed;

        if (!session.failureReported)
        {
          session.failureReported = true;

          const auto localText = GenericUtf8(session.localPath);
          const std::string errorText{mTranslations->Text(
              "externalEditor.error.editedCopyInvalid")};

          LocalizedMessageBox(
              *mTranslations,
              TranslatedFormat(
                  *mTranslations,
                  "externalEditor.inspectionFailedMessage",
                  localText,
                  errorText),
              Translated(
                  *mTranslations,
                  "externalEditor.inspectionFailedTitle"),
              wxOK | wxICON_WARNING,
              this);

          AppendLog(
              DiagnosticLevel::Error,
              mTranslations->Format(
                  "externalEditor.inspectionFailedLog",
                  std::array<std::string_view, 2>{localText,
                                                  errorText}));
        }

        continue;
      }

      session.missingSince.reset();

      if (!session.observedStamp || *session.observedStamp != *stamp)
      {
        session.observedStamp = *stamp;
        session.stableSince = now;
      }

      if (session.phase == ExternalEditSession::Phase::Watching &&
          session.observedStamp != session.synchronizedStamp &&
          session.stableSince && now - *session.stableSince >= SaveDebounce)
      {
        BeginExternalEditUpload(session);
      }
    }
  }

  void MainFrame::AppendLog(const DiagnosticLevel level, const std::string_view message)
  {
    if (!mMessageLog)
    {
      return;
    }

    const auto now = wxDateTime::Now().FormatISOTime();
    const auto sanitized = SanitizeDiagnosticText(message);
    const auto colour = MessageLogColour(level, mActiveAppearanceTheme);

    (void)mMessageLog->SetDefaultStyle(
        wxTextAttr{wxColour{colour.red, colour.green, colour.blue}});

    mMessageLog->AppendText(now + " [" + FromUtf8(DiagnosticPrefix(*mTranslations, level)) + "] " +
                            FromUtf8(sanitized) + "\n");

    mMessageLogClearButton->Enable(true);
  }

  void MainFrame::OnMessageLogClear(wxCommandEvent &)
  {
    if (!mMessageLog || !mMessageLogClearButton ||
        !mMessageLogClearButton->IsEnabled())
    {
      return;
    }

    mMessageLog->Clear();
    mMessageLog->SetFocus();

    mMessageLogClearButton->Disable();
  }

  void MainFrame::SetQuickSite(ConnectionTab &tab,
                               const SiteProfile &site,
                               const bool navigateToInitialLocalDirectory)
  {
    const bool persistedSite = std::ranges::any_of(
        mConfiguration.sites,
        [&](const SiteProfile &candidate)
        { return candidate.id == site.id; });

    if (persistedSite || site.host.empty() ||
        (tab.quickRuntimeEndpoint &&
         !EndpointIdentitiesEqual(*tab.quickRuntimeEndpoint,
                                  EndpointIdentity(site))))
    {
      tab.quickRuntimeSiteId.clear();
      tab.quickRuntimeEndpoint.reset();
    }

    tab.quickSiteId = site.id;
    tab.quickDraft = site;
    tab.workspaceIdentity =
        WorkspaceIdentityForSite(mConfiguration, site, false);

    const auto endpoint = EndpointIdentity(site);
    const auto *remembered = tab.workspaceIdentity
                                 ? RememberedDirectories(
                                       mConfiguration,
                                       *tab.workspaceIdentity,
                                       endpoint)
                                 : nullptr;

    if (remembered != nullptr)
    {
      tab.quickDraft.initialRemoteDirectory =
          remembered->remoteDirectory;
    }

    ClearSensitiveString(tab.ephemeralPassword);
    ClearSensitiveString(tab.activeCredentialSecret);
    ClearSensitiveString(tab.activePassphraseSecret);

    if (&tab == SelectedConnectionTab())
    {
      ShowQuickConnectDraft(tab);
    }

    UpdateConnectionTabTitle(tab);

    if (!navigateToInitialLocalDirectory)
    {
      return;
    }

    const auto rememberedLocal =
        remembered != nullptr
            ? ConfiguredLocalDirectory(remembered->localDirectory)
            : std::nullopt;

    const auto configuredInitial =
        ConfiguredLocalDirectory(site.initialLocalDirectory);

    const auto localDirectory = rememberedLocal ? rememberedLocal
                                                : configuredInitial;
    if (localDirectory)
    {
      std::optional<std::filesystem::path> fallback;

      if (!tab.localDirectory.empty() &&
          tab.localDirectory != *localDirectory)
      {
        fallback = tab.localDirectory;
      }
      else if (tab.localDirectoryFallback &&
               *tab.localDirectoryFallback != *localDirectory)
      {
        fallback = tab.localDirectoryFallback;
      }
      else if (tab.localDirectoryLoading)
      {
        auto requested = platform::FromToolkitPath(tab.localPath->GetPath());

        if (!requested.empty() && requested != *localDirectory)
        {
          fallback = std::move(requested);
        }
      }

      if (!fallback && rememberedLocal && configuredInitial &&
          *configuredInitial != *localDirectory)
      {
        fallback = configuredInitial;
      }

      PopulateLocalDirectory(tab, *localDirectory, std::move(fallback));
    }
  }

  void MainFrame::RecordSuccessfulQuickConnection(ConnectionTab &tab)
  {
    if (!mConfigurationWritable || !tab.activeConnectionSite ||
        !tab.workspaceIdentity)
    {
      return;
    }

    const auto activeEndpoint = EndpointIdentity(*tab.activeConnectionSite);
    const auto *quickIdentity =
        std::get_if<config::QuickConnectWorkspaceIdentity>(
            &*tab.workspaceIdentity);

    if (quickIdentity == nullptr ||
        !EndpointIdentitiesEqual(quickIdentity->endpoint, activeEndpoint))
    {
      return;
    }

    // Saved profiles may use credentials or authentication settings that an
    // endpoint-only Quick Connect history entry cannot reproduce.
    if (std::ranges::any_of(mConfiguration.sites, [&](const auto &site)
                            { return site.id == tab.activeConnectionSite->id; }) ||
        UniqueSavedSiteForEndpoint(mConfiguration, activeEndpoint) !=
            nullptr)
    {
      return;
    }

    auto previousHistory = mConfiguration.quickConnectHistory;
    auto previousWorkspace = mConfiguration.workspace;
    auto entry = activeEndpoint;

    std::erase_if(mConfiguration.quickConnectHistory, [&](const auto &existing)
                  { return existing.protocol == entry.protocol &&
                           EndpointHostEqual(existing.host, entry.host) &&
                           existing.port == entry.port &&
                           existing.username == entry.username; });

    mConfiguration.quickConnectHistory.insert(
        mConfiguration.quickConnectHistory.begin(), std::move(entry));

    if (mConfiguration.quickConnectHistory.size() >
        config::MaxQuickConnectHistoryEntries)
    {
      mConfiguration.quickConnectHistory.resize(
          config::MaxQuickConnectHistoryEntries);
    }

    // A path remembered for an entry that fell out of the bounded history is
    // history metadata too. Remove it in the same atomic configuration save.
    std::erase_if(
        mConfiguration.workspace.connectionDirectories,
        [&](const auto &remembered)
        {
          const auto *quick =
              std::get_if<config::QuickConnectWorkspaceIdentity>(
                  &remembered.connection);

          return quick != nullptr &&
                 std::ranges::any_of(
                     previousHistory,
                     [&](const auto &previous)
                     {
                       return EndpointIdentitiesEqual(previous,
                                                      quick->endpoint);
                     }) &&
                 !QuickHistoryContains(mConfiguration, quick->endpoint);
        });

    if (mConfiguration.quickConnectHistory == previousHistory &&
        mConfiguration.workspace == previousWorkspace)
    {
      return;
    }

    if (!SaveConfiguration())
    {
      mConfiguration.quickConnectHistory = std::move(previousHistory);
      mConfiguration.workspace = std::move(previousWorkspace);
    }
  }

  void MainFrame::RememberLocalDirectory(
      ConnectionTab &tab,
      const std::filesystem::path &directory)
  {
    RememberConnectionDirectories(tab, &directory, nullptr);
  }

  void MainFrame::RememberRemoteDirectory(ConnectionTab &tab,
                                          const RemotePath &directory)
  {
    RememberConnectionDirectories(tab, nullptr, &directory);
  }

  void MainFrame::RememberConnectionDirectories(
      ConnectionTab &tab,
      const std::filesystem::path *const localDirectory,
      const RemotePath *const remoteDirectory)
  {
    if (!mConfigurationWritable)
    {
      return;
    }

    // Quick Connect controls are shared presentation for the selected tab.
    // Re-read them before attributing a local navigation so an edited endpoint
    // can never write workspace state under the tab's previous identity.
    if (localDirectory != nullptr &&
        tab.connectionState == ConnectionState::Disconnected &&
        &tab == SelectedConnectionTab())
    {
      CaptureQuickConnectDraft(tab);
    }

    auto previousWorkspace = mConfiguration.workspace;
    bool changed = false;
    bool persistConnectionDirectories = tab.workspaceIdentity.has_value();

    if (localDirectory != nullptr && !localDirectory->empty() &&
        mConfiguration.workspace.lastLocalDirectory != *localDirectory)
    {
      mConfiguration.workspace.lastLocalDirectory = *localDirectory;

      changed = true;
    }

    if (tab.workspaceIdentity)
    {
      if (const auto *quick =
              std::get_if<config::QuickConnectWorkspaceIdentity>(
                  &*tab.workspaceIdentity);
          quick != nullptr)
      {
        if (!QuickHistoryContains(mConfiguration, quick->endpoint))
        {
          if (const auto *saved = UniqueSavedSiteForEndpoint(
                  mConfiguration, quick->endpoint))
          {
            tab.workspaceIdentity =
                config::SavedSiteWorkspaceIdentity{saved->id};
          }
          else if (tab.connectionState == ConnectionState::Connecting)
          {
            // Keep the pending identity until ConnectionEvent records the
            // successful Quick Connect history entry. A local loader can
            // legitimately finish first, but the unrecorded endpoint must
            // not enter CSON yet.
            persistConnectionDirectories = false;
          }
          else
          {
            tab.workspaceIdentity.reset();

            persistConnectionDirectories = false;
          }
        }
      }
      else
      {
        // The identity was already known to be the saved-site alternative.
        // Keep it only while its stable UUID still exists.
        const auto *saved =
            std::get_if<config::SavedSiteWorkspaceIdentity>(
                &*tab.workspaceIdentity);

        if (saved == nullptr ||
            std::ranges::none_of(
                mConfiguration.sites,
                [&](const auto &site)
                { return site.id == saved->siteId; }))
        {
          tab.workspaceIdentity.reset();

          persistConnectionDirectories = false;
        }
      }
    }

    // If this endpoint was first used through Quick Connect and was later
    // saved as a site, the stable site UUID becomes authoritative.
    if (tab.workspaceIdentity &&
        persistConnectionDirectories &&
        std::holds_alternative<config::SavedSiteWorkspaceIdentity>(
            *tab.workspaceIdentity))
    {
      const auto endpoint = EndpointIdentity(
          tab.activeConnectionSite ? *tab.activeConnectionSite
                                   : tab.quickDraft);

      auto &directories = mConfiguration.workspace.connectionDirectories;

      const auto saved = std::ranges::find_if(
          directories,
          [&](const auto &remembered)
          {
            return WorkspaceConnectionsEqual(remembered.connection,
                                             *tab.workspaceIdentity);
          });

      auto quick = directories.end();

      const auto *savedIdentity =
          std::get_if<config::SavedSiteWorkspaceIdentity>(
              &*tab.workspaceIdentity);

      const auto *uniqueSaved =
          UniqueSavedSiteForEndpoint(mConfiguration, endpoint);

      if (savedIdentity != nullptr && uniqueSaved != nullptr &&
          savedIdentity->siteId == uniqueSaved->id)
      {
        quick = std::ranges::find_if(
            directories,
            [&](const auto &remembered)
            {
              const auto *identity =
                  std::get_if<config::QuickConnectWorkspaceIdentity>(
                      &remembered.connection);

              return identity != nullptr &&
                     EndpointIdentitiesEqual(identity->endpoint,
                                             endpoint);
            });
      }

      if (quick != directories.end())
      {
        if (saved == directories.end())
        {
          quick->connection = *tab.workspaceIdentity;
        }
        else
        {
          directories.erase(quick);
        }

        changed = true;
      }
    }

    if (tab.workspaceIdentity && persistConnectionDirectories)
    {
      auto &directories = mConfiguration.workspace.connectionDirectories;

      const auto existing = std::ranges::find_if(
          directories,
          [&](const auto &remembered)
          {
            return WorkspaceConnectionsEqual(remembered.connection,
                                             *tab.workspaceIdentity);
          });

      if (existing == directories.end())
      {
        auto rememberedLocal = localDirectory != nullptr
                                   ? *localDirectory
                                   : tab.localDirectory;

        if (rememberedLocal.empty())
        {
          rememberedLocal =
              mConfiguration.workspace.lastLocalDirectory;
        }

        const auto rememberedRemote =
            remoteDirectory != nullptr
                ? *remoteDirectory
                : (tab.remoteViewReady
                       ? tab.remoteDirectory
                       : tab.quickDraft.initialRemoteDirectory);

        if (!rememberedLocal.empty() &&
            !rememberedRemote.DisplayUtf8().empty())
        {
          directories.push_back({
              .connection = *tab.workspaceIdentity,
              .localDirectory = std::move(rememberedLocal),
              .remoteDirectory = rememberedRemote,
          });

          changed = true;
        }
      }
      else
      {
        if (localDirectory != nullptr && !localDirectory->empty() &&
            existing->localDirectory != *localDirectory)
        {
          existing->localDirectory = *localDirectory;

          changed = true;
        }

        if (remoteDirectory != nullptr &&
            !remoteDirectory->DisplayUtf8().empty() &&
            existing->remoteDirectory.DisplayUtf8() !=
                remoteDirectory->DisplayUtf8())
        {
          existing->remoteDirectory = *remoteDirectory;

          changed = true;
        }
      }
    }

    if (changed && !SaveConfiguration())
    {
      mConfiguration.workspace = std::move(previousWorkspace);
    }
  }

  void MainFrame::EraseReleasedCredentials()
  {
    if (!mConfigurationWritable || mCredentialChangeInProgress ||
        mConfiguration.pendingCredentialDeletions.empty())
    {
      return;
    }

    mCredentialChangeInProgress = true;

    const auto cleanupGuard = wxMakeVarSetter(mCredentialChangeInProgress, false);

    std::vector<Authentication> activeAuthentications;

    for (const auto &tab : mConnectionTabs)
    {
      if (tab->activeConnectionSite)
      {
        activeAuthentications.push_back(tab->activeConnectionSite->authentication);
      }
    }

    auto cleanup = platform::EraseReleasedCredentials(
        mConfiguration.pendingCredentialDeletions, mConfiguration.sites,
        activeAuthentications, *mCredentialStore);

    for (const auto &error : cleanup.errors)
    {
      AppendLog(DiagnosticLevel::Warning,
                mTranslations->Format(
                    "authentication.obsoleteRemoveFailedLog",
                    std::array<std::string_view, 1>{error.message}));
    }

    if (cleanup.pendingIds == mConfiguration.pendingCredentialDeletions)
    {
      return;
    }

    auto previousPending = std::exchange(
        mConfiguration.pendingCredentialDeletions, std::move(cleanup.pendingIds));

    // Preserve the workspace snapshot when cleanup runs during shutdown
    if (auto saved = mConfigRepository->Save(mConfiguration); !saved)
    {
      mConfiguration.pendingCredentialDeletions = std::move(previousPending);

      AppendLog(DiagnosticLevel::Error, ConfigErrorMessage(saved.error()));
    }
  }

  std::optional<std::size_t> MainFrame::SelectedLocalIndex(
      const ConnectionTab &tab) const
  {
    const auto selected = tab.localList->SingleSelectedRow();

    return !selected || !selected->sourceIndex ||
                   *selected->sourceIndex >= tab.localEntries.size()
               ? std::nullopt
               : selected->sourceIndex;
  }

  std::optional<std::size_t> MainFrame::SelectedRemoteIndex(
      const ConnectionTab &tab) const
  {
    const auto selected = tab.remoteList->SingleSelectedRow();

    return !selected || !selected->sourceIndex ||
                   *selected->sourceIndex >= tab.remoteEntries.size()
               ? std::nullopt
               : selected->sourceIndex;
  }

  std::vector<std::size_t> MainFrame::SelectedLocalIndices(
      const ConnectionTab &tab) const
  {
    auto selected = tab.localList->SelectedSourceIndices();

    std::erase_if(selected, [&tab](const std::size_t index)
                  { return index >= tab.localEntries.size(); });

    return selected;
  }

  std::vector<std::size_t> MainFrame::SelectedRemoteIndices(
      const ConnectionTab &tab) const
  {
    auto selected = tab.remoteList->SelectedSourceIndices();

    std::erase_if(selected, [&tab](const std::size_t index)
                  { return index >= tab.remoteEntries.size(); });

    return selected;
  }

  std::optional<MainFrame::QueueJobRef> MainFrame::SelectedQueueJob() const
  {
    wxListCtrl *list = mQueuedList;

    const std::vector<QueueJobRef> *ids = &mQueuedJobIds;

    if (mTransferNotebook->GetSelection() == 1)
    {
      list = mFailedList;
      ids = &mFailedJobIds;
    }
    else if (mTransferNotebook->GetSelection() == 2)
    {
      list = mCompletedList;
      ids = &mCompletedJobIds;
    }

    // Transfer-changing commands remain single-item actions. Multi-selection
    // is for copying and must never silently mutate just its first transfer.
    if (list->GetSelectedItemCount() != 1)
    {
      return std::nullopt;
    }

    const long selected = list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);

    return selected < 0 || static_cast<std::size_t>(selected) >= ids->size()
               ? std::nullopt
               : std::optional<QueueJobRef>{
                     ids->at(static_cast<std::size_t>(selected))};
  }

  wxListCtrl *MainFrame::ActiveTransferList() const
  {
    return mTransferNotebook
               ? dynamic_cast<wxListCtrl *>(mTransferNotebook->GetCurrentPage())
               : nullptr;
  }

  std::string MainFrame::TransferListText(
      const wxListCtrl &list, const bool allRows,
      const TransferTextFormat format) const
  {
    if (list.GetItemCount() == 0 ||
        (!allRows && list.GetSelectedItemCount() == 0))
    {
      return {};
    }

    auto columns = list.GetColumnsOrder();

    if (columns.size() != static_cast<std::size_t>(list.GetColumnCount()))
    {
      columns.clear();

      for (int column = 0; column < list.GetColumnCount(); ++column)
      {
        columns.Add(column);
      }
    }

    std::vector<std::string> headers;

    for (const int column : columns)
    {
      wxListItem header;
      header.SetMask(wxLIST_MASK_TEXT);

      if (!list.GetColumn(column, header))
      {
        return {};
      }

      headers.push_back(ToUtf8(header.GetText()));
    }

    std::vector<TransferClipboardRow> rows;

    for (long row = 0; row < list.GetItemCount(); ++row)
    {
      if (!allRows &&
          (list.GetItemState(row, wxLIST_STATE_SELECTED) & wxLIST_STATE_SELECTED) == 0)
      {
        continue;
      }

      TransferClipboardRow cells;
      cells.reserve(headers.size());

      for (const int column : columns)
      {
        // GetItemText returns the full value, not the clipped on-screen label
        cells.push_back(ToUtf8(list.GetItemText(row, column)));
      }

      rows.push_back(std::move(cells));
    }

    return format == TransferTextFormat::Csv
               ? FormatTransferCsvText(headers, rows)
               : FormatTransferClipboardText(headers, rows);
  }

  void MainFrame::CopyTransferText(const std::string &text)
  {
    if (text.empty())
    {
      return;
    }

    const bool copied = [&text]
    {
      // Close the clipboard before reporting failure. wx's own clipboard
      // error dialog must not duplicate our localized message.
      wxLogNull suppressClipboardErrors;
      wxClipboardLocker clipboard;

      if (!clipboard)
      {
        return false;
      }

      auto data = std::make_unique<wxTextDataObject>(FromUtf8(text));

      if (!wxTheClipboard->SetData(data.get()))
      {
        return false;
      }

      // The MSW clipboard takes ownership only after SetData succeeds
      (void)data.release();

      return wxTheClipboard->Flush();
    }();

    if (!copied)
    {
      AppendLog(DiagnosticLevel::Warning,
                mTranslations->Text("queue.copyFailedMessage"));

      LocalizedMessageBox(*mTranslations,
                          Translated(*mTranslations, "queue.copyFailedMessage"),
                          Translated(*mTranslations, "queue.copyFailedTitle"),
                          wxOK | wxICON_WARNING,
                          this);
    }
  }

  Result<std::string> MainFrame::RequestCredential(
      ConnectionTab &tab,
      const CredentialRequest &request)
  {
    if (!tab.activeConnectionSite ||
        request.siteId != tab.activeConnectionSiteId)
    {
      return std::unexpected(
          UiError(RemoteErrorCode::Cancelled,
                  std::string{
                      mTranslations->Text("authentication.inactiveRequest")}));
    }

    if (!request.forcePrompt &&
        request.kind == CredentialKind::Password &&
        !tab.ephemeralPassword.empty())
    {
      return tab.ephemeralPassword;
    }

    std::string *activeSecret{};

    if (request.kind == CredentialKind::Password)
    {
      activeSecret = &tab.activeCredentialSecret;
    }
    else if (request.kind == CredentialKind::PrivateKeyPassphrase)
    {
      activeSecret = &tab.activePassphraseSecret;
    }

    if (request.forcePrompt &&
        request.kind == CredentialKind::PrivateKeyPassphrase)
    {
      ClearSensitiveString(tab.activePassphraseSecret);
    }

    if (!request.forcePrompt &&
        activeSecret != nullptr && !activeSecret->empty())
    {
      return *activeSecret;
    }

    const auto &authentication = tab.activeConnectionSite->authentication;
    const auto credentialId = CredentialIdForRequest(authentication, request.kind);
    if (!request.forcePrompt && !credentialId.empty())
    {
      auto credential = mCredentialStore->Load(credentialId);
      if (credential)
      {
        if (activeSecret != nullptr)
        {
          *activeSecret = BytesToString(credential->Secret());

          return *activeSecret;
        }

        return BytesToString(credential->Secret());
      }

      if (credential.error().code != platform::PlatformErrorCode::NotFound)
      {
        return std::unexpected(UiError(RemoteErrorCode::CredentialUnavailable,
                                       credential.error().message));
      }
    }

    if (mPromptActive)
    {
      AppendLog(DiagnosticLevel::Warning,
                mTranslations->Text("connection.promptBusyLog"));

      return std::unexpected(UiError(
          RemoteErrorCode::Cancelled,
          std::string{mTranslations->Text("connection.promptBusy")}));
    }

    const int pageIndex = mConnectionNotebook->GetPageIndex(tab.panel);
    if (pageIndex != wxNOT_FOUND)
    {
      mConnectionNotebook->SetSelection(
          static_cast<std::size_t>(pageIndex));
    }

    mPromptActive = true;

    struct PromptGuard final
    {
      bool &active;
      ~PromptGuard() { active = false; }
    } promptGuard{mPromptActive};

    wxTextEntryDialog dialog(this,
                             request.prompt.empty()
                                 ? Translated(*mTranslations, "authentication.enterCredential")
                                 : FromUtf8(request.prompt),
                             Translated(*mTranslations, "authentication.requiredTitle"),
                             {},
                             request.echo ? wxOK | wxCANCEL : wxOK | wxCANCEL | wxTE_PASSWORD);

    LocalizeStandardButtons(dialog, *mTranslations);

    dialog.CentreOnParent();

    if (dialog.ShowModal() != wxID_OK)
    {
      return std::unexpected(
          UiError(RemoteErrorCode::Cancelled,
                  std::string{
                      mTranslations->Text("authentication.promptCanceled")}));
    }

    if (activeSecret != nullptr)
    {
      *activeSecret = ToUtf8(dialog.GetValue());

      return *activeSecret;
    }

    return ToUtf8(dialog.GetValue());
  }

  Result<TrustDecision> MainFrame::VerifyTrust(
      ConnectionTab &tab,
      const TrustChallenge &challenge)
  {
    if (challenge.siteId != tab.activeConnectionSiteId)
    {
      return TrustDecision::Reject;
    }

    if (challenge.kind == TrustKind::TlsCertificate)
    {
      const auto trusted = std::ranges::find_if(mConfiguration.tlsTrust, [&](const auto &pin)
                                                { return EndpointHostEqual(pin.host, challenge.host) && pin.port == challenge.port &&
                                                         CurlTlsPin(pin.publicKeyPin) == CurlTlsPin(challenge.sha256Fingerprint); });

      if (trusted != mConfiguration.tlsTrust.end())
      {
        return TrustDecision::AcceptOnce;
      }
    }

    if (mPromptActive)
    {
      AppendLog(DiagnosticLevel::Warning,
                mTranslations->Text("connection.promptBusyLog"));

      return TrustDecision::Reject;
    }

    const int pageIndex = mConnectionNotebook->GetPageIndex(tab.panel);

    if (pageIndex != wxNOT_FOUND)
    {
      mConnectionNotebook->SetSelection(
          static_cast<std::size_t>(pageIndex));
    }

    mPromptActive = true;

    struct PromptGuard final
    {
      bool &active;
      ~PromptGuard() { active = false; }
    } promptGuard{mPromptActive};

    const auto storeTlsPin = [&]()
    {
      if (!mConfigurationWritable)
      {
        LocalizedMessageBox(*mTranslations, Translated(*mTranslations, "trust.saveDisabledMessage"),
                            Translated(*mTranslations, "trust.saveDisabledTitle"),
                            wxOK | wxICON_WARNING,
                            this);

        return false;
      }

      auto previousTrust = mConfiguration.tlsTrust;

      std::erase_if(mConfiguration.tlsTrust, [&](const auto &pin)
                    { return EndpointHostEqual(pin.host, challenge.host) && pin.port == challenge.port; });

      mConfiguration.tlsTrust.push_back(
          {challenge.host, challenge.port, CurlTlsPin(challenge.sha256Fingerprint)});

      if (SaveConfiguration())
      {
        return true;
      }

      mConfiguration.tlsTrust = std::move(previousTrust);

      return false;
    };

    const auto description = mTranslations->Format(
        "trust.description",
        std::array<std::string_view, 5>{
            challenge.host,
            std::to_string(challenge.port),
            challenge.algorithm,
            challenge.sha256Fingerprint,
            challenge.diagnostic});

    if (challenge.status == TrustStatus::Changed)
    {
      if (LocalizedMessageBox(*mTranslations, TranslatedFormat(*mTranslations, "trust.changedMessage", description),
                              Translated(*mTranslations, "trust.changedTitle"),
                              wxYES_NO | wxNO_DEFAULT | wxICON_ERROR,
                              this) != wxID_YES)
      {
        return TrustDecision::Reject;
      }

      if (challenge.kind == TrustKind::TlsCertificate)
      {
        if (!storeTlsPin())
        {
          return TrustDecision::Reject;
        }
      }

      return TrustDecision::ReplaceStored;
    }

    wxMessageDialog dialog(this,
                           TranslatedFormat(*mTranslations,
                                            "trust.verifyMessage",
                                            description),
                           challenge.kind == TrustKind::SshHostKey
                               ? Translated(*mTranslations, "trust.unknownSshTitle")
                               : Translated(*mTranslations, "trust.untrustedTlsTitle"),
                           wxYES_NO | wxCANCEL | wxCANCEL_DEFAULT | wxICON_WARNING);

    dialog.SetYesNoCancelLabels(
        Translated(*mTranslations, "trust.permanent"),
        Translated(*mTranslations, "trust.once"),
        Translated(*mTranslations, "trust.reject"));

    dialog.CentreOnParent();

    const int answer = dialog.ShowModal();

    if (answer == wxID_CANCEL)
    {
      return TrustDecision::Reject;
    }

    if (answer == wxID_NO)
    {
      return TrustDecision::AcceptOnce;
    }

    if (challenge.kind == TrustKind::TlsCertificate)
    {
      if (!storeTlsPin())
      {
        return TrustDecision::Reject;
      }
    }

    return TrustDecision::AcceptPermanently;
  }

  Result<ConflictResolution> MainFrame::ResolveConflict(
      ConnectionTab &tab,
      const ConflictChallenge &challenge)
  {
    if (mPromptActive)
    {
      AppendLog(DiagnosticLevel::Warning,
                mTranslations->Text("connection.promptBusyLog"));

      return std::unexpected(UiError(
          RemoteErrorCode::Cancelled,
          std::string{mTranslations->Text("connection.promptBusy")}));
    }

    const int pageIndex = mConnectionNotebook->GetPageIndex(tab.panel);
    if (pageIndex != wxNOT_FOUND)
    {
      mConnectionNotebook->SetSelection(
          static_cast<std::size_t>(pageIndex));
    }

    mPromptActive = true;

    struct PromptGuard final
    {
      bool &active;
      ~PromptGuard() { active = false; }
    } promptGuard{mPromptActive};

    ConflictDialog dialog(this, challenge, *mTranslations);

    if (dialog.ShowModal() != wxID_OK)
    {
      return std::unexpected(
          UiError(RemoteErrorCode::Cancelled,
                  std::string{mTranslations->Text("conflict.canceled")}));
    }

    return dialog.Resolution();
  }

  void MainFrame::OnTransferFailureTimer(wxTimerEvent &)
  {
    if (mPendingTransferFailures.empty())
    {
      mTransferFailureTimer->Stop();

      return;
    }

    // The modal hook also tracks native message/file dialogs, which need not
    // appear as an ordinary wxDialog in the top-level window list.
    if (mTransferFailureDialogActive || mPromptActive || !IsEnabled() ||
        IsBeingDeleted() || wxModalDialogHook::GetOpenCount() != 0)
    {
      return;
    }

    // Drain a batch before showing a modal: its nested event loop can deliver
    // further failures, which belong to the next batch, never another dialog
    // on top of this one. Resolve ids afresh so retries/removals/closed tabs
    // cannot leave stale errors or window pointers in the notification.
    const auto pending = std::exchange(mPendingTransferFailures, {});

    mTransferFailureTimer->Stop();

    wxString message;

    std::size_t count{};

    constexpr std::size_t maxDetails = 5;

    for (const auto &failure : pending)
    {
      const auto *tab = FindConnectionTab(failure.job.connectionId);
      if (!tab)
      {
        continue;
      }

      const auto transfer = std::ranges::find(
          tab->transfers, failure.job.jobId,
          [](const QueuedTransfer &item)
          { return item.job.id; });

      if (transfer == tab->transfers.end() ||
          transfer->state != TransferState::Failed ||
          transfer->attempt != failure.attempt)
      {
        continue;
      }

      if (++count > maxDetails)
      {
        continue;
      }

      const auto page = mConnectionNotebook->GetPageIndex(tab->panel);
      const auto label = page == wxNOT_FOUND
                             ? std::string{mTranslations->Text("connection.tab.closed")}
                             : ToUtf8(mConnectionNotebook->GetPageText(
                                   static_cast<std::size_t>(page)));

      if (!message.empty())
      {
        message += "\n\n";
      }

      message += TranslatedFormat(
          *mTranslations, "transfer.failureItem",
          SanitizeDiagnosticText(label),
          mTranslations->Text(transfer->job.direction == TransferDirection::Upload
                                  ? "transfer.direction.upload"
                                  : "transfer.direction.download"),
          SanitizeDiagnosticText(transfer->job.remotePath.DisplayUtf8()),
          SanitizeDiagnosticText(transfer->error
                                     ? transfer->error->message
                                     : std::string{mTranslations->Text("transfer.state.failed")}));
    }

    if (count == 0U)
    {
      return;
    }

    if (count > maxDetails)
    {
      message += "\n\n";
      message += TranslatedFormat(*mTranslations, "transfer.failuresMore",
                                  count - maxDetails);
    }

    message += "\n\n";
    message += Translated(*mTranslations, "transfer.failureRetry");

    mTransferFailureDialogActive = true;

    struct DialogGuard final
    {
      bool &active;
      ~DialogGuard() { active = false; }
    } guard{mTransferFailureDialogActive};

    LocalizedMessageBox(
        *mTranslations, message,
        Translated(*mTranslations, count == 1U ? "transfer.failedTitle"
                                               : "transfer.failuresTitle"),
        wxOK | wxICON_ERROR, this);
  }

  void MainFrame::OnControllerEvent(wxThreadEvent &event)
  {
    const auto payload = event.GetPayload<ControllerEventEnvelopePtr>();
    if (!payload)
    {
      return;
    }

    auto *const tab = FindConnectionTab(payload->connectionId);
    if (!tab)
    {
      return;
    }

    const auto connectionLabel = [&]()
    {
      const int index = mConnectionNotebook->GetPageIndex(tab->panel);

      return index == wxNOT_FOUND
                 ? Translated(*mTranslations, "connection.tab.closed")
                 : mConnectionNotebook->GetPageText(
                       static_cast<std::size_t>(index));
    };

    const auto appendConnectionLog =
        [this, &connectionLabel](const DiagnosticLevel level,
                                 const std::string_view message)
    {
      wxString prefix{"["};
      prefix += connectionLabel();
      prefix += "] ";

      AppendLog(level,
                ToUtf8(prefix) + std::string{message});
    };

    std::visit(
        [this, tab, &appendConnectionLog](const auto &value)
        {
          using Event = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Event, ConnectionAttemptEvent>)
          {
            if (tab->activeConnectionSiteId.empty() ||
                tab->activeConnectionGeneration == 0 ||
                value.siteId != tab->activeConnectionSiteId ||
                value.generation != tab->activeConnectionGeneration)
            {
              return;
            }

            auto host = value.endpoint.host;
            if (host.find(':') != std::string::npos)
            {
              host = '[' + host + ']';
            }

            const auto port = std::to_string(value.endpoint.port);
            const auto protocol = ToUtf8(Translated(
                *mTranslations, ProtocolTranslationKey(value.endpoint.protocol)));

            if (value.kind == ConnectionAttemptKind::Browser)
            {
              appendConnectionLog(
                  DiagnosticLevel::Information,
                  mTranslations->Format(
                      "connection.connectingLog",
                      std::array<std::string_view, 3>{host, port, protocol}));
            }
            else
            {
              const auto worker = std::to_string(value.workerNumber);
              const auto attempt = std::to_string(value.transferAttempt);

              appendConnectionLog(
                  DiagnosticLevel::Information,
                  mTranslations->Format(
                      "connection.transferConnectingLog",
                      std::array<std::string_view, 5>{
                          worker, host, port, protocol, attempt}));
            }
          }
          else if constexpr (std::is_same_v<Event, ConnectionEvent>)
          {
            if (tab->activeConnectionSiteId.empty() ||
                tab->activeConnectionGeneration == 0 ||
                value.siteId != tab->activeConnectionSiteId ||
                value.generation != tab->activeConnectionGeneration)
            {
              return;
            }

            std::string_view displayMessage = value.message;

            if (value.connected)
            {
              displayMessage = mTranslations->Text("status.connected");
            }
            else if (value.message == "Connection lost")
            {
              displayMessage = mTranslations->Text("status.connectionLost");
            }
            else if (value.message == "Disconnected")
            {
              displayMessage = mTranslations->Text("status.disconnected");
            }

            appendConnectionLog(
                value.connected || value.message == "Disconnected"
                    ? DiagnosticLevel::Information
                    : DiagnosticLevel::Warning,
                displayMessage);

            if (value.connected)
            {
              SetConnectionState(*tab, ConnectionState::Connected);

              RecordSuccessfulQuickConnection(*tab);

              if (!tab->localDirectory.empty())
              {
                RememberLocalDirectory(*tab, tab->localDirectory);
              }
            }
            else
            {
              MarkExternalEditDetached(tab->connectionId);

              SetConnectionState(*tab, ConnectionState::Disconnected);

              tab->activeConnectionSite.reset();
              tab->activeConnectionSiteId.clear();
              tab->activeConnectionGeneration = 0;

              ClearSensitiveString(tab->ephemeralPassword);
              ClearSensitiveString(tab->activeCredentialSecret);
              ClearSensitiveString(tab->activePassphraseSecret);

              EraseReleasedCredentials();
            }
          }
          else if constexpr (std::is_same_v<Event, DirectoryEvent>)
          {
            if (tab->activeConnectionSiteId.empty() ||
                tab->activeConnectionGeneration == 0 ||
                value.siteId != tab->activeConnectionSiteId ||
                value.generation != tab->activeConnectionGeneration ||
                tab->connectionState != ConnectionState::Connected)
            {
              return;
            }

            if (value.preferredSelectionIdentity)
            {
              tab->preferredRemoteSelectionIdentity =
                  *value.preferredSelectionIdentity;
            }

            PopulateRemoteDirectory(*tab, value);

            tab->remoteViewReady = true;

            RememberRemoteDirectory(*tab, value.directory);

            if (tab == SelectedConnectionTab())
            {
              UpdateFileActionState();
            }
          }
          else if constexpr (std::is_same_v<Event, RemoteInspectionEvent>)
          {
            const auto found = std::ranges::find_if(
                mExternalEditSessions,
                [&](const auto &session)
                {
                  return session->connectionId == tab->connectionId &&
                         session->inspectionRequestId &&
                         *session->inspectionRequestId == value.requestId;
                });

            if (found == mExternalEditSessions.end())
            {
              return;
            }

            auto &session = **found;

            if (value.siteId != session.siteId ||
                value.generation != session.generation ||
                session.phase != ExternalEditSession::Phase::Inspecting)
            {
              return;
            }

            if (value.result &&
                value.result->kind == RemoteEntryKind::File &&
                session.queuedStamp &&
                value.result->size == session.queuedStamp->size)
            {
              session.remoteRevision =
                  MakeRemoteFileRevision(*value.result);
              session.synchronizedStamp = session.queuedStamp;
              session.queuedStamp.reset();
              session.uploadJobId.reset();
              session.inspectionRequestId.reset();
              session.phase = ExternalEditSession::Phase::Watching;
              session.failureReported = false;

              const auto remoteText = session.remotePath.DisplayUtf8();

              AppendLog(
                  DiagnosticLevel::Information,
                  mTranslations->Format(
                      "externalEditor.uploadedLog",
                      std::array<std::string_view, 1>{remoteText}));
            }
            else
            {
              session.phase = ExternalEditSession::Phase::Failed;
              session.inspectionRequestId.reset();

              const std::string errorText = value.result
                                                ? std::string{mTranslations->Text(
                                                      "externalEditor.error.uploadedCopyMismatch")}
                                                : value.result.error().message;

              if (!session.failureReported)
              {
                session.failureReported = true;

                const auto remoteText = session.remotePath.DisplayUtf8();
                const auto localText = GenericUtf8(session.localPath);

                wxString message = TranslatedFormat(
                    *mTranslations,
                    "externalEditor.verificationFailedMessage",
                    remoteText,
                    errorText);

                message += "\n\n";
                message += TranslatedFormat(
                    *mTranslations,
                    "externalEditor.recoveryCopyMessage",
                    localText);

                LocalizedMessageBox(
                    *mTranslations,
                    message,
                    Translated(
                        *mTranslations,
                        "externalEditor.verificationFailedTitle"),
                    wxOK | wxICON_ERROR,
                    this);

                AppendLog(
                    DiagnosticLevel::Error,
                    mTranslations->Format(
                        "externalEditor.verificationFailedLog",
                        std::array<std::string_view, 2>{remoteText,
                                                        errorText}));

                AppendLog(
                    DiagnosticLevel::Warning,
                    mTranslations->Format(
                        "externalEditor.recoveryCopyLog",
                        std::array<std::string_view, 1>{localText}));
              }
            }
          }
          else if constexpr (std::is_same_v<Event, havremote::ui::QueueEvent>)
          {
            for (const auto &transfer : value.transfers)
            {
              // Active editor sessions report both directions through their
              // recovery dialogs. Detached jobs use normal queue reporting.
              const bool externalEditorTransfer = std::ranges::any_of(
                  mExternalEditSessions,
                  [&](const auto &session)
                  {
                    return session->connectionId == tab->connectionId &&
                           session->phase != ExternalEditSession::Phase::Detached &&
                           (session->uploadJobId == transfer.job.id ||
                            session->downloadJobId == transfer.job.id);
                  });

              if (!externalEditorTransfer &&
                  IsNewlyFailedTransfer(tab->transfers, transfer))
              {
                mPendingTransferFailures.push_back(
                    {{tab->connectionId, transfer.job.id}, transfer.attempt});

                appendConnectionLog(
                    DiagnosticLevel::Error,
                    std::string{mTranslations->Text(
                        transfer.job.direction == TransferDirection::Upload
                            ? "transfer.direction.upload"
                            : "transfer.direction.download")} +
                        ": " +
                        transfer.job.remotePath.DisplayUtf8() + ": " +
                        (transfer.error ? transfer.error->message
                                        : std::string{mTranslations->Text("transfer.state.failed")}));
              }
            }

            const auto isExternalEditorDownload =
                [this, tab](const std::string_view jobId)
            {
              return std::ranges::any_of(
                  mExternalEditSessions,
                  [&](const auto &session)
                  {
                    return session->connectionId == tab->connectionId &&
                           session->downloadJobId == jobId;
                  });
            };

            const bool refreshLocalDirectory = std::ranges::any_of(
                value.transfers,
                [&](const QueuedTransfer &transfer)
                {
                  return transfer.job.direction ==
                             TransferDirection::Download &&
                         !isExternalEditorDownload(transfer.job.id) &&
                         IsNewlyCompletedTransfer(tab->transfers, transfer);
                });

            tab->transfers = value.transfers;

            HandleExternalEditTransfers(*tab, value.transfers);

            PopulateTransferLists();

            if (value.durableChange)
            {
              (void)PersistTransferQueue();
            }

            if (refreshLocalDirectory)
            {
              RequestLocalDirectoryRefresh(*tab);
            }

            if (!mPendingTransferFailures.empty() &&
                !mTransferFailureTimer->IsRunning())
            {
              // Coalesce independent workers and multi-file selections. Keep
              // ticking while another modal owns the UI instead of covering it.
              mTransferFailureTimer->Start(500);
            }
          }
          else if constexpr (std::is_same_v<Event, OperationErrorEvent>)
          {
            if (!value.siteId.empty() &&
                (value.siteId != tab->activeConnectionSiteId ||
                 value.generation != tab->activeConnectionGeneration))
            {
              return;
            }

            if (value.operation == "Rename" ||
                value.operation == "Create file")
            {
              tab->preferredRemoteSelectionIdentity.reset();
            }

            const auto operation =
                TranslatedOperation(*mTranslations, value.operation);

            appendConnectionLog(
                DiagnosticLevel::Error,
                std::string{operation} + ": " + value.error.message);

            if (tab == SelectedConnectionTab())
            {
              LocalizedMessageBox(*mTranslations,
                                  FromUtf8(value.error.message),
                                  FromUtf8(operation),
                                  wxOK | wxICON_ERROR,
                                  this);
            }
          }
          else if constexpr (std::is_same_v<Event, DiagnosticEvent>)
          {
            appendConnectionLog(value.level, value.message);
          }
        },
        payload->event);
  }

  void MainFrame::OnQuickConnectButton(wxCommandEvent &event)
  {
    auto *const tab = SelectedConnectionTab();

    if (!tab)
    {
      return;
    }

    if (tab->connectionState == ConnectionState::Connected)
    {
      OnDisconnect(event);

      return;
    }

    if (tab->connectionState == ConnectionState::Disconnected)
    {
      OnQuickConnect(event);
    }
  }

  void MainFrame::OnQuickConnect(wxCommandEvent &)
  {
    auto *const tab = SelectedConnectionTab();

    if (!tab || tab->connectionState != ConnectionState::Disconnected)
    {
      return;
    }

    CaptureQuickConnectDraft(*tab);

    const auto host = ToUtf8(mQuickHost->GetValue().Trim(true).Trim(false));
    const auto protocol = ProtocolAt(mQuickProtocol->GetSelection());
    const auto port = static_cast<std::uint16_t>(mQuickPort->GetValue());
    const auto username = ToUtf8(mQuickUsername->GetValue());

    const auto requestedEndpoint = SiteEndpointIdentity{
        .protocol = protocol,
        .host = host,
        .port = port,
        .username = username,
    };

    auto runtimeSiteId = tab->quickRuntimeSiteId;

    if (!tab->quickRuntimeEndpoint || runtimeSiteId.empty() ||
        !EndpointIdentitiesEqual(*tab->quickRuntimeEndpoint,
                                 requestedEndpoint))
    {
      runtimeSiteId = GenerateId();
    }

    auto resolution = ResolveConnectionProfile(
        mConfiguration.sites,
        tab->quickSiteId,
        requestedEndpoint,
        runtimeSiteId,
        tab->localDirectory);

    if (resolution.savedSite)
    {
      tab->quickRuntimeSiteId.clear();
      tab->quickRuntimeEndpoint.reset();
    }
    else
    {
      tab->quickRuntimeSiteId = runtimeSiteId;
      tab->quickRuntimeEndpoint = requestedEndpoint;
    }

    (void)BeginConnection(*tab, std::move(resolution.site));
  }

  bool MainFrame::BeginConnection(ConnectionTab &tab, SiteProfile selected)
  {
    if (tab.connectionState != ConnectionState::Disconnected)
    {
      return false;
    }

    if (selected.host.empty())
    {
      LocalizedMessageBox(*mTranslations, Translated(*mTranslations, "quick.hostRequired"),
                          Translated(*mTranslations, "quick.title"),
                          wxOK | wxICON_WARNING,
                          this);

      return false;
    }

    if (!IsValidEndpointHost(selected.host))
    {
      LocalizedMessageBox(*mTranslations, Translated(*mTranslations, "quick.hostInvalid"),
                          Translated(*mTranslations, "quick.title"),
                          wxOK | wxICON_WARNING,
                          this);

      return false;
    }

    if (selected.protocol == ProtocolKind::Ftp &&
        LocalizedMessageBox(*mTranslations, Translated(*mTranslations, "quick.insecureMessage"),
                            Translated(*mTranslations, "quick.insecureTitle"),
                            wxOK | wxCANCEL | wxCANCEL_DEFAULT | wxICON_WARNING,
                            this) != wxID_OK)
    {
      return false;
    }

    tab.quickDraft = selected;

    const auto selectedEndpoint = EndpointIdentity(selected);

    tab.workspaceIdentity =
        WorkspaceIdentityForSite(mConfiguration, selected, true);

    if (tab.workspaceIdentity)
    {
      if (const auto *remembered = RememberedDirectories(
              mConfiguration,
              *tab.workspaceIdentity,
              selectedEndpoint))
      {
        selected.initialRemoteDirectory = remembered->remoteDirectory;

        if (const auto local = ConfiguredLocalDirectory(
                remembered->localDirectory))
        {
          selected.initialLocalDirectory = *local;

          if (tab.localDirectory != *local)
          {
            PopulateLocalDirectory(
                tab,
                *local,
                tab.localDirectory.empty()
                    ? std::nullopt
                    : std::optional<std::filesystem::path>{
                          tab.localDirectory});
          }
        }
      }
    }

    tab.quickDraft = selected;

    ClearSensitiveString(tab.activeCredentialSecret);
    ClearSensitiveString(tab.activePassphraseSecret);

    if (&tab == SelectedConnectionTab())
    {
      mQuickPassword->Clear();
    }

    tab.activeConnectionSiteId = selected.id;
    tab.activeConnectionSite = selected;
    tab.activeConnectionGeneration = 0;

    SetConnectionState(tab, ConnectionState::Connecting);

    tab.activeConnectionGeneration =
        tab.controller->Connect(std::move(selected));

    return true;
  }

  void MainFrame::OnQuickClear(wxCommandEvent &)
  {
    auto *const tab = SelectedConnectionTab();

    if (!tab || tab->connectionState != ConnectionState::Disconnected)
    {
      return;
    }

    SetQuickSite(*tab, SiteProfile{});

    mQuickHost->SetFocus();
  }

  void MainFrame::OnQuickHistory(wxCommandEvent &)
  {
    wxMenu menu;

    if (mConfiguration.quickConnectHistory.empty())
    {
      auto *empty = menu.Append(wxID_ANY, Translated(*mTranslations, "quick.historyEmpty"));

      empty->Enable(false);
    }
    else
    {
      for (std::size_t index = 0; index < mConfiguration.quickConnectHistory.size(); ++index)
      {
        menu.Append(
            IdQuickHistoryEntryBase + static_cast<int>(index),
            QuickHistoryLabel(mConfiguration.quickConnectHistory[index],
                              *mTranslations));
      }
    }

    menu.AppendSeparator();

    auto *clear = menu.Append(
        IdQuickHistoryClear,
        Translated(*mTranslations, "quick.clearHistory"));

    clear->SetBitmap(EmbeddedButtonIcon("HAVREMOTE_CLEAR_HISTORY_ICON"));
    clear->Enable(!mConfiguration.quickConnectHistory.empty() ||
                  HasQuickWorkspaceDirectories(mConfiguration));

    if (auto *button = wxWindow::FindWindowById(IdQuickHistory, this))
    {
      auto position = button->GetPosition();
      position.y += button->GetSize().y;

      PopupMenu(&menu, position);
    }
    else
    {
      PopupMenu(&menu);
    }
  }

  void MainFrame::OnQuickHistorySelection(wxCommandEvent &event)
  {
    const auto index = event.GetId() - IdQuickHistoryEntryBase;

    if (index < 0 ||
        static_cast<std::size_t>(index) >= mConfiguration.quickConnectHistory.size())
    {
      return;
    }

    const auto entry =
        mConfiguration.quickConnectHistory[static_cast<std::size_t>(index)];

    auto *tab = SelectedConnectionTab();

    if (!tab || tab->connectionState != ConnectionState::Disconnected)
    {
      tab = &CreateConnectionTab();
    }

    SiteProfile draft;
    draft.protocol = entry.protocol;
    draft.host = entry.host;
    draft.port = entry.port;
    draft.username = entry.username;
    draft.name = entry.host;

    SetQuickSite(*tab, draft);

    mQuickPassword->SetFocus();
  }

  void MainFrame::PopulateSavedSitesMenu(wxMenu &menu)
  {
    if (mConfiguration.sites.empty() && mConfiguration.siteFolders.empty())
    {
      auto *const empty = menu.Append(
          wxID_ANY,
          Translated(*mTranslations, "quick.savedSitesEmpty"));

      empty->Enable(false);

      return;
    }

    const auto appendSite = [this](wxMenu &target, const SiteProfile &site)
    {
      const auto siteId = site.id;

      auto *const item = target.Append(
          wxID_ANY,
          SavedSiteMenuLabel(site));

      target.Bind(
          wxEVT_MENU,
          [this, siteId](wxCommandEvent &)
          { ConnectSavedSite(siteId); },
          item->GetId());
    };

    const auto appendChildren =
        [this, &appendSite](auto &&self,
                            wxMenu &target,
                            const std::string_view parentFolderId) -> void
    {
      for (const auto &entry : OrderedSiteManagerChildren(
               mConfiguration.sites,
               mConfiguration.siteFolders,
               mConfiguration.siteManagerOrder,
               parentFolderId))
      {
        if (entry.kind == SiteManagerEntryKind::Site)
        {
          const auto site = std::ranges::find(
              mConfiguration.sites, entry.id, &SiteProfile::id);

          if (site != mConfiguration.sites.end())
          {
            appendSite(target, *site);
          }

          continue;
        }

        const auto folder = std::ranges::find(
            mConfiguration.siteFolders, entry.id,
            &config::SiteFolder::id);

        if (folder == mConfiguration.siteFolders.end())
        {
          continue;
        }

        auto *submenu = new wxMenu;

        self(self, *submenu, folder->id);

        if (submenu->GetMenuItemCount() == 0U)
        {
          auto *const empty = submenu->Append(
              wxID_ANY,
              Translated(*mTranslations, "quick.savedSitesEmpty"));

          empty->Enable(false);
        }

        target.AppendSubMenu(submenu, EscapedMenuLabel(folder->name));
      }
    };

    appendChildren(appendChildren, menu, {});
  }

  void MainFrame::ConnectSavedSite(const std::string_view siteId)
  {
    const auto selected = std::ranges::find(
        mConfiguration.sites, siteId, &SiteProfile::id);

    if (selected == mConfiguration.sites.end())
    {
      return;
    }

    auto *tab = SelectedConnectionTab();
    if (tab && tab->connectionState == ConnectionState::Disconnected)
    {
      CaptureQuickConnectDraft(*tab);
    }

    const bool blankDraft =
        tab && tab->connectionState == ConnectionState::Disconnected &&
        tab->quickSiteId.empty() && tab->quickDraft.name.empty() &&
        tab->quickDraft.host.empty() && tab->quickDraft.username.empty() &&
        tab->ephemeralPassword.empty() &&
        tab->quickDraft.protocol == ProtocolKind::Sftp &&
        tab->quickDraft.port == DefaultPort(ProtocolKind::Sftp);

    // A blank selected tab opens a new connection even if this site is already open
    if (!blankDraft)
    {
      const auto matchingTab = [&](const ConnectionTab &candidate,
                                   const ConnectionState state)
      {
        if (candidate.connectionState != state)
        {
          return false;
        }

        if (state == ConnectionState::Disconnected)
        {
          return ConnectionTabMatchesSavedSite(
              candidate.quickSiteId,
              EndpointIdentity(candidate.quickDraft),
              *selected);
        }

        return candidate.activeConnectionSite &&
               ConnectionTabMatchesSavedSite(
                   candidate.activeConnectionSiteId,
                   EndpointIdentity(*candidate.activeConnectionSite),
                   *selected);
      };

      // Prefer the selected matching tab, then connected, connecting, and
      // disconnected background tabs in that order.
      ConnectionTab *existing =
          tab && matchingTab(*tab, tab->connectionState) ? tab : nullptr;

      for (const auto state : {ConnectionState::Connected,
                               ConnectionState::Connecting,
                               ConnectionState::Disconnected})
      {
        if (existing != nullptr)
        {
          break;
        }

        const auto found = std::ranges::find_if(
            mConnectionTabs,
            [&](const auto &candidate)
            { return matchingTab(*candidate, state); });

        if (found != mConnectionTabs.end())
        {
          existing = found->get();

          break;
        }
      }

      if (existing != nullptr)
      {
        const int index = mConnectionNotebook->GetPageIndex(existing->panel);

        if (index != wxNOT_FOUND &&
            mConnectionNotebook->GetSelection() != index)
        {
          mConnectionNotebook->SetSelection(
              static_cast<std::size_t>(index));
        }

        if (existing->connectionState == ConnectionState::Disconnected)
        {
          // Reload non-endpoint edits such as credentials and initial paths
          // before reconnecting the existing saved-site tab.
          SetQuickSite(*existing, *selected);

          (void)BeginConnection(*existing, *selected);
        }

        return;
      }

      tab = &CreateConnectionTab();
    }

    SetQuickSite(*tab, *selected);

    // A saved-site action has an explicit identity. Connect its complete
    // profile directly so private-key, agent, and keyboard-interactive
    // authentication can never be downgraded by the generic Quick Connect
    // controls or by a stale draft in the reused tab.
    (void)BeginConnection(*tab, *selected);
  }

  void MainFrame::OnQuickHistoryClear(wxCommandEvent &)
  {
    if (mConfiguration.quickConnectHistory.empty() &&
        !HasQuickWorkspaceDirectories(mConfiguration))
    {
      return;
    }

    auto previousHistory = mConfiguration.quickConnectHistory;
    auto previousWorkspace = mConfiguration.workspace;

    mConfiguration.quickConnectHistory.clear();

    RemoveQuickWorkspaceDirectories(mConfiguration);

    if (!SaveConfiguration())
    {
      mConfiguration.quickConnectHistory = std::move(previousHistory);
      mConfiguration.workspace = std::move(previousWorkspace);

      return;
    }

    // Clearing history is a privacy boundary: already connected or in-flight
    // Quick tabs must not recreate the just-cleared endpoint metadata.
    for (auto &tab : mConnectionTabs)
    {
      if (tab->workspaceIdentity &&
          std::holds_alternative<config::QuickConnectWorkspaceIdentity>(
              *tab->workspaceIdentity))
      {
        tab->workspaceIdentity.reset();
      }
    }
  }

  void MainFrame::OnNewConnectionTab(wxCommandEvent &)
  {
    if (auto *const current = SelectedConnectionTab())
    {
      CaptureQuickConnectDraft(*current);
    }

    (void)CreateConnectionTab();

    PersistOpenConnectionTabs();

    mQuickHost->SetFocus();
  }

  void MainFrame::OnCloseConnectionTab(wxCommandEvent &)
  {
    auto *const tab = SelectedConnectionTab();

    if (!tab || !ConfirmCloseConnectionTab(*tab))
    {
      return;
    }

    CaptureQuickConnectDraft(*tab);

    const int index = mConnectionNotebook->GetPageIndex(tab->panel);

    if (index == wxNOT_FOUND)
    {
      return;
    }

    auto *const panel = tab->panel;

    mConnectionNotebook->RemovePage(static_cast<std::size_t>(index));

    DestroyConnectionTab(*tab);

    panel->Destroy();

    EnsureConnectionTab();
  }

  void MainFrame::OnConnectionTabChanging(wxAuiNotebookEvent &event)
  {
    const int oldSelection = event.GetOldSelection();

    if (oldSelection != wxNOT_FOUND &&
        static_cast<std::size_t>(oldSelection) <
            mConnectionNotebook->GetPageCount())
    {
      auto *const oldPage =
          mConnectionNotebook->GetPage(static_cast<std::size_t>(oldSelection));

      const auto found = std::ranges::find_if(
          mConnectionTabs,
          [oldPage](const auto &tab)
          { return tab->panel == oldPage; });

      if (found != mConnectionTabs.end())
      {
        CaptureQuickConnectDraft(**found);
      }
    }

    event.Skip();
  }

  void MainFrame::OnConnectionTabChanged(wxAuiNotebookEvent &event)
  {
    UpdateSelectedConnectionUi();

    PersistOpenConnectionTabs();

    event.Skip();
  }

  void MainFrame::OnConnectionTabClose(wxAuiNotebookEvent &event)
  {
    const int selection = event.GetSelection();

    if (selection == wxNOT_FOUND ||
        static_cast<std::size_t>(selection) >=
            mConnectionNotebook->GetPageCount())
    {
      event.Veto();

      return;
    }

    auto *const page =
        mConnectionNotebook->GetPage(static_cast<std::size_t>(selection));

    const auto found = std::ranges::find_if(
        mConnectionTabs,
        [page](const auto &tab)
        { return tab->panel == page; });

    if (found == mConnectionTabs.end() ||
        !ConfirmCloseConnectionTab(**found))
    {
      event.Veto();

      return;
    }

    if (found->get() == SelectedConnectionTab())
    {
      CaptureQuickConnectDraft(**found);
    }

    mPendingClosingConnectionId = (*found)->connectionId;

    event.Skip();
  }

  void MainFrame::OnConnectionTabClosed(wxAuiNotebookEvent &event)
  {
    if (mPendingClosingConnectionId)
    {
      if (auto *const tab = FindConnectionTab(*mPendingClosingConnectionId))
      {
        DestroyConnectionTab(*tab);
      }

      mPendingClosingConnectionId.reset();
    }

    EnsureConnectionTab();

    event.Skip();
  }

  void MainFrame::OnSiteManager(wxCommandEvent &)
  {
    if (mCredentialChangeInProgress)
    {
      return;
    }

    if (!mConfigurationWritable)
    {
      SaveConfiguration();

      return;
    }

    SiteManagerDialog dialog(this,
                             mConfiguration.sites,
                             mConfiguration.siteFolders,
                             mConfiguration.siteManagerOrder,
                             *mTranslations,
                             ProtectedExportPaths());

    if (dialog.ShowModal() != wxID_OK)
    {
      return;
    }

    mCredentialChangeInProgress = true;

    const auto credentialGuard = wxMakeVarSetter(mCredentialChangeInProgress, false);
    auto credentialUpdates = dialog.TakePendingCredentialUpdates();
    auto previousSites = mConfiguration.sites;
    auto previousSiteFolders = mConfiguration.siteFolders;
    auto previousSiteManagerOrder = mConfiguration.siteManagerOrder;
    auto previousHistory = mConfiguration.quickConnectHistory;
    auto previousWorkspace = mConfiguration.workspace;
    auto previousPendingCredentialDeletions = mConfiguration.pendingCredentialDeletions;

    mConfiguration.sites = dialog.Sites();
    mConfiguration.siteFolders = dialog.SiteFolders();
    mConfiguration.siteManagerOrder = dialog.SiteManagerOrder();

    struct CredentialBackup final
    {
      std::string id;
      std::optional<platform::CredentialPayload> value;
    };

    std::vector<CredentialBackup> credentialBackups;

    std::unordered_set<std::string> backedUpIds;

    for (const auto &update : credentialUpdates)
    {
      if (update.CredentialId().empty())
      {
        mConfiguration.sites = previousSites;
        mConfiguration.siteFolders = previousSiteFolders;
        mConfiguration.siteManagerOrder = previousSiteManagerOrder;

        LocalizedMessageBox(*mTranslations, Translated(*mTranslations, "authentication.missingIdentifier"),
                            Translated(*mTranslations,
                                       "authentication.saveFailedTitle"),
                            wxOK | wxICON_ERROR,
                            this);

        return;
      }

      if (!backedUpIds.insert(update.CredentialId()).second)
      {
        continue;
      }

      auto existing = mCredentialStore->Load(update.CredentialId());

      if (existing)
      {
        credentialBackups.push_back(
            CredentialBackup{update.CredentialId(), std::move(*existing)});
      }
      else if (existing.error().code == platform::PlatformErrorCode::NotFound)
      {
        credentialBackups.push_back(CredentialBackup{update.CredentialId(), std::nullopt});
      }
      else
      {
        mConfiguration.sites = previousSites;
        mConfiguration.siteFolders = previousSiteFolders;
        mConfiguration.siteManagerOrder = previousSiteManagerOrder;

        LocalizedMessageBox(*mTranslations, FromUtf8(existing.error().message),
                            Translated(*mTranslations, "authentication.prepareFailedTitle"),
                            wxOK | wxICON_ERROR,
                            this);

        return;
      }
    }

    const auto rollbackCredentials =
        [&](const std::unordered_set<std::string> &changedIds)
    {
      for (auto &backup : credentialBackups)
      {
        if (!changedIds.contains(backup.id))
        {
          continue;
        }

        const auto restored = backup.value
                                  ? mCredentialStore->Store(backup.id,
                                                            backup.value->Username(),
                                                            backup.value->Secret())
                                  : mCredentialStore->Erase(backup.id);

        if (!restored)
        {
          AppendLog(DiagnosticLevel::Error,
                    mTranslations->Format(
                        "authentication.rollbackFailedLog",
                        std::array<std::string_view, 2>{
                            backup.id,
                            restored.error().message}));
        }
      }
    };

    std::unordered_set<std::string> changedCredentialIds;

    for (const auto &update : credentialUpdates)
    {
      auto stored = mCredentialStore->Store(update.CredentialId(), update.Username(), update.Secret());

      if (!stored)
      {
        rollbackCredentials(changedCredentialIds);

        mConfiguration.sites = previousSites;
        mConfiguration.siteFolders = previousSiteFolders;
        mConfiguration.siteManagerOrder = previousSiteManagerOrder;

        LocalizedMessageBox(*mTranslations, FromUtf8(stored.error().message),
                            update.Kind() == CredentialKind::PrivateKeyPassphrase
                                ? Translated(*mTranslations,
                                             "authentication.passphraseSaveFailedTitle")
                                : Translated(*mTranslations,
                                             "authentication.saveFailedTitle"),
                            wxOK | wxICON_ERROR,
                            this);

        return;
      }

      changedCredentialIds.insert(update.CredentialId());
    }

    PruneSavedSitesFromQuickConnectHistory(mConfiguration);

    RemoveWorkspaceDirectoriesForDeletedSites(mConfiguration, previousSites);

    mConfiguration.pendingCredentialDeletions = platform::QueueRemovedCredentials(
        previousPendingCredentialDeletions, previousSites, mConfiguration.sites);

    if (!SaveConfiguration())
    {
      // The dialog edits a working copy. Keep the in-memory configuration in
      // sync with the unchanged file when its atomic save is rejected.
      rollbackCredentials(changedCredentialIds);

      mConfiguration.sites = previousSites;
      mConfiguration.siteFolders = previousSiteFolders;
      mConfiguration.siteManagerOrder = previousSiteManagerOrder;
      mConfiguration.quickConnectHistory = std::move(previousHistory);
      mConfiguration.workspace = std::move(previousWorkspace);
      mConfiguration.pendingCredentialDeletions = std::move(previousPendingCredentialDeletions);

      return;
    }

    // A deleted saved site must not be recreated in workspace state by an
    // already connected tab's next successful browse.
    for (auto &tab : mConnectionTabs)
    {
      if (!tab->workspaceIdentity)
      {
        continue;
      }

      if (const auto *identity =
              std::get_if<config::SavedSiteWorkspaceIdentity>(
                  &*tab->workspaceIdentity))
      {
        if (std::ranges::none_of(
                mConfiguration.sites,
                [&](const auto &site)
                {
                  return site.id == identity->siteId;
                }))
        {
          tab->workspaceIdentity.reset();
        }

        continue;
      }

      const auto &quick =
          std::get<config::QuickConnectWorkspaceIdentity>(
              *tab->workspaceIdentity);

      if (!QuickHistoryContains(mConfiguration, quick.endpoint))
      {
        if (const auto *saved = UniqueSavedSiteForEndpoint(
                mConfiguration, quick.endpoint))
        {
          tab->workspaceIdentity =
              config::SavedSiteWorkspaceIdentity{saved->id};
        }
      }
    }

    // Active connection snapshots retain their identifiers until the last
    // tab releases them. Unreferenced identifiers are removed immediately.
    mCredentialChangeInProgress = false;
    credentialGuard.Dismiss();

    EraseReleasedCredentials();

    if (const auto &siteId = dialog.ConnectionSiteId())
    {
      // Reach this only after the edited configuration and credentials were
      // saved successfully. Reuse the normal UUID-based saved-site path.
      ConnectSavedSite(*siteId);
    }
  }

  void MainFrame::OnSettings(wxCommandEvent &)
  {
    if (!mConfigurationWritable)
    {
      SaveConfiguration();

      return;
    }

    wxDialog dialog(this,
                    wxID_ANY,
                    Translated(*mTranslations, "settings.title"),
                    wxDefaultPosition,
                    wxDefaultSize,
                    wxDEFAULT_DIALOG_STYLE);

    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *form = new wxGridBagSizer(6, 10);

    int formRow{};

    const auto addField = [&](const wxString &label, wxWindow *field)
    {
      form->Add(new wxStaticText(&dialog, wxID_ANY, label),
                wxGBPosition(formRow, 0),
                wxDefaultSpan,
                wxALIGN_CENTER_VERTICAL);

      form->Add(field,
                wxGBPosition(formRow, 1),
                wxDefaultSpan,
                wxEXPAND);

      ++formRow;
    };

    auto *theme = new wxChoice(&dialog, wxID_ANY);
    theme->Append(Translated(*mTranslations, "settings.themeLight"));
    theme->Append(Translated(*mTranslations, "settings.themeDark"));
    theme->SetSelection(mConfiguration.settings.theme == config::AppearanceTheme::Dark
                            ? 1
                            : 0);

    addField(Translated(*mTranslations, "settings.themeLabel"), theme);

    auto *language = new wxChoice(&dialog, wxID_ANY);

    int selectedLanguage = wxNOT_FOUND;

    const auto languages = mTranslations->Languages();

    for (std::size_t index = 0; index < languages.size(); ++index)
    {
      language->Append(FromUtf8(languages[index].displayName));

      if (languages[index].code == mConfiguration.settings.language)
      {
        selectedLanguage = static_cast<int>(index);
      }
    }

    if (selectedLanguage == wxNOT_FOUND)
    {
      const auto effective = mTranslations->SelectedLanguageCode();

      for (std::size_t index = 0; index < languages.size(); ++index)
      {
        if (languages[index].code == effective)
        {
          selectedLanguage = static_cast<int>(index);

          break;
        }
      }
    }

    language->SetSelection(selectedLanguage == wxNOT_FOUND ? 0 : selectedLanguage);

    addField(Translated(*mTranslations, "settings.languageLabel"), language);

    bool restoreDefaultWindowSize{};

    auto *restoreWindowSize = new wxButton(
        &dialog,
        wxID_ANY,
        Translated(*mTranslations, "settings.restoreWindowSize"));
    restoreWindowSize->SetToolTip(Translated(
        *mTranslations, "settings.restoreWindowSizeTooltip"));
    restoreWindowSize->Bind(
        wxEVT_BUTTON,
        [&restoreDefaultWindowSize,
         restoreWindowSize](wxCommandEvent &)
        {
          restoreDefaultWindowSize = true;
          restoreWindowSize->Disable();
        });

    addField(Translated(*mTranslations, "settings.mainWindowLabel"),
             restoreWindowSize);

    auto *automaticUpdateCheck = new wxCheckBox(
        &dialog,
        wxID_ANY,
        Translated(*mTranslations,
                   "settings.updates.checkAutomatically"));
    automaticUpdateCheck->SetValue(
        mConfiguration.settings.updates.checkAutomatically);
    automaticUpdateCheck->SetToolTip(Translated(
        *mTranslations,
        "settings.updates.checkAutomaticallyTooltip"));

    addField(Translated(*mTranslations, "settings.updates.label"),
             automaticUpdateCheck);

    form->Add(new wxStaticLine(&dialog),
              wxGBPosition(formRow, 0),
              wxGBSpan(1, 2),
              wxEXPAND | wxTOP | wxBOTTOM,
              4);

    ++formRow;

    auto *transferHeading = new wxStaticText(
        &dialog,
        wxID_ANY,
        Translated(*mTranslations, "settings.transfers.heading"));
    auto transferHeadingFont = transferHeading->GetFont();
    transferHeadingFont.SetWeight(wxFONTWEIGHT_BOLD);
    transferHeading->SetFont(transferHeadingFont);

    form->Add(transferHeading,
              wxGBPosition(formRow, 0),
              wxGBSpan(1, 2),
              wxALIGN_CENTER_VERTICAL);

    ++formRow;

    auto *transferConcurrency = new wxSpinCtrl(
        &dialog,
        wxID_ANY,
        {},
        wxDefaultPosition,
        wxDefaultSize,
        wxSP_ARROW_KEYS,
        1,
        16,
        static_cast<int>(mConfiguration.settings.transferConcurrency));
    transferConcurrency->SetToolTip(Translated(
        *mTranslations, "settings.transfers.concurrencyTooltip"));

    addField(Translated(*mTranslations, "settings.transfers.concurrencyLabel"),
             transferConcurrency);

    auto *connectionTimeout = new wxSpinCtrl(
        &dialog,
        wxID_ANY,
        {},
        wxDefaultPosition,
        wxDefaultSize,
        wxSP_ARROW_KEYS,
        1,
        600,
        static_cast<int>(mConfiguration.settings.connectionTimeoutSeconds));
    connectionTimeout->SetToolTip(Translated(
        *mTranslations, "settings.transfers.connectionTimeoutTooltip"));

    addField(
        Translated(*mTranslations, "settings.transfers.connectionTimeoutLabel"),
        connectionTimeout);

    auto *idleTimeout = new wxSpinCtrl(
        &dialog,
        wxID_ANY,
        {},
        wxDefaultPosition,
        wxDefaultSize,
        wxSP_ARROW_KEYS,
        1,
        3600,
        static_cast<int>(mConfiguration.settings.commandIdleTimeoutSeconds));
    idleTimeout->SetToolTip(Translated(
        *mTranslations, "settings.transfers.idleTimeoutTooltip"));

    addField(Translated(*mTranslations, "settings.transfers.idleTimeoutLabel"),
             idleTimeout);

    auto *conflictPolicy = new wxChoice(&dialog, wxID_ANY);

    const std::array conflictPolicies{
        ConflictPolicy::Ask,
        ConflictPolicy::Overwrite,
        ConflictPolicy::Skip,
        ConflictPolicy::Rename,
        ConflictPolicy::Resume,
    };

    for (const auto key : {"settings.transfers.conflictAsk",
                           "settings.transfers.conflictOverwrite",
                           "settings.transfers.conflictSkip",
                           "settings.transfers.conflictRename",
                           "settings.transfers.conflictResume"})
    {
      conflictPolicy->Append(Translated(*mTranslations, key));
    }

    const auto configuredConflict = std::ranges::find(
        conflictPolicies, mConfiguration.settings.defaultConflictPolicy);
    conflictPolicy->SetSelection(static_cast<int>(
        std::distance(conflictPolicies.begin(), configuredConflict)));
    conflictPolicy->SetToolTip(Translated(
        *mTranslations, "settings.transfers.conflictTooltip"));

    addField(Translated(*mTranslations, "settings.transfers.conflictLabel"),
             conflictPolicy);

    form->Add(new wxStaticLine(&dialog),
              wxGBPosition(formRow, 0),
              wxGBSpan(1, 2),
              wxEXPAND | wxTOP | wxBOTTOM,
              4);

    ++formRow;

    auto *editorHeading = new wxStaticText(
        &dialog,
        wxID_ANY,
        Translated(*mTranslations, "settings.externalEditor.heading"));
    auto headingFont = editorHeading->GetFont();
    headingFont.SetWeight(wxFONTWEIGHT_BOLD);
    editorHeading->SetFont(headingFont);

    form->Add(editorHeading,
              wxGBPosition(formRow, 0),
              wxGBSpan(1, 2),
              wxALIGN_CENTER_VERTICAL);

    ++formRow;

    auto *editorMode = new wxChoice(&dialog, wxID_ANY);
    editorMode->Append(Translated(
        *mTranslations, "settings.externalEditor.modeSystem"));
    editorMode->Append(Translated(
        *mTranslations, "settings.externalEditor.modeCustom"));
    editorMode->SetSelection(
        mConfiguration.settings.externalEditor.mode ==
                config::ExternalEditorMode::Custom
            ? 1
            : 0);

    addField(Translated(*mTranslations,
                        "settings.externalEditor.modeLabel"),
             editorMode);

    auto *editorExecutable = new wxFilePickerCtrl(
        &dialog,
        wxID_ANY,
        mConfiguration.settings.externalEditor.executable.empty()
            ? wxString{}
            : platform::ToToolkitPath(mConfiguration.settings.externalEditor.executable),
        Translated(*mTranslations,
                   "settings.externalEditor.chooseExecutable"),
        Translated(*mTranslations,
#ifdef _WIN32
                   "settings.externalEditor.executableFilter"),
#else
                   "settings.externalEditor.allFilesFilter"),
#endif
        wxDefaultPosition,
        wxDefaultSize,
        wxFLP_OPEN | wxFLP_FILE_MUST_EXIST | wxFLP_USE_TEXTCTRL);

    SetPickerButtonLabel(*editorExecutable,
                         Translated(*mTranslations, "common.browse"));

    addField(Translated(*mTranslations,
                        "settings.externalEditor.executableLabel"),
             editorExecutable);

    auto *editorHint = new wxStaticText(&dialog, wxID_ANY, {});

    form->Add(editorHint,
              wxGBPosition(formRow, 1),
              wxDefaultSpan,
              wxEXPAND);

    ++formRow;

    auto *editorArguments = new wxTextCtrl(
        &dialog,
        wxID_ANY,
        FromUtf8(mConfiguration.settings.externalEditor.arguments));

    addField(Translated(*mTranslations,
                        "settings.externalEditor.argumentsLabel"),
             editorArguments);

    form->AddGrowableCol(1, 1);

    root->Add(form, 0, wxEXPAND | wxALL, 12);

    const auto updateEditorControls = [editorMode,
                                       editorExecutable,
                                       editorArguments,
                                       editorHint,
                                       this,
                                       &dialog]
    {
      const bool custom = editorMode->GetSelection() == 1;

      editorExecutable->Enable(custom);
      editorArguments->Enable(custom);

      editorHint->SetLabel(
          custom
              ? TranslatedFormat(
                    *mTranslations,
                    "settings.externalEditor.argumentsHint",
                    "{file}")
              : Translated(
                    *mTranslations,
                    "settings.externalEditor.systemHint"));

      if (dialog.GetSizer() != nullptr)
      {
        dialog.Fit();
      }
    };

    editorMode->Bind(
        wxEVT_CHOICE,
        [updateEditorControls](wxCommandEvent &)
        { updateEditorControls(); });

    updateEditorControls();

    auto *settingsHint = new wxStaticText(
        &dialog,
        wxID_ANY,
        Translated(*mTranslations, "settings.restartHint"));
    root->Add(new wxStaticLine(&dialog),
              0,
              wxEXPAND | wxLEFT | wxRIGHT,
              12);
    root->Add(settingsHint,
              0,
              wxEXPAND | wxALL,
              12);

    auto *dialogButtons = dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL);

    root->Add(dialogButtons,
              0,
              wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM,
              12);

    dialog.SetSizerAndFit(root);

    LocalizeStandardButtons(dialog, *mTranslations);

    dialog.CentreOnParent();

    dialog.Bind(
        wxEVT_BUTTON,
        [&, editorMode, editorExecutable, editorArguments](wxCommandEvent &event)
        {
          const auto arguments = ToUtf8(editorArguments->GetValue());
          const auto expanded =
              platform::ExpandExternalEditorArguments(
                  arguments,
                  "C:\\havRemote validation file.txt");
          const bool containsControl = std::ranges::any_of(
              arguments,
              [](const unsigned char character)
              {
                return character < 0x20 || character == 0x7f;
              });

          if (!expanded || containsControl)
          {
            LocalizedMessageBox(
                *mTranslations,
                TranslatedFormat(
                    *mTranslations,
                    "settings.externalEditor.argumentsInvalid",
                    "{file}"),
                Translated(
                    *mTranslations,
                    "settings.externalEditor.validationTitle"),
                wxOK | wxICON_WARNING,
                &dialog);

            return;
          }

          if (editorMode->GetSelection() == 1)
          {
            const auto executable = platform::FromToolkitPath(editorExecutable->GetPath());

            std::error_code error;

            if (executable.empty() ||
                !std::filesystem::is_regular_file(executable, error) ||
                error)
            {
              LocalizedMessageBox(
                  *mTranslations,
                  Translated(
                      *mTranslations,
                      "settings.externalEditor.executableRequired"),
                  Translated(
                      *mTranslations,
                      "settings.externalEditor.validationTitle"),
                  wxOK | wxICON_WARNING,
                  &dialog);

              return;
            }
          }

          event.Skip();
        },
        wxID_OK);

    if (dialog.ShowModal() != wxID_OK)
    {
      return;
    }

    const auto selected = theme->GetSelection() == 1
                              ? config::AppearanceTheme::Dark
                              : config::AppearanceTheme::Light;
    const auto selectedLanguageIndex = static_cast<std::size_t>(language->GetSelection());
    const auto selectedLanguageCode = languages[selectedLanguageIndex].code;

    const config::ExternalEditorSettings selectedExternalEditor{
        .mode = editorMode->GetSelection() == 1
                    ? config::ExternalEditorMode::Custom
                    : config::ExternalEditorMode::SystemDefault,
        .executable = platform::FromToolkitPath(editorExecutable->GetPath()),
        .arguments = ToUtf8(editorArguments->GetValue())};

    const auto conflictSelection = std::clamp(
        conflictPolicy->GetSelection(),
        0,
        static_cast<int>(conflictPolicies.size()) - 1);

    auto selectedSettings = mConfiguration.settings;
    selectedSettings.theme = selected;
    selectedSettings.language = selectedLanguageCode;
    selectedSettings.externalEditor = selectedExternalEditor;
    selectedSettings.transferConcurrency =
        static_cast<std::uint32_t>(transferConcurrency->GetValue());
    selectedSettings.connectionTimeoutSeconds =
        static_cast<std::uint32_t>(connectionTimeout->GetValue());
    selectedSettings.commandIdleTimeoutSeconds =
        static_cast<std::uint32_t>(idleTimeout->GetValue());
    selectedSettings.defaultConflictPolicy =
        conflictPolicies[static_cast<std::size_t>(conflictSelection)];
    selectedSettings.updates.checkAutomatically =
        automaticUpdateCheck->GetValue();

    const auto previousSettings = mConfiguration.settings;
    const bool themeChanged = selectedSettings.theme != previousSettings.theme;
    const bool languageChanged =
        selectedSettings.language != previousSettings.language;
    const bool concurrencyChanged = selectedSettings.transferConcurrency !=
                                    previousSettings.transferConcurrency;
    const bool timeoutsChanged =
        selectedSettings.connectionTimeoutSeconds !=
            previousSettings.connectionTimeoutSeconds ||
        selectedSettings.commandIdleTimeoutSeconds !=
            previousSettings.commandIdleTimeoutSeconds;
    const bool automaticUpdateCheckChanged =
        selectedSettings.updates.checkAutomatically !=
        previousSettings.updates.checkAutomatically;

    if (selectedSettings == previousSettings &&
        !restoreDefaultWindowSize)
    {
      return;
    }

    CaptureWindowGeometry();

    const auto previousWindowState = mNormalWindowState;

    if (restoreDefaultWindowSize)
    {
      if (mWindowGeometrySaveTimer)
      {
        mWindowGeometrySaveTimer->Stop();
      }

      ApplyWindowGeometry(DefaultWindowStateForCurrentDisplay());

      CaptureWindowGeometry();
    }

    mConfiguration.settings = selectedSettings;
    mConfiguration.workspace.mainWindow = mNormalWindowState;

    if (!SaveConfiguration())
    {
      mConfiguration.settings = previousSettings;

      if (restoreDefaultWindowSize)
      {
        ApplyWindowGeometry(previousWindowState);

        CaptureWindowGeometry();

        mConfiguration.workspace.mainWindow = mNormalWindowState;

        if (mWindowGeometrySaveTimer)
        {
          mWindowGeometrySaveTimer->Stop();
        }
      }

      return;
    }

    if (timeoutsChanged)
    {
      for (auto &tab : mConnectionTabs)
      {
        (void)tab->controller->UpdateTimeouts(
            std::chrono::seconds{
                mConfiguration.settings.connectionTimeoutSeconds},
            std::chrono::seconds{
                mConfiguration.settings.commandIdleTimeoutSeconds});
      }
    }

    if (languageChanged)
    {
      if (auto *const tab = SelectedConnectionTab())
      {
        CaptureQuickConnectDraft(*tab);
      }

      mTranslations =
          std::make_shared<localization::TranslationCatalog>(
              mTranslations->WithLanguage(selectedLanguageCode));

      ApplyTranslations();

      AppendLog(DiagnosticLevel::Information,
                mTranslations->Text("settings.languageAppliedLog"));
    }

    if (automaticUpdateCheckChanged &&
        mConfiguration.settings.updates.checkAutomatically)
    {
      CallAfter(
          [this]
          {
            if (AutomaticUpdateCheckDue())
            {
              StartUpdateCheck(false);
            }
          });
    }

    if (!themeChanged && !concurrencyChanged)
    {
      return;
    }

    const bool unfinishedTransfers = HasUnfinishedTransfers();

    wxString restartMessage = Translated(*mTranslations, "settings.restartMessage");

    if (unfinishedTransfers)
    {
      restartMessage += Translated(
          *mTranslations, "settings.restartTransferWarning");
    }

    if (HasExternalEditSessions())
    {
      restartMessage += Translated(
          *mTranslations,
          "settings.restartExternalEditorWarning");
    }

    if (LocalizedMessageBox(*mTranslations, restartMessage,
                            Translated(*mTranslations, "settings.restartTitle"),
                            wxYES_NO |
                                (unfinishedTransfers ? wxNO_DEFAULT : wxYES_DEFAULT) |
                                wxICON_INFORMATION,
                            this) != wxID_YES)
    {
      AppendLog(DiagnosticLevel::Information,
                mTranslations->Text("settings.savedLog"));

      return;
    }

    // Finish and persist the old process's transfer state before starting the
    // replacement process. Otherwise the new instance can read queue.cson
    // while a final download checkpoint is still only in memory.
    PrepareTransferQueueForShutdown();

    if (mQueuePersistenceWritable && !PersistTransferQueue())
    {
      for (auto &tab : mConnectionTabs)
      {
        SetConnectionState(*tab, ConnectionState::Disconnected);
      }

      return;
    }

    const wxString executable = wxStandardPaths::Get().GetExecutablePath();
    wxString command = "\"";
    command += executable;
    command += "\"";

#if defined(HAVREMOTE_UPDATE_SIMULATION)
    if (mUpdateSimulation)
    {
      // Keep both the fake transport and its isolated state after a restart
      command += " --simulate-update=";
      command += FromUtf8(updates::UpdateSimulationScenarioName(*mUpdateSimulation));
    }
#endif

    const long processId =
        wxExecute(command, wxEXEC_ASYNC | wxEXEC_MAKE_GROUP_LEADER);

    if (processId <= 0)
    {
      for (auto &tab : mConnectionTabs)
      {
        SetConnectionState(*tab, ConnectionState::Disconnected);
      }

      LocalizedMessageBox(*mTranslations, Translated(*mTranslations, "settings.restartFailedMessage"),
                          Translated(*mTranslations, "settings.restartFailedTitle"),
                          wxOK | wxICON_ERROR,
                          this);

      AppendLog(DiagnosticLevel::Error,
                mTranslations->Text("settings.restartFailedLog"));

      return;
    }

    Close(true);
  }

  void MainFrame::OnDisconnect(wxCommandEvent &)
  {
    auto *const tab = SelectedConnectionTab();
    if (!tab || tab->connectionState == ConnectionState::Disconnected)
    {
      return;
    }

    const auto disconnectLog = IntentionalDisconnectLog(*tab);

    MarkExternalEditDetached(tab->connectionId);

    tab->activeConnectionSite.reset();
    tab->activeConnectionSiteId.clear();
    tab->activeConnectionGeneration = 0;

    SetConnectionState(*tab, ConnectionState::Disconnected);
    SetStatusText(Translated(*mTranslations, "status.disconnected"), 0);

    tab->controller->Disconnect();

    if (disconnectLog)
    {
      AppendLog(DiagnosticLevel::Information, *disconnectLog);
    }

    ClearSensitiveString(tab->ephemeralPassword);
    ClearSensitiveString(tab->activeCredentialSecret);
    ClearSensitiveString(tab->activePassphraseSecret);

    EraseReleasedCredentials();
  }

  bool MainFrame::AutomaticUpdateCheckDue() const
  {
    if (!mConfigurationWritable ||
        !mConfiguration.settings.updates.checkAutomatically)
    {
      return false;
    }

    const auto &stored =
        mConfiguration.settings.updates.lastCheckUnixSeconds;

    if (stored.empty())
    {
      return true;
    }

    std::uint64_t lastCheck{};
    const auto [end, errorCode] =
        std::from_chars(stored.data(), stored.data() + stored.size(), lastCheck);

    if (errorCode != std::errc{} || end != stored.data() + stored.size())
    {
      // An invalid last-check timestamp makes another check due
      return true;
    }

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    if (now < 0)
    {
      return true;
    }

    const auto current = static_cast<std::uint64_t>(now);

    return current < lastCheck ||
           current - lastCheck >= UpdateCheckIntervalSeconds;
  }

  void MainFrame::ScheduleAutomaticUpdateCheck()
  {
    if (mAutomaticUpdateCheckScheduled)
    {
      return;
    }

    mAutomaticUpdateCheckScheduled = true;

    if (!AutomaticUpdateCheckDue())
    {
      return;
    }

    CallAfter([this]
              { StartUpdateCheck(false); });
  }

  void MainFrame::StartUpdateCheck(const bool manuallyRequested)
  {
    if (mUpdateCheckInProgress)
    {
      return;
    }

    const auto reportStartFailure =
        [this, manuallyRequested](const std::string_view message)
    {
      mUpdateCheckInProgress = false;

      if (auto *menu = GetMenuBar())
      {
        menu->Enable(IdCheckForUpdates, true);
      }

      AppendLog(
          DiagnosticLevel::Error,
          mTranslations->Format("update.failedLog", {message}));

      if (manuallyRequested)
      {
        LocalizedMessageBox(
            *mTranslations,
            TranslatedFormat(*mTranslations, "update.failedMessage", message),
            Translated(*mTranslations, "update.failedTitle"),
            wxOK | wxICON_ERROR,
            this);
      }
    };

    if (!mUpdateService)
    {
      reportStartFailure(
          "The update service could not be initialized for this build");

      return;
    }

    mUpdateCheckInProgress = true;

    if (auto *menu = GetMenuBar())
    {
      menu->Enable(IdCheckForUpdates, false);
    }

    AppendLog(DiagnosticLevel::Information,
              mTranslations->Text("update.checkingLog"));

    try
    {
      mUpdateCheckWorker = std::jthread(
          [this, manuallyRequested](const std::stop_token stopToken)
          {
            std::expected<updates::UpdateCheck, updates::UpdateError> result =
                std::unexpected(updates::UpdateError{
                    .kind = updates::UpdateErrorKind::Network,
                    .message = "The update check did not complete",
                    .httpStatus = std::nullopt,
                    .curlCode = std::nullopt,
                });

            try
            {
              result = mUpdateService->CheckLatest(stopToken);
            }
            catch (const std::exception &exception)
            {
              result = std::unexpected(updates::UpdateError{
                  .kind = updates::UpdateErrorKind::Network,
                  .message = std::string{"Unexpected update-check failure: "} +
                             exception.what(),
                  .httpStatus = std::nullopt,
                  .curlCode = std::nullopt,
              });
            }
            catch (...)
            {
              result = std::unexpected(updates::UpdateError{
                  .kind = updates::UpdateErrorKind::Network,
                  .message = "Unexpected update-check failure",
                  .httpStatus = std::nullopt,
                  .curlCode = std::nullopt,
              });
            }

            auto *event = new wxThreadEvent(EVT_HAVREMOTE_UPDATE_CHECK);
            event->SetPayload(UpdateCheckEventPayload{
                .manuallyRequested = manuallyRequested,
                .result = std::move(result),
            });

            wxQueueEvent(this, event);
          });
    }
    catch (const std::exception &exception)
    {
      reportStartFailure(
          std::string{"Could not start the update worker: "} +
          exception.what());
    }
    catch (...)
    {
      reportStartFailure("Could not start the update worker");
    }
  }

  void MainFrame::OnCheckForUpdates(wxCommandEvent &)
  {
    StartUpdateCheck(true);
  }

  void MainFrame::OnUpdateCheckFinished(wxThreadEvent &event)
  {
    auto payload = event.GetPayload<UpdateCheckEventPayload>();

    mUpdateCheckInProgress = false;

    if (auto *menu = GetMenuBar())
    {
      menu->Enable(IdCheckForUpdates, true);
    }

    if (!payload.result &&
        payload.result.error().kind == updates::UpdateErrorKind::Cancelled)
    {
      return;
    }

    const auto previousUpdateSettings = mConfiguration.settings.updates;

    if (mConfigurationWritable)
    {
      const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();

      if (now >= 0)
      {
        mConfiguration.settings.updates.lastCheckUnixSeconds =
            std::to_string(static_cast<std::uint64_t>(now));
      }

      if (payload.result &&
          payload.result->disposition == updates::UpdateDisposition::Available &&
          payload.result->release &&
          !mConfiguration.settings.updates.skippedVersion.empty() &&
          mConfiguration.settings.updates.skippedVersion !=
              payload.result->release->canonicalVersion)
      {
        mConfiguration.settings.updates.skippedVersion.clear();
      }

      if (!SaveConfiguration())
      {
        mConfiguration.settings.updates = previousUpdateSettings;
      }
    }

    if (!payload.result)
    {
      AppendLog(
          DiagnosticLevel::Warning,
          mTranslations->Format(
              "update.failedLog",
              {std::string_view{payload.result.error().message}}));

      if (payload.manuallyRequested)
      {
        LocalizedMessageBox(
            *mTranslations,
            TranslatedFormat(*mTranslations,
                             "update.failedMessage",
                             payload.result.error().message),
            Translated(*mTranslations, "update.failedTitle"),
            wxOK | wxICON_WARNING,
            this);
      }

      return;
    }

    if (payload.result->disposition == updates::UpdateDisposition::UpToDate)
    {
      AppendLog(
          DiagnosticLevel::Information,
          mTranslations->Format("update.upToDateLog",
                                {ApplicationVersion}));

      if (payload.manuallyRequested)
      {
        LocalizedMessageBox(
            *mTranslations,
            TranslatedFormat(*mTranslations,
                             "update.upToDateMessage",
                             ApplicationVersion),
            Translated(*mTranslations, "update.upToDateTitle"),
            wxOK | wxICON_INFORMATION,
            this);
      }

      return;
    }

    if (!payload.result->release)
    {
      return;
    }

    const auto &release = *payload.result->release;

    if (!payload.manuallyRequested &&
        !mConfiguration.settings.updates.checkAutomatically)
    {
      return;
    }

    if (!payload.manuallyRequested &&
        mConfiguration.settings.updates.skippedVersion ==
            release.canonicalVersion)
    {
      AppendLog(
          DiagnosticLevel::Information,
          mTranslations->Format(
              "update.skippedAvailableLog",
              {std::string_view{release.canonicalVersion}}));

      return;
    }

    AppendLog(
        DiagnosticLevel::Information,
        mTranslations->Format("update.availableLog",
                              {std::string_view{release.canonicalVersion}}));

    const auto action = ShowUpdateAvailableDialog(
        this, *mTranslations, ApplicationVersion, release);

    if (action == UpdateDialogAction::Later)
    {
      return;
    }

    if (action == UpdateDialogAction::SkipVersion)
    {
      const auto previousSkipped =
          mConfiguration.settings.updates.skippedVersion;

      mConfiguration.settings.updates.skippedVersion =
          release.canonicalVersion;

      if (mConfigurationWritable && SaveConfiguration())
      {
        AppendLog(
            DiagnosticLevel::Information,
            mTranslations->Format(
                "update.skippedLog",
                {std::string_view{release.canonicalVersion}}));
      }
      else
      {
        mConfiguration.settings.updates.skippedVersion = previousSkipped;

        if (!mConfigurationWritable)
        {
          (void)SaveConfiguration();
        }
      }

      return;
    }

    const auto &url = action == UpdateDialogAction::Download && release.runtimeAsset
                          ? release.runtimeAsset->downloadUrl
                          : release.releasePageUrl;

#if defined(HAVREMOTE_UPDATE_SIMULATION)
    if (mUpdateSimulation)
    {
      AppendLog(DiagnosticLevel::Information,
                "Update simulation would open: " + url);

      LocalizedMessageBox(
          *mTranslations,
          "No browser was opened. The simulated update action would open:\n\n" +
              FromUtf8(url),
          "havRemote update simulation", wxOK | wxICON_INFORMATION, this);

      return;
    }
#endif

    const auto launched = platform::LaunchHttpsUrl(url);
    if (launched)
    {
      return;
    }

    AppendLog(
        DiagnosticLevel::Error,
        mTranslations->Format("update.browserFailedLog",
                              {std::string_view{url}}));

    LocalizedMessageBox(
        *mTranslations,
        TranslatedFormat(*mTranslations, "update.browserFailedMessage", url),
        Translated(*mTranslations, "update.browserFailedTitle"),
        wxOK | wxICON_ERROR,
        this);
  }

  void MainFrame::OnHelp(wxCommandEvent &)
  {
    const auto launched = platform::LaunchOfflineHelp(
        mHelpRoot, mTranslations->SelectedLanguageCode());
    if (launched)
    {
      return;
    }

    LocalizedMessageBox(
        *mTranslations,
        TranslatedFormat(*mTranslations, "help.openFailedMessage",
                         GenericUtf8(mHelpRoot)),
        Translated(*mTranslations, "help.openFailedTitle"),
        wxOK | wxICON_ERROR,
        this);

    AppendLog(
        DiagnosticLevel::Error,
        mTranslations->Format(
            "help.openFailedLog",
            {std::string_view{launched.error().message}}));
  }

  void MainFrame::OnAbout(wxCommandEvent &)
  {
    ShowAboutDialog(this, *mTranslations);
  }

  void MainFrame::OnExit(wxCommandEvent &) { Close(); }

  void MainFrame::OnClose(wxCloseEvent &event)
  {
    if (HasUnfinishedTransfers() && event.CanVeto() &&
        LocalizedMessageBox(*mTranslations, Translated(*mTranslations, "exit.activeTransfersMessage"),
                            Translated(*mTranslations, "exit.activeTransfersTitle"),
                            wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
                            this) != wxID_YES)
    {
      event.Veto();

      return;
    }

    if (HasExternalEditSessions() && event.CanVeto() &&
        LocalizedMessageBox(
            *mTranslations,
            Translated(*mTranslations,
                       "externalEditor.exitActiveMessage"),
            Translated(*mTranslations,
                       "externalEditor.exitActiveTitle"),
            wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
            this) != wxID_YES)
    {
      event.Veto();

      return;
    }

    mTransferFailureTimer->Stop();
    mPendingTransferFailures.clear();

    if (mConfigurationWritable)
    {
      if (mWindowGeometrySaveTimer)
      {
        mWindowGeometrySaveTimer->Stop();
      }

      PersistWindowGeometry();

      PersistOpenConnectionTabs();
    }

    PrepareTransferQueueForShutdown();

    (void)PersistTransferQueue();

    event.Skip();
  }

  void MainFrame::NavigateToLocalPath(ConnectionTab &tab,
                                      const wxString &path)
  {
    auto requested = platform::FromToolkitPath(path);
    if (requested.empty())
    {
      tab.localPath->SetPath(platform::ToToolkitPath(tab.localDirectory));

      return;
    }

    if (requested.is_relative())
    {
      requested = tab.localDirectory / requested;
    }

    std::error_code error;

    requested = std::filesystem::absolute(requested, error).lexically_normal();

    if (error)
    {
      tab.localPath->SetPath(platform::ToToolkitPath(tab.localDirectory));

      LocalizedMessageBox(*mTranslations, FromUtf8(error.message()),
                          Translated(*mTranslations, "local.openFailedTitle"),
                          wxOK | wxICON_WARNING,
                          this);

      return;
    }

    PopulateLocalDirectory(tab, requested);
  }

  void MainFrame::OnLocalPathEnter(wxCommandEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));
    if (tab)
    {
      NavigateToLocalPath(*tab, tab->localPath->GetTextCtrl()->GetValue());
    }
  }

  void MainFrame::OnLocalDirectoryPicked(wxFileDirPickerEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));
    if (tab)
    {
      NavigateToLocalPath(*tab, event.GetPath());
    }

    // wxDirPickerCtrl also consumes this child event to synchronize its
    // composite state and forward the public picker-changed notification.
    event.Skip();
  }

  void MainFrame::NavigateToLocalParent(ConnectionTab &tab)
  {
    const auto parent = tab.localDirectory.parent_path();
    if (!parent.empty() && parent != tab.localDirectory)
    {
      PopulateLocalDirectory(tab, parent);
    }
  }

  void MainFrame::NavigateToRemoteParent(ConnectionTab &tab)
  {
    if (RemoteActionsAvailable(tab) && !tab.remoteDirectory.IsRoot())
    {
      tab.controller->Browse(tab.remoteDirectory.Parent());
    }
  }

  void MainFrame::OnRemotePathEnter(wxCommandEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));
    if (!tab)
    {
      return;
    }
    if (tab->connectionState != ConnectionState::Connected)
    {
      tab->remotePath->ChangeValue(
          FromUtf8(tab->remoteDirectory.DisplayUtf8()));

      return;
    }

    const auto text =
        ToUtf8(tab->remotePath->GetValue().Trim(true).Trim(false));
    if (text.empty() || text.find('\0') != std::string::npos ||
        text.find('\r') != std::string::npos || text.find('\n') != std::string::npos)
    {
      tab->remotePath->ChangeValue(
          FromUtf8(tab->remoteDirectory.DisplayUtf8()));

      LocalizedMessageBox(*mTranslations, Translated(*mTranslations, "remote.invalidPath"),
                          Translated(*mTranslations, "remote.openFailedTitle"),
                          wxOK | wxICON_WARNING,
                          this);

      return;
    }

    RemotePath requested{text};

    if (!requested.IsAbsolute())
    {
      requested = tab->remoteDirectory.Joined(requested);
    }

    // Keep the displayed directory synchronized with the last successful
    // listing while the serialized browser worker resolves this request.
    tab->remotePath->ChangeValue(
        FromUtf8(tab->remoteDirectory.DisplayUtf8()));
    tab->controller->Browse(std::move(requested));
  }

  void MainFrame::OnLocalUp(wxCommandEvent &event)
  {
    if (auto *const tab = ConnectionTabForWindow(
            dynamic_cast<wxWindow *>(event.GetEventObject())))
    {
      NavigateToLocalParent(*tab);
    }
  }

  void MainFrame::OnLocalRefresh(wxCommandEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));
    if (tab && !tab->localDirectory.empty() &&
        !tab->localDirectoryLoading)
    {
      PopulateLocalDirectory(*tab, tab->localDirectory);
    }
  }

  void MainFrame::OnRemoteUp(wxCommandEvent &event)
  {
    if (auto *const tab = ConnectionTabForWindow(
            dynamic_cast<wxWindow *>(event.GetEventObject())))
    {
      NavigateToRemoteParent(*tab);
    }
  }

  void MainFrame::OnRemoteRefresh(wxCommandEvent &event)
  {
    if (auto *const tab = ConnectionTabForWindow(
            dynamic_cast<wxWindow *>(event.GetEventObject()));
        tab && RemoteActionsAvailable(*tab))
    {
      tab->controller->Refresh();
    }
  }

  void MainFrame::OnLocalActivated(wxListEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));
    if (!tab)
    {
      return;
    }

    const auto *const row = tab->localList->RowAt(event.GetIndex());
    if (row && row->kind == FileListRowKind::ParentDirectory)
    {
      NavigateToLocalParent(*tab);

      return;
    }

    const auto sourceIndex = tab->localList->SourceIndexAt(event.GetIndex());
    if (!sourceIndex || *sourceIndex >= tab->localEntries.size())
    {
      return;
    }

    const auto &entry = tab->localEntries[*sourceIndex];
    if (entry.kind == LocalEntryKind::Directory)
    {
      PopulateLocalDirectory(*tab, entry.path);
    }
    else if (entry.kind == LocalEntryKind::File)
    {
      QueueUploads(*tab, {*sourceIndex});
    }
  }

  void MainFrame::OnRemoteActivated(wxListEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));
    if (!tab || !RemoteActionsAvailable(*tab))
    {
      return;
    }

    const auto *const row = tab->remoteList->RowAt(event.GetIndex());
    if (row && row->kind == FileListRowKind::ParentDirectory)
    {
      NavigateToRemoteParent(*tab);

      return;
    }

    const auto sourceIndex = tab->remoteList->SourceIndexAt(event.GetIndex());
    if (!sourceIndex || *sourceIndex >= tab->remoteEntries.size())
    {
      return;
    }

    if (tab->remoteEntries[*sourceIndex].kind == RemoteEntryKind::Directory)
    {
      tab->controller->Browse(tab->remoteEntries[*sourceIndex].path);
    }
    else if (tab->remoteEntries[*sourceIndex].kind == RemoteEntryKind::File)
    {
      QueueDownloads(*tab, {*sourceIndex});
    }
  }

  void MainFrame::OnFileListColumnClick(wxListEvent &event)
  {
    FileListCtrl *list{};

    config::FileListSortSettings *settings{};

    if (event.GetId() == IdLocalList)
    {
      auto *const tab = ConnectionTabForWindow(
          dynamic_cast<wxWindow *>(event.GetEventObject()));
      list = tab ? tab->localList : nullptr;
      settings = &mConfiguration.settings.fileLists.local;
    }
    else if (event.GetId() == IdRemoteList)
    {
      auto *const tab = ConnectionTabForWindow(
          dynamic_cast<wxWindow *>(event.GetEventObject()));
      list = tab ? tab->remoteList : nullptr;
      settings = &mConfiguration.settings.fileLists.remote;
    }

    if (list == nullptr || settings == nullptr || !mConfigurationWritable)
    {
      event.Skip();

      return;
    }

    const auto previous = *settings;

    settings->sortColumn = PersistedFileListColumn(list->SortColumn());
    settings->sortAscending = list->IsSortAscending();

    if (*settings != previous && !SaveConfiguration())
    {
      *settings = previous;
      list->SortBy(ToFileListColumn(previous.sortColumn),
                   previous.sortAscending);
    }
    else
    {
      for (auto &tab : mConnectionTabs)
      {
        auto *const otherList = event.GetId() == IdLocalList
                                    ? tab->localList
                                    : tab->remoteList;

        if (otherList != list)
        {
          otherList->SortBy(
              ToFileListColumn(settings->sortColumn),
              settings->sortAscending);
        }
      }
    }

    event.Skip();
  }

  void MainFrame::OnFileListColumnResized(wxListEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));
    if (!tab || !mConfigurationWritable)
    {
      event.Skip();

      return;
    }

    const bool local = event.GetId() == IdLocalList;

    auto *const list = local ? tab->localList : tab->remoteList;

    auto &settings = local ? mConfiguration.settings.fileLists.local
                           : mConfiguration.settings.fileLists.remote;

    const auto previous = settings.columnWidths;

    settings.columnWidths = PersistedColumnWidths(list->ColumnWidthsDips());

    if (settings.columnWidths != previous && !SaveConfiguration())
    {
      settings.columnWidths = previous;

      list->ApplyColumnWidthsDips(DisplayedColumnWidths(previous));

      event.Skip();

      return;
    }

    const auto widths = DisplayedColumnWidths(settings.columnWidths);

    for (auto &other : mConnectionTabs)
    {
      auto *const otherList = local ? other->localList : other->remoteList;
      if (otherList != list)
      {
        otherList->ApplyColumnWidthsDips(widths);
      }
    }

    event.Skip();
  }

  void MainFrame::OnFileSelectionChanged(wxListEvent &event)
  {
    UpdateFileActionState();

    event.Skip();
  }

  void MainFrame::OnFileListKeyDown(wxKeyEvent &event)
  {
    auto *const list = dynamic_cast<FileListCtrl *>(event.GetEventObject());
    auto *const focus = wxWindow::FindFocus();

    if (!list || !IsFileListDeleteKey(event) || !list->IsEnabled() ||
        list->GetEditControl() != nullptr ||
        (focus != list && (!focus || !list->IsDescendant(focus))))
    {
      event.Skip();

      return;
    }

    auto *const tab = ConnectionTabForWindow(list);
    if (!tab)
    {
      event.Skip();

      return;
    }

    const bool local = list == tab->localList;
    auto *const button = local ? tab->localDeleteButton : tab->remoteDeleteButton;

    if (!button->IsEnabled())
    {
      return;
    }

    wxCommandEvent command{wxEVT_BUTTON, button->GetId()};
    command.SetEventObject(button);

    if (local)
    {
      OnLocalDelete(command);
    }
    else
    {
      OnRemoteDelete(command);
    }
  }

  void MainFrame::OnLocalListRightClick(wxListEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));
    if (!tab)
    {
      return;
    }

    if (event.GetIndex() >= 0 &&
        (tab->localList->GetItemState(
             event.GetIndex(), wxLIST_STATE_SELECTED) &
         wxLIST_STATE_SELECTED) == 0)
    {
      tab->localList->SetItemState(-1, 0, wxLIST_STATE_SELECTED);
      tab->localList->SetItemState(
          event.GetIndex(), wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
          wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);

      UpdateFileActionState();
    }

    ShowLocalContextMenu(*tab);
  }

  void MainFrame::OnRemoteListRightClick(wxListEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));
    if (!tab)
    {
      return;
    }

    if (event.GetIndex() >= 0 &&
        (tab->remoteList->GetItemState(
             event.GetIndex(), wxLIST_STATE_SELECTED) &
         wxLIST_STATE_SELECTED) == 0)
    {
      tab->remoteList->SetItemState(-1, 0, wxLIST_STATE_SELECTED);
      tab->remoteList->SetItemState(
          event.GetIndex(), wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
          wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);

      UpdateFileActionState();
    }

    ShowRemoteContextMenu(*tab);
  }

  void MainFrame::OnLocalContextMenu(wxContextMenuEvent &event)
  {
    // Mouse context menus are handled by wxEVT_LIST_ITEM_RIGHT_CLICK so the
    // clicked row can be selected first. This path is for the keyboard menu
    // key / Shift+F10, whose position is wxDefaultPosition.
    if (event.GetPosition() != wxDefaultPosition)
    {
      return;
    }

    if (auto *const tab = ConnectionTabForWindow(
            dynamic_cast<wxWindow *>(event.GetEventObject())))
    {
      ShowLocalContextMenu(*tab);
    }
  }

  void MainFrame::OnRemoteContextMenu(wxContextMenuEvent &event)
  {
    if (event.GetPosition() != wxDefaultPosition)
    {
      return;
    }

    if (auto *const tab = ConnectionTabForWindow(
            dynamic_cast<wxWindow *>(event.GetEventObject())))
    {
      ShowRemoteContextMenu(*tab);
    }
  }

  void MainFrame::ShowLocalContextMenu(ConnectionTab &tab)
  {
    wxMenu menu;

    const auto append = [this, &menu](
                            const int id,
                            const std::string_view labelKey,
                            wxWindow *source,
                            void (MainFrame::*handler)(wxCommandEvent &))
    {
      auto *const item = menu.Append(id, Translated(*mTranslations, labelKey));

      item->Enable(source != nullptr && source->IsEnabled());

      menu.Bind(
          wxEVT_MENU,
          [this, id, source, handler](wxCommandEvent &)
          {
            wxCommandEvent command{wxEVT_BUTTON, id};
            command.SetEventObject(source);
            std::invoke(handler, this, command);
          },
          id);
    };

    append(IdUpload, "transfer.uploadButton", tab.uploadButton,
           &MainFrame::OnUpload);
    menu.AppendSeparator();
    append(IdLocalEdit, "local.edit", tab.localEditButton,
           &MainFrame::OnLocalEdit);
    append(IdLocalRename, "local.rename", tab.localRenameButton,
           &MainFrame::OnLocalRename);
    append(IdLocalDelete, "local.delete", tab.localDeleteButton,
           &MainFrame::OnLocalDelete);
    menu.AppendSeparator();
    append(IdLocalCreate, "local.newFolder", tab.localCreateButton,
           &MainFrame::OnLocalCreate);
    append(IdLocalRefresh, "local.refresh", tab.localRefreshButton,
           &MainFrame::OnLocalRefresh);

    tab.localList->PopupMenu(&menu);
  }

  void MainFrame::ShowRemoteContextMenu(ConnectionTab &tab)
  {
    wxMenu menu;

    const auto append = [this, &menu](
                            const int id,
                            const std::string_view labelKey,
                            wxWindow *source,
                            void (MainFrame::*handler)(wxCommandEvent &))
    {
      auto *const item = menu.Append(id, Translated(*mTranslations, labelKey));

      item->Enable(source != nullptr && source->IsEnabled());

      menu.Bind(
          wxEVT_MENU,
          [this, id, source, handler](wxCommandEvent &)
          {
            wxCommandEvent command{wxEVT_BUTTON, id};
            command.SetEventObject(source);
            std::invoke(handler, this, command);
          },
          id);
    };

    append(IdDownload, "transfer.downloadButton", tab.downloadButton,
           &MainFrame::OnDownload);
    menu.AppendSeparator();
    append(IdRemoteEdit, "remote.edit", tab.remoteEditButton,
           &MainFrame::OnRemoteEdit);
    append(IdRemoteRename, "remote.rename", tab.remoteRenameButton,
           &MainFrame::OnRemoteRename);
    append(IdRemotePermissions, "remote.permissions",
           tab.remotePermissionsButton, &MainFrame::OnRemotePermissions);
    append(IdRemoteDelete, "remote.deletePermanently",
           tab.remoteDeleteButton, &MainFrame::OnRemoteDelete);
    menu.AppendSeparator();
    append(IdRemoteCreateDirectory, "remote.newFolder",
           tab.remoteCreateDirectoryButton,
           &MainFrame::OnRemoteCreateDirectory);
    append(IdRemoteCreateFile, "remote.newFile",
           tab.remoteCreateFileButton, &MainFrame::OnRemoteCreateFile);
    append(IdRemoteRefresh, "remote.refresh", tab.remoteRefreshButton,
           &MainFrame::OnRemoteRefresh);
    tab.remoteList->PopupMenu(&menu);
  }

  void MainFrame::OnFileListBeginDrag(wxListEvent &event)
  {
    auto *const sourceTab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));
    if (!sourceTab)
    {
      return;
    }

    const bool sourceIsLocal = event.GetId() == IdLocalList;

    const auto indices = sourceIsLocal ? SelectedLocalIndices(*sourceTab)
                                       : SelectedRemoteIndices(*sourceTab);

    if (indices.empty() ||
        (!sourceIsLocal && !RemoteActionsAvailable(*sourceTab)))
    {
      return;
    }

    mPendingFileDrag = std::make_unique<PendingFileDrag>(PendingFileDrag{
        .token = GenerateId(),
        .sourceConnectionId = sourceTab->connectionId,
        .sourceIsLocal = sourceIsLocal,
        .sourceIdentities = {},
    });
    mPendingFileDrag->sourceIdentities.reserve(indices.size());

    for (const auto index : indices)
    {
      if (sourceIsLocal && index < sourceTab->localEntries.size())
      {
        mPendingFileDrag->sourceIdentities.push_back(
            GenericUtf8(sourceTab->localEntries[index].path));
      }
      else if (!sourceIsLocal && index < sourceTab->remoteEntries.size())
      {
        mPendingFileDrag->sourceIdentities.push_back(
            sourceTab->remoteEntries[index].path.Bytes());
      }
    }

    auto data = std::make_unique<wxDataObjectComposite>();

    auto *const internal = new wxCustomDataObject(FileListDragFormat());
    internal->SetData(mPendingFileDrag->token.size(),
                      mPendingFileDrag->token.data());

    data->Add(internal, true);

    if (sourceIsLocal)
    {
      auto *const files = new wxFileDataObject();

      for (const auto &identity : mPendingFileDrag->sourceIdentities)
      {
        const auto entry = std::ranges::find_if(
            sourceTab->localEntries,
            [&](const auto &candidate)
            { return GenericUtf8(candidate.path) == identity; });

        if (entry != sourceTab->localEntries.end())
        {
          if (entry->kind == LocalEntryKind::File ||
              entry->kind == LocalEntryKind::Directory)
          {
            files->AddFile(platform::ToToolkitPath(entry->path));
          }
        }
      }

      data->Add(files);
    }

    auto *const sourceList = sourceIsLocal ? sourceTab->localList
                                           : sourceTab->remoteList;

    wxDropSource dragSource{sourceList};
    dragSource.SetData(*data);
    (void)dragSource.DoDragDrop(wxDrag_CopyOnly);

    mPendingFileDrag.reset();
  }

  bool MainFrame::HandleInternalFileDrop(ConnectionTab &target,
                                         const bool targetIsRemote,
                                         const std::string_view token)
  {
    if (!mPendingFileDrag || mPendingFileDrag->token != token ||
        mPendingFileDrag->sourceConnectionId != target.connectionId ||
        mPendingFileDrag->sourceIsLocal == !targetIsRemote)
    {
      return false;
    }

    if (mPendingFileDrag->sourceIsLocal)
    {
      std::vector<std::filesystem::path> paths;
      paths.reserve(mPendingFileDrag->sourceIdentities.size());

      for (const auto &identity : mPendingFileDrag->sourceIdentities)
      {
        const auto entry = std::ranges::find_if(
            target.localEntries,
            [&](const auto &candidate)
            { return GenericUtf8(candidate.path) == identity; });

        if (entry != target.localEntries.end())
        {
          paths.push_back(entry->path);
        }
      }

      QueueUploadPaths(target, paths);
    }
    else
    {
      std::vector<std::size_t> indices;
      indices.reserve(mPendingFileDrag->sourceIdentities.size());

      for (const auto &identity : mPendingFileDrag->sourceIdentities)
      {
        const auto entry = std::ranges::find(
            target.remoteEntries, identity,
            [](const RemoteEntry &candidate)
            { return candidate.path.Bytes(); });

        if (entry != target.remoteEntries.end())
        {
          indices.push_back(static_cast<std::size_t>(
              std::distance(target.remoteEntries.begin(), entry)));
        }
      }

      QueueDownloads(target, indices);
    }

    return true;
  }

  bool MainFrame::HandleExternalFileDrop(
      ConnectionTab &target,
      const bool targetIsRemote,
      const std::vector<std::filesystem::path> &paths)
  {
    if (!targetIsRemote || paths.empty() ||
        !RemoteActionsAvailable(target))
    {
      return false;
    }

    QueueUploadPaths(target, paths);

    return true;
  }

  void MainFrame::OnLocalBeginLabelEdit(wxListEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));

    const auto sourceIndex =
        tab ? tab->localList->SourceIndexAt(event.GetIndex()) : std::nullopt;

    if (!tab || !sourceIndex || tab->localList->SelectedRowCount() != 1U)
    {
      event.Veto();
    }
  }

  void MainFrame::OnLocalEndLabelEdit(wxListEvent &event)
  {
    // Virtual rows are immutable presentation snapshots. Always reject the
    // native optimistic text replacement, then apply the filesystem rename and
    // rebuild the snapshot if validation succeeds.
    event.Veto();

    if (event.IsEditCancelled())
    {
      return;
    }

    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));

    const auto sourceIndex =
        tab ? tab->localList->SourceIndexAt(event.GetIndex()) : std::nullopt;

    if (tab && sourceIndex)
    {
      RenameLocalEntry(*tab, *sourceIndex, event.GetText());
    }
  }

  void MainFrame::OnRemoteBeginLabelEdit(wxListEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));

    const auto sourceIndex =
        tab ? tab->remoteList->SourceIndexAt(event.GetIndex()) : std::nullopt;

    if (!tab || !RemoteActionsAvailable(*tab) || !sourceIndex ||
        tab->remoteList->SelectedRowCount() != 1U)
    {
      event.Veto();
    }
  }

  void MainFrame::OnRemoteEndLabelEdit(wxListEvent &event)
  {
    event.Veto();

    if (event.IsEditCancelled())
    {
      return;
    }

    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));

    const auto sourceIndex =
        tab ? tab->remoteList->SourceIndexAt(event.GetIndex()) : std::nullopt;

    if (tab && sourceIndex)
    {
      RenameRemoteEntry(*tab, *sourceIndex, event.GetText());
    }
  }

  void MainFrame::OnUpload(wxCommandEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));

    if (!tab || !RemoteActionsAvailable(*tab))
    {
      return;
    }

    QueueUploads(*tab, SelectedLocalIndices(*tab));
  }

  void MainFrame::QueueUploads(
      ConnectionTab &tab,
      const std::vector<std::size_t> &sourceIndices)
  {
    if (!RemoteActionsAvailable(tab) || tab.localDirectoryLoading ||
        tab.localDirectory.empty() || sourceIndices.empty())
    {
      return;
    }

    std::size_t skipped = 0;

    std::vector<std::filesystem::path> paths;
    paths.reserve(sourceIndices.size());

    for (const auto sourceIndex : sourceIndices)
    {
      if (sourceIndex >= tab.localEntries.size())
      {
        ++skipped;

        continue;
      }

      const auto &entry = tab.localEntries[sourceIndex];

      if (entry.kind != LocalEntryKind::File &&
          entry.kind != LocalEntryKind::Directory)
      {
        ++skipped;

        continue;
      }

      paths.push_back(entry.path);
    }

    if (skipped != 0U)
    {
      LocalizedMessageBox(
          *mTranslations,
          TranslatedFormat(*mTranslations,
                           "transfer.unsupportedSelectedMessage",
                           skipped),
          Translated(*mTranslations, "transfer.uploadSkippedTitle"),
          wxOK | wxICON_WARNING,
          this);
    }

    QueueUploadPaths(tab, paths);
  }

  void MainFrame::QueueUploadPaths(
      ConnectionTab &tab,
      const std::vector<std::filesystem::path> &sourcePaths)
  {
    if (!RemoteActionsAvailable(tab) || tab.localDirectoryLoading ||
        tab.localDirectory.empty() || sourcePaths.empty())
    {
      return;
    }

    std::size_t skipped = 0;
    std::size_t failed = 0;
    std::string firstError;

    for (const auto &sourcePath : sourcePaths)
    {
      std::error_code error;

      const auto status = std::filesystem::symlink_status(sourcePath, error);
      const bool directory = !error && std::filesystem::is_directory(status);
      const bool file = !error && std::filesystem::is_regular_file(status);
      const auto name = GenericUtf8(sourcePath.filename());

      if (error || std::filesystem::is_symlink(status) ||
          (!file && !directory) || name.empty() ||
          !IsValidRemoteChildName(name))
      {
        ++skipped;

        continue;
      }

      TransferJob job{
          .id = {},
          .siteId = tab.activeConnectionSiteId,
          .siteEndpoint = std::nullopt,
          .direction = TransferDirection::Upload,
          .localPath = sourcePath,
          .remotePath = tab.remoteDirectory.Joined(RemotePath{name}),
          .recursive = directory,
          .conflictPolicy = mConfiguration.settings.defaultConflictPolicy,
          .expectedRemoteRevision = std::nullopt};

      if (auto queued = tab.controller->Enqueue(std::move(job)); !queued)
      {
        if (firstError.empty())
        {
          firstError = queued.error().message;
        }

        ++failed;
      }
    }

    if (skipped != 0U)
    {
      LocalizedMessageBox(
          *mTranslations,
          TranslatedFormat(*mTranslations,
                           "transfer.unsupportedSelectedMessage",
                           skipped),
          Translated(*mTranslations, "transfer.uploadSkippedTitle"),
          wxOK | wxICON_WARNING,
          this);
    }

    if (failed != 0U)
    {
      const auto message = failed == 1U
                               ? FromUtf8(firstError)
                               : TranslatedFormat(
                                     *mTranslations,
                                     "transfer.queueSelectedFailedMessage",
                                     failed,
                                     firstError);

      LocalizedMessageBox(
          *mTranslations,
          message,
          Translated(*mTranslations, "transfer.queueUploadFailedTitle"),
          wxOK | wxICON_ERROR,
          this);
    }
  }

  void MainFrame::OnDownload(wxCommandEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));

    if (!tab || !RemoteActionsAvailable(*tab))
    {
      return;
    }

    QueueDownloads(*tab, SelectedRemoteIndices(*tab));
  }

  void MainFrame::QueueDownloads(
      ConnectionTab &tab,
      const std::vector<std::size_t> &sourceIndices)
  {
    if (!RemoteActionsAvailable(tab) || tab.localDirectoryLoading ||
        tab.localDirectory.empty() || sourceIndices.empty())
    {
      return;
    }

    platform::SafeDownloadMapper mapper{tab.localDirectory};

    std::size_t skipped = 0;
    std::size_t unsafe = 0;
    std::size_t failed = 0;
    std::string firstUnsafeError;
    std::string firstQueueError;

    for (const auto sourceIndex : sourceIndices)
    {
      if (sourceIndex >= tab.remoteEntries.size())
      {
        ++skipped;

        continue;
      }

      const auto &entry = tab.remoteEntries[sourceIndex];

      if (entry.kind == RemoteEntryKind::Symlink ||
          (entry.kind != RemoteEntryKind::File &&
           entry.kind != RemoteEntryKind::Directory))
      {
        ++skipped;

        continue;
      }

      if (!platform::IsLocalSafeFilename(entry.name.DisplayUtf8()))
      {
        if (firstUnsafeError.empty())
        {
          firstUnsafeError =
              std::string{mTranslations->Text(
                  "transfer.unsafeRemoteName")};
        }

        ++unsafe;

        continue;
      }

      auto localPath = mapper.Map(entry.name);
      if (!localPath)
      {
        if (firstUnsafeError.empty())
        {
          firstUnsafeError = localPath.error().message;
        }

        ++unsafe;

        continue;
      }

      TransferJob job{
          .id = {},
          .siteId = tab.activeConnectionSiteId,
          .siteEndpoint = std::nullopt,
          .direction = TransferDirection::Download,
          .localPath = std::move(*localPath),
          .remotePath = entry.path,
          .recursive = entry.kind == RemoteEntryKind::Directory,
          .conflictPolicy = mConfiguration.settings.defaultConflictPolicy,
          .expectedRemoteRevision = std::nullopt};

      if (auto queued = tab.controller->Enqueue(std::move(job)); !queued)
      {
        if (firstQueueError.empty())
        {
          firstQueueError = queued.error().message;
        }

        ++failed;
      }
    }

    if (skipped != 0U)
    {
      LocalizedMessageBox(
          *mTranslations,
          TranslatedFormat(*mTranslations,
                           "transfer.unsupportedSelectedMessage",
                           skipped),
          Translated(*mTranslations, "transfer.downloadSkippedTitle"),
          wxOK | wxICON_WARNING,
          this);
    }

    if (unsafe != 0U)
    {
      const auto message = unsafe == 1U
                               ? FromUtf8(firstUnsafeError)
                               : TranslatedFormat(
                                     *mTranslations,
                                     "transfer.unsafeSelectedMessage",
                                     unsafe,
                                     firstUnsafeError);

      LocalizedMessageBox(
          *mTranslations,
          message,
          Translated(*mTranslations, "transfer.unsafeRemoteNameTitle"),
          wxOK | wxICON_ERROR,
          this);
    }

    if (failed != 0U)
    {
      const auto message = failed == 1U
                               ? FromUtf8(firstQueueError)
                               : TranslatedFormat(
                                     *mTranslations,
                                     "transfer.queueSelectedFailedMessage",
                                     failed,
                                     firstQueueError);

      LocalizedMessageBox(
          *mTranslations,
          message,
          Translated(*mTranslations, "transfer.queueDownloadFailedTitle"),
          wxOK | wxICON_ERROR,
          this);
    }
  }

  void MainFrame::OnLocalCreate(wxCommandEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));

    if (!tab || tab->localDirectory.empty() || tab->localDirectoryLoading)
    {
      return;
    }

    wxTextEntryDialog dialog(this,
                             Translated(*mTranslations, "local.createPrompt"),
                             Translated(*mTranslations, "local.createTitle"));

    LocalizeStandardButtons(dialog, *mTranslations);

    dialog.CentreOnParent();

    if (dialog.ShowModal() != wxID_OK)
    {
      return;
    }

    const auto name = LocalChildName(dialog.GetValue());
    if (!name)
    {
      LocalizedMessageBox(*mTranslations, Translated(*mTranslations, "local.invalidFolderName"),
                          Translated(*mTranslations, "local.invalidNameTitle"),
                          wxOK | wxICON_WARNING,
                          this);

      return;
    }

    const auto path = tab->localDirectory / *name;

    std::error_code error;

    if (!std::filesystem::create_directory(path, error))
    {
      LocalizedMessageBox(*mTranslations,
                          error
                              ? FromUtf8(error.message())
                              : Translated(*mTranslations, "local.folderExists"),
                          Translated(*mTranslations, "local.createFailedTitle"),
                          wxOK | wxICON_ERROR,
                          this);
    }

    PopulateLocalDirectory(*tab, tab->localDirectory);
  }

  void MainFrame::LaunchLocalEditor(const std::filesystem::path &path)
  {
    const auto pathText = GenericUtf8(path);

    const auto launched = platform::LaunchExternalEditor(
        mConfiguration.settings.externalEditor, path);

    if (!launched)
    {
      const auto errorText =
          ExternalEditorErrorText(*mTranslations, launched.error());

      LocalizedMessageBox(
          *mTranslations,
          TranslatedFormat(*mTranslations,
                           "externalEditor.launchFailedMessage",
                           pathText,
                           errorText),
          Translated(*mTranslations, "externalEditor.launchFailedTitle"),
          wxOK | wxICON_ERROR,
          this);

      AppendLog(
          DiagnosticLevel::Error,
          mTranslations->Format(
              "externalEditor.launchFailedLog",
              std::array<std::string_view, 2>{pathText, errorText}));
    }
    else
    {
      AppendLog(
          DiagnosticLevel::Information,
          mTranslations->Format(
              "externalEditor.openedLog",
              std::array<std::string_view, 1>{pathText}));
    }
  }

  void MainFrame::OnLocalEdit(wxCommandEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));

    const auto sourceIndex = tab ? SelectedLocalIndex(*tab) : std::nullopt;

    if (!tab || !sourceIndex ||
        tab->localEntries[*sourceIndex].kind != LocalEntryKind::File)
    {
      return;
    }

    LaunchLocalEditor(tab->localEntries[*sourceIndex].path);
  }

  void MainFrame::OnLocalRename(wxCommandEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));

    if (!tab || !SelectedLocalIndex(*tab))
    {
      return;
    }

    const long selected = tab->localList->GetNextItem(
        -1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);

    if (selected >= 0)
    {
      tab->localList->EditLabel(selected);
    }
  }

  void MainFrame::RenameLocalEntry(ConnectionTab &tab,
                                   const std::size_t sourceIndex,
                                   const wxString &newName)
  {
    if (sourceIndex >= tab.localEntries.size())
    {
      return;
    }

    const auto name = LocalChildName(newName);
    if (!name)
    {
      LocalizedMessageBox(*mTranslations, Translated(*mTranslations, "local.invalidItemName"),
                          Translated(*mTranslations, "local.invalidNameTitle"),
                          wxOK | wxICON_WARNING,
                          this);

      return;
    }

    const auto source = tab.localEntries[sourceIndex].path;
    const auto destination = tab.localDirectory / *name;

    if (source == destination)
    {
      return;
    }

    std::error_code error;

    std::filesystem::rename(source, destination, error);

    if (error)
    {
      LocalizedMessageBox(*mTranslations, FromUtf8(error.message()),
                          Translated(*mTranslations, "local.renameFailedTitle"),
                          wxOK | wxICON_ERROR,
                          this);

      return;
    }

    tab.preferredLocalSelectionIdentity =
        GenericUtf8(destination.lexically_normal());

    PopulateLocalDirectory(tab, tab.localDirectory);
  }

  void MainFrame::OnLocalDelete(wxCommandEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));

    if (!tab || tab->localDirectoryLoading || tab->localDirectory.empty())
    {
      return;
    }

    const auto selected = SelectedLocalIndices(*tab);

    // A directory refresh can arrive in the confirmation dialog's nested
    // event loop. Preserve the selected paths, not indices into a later list.
    std::vector<std::filesystem::path> paths;
    paths.reserve(selected.size());

    for (const auto sourceIndex : selected)
    {
      paths.push_back(tab->localEntries[sourceIndex].path);
    }

    if (paths.empty())
    {
      return;
    }

    const auto warning = paths.size() == 1U
                             ? Translated(*mTranslations, "local.deleteConfirm")
                             : TranslatedFormat(
                                   *mTranslations,
                                   "local.deleteSelectedConfirm",
                                   paths.size());

    if (LocalizedMessageBox(*mTranslations, warning,
                            Translated(*mTranslations, "local.deleteTitle"),
                            wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
                            this) != wxID_YES)
    {
      return;
    }

    auto removed = mRecycleBin->MoveToRecycleBin(paths);
    if (!removed)
    {
      LocalizedMessageBox(*mTranslations, FromUtf8(removed.error().message),
                          Translated(*mTranslations, "local.deleteFailedTitle"),
                          wxOK | wxICON_ERROR,
                          this);
    }

    PopulateLocalDirectory(*tab, tab->localDirectory);
  }

  void MainFrame::OnRemoteCreateDirectory(wxCommandEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));

    if (!tab || !RemoteActionsAvailable(*tab))
    {
      return;
    }

    wxTextEntryDialog dialog(this,
                             Translated(*mTranslations, "remote.createPrompt"),
                             Translated(*mTranslations, "remote.createTitle"));

    LocalizeStandardButtons(dialog, *mTranslations);

    dialog.CentreOnParent();

    if (dialog.ShowModal() != wxID_OK)
    {
      return;
    }

    const auto name = ToUtf8(dialog.GetValue());

    if (!IsValidRemoteChildName(name))
    {
      LocalizedMessageBox(*mTranslations, Translated(*mTranslations, "remote.invalidFolderName"),
                          Translated(*mTranslations, "local.invalidNameTitle"),
                          wxOK | wxICON_WARNING,
                          this);

      return;
    }

    if (!RemoteActionsAvailable(*tab))
    {
      return;
    }

    tab->controller->CreateRemoteDirectory(
        tab->remoteDirectory.Joined(RemotePath{name}));
  }

  void MainFrame::OnRemoteCreateFile(wxCommandEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));

    if (!tab || !RemoteActionsAvailable(*tab))
    {
      return;
    }

    wxTextEntryDialog dialog(
        this,
        Translated(*mTranslations, "remote.createFilePrompt"),
        Translated(*mTranslations, "remote.createFileTitle"));

    LocalizeStandardButtons(dialog, *mTranslations);

    dialog.CentreOnParent();

    if (dialog.ShowModal() != wxID_OK)
    {
      return;
    }

    const auto name = ToUtf8(dialog.GetValue());
    if (!IsValidRemoteChildName(name))
    {
      LocalizedMessageBox(
          *mTranslations,
          Translated(*mTranslations, "remote.invalidFileName"),
          Translated(*mTranslations, "local.invalidNameTitle"),
          wxOK | wxICON_WARNING,
          this);

      return;
    }

    if (!RemoteActionsAvailable(*tab))
    {
      return;
    }

    const auto destination = tab->remoteDirectory.Joined(RemotePath{name});
    tab->preferredRemoteSelectionIdentity = destination.Bytes();
    tab->controller->CreateRemoteFile(destination);
  }

  void MainFrame::StartRemoteEdit(ConnectionTab &tab,
                                  const std::size_t sourceIndex)
  {
    if (!RemoteActionsAvailable(tab) ||
        sourceIndex >= tab.remoteEntries.size())
    {
      return;
    }

    const auto &entry = tab.remoteEntries[sourceIndex];
    if (entry.kind != RemoteEntryKind::File ||
        !platform::IsLocalSafeFilename(entry.name.DisplayUtf8()))
    {
      LocalizedMessageBox(
          *mTranslations,
          Translated(*mTranslations, "transfer.unsafeRemoteName"),
          Translated(*mTranslations, "transfer.unsafeRemoteNameTitle"),
          wxOK | wxICON_WARNING,
          this);

      return;
    }

    const auto existing = std::ranges::find_if(
        mExternalEditSessions,
        [&](const auto &session)
        {
          return session->connectionId == tab.connectionId &&
                 session->remotePath == entry.path &&
                 session->phase != ExternalEditSession::Phase::Failed &&
                 session->phase != ExternalEditSession::Phase::Detached;
        });

    if (existing != mExternalEditSessions.end())
    {
      if ((*existing)->phase == ExternalEditSession::Phase::Watching ||
          (*existing)->phase == ExternalEditSession::Phase::Uploading ||
          (*existing)->phase == ExternalEditSession::Phase::Inspecting)
      {
        LaunchLocalEditor((*existing)->localPath);
      }

      return;
    }

    auto session = std::make_unique<ExternalEditSession>();
    session->id = GenerateId();
    session->connectionId = tab.connectionId;
    session->siteId = tab.activeConnectionSiteId;
    session->generation = tab.activeConnectionGeneration;
    session->remotePath = entry.path;
    session->remoteRevision = MakeRemoteFileRevision(entry);

    const auto localData = platform::FromToolkitPath(wxStandardPaths::Get().GetUserLocalDataDir());
    if (localData.empty())
    {
      const std::string errorText{mTranslations->Text(
          "externalEditor.error.localDataUnavailable")};

      LocalizedMessageBox(
          *mTranslations,
          TranslatedFormat(*mTranslations,
                           "externalEditor.downloadFailedMessage",
                           entry.path.DisplayUtf8(),
                           errorText),
          Translated(*mTranslations, "externalEditor.downloadFailedTitle"),
          wxOK | wxICON_ERROR,
          this);

      const auto remoteText = entry.path.DisplayUtf8();

      AppendLog(
          DiagnosticLevel::Error,
          mTranslations->Format(
              "externalEditor.downloadFailedLog",
              std::array<std::string_view, 2>{remoteText, errorText}));

      return;
    }

    const auto workspace = localData / L"edit" /
                           platform::FromToolkitPath(FromUtf8(session->id));

    session->localPath =
        workspace / platform::FromToolkitPath(FromUtf8(entry.name.DisplayUtf8()));

    std::optional<std::string> workspaceError;

    if (auto safe = platform::ValidateLocalWriteTarget(
            localData, session->localPath);
        !safe)
    {
      workspaceError = safe.error().message;
    }
    else
    {
      std::error_code error;

      std::filesystem::create_directories(workspace, error);

      if (error)
      {
        workspaceError = error.message();
      }
      else if (safe = platform::ValidateLocalWriteTarget(
                   localData, session->localPath);
               !safe)
      {
        workspaceError = safe.error().message;
      }
    }

    if (workspaceError)
    {
      const auto remoteText = entry.path.DisplayUtf8();

      LocalizedMessageBox(
          *mTranslations,
          TranslatedFormat(*mTranslations,
                           "externalEditor.downloadFailedMessage",
                           remoteText,
                           *workspaceError),
          Translated(*mTranslations, "externalEditor.downloadFailedTitle"),
          wxOK | wxICON_ERROR,
          this);

      AppendLog(
          DiagnosticLevel::Error,
          mTranslations->Format(
              "externalEditor.downloadFailedLog",
              std::array<std::string_view, 2>{remoteText, *workspaceError}));

      return;
    }

    TransferJob job{
        .id = {},
        .siteId = session->siteId,
        .siteEndpoint = std::nullopt,
        .direction = TransferDirection::Download,
        .localPath = session->localPath,
        .remotePath = session->remotePath,
        .recursive = false,
        .conflictPolicy = ConflictPolicy::Overwrite,
        .expectedRemoteRevision = entry.modifiedAt
                                      ? std::optional{session->remoteRevision}
                                      : std::nullopt};

    auto queued = tab.controller->Enqueue(std::move(job));
    if (!queued)
    {
      LocalizedMessageBox(
          *mTranslations,
          TranslatedFormat(*mTranslations,
                           "externalEditor.downloadFailedMessage",
                           entry.path.DisplayUtf8(),
                           queued.error().message),
          Translated(*mTranslations, "externalEditor.downloadFailedTitle"),
          wxOK | wxICON_ERROR,
          this);

      AppendLog(
          DiagnosticLevel::Error,
          mTranslations->Format(
              "externalEditor.downloadFailedLog",
              std::array<std::string_view, 2>{entry.path.DisplayUtf8(),
                                              queued.error().message}));

      return;
    }

    session->downloadJobId = std::move(*queued);

    mExternalEditSessions.push_back(std::move(session));
  }

  void MainFrame::OnRemoteEdit(wxCommandEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));

    const auto sourceIndex = tab ? SelectedRemoteIndex(*tab) : std::nullopt;

    if (tab && sourceIndex)
    {
      StartRemoteEdit(*tab, *sourceIndex);
    }
  }

  void MainFrame::OnRemoteRename(wxCommandEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));

    if (!tab || !RemoteActionsAvailable(*tab) ||
        !SelectedRemoteIndex(*tab))
    {
      return;
    }

    const long selected = tab->remoteList->GetNextItem(
        -1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);

    if (selected >= 0)
    {
      tab->remoteList->EditLabel(selected);
    }
  }

  void MainFrame::RenameRemoteEntry(ConnectionTab &tab,
                                    const std::size_t sourceIndex,
                                    const wxString &newName)
  {
    if (!RemoteActionsAvailable(tab) ||
        sourceIndex >= tab.remoteEntries.size())
    {
      return;
    }

    const auto name = ToUtf8(newName);
    if (!IsValidRemoteChildName(name))
    {
      LocalizedMessageBox(*mTranslations, Translated(*mTranslations, "remote.invalidItemName"),
                          Translated(*mTranslations, "local.invalidNameTitle"),
                          wxOK | wxICON_WARNING,
                          this);

      return;
    }

    const auto destination = tab.remoteDirectory.Joined(RemotePath{name});
    if (tab.remoteEntries[sourceIndex].path == destination)
    {
      return;
    }

    tab.preferredRemoteSelectionIdentity = destination.Bytes();
    tab.controller->Rename(tab.remoteEntries[sourceIndex].path, destination);
  }

  void MainFrame::OnRemotePermissions(wxCommandEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));

    if (!tab || !RemoteActionsAvailable(*tab))
    {
      return;
    }

    const auto selected = SelectedRemoteIndices(*tab);

    if (selected.empty() ||
        tab->remoteList->SelectedRowCount() != selected.size())
    {
      return;
    }

    std::vector<RemotePath> paths;
    paths.reserve(selected.size());

    std::optional<std::uint32_t> commonPermissions;

    bool first = true;

    for (const auto sourceIndex : selected)
    {
      if (sourceIndex >= tab->remoteEntries.size())
      {
        return;
      }

      const auto &entry = tab->remoteEntries[sourceIndex];
      if (entry.kind != RemoteEntryKind::File &&
          entry.kind != RemoteEntryKind::Directory)
      {
        return;
      }

      paths.push_back(entry.path);

      if (first)
      {
        commonPermissions = entry.permissions;

        first = false;
      }
      else if (!commonPermissions || !entry.permissions ||
               *commonPermissions != *entry.permissions)
      {
        commonPermissions.reset();
      }
    }

    RemotePermissionsDialog dialog(
        this,
        paths.size(),
        commonPermissions,
        *mTranslations);

    if (dialog.ShowModal() != wxID_OK)
    {
      return;
    }

    // Controller events continue to run in wxWidgets' nested modal event
    // loop. Do not enqueue a mutation if this connection was lost while the
    // permissions dialog was open.
    if (!RemoteActionsAvailable(*tab))
    {
      return;
    }

    tab->controller->SetPermissions(std::move(paths), dialog.Permissions());
  }

  void MainFrame::OnRemoteDelete(wxCommandEvent &event)
  {
    auto *const tab = ConnectionTabForWindow(
        dynamic_cast<wxWindow *>(event.GetEventObject()));

    if (!tab || !RemoteActionsAvailable(*tab))
    {
      return;
    }

    const auto selected = SelectedRemoteIndices(*tab);

    std::vector<std::pair<RemotePath, bool>> removals;
    removals.reserve(selected.size());

    for (const auto sourceIndex : selected)
    {
      const auto &entry = tab->remoteEntries[sourceIndex];

      removals.emplace_back(
          entry.path, entry.kind == RemoteEntryKind::Directory);
    }

    if (removals.empty())
    {
      return;
    }

    const bool includesDirectory = std::ranges::any_of(
        removals, [](const auto &item)
        { return item.second; });
    const auto generation = tab->activeConnectionGeneration;
    const auto warning = removals.size() > 1U
                             ? TranslatedFormat(
                                   *mTranslations,
                                   "remote.deleteSelectedConfirm",
                                   removals.size())
                             : Translated(
                                   *mTranslations,
                                   includesDirectory
                                       ? "remote.deleteDirectoryConfirm"
                                       : "remote.deleteItemConfirm");

    if (LocalizedMessageBox(*mTranslations, warning,
                            Translated(*mTranslations, "remote.deleteTitle"),
                            wxYES_NO | wxNO_DEFAULT | wxICON_ERROR,
                            this) == wxID_YES)
    {
      // Use the paths confirmed above, even if the listing was refreshed
      // meanwhile, and never apply the confirmation to a replacement session.
      if (!RemoteActionsAvailable(*tab) ||
          tab->activeConnectionGeneration != generation)
      {
        return;
      }

      for (auto &[path, recursive] : removals)
      {
        tab->controller->Remove(std::move(path), recursive);
      }
    }
  }

  void MainFrame::OnQueueSelectionChanged(wxListEvent &event)
  {
    UpdateQueueActionState();

    event.Skip();
  }

  void MainFrame::OnQueueListKeyDown(wxKeyEvent &event)
  {
    auto *const list = dynamic_cast<wxListCtrl *>(event.GetEventObject());
    if (!list || event.GetModifiers() != wxMOD_CONTROL)
    {
      event.Skip();

      return;
    }

    if (event.GetKeyCode() == 'C' || event.GetKeyCode() == WXK_INSERT)
    {
      CopyTransferText(TransferListText(*list, false));

      return;
    }

    if (event.GetKeyCode() == 'A')
    {
      {
        wxWindowUpdateLocker redrawLock{list};
        wxEventBlocker selectionEvents{list};

        list->SetItemState(-1, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);

        if (list->GetItemCount() != 0 &&
            list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_FOCUSED) == -1)
        {
          list->SetItemState(0, wxLIST_STATE_FOCUSED, wxLIST_STATE_FOCUSED);
        }
      }

      UpdateQueueActionState();

      return;
    }

    event.Skip();
  }

  void MainFrame::OnQueueContextMenu(wxContextMenuEvent &event)
  {
    auto *const list = dynamic_cast<wxListCtrl *>(event.GetEventObject());
    if (!list)
    {
      event.Skip();

      return;
    }

    wxPoint position = event.GetPosition();
    if (position != wxDefaultPosition)
    {
      position = list->ScreenToClient(position);

      int hitFlags{};

      const auto row = list->HitTest(position, hitFlags);

      if (row != -1 &&
          (list->GetItemState(row, wxLIST_STATE_SELECTED) & wxLIST_STATE_SELECTED) == 0)
      {
        wxEventBlocker selectionEvents{list};

        list->SetItemState(-1, 0, wxLIST_STATE_SELECTED);
        list->SetItemState(row, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                           wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
      }
    }

    // A context menu is also useful over empty space: Copy all does not
    // require a selected row. A keyboard invocation preserves the selection.
    list->SetFocus();

    ShowQueueContextMenu(*list, position);
  }

  void MainFrame::ShowQueueContextMenu(wxListCtrl &list, wxPoint position)
  {
    UpdateQueueActionState();

    const auto selected = SelectedQueueJob();

    wxMenu menu;

    const auto appendAction = [this, &menu, &list, &selected](
                                  const int id,
                                  const std::string_view labelKey,
                                  wxButton *button,
                                  void (MainFrame::*handler)(wxCommandEvent &),
                                  const bool singleItem)
    {
      auto label = Translated(*mTranslations, labelKey);

      if (id == IdQueueCopy)
      {
        label += "\tCtrl+C";
      }

      menu.Append(id, label)->Enable(button->IsEnabled());
      menu.Bind(wxEVT_MENU, [this, &list, selected, id, button, handler, singleItem](wxCommandEvent &)
                {
                  // Progress events can update the views while a popup is
                  // open. Never apply an action to a different selected job.
                  if (ActiveTransferList() != &list ||
                      (singleItem && SelectedQueueJob() != selected))
                  {
                    return;
                  }

                  UpdateQueueActionState();

                  if (!button->IsEnabled())
                  {
                    return;
                  }

                  wxCommandEvent command{wxEVT_BUTTON, id};
                  command.SetEventObject(button);

                  std::invoke(handler, this, command); }, id);
    };

    appendAction(IdQueuePause, "queue.pause", mQueuePauseButton,
                 &MainFrame::OnQueuePause, true);
    appendAction(IdQueueCancel, "queue.cancel", mQueueCancelButton,
                 &MainFrame::OnQueueCancel, true);
    appendAction(IdQueueRetry, "queue.retry", mQueueRetryButton,
                 &MainFrame::OnQueueRetry, true);
    appendAction(IdQueueRemove, "queue.remove", mQueueRemoveButton,
                 &MainFrame::OnQueueRemove, true);
    appendAction(IdQueueClear, "queue.clearView", mQueueClearButton,
                 &MainFrame::OnQueueClear, false);
    menu.AppendSeparator();
    appendAction(IdQueueCopy, "queue.copy", mQueueCopyButton,
                 &MainFrame::OnQueueCopy, false);
    menu.Append(IdQueueCopyAll, Translated(*mTranslations, "queue.copyAll"))
        ->Enable(list.GetItemCount() != 0);
    menu.Bind(wxEVT_MENU, [this, &list](wxCommandEvent &)
              { CopyTransferText(TransferListText(list, true)); }, IdQueueCopyAll);

    auto *exportMenu = new wxMenu;
    AppendQueueExportActions(*exportMenu, list);
    menu.AppendSubMenu(exportMenu, Translated(*mTranslations, "queue.export"))
        ->Enable(list.GetItemCount() != 0);

    if (position == wxDefaultPosition)
    {
      const long focused = list.GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_FOCUSED);
      wxRect bounds;
      position = focused != -1 && list.GetItemRect(focused, bounds) &&
                         list.GetClientRect().Intersects(bounds)
                     ? wxPoint{bounds.GetLeft() + 12, bounds.GetBottom()}
                     : wxPoint{12, 12};
    }

    list.PopupMenu(&menu, position);
  }

  void MainFrame::OnQueueCopy(wxCommandEvent &)
  {
    if (const auto *const list = ActiveTransferList())
    {
      CopyTransferText(TransferListText(*list, false));
    }
  }

  std::vector<std::filesystem::path> MainFrame::ProtectedExportPaths() const
  {
    std::vector<std::filesystem::path> paths{mKnownHostsFile};

    if (const auto path = mConfigRepository->ConfigPath())
    {
      paths.push_back(*path);
    }

    if (mQueueRepository)
    {
      if (const auto path = mQueueRepository->QueuePath())
      {
        paths.push_back(*path);
      }
    }

    return paths;
  }

  void MainFrame::AppendQueueExportActions(wxMenu &menu, wxListCtrl &list)
  {
    menu.Append(IdQueueExportSelected,
                Translated(*mTranslations, "queue.exportSelected"))
        ->Enable(list.GetSelectedItemCount() != 0);
    menu.Append(IdQueueExportAll,
                Translated(*mTranslations, "queue.exportAll"))
        ->Enable(list.GetItemCount() != 0);

    for (const auto id : {IdQueueExportSelected, IdQueueExportAll})
    {
      menu.Bind(wxEVT_MENU, [this, &list, id](wxCommandEvent &)
                { ExportTransferRows(list, id == IdQueueExportAll); }, id);
    }
  }

  void MainFrame::OnQueueExport(wxCommandEvent &)
  {
    auto *const list = ActiveTransferList();
    if (!list || list->GetItemCount() == 0)
    {
      return;
    }

    wxMenu menu;

    AppendQueueExportActions(menu, *list);

    mQueueExportButton->PopupMenu(&menu, 0, mQueueExportButton->GetSize().y);
  }

  void MainFrame::ExportTransferRows(const wxListCtrl &list, const bool allRows)
  {
    // Snapshot before the modal file dialog: worker events can move entries
    // between views while the user chooses a filename.
    const auto csv = TransferListText(list, allRows, TransferTextFormat::Csv);
    if (csv.empty())
    {
      return;
    }

    const wxString filename = &list == mFailedList      ? "havRemote-failed.csv"
                              : &list == mCompletedList ? "havRemote-completed.csv"
                                                        : "havRemote-queue.csv";

    wxFileDialog dialog(this,
                        Translated(*mTranslations, "queue.exportTitle"),
                        {}, filename,
                        Translated(*mTranslations, "queue.exportFilter"),
                        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

    dialog.CentreOnParent();

    if (dialog.ShowModal() != wxID_OK)
    {
      return;
    }

    const auto path = platform::FromToolkitPath(dialog.GetPath());

    if (platform::IsProtectedExportPath(path, ProtectedExportPaths()))
    {
      LocalizedMessageBox(*mTranslations,
                          Translated(*mTranslations, "export.protectedPathMessage"),
                          Translated(*mTranslations, "queue.exportFailedTitle"),
                          wxOK | wxICON_WARNING, this);

      return;
    }

    if (auto written = platform::WriteReportAtomic(path, csv); !written)
    {
      const auto detail = FromUtf8(SanitizeDiagnosticText(written.error().message));

      AppendLog(DiagnosticLevel::Warning, ToUtf8(detail));

      LocalizedMessageBox(*mTranslations,
                          TranslatedFormat(*mTranslations, "queue.exportFailedMessage",
                                           ToUtf8(dialog.GetPath()), ToUtf8(detail)),
                          Translated(*mTranslations, "queue.exportFailedTitle"),
                          wxOK | wxICON_WARNING, this);
    }
  }

  void MainFrame::OnTransferViewChanged(wxBookCtrlEvent &event)
  {
    UpdateQueueActionState();

    event.Skip();
  }

  void MainFrame::OnQueuePause(wxCommandEvent &)
  {
    if (!mQueuePauseButton->IsEnabled())
    {
      return;
    }

    if (const auto ref = SelectedQueueJob())
    {
      auto *const tab = FindConnectionTab(ref->connectionId);
      if (tab)
      {
        if (auto result = tab->controller->Pause(ref->jobId); !result)
        {
          LocalizedMessageBox(*mTranslations, FromUtf8(result.error().message),
                              Translated(*mTranslations, "transfer.pauseFailedTitle"),
                              wxOK | wxICON_WARNING,
                              this);
        }
      }
    }

    UpdateQueueActionState();
  }

  void MainFrame::OnQueueCancel(wxCommandEvent &)
  {
    if (!mQueueCancelButton->IsEnabled())
    {
      return;
    }

    if (const auto ref = SelectedQueueJob())
    {
      auto *const tab = FindConnectionTab(ref->connectionId);
      if (tab)
      {
        if (auto result = tab->controller->Cancel(ref->jobId); !result)
        {
          LocalizedMessageBox(*mTranslations, FromUtf8(result.error().message),
                              Translated(*mTranslations, "transfer.cancelFailedTitle"),
                              wxOK | wxICON_WARNING,
                              this);
        }
      }
    }

    UpdateQueueActionState();
  }

  void MainFrame::OnQueueRetry(wxCommandEvent &)
  {
    if (!mQueueRetryButton->IsEnabled())
    {
      return;
    }

    if (const auto ref = SelectedQueueJob())
    {
      auto *const tab = FindConnectionTab(ref->connectionId);
      if (!tab)
      {
        return;
      }

      const auto transfers = tab->controller->Transfers();
      const auto transfer = std::ranges::find(transfers, ref->jobId, [](const QueuedTransfer &item)
                                              { return item.job.id; });

      if (transfer != transfers.end() && transfer->error &&
          transfer->error->operationMayHaveSucceeded &&
          LocalizedMessageBox(*mTranslations,
                              Translated(*mTranslations, "transfer.uncertainRetryMessage"),
                              Translated(*mTranslations, "transfer.uncertainRetryTitle"),
                              wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
                              this) != wxID_YES)
      {
        return;
      }

      if (auto result = tab->controller->Retry(ref->jobId); !result)
      {
        LocalizedMessageBox(*mTranslations, FromUtf8(result.error().message),
                            Translated(*mTranslations, "transfer.retryFailedTitle"),
                            wxOK | wxICON_WARNING,
                            this);
      }
    }

    UpdateQueueActionState();
  }

  void MainFrame::OnQueueRemove(wxCommandEvent &)
  {
    if (!mQueueRemoveButton->IsEnabled())
    {
      return;
    }

    const auto ref = SelectedQueueJob();
    auto *const tab = ref ? FindConnectionTab(ref->connectionId) : nullptr;
    if (!ref || !tab)
    {
      return;
    }

    const auto transfer = std::ranges::find(
        tab->transfers,
        ref->jobId,
        [](const QueuedTransfer &candidate)
        { return std::string_view{candidate.job.id}; });
    if (transfer == tab->transfers.end())
    {
      return;
    }

    if (transfer->state == TransferState::Failed &&
        LocalizedMessageBox(
            *mTranslations,
            Translated(*mTranslations, "queue.removeFailedMessage"),
            Translated(*mTranslations, "queue.removeFailedConfirmTitle"),
            wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
            this) != wxID_YES)
    {
      return;
    }

    if (auto removed = tab->controller->RemoveTerminalTransfer(ref->jobId);
        !removed)
    {
      LocalizedMessageBox(
          *mTranslations,
          FromUtf8(removed.error().message),
          Translated(*mTranslations, "queue.removeFailedTitle"),
          wxOK | wxICON_WARNING,
          this);

      return;
    }

    // The controller posts an ordered immutable snapshot. Wait for it instead
    // of rebuilding from a direct snapshot that an older queued event could
    // temporarily overwrite.
    for (auto *button : {mQueuePauseButton,
                         mQueueCancelButton,
                         mQueueRetryButton,
                         mQueueRemoveButton,
                         mQueueClearButton})
    {
      button->Disable();
    }
  }

  void MainFrame::OnQueueClear(wxCommandEvent &)
  {
    if (!mQueueClearButton->IsEnabled())
    {
      return;
    }

    const int view = mTransferNotebook->GetSelection();
    const auto *refs = view == 1   ? &mFailedJobIds
                       : view == 2 ? &mCompletedJobIds
                                   : nullptr;
    if (refs == nullptr || refs->empty())
    {
      return;
    }

    const bool discardsRetryableFailure =
        view == 1 && std::ranges::any_of(
                         *refs,
                         [this](const QueueJobRef &ref)
                         {
                           const auto *const tab = FindConnectionTab(ref.connectionId);
                           return tab != nullptr && std::ranges::any_of(
                                                        tab->transfers,
                                                        [&](const QueuedTransfer &item)
                                                        {
                                                          return item.job.id == ref.jobId &&
                                                                 item.state == TransferState::Failed;
                                                        });
                         });

    if (discardsRetryableFailure &&
        LocalizedMessageBox(
            *mTranslations,
            Translated(*mTranslations, "queue.clearFailedMessage"),
            Translated(*mTranslations, "queue.clearFailedTitle"),
            wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
            this) != wxID_YES)
    {
      return;
    }

    std::unordered_map<std::string, std::vector<std::string>> grouped;

    for (const auto &ref : *refs)
    {
      grouped[ref.connectionId].push_back(ref.jobId);
    }

    for (const auto &[connectionId, jobIds] : grouped)
    {
      auto *const tab = FindConnectionTab(connectionId);
      if (!tab)
      {
        continue;
      }

      if (auto removed = tab->controller->RemoveTerminalTransfers(jobIds);
          !removed)
      {
        LocalizedMessageBox(
            *mTranslations,
            FromUtf8(removed.error().message),
            Translated(*mTranslations, "queue.removeFailedTitle"),
            wxOK | wxICON_WARNING,
            this);

        return;
      }
    }

    // Each controller posts its updated snapshot. Those events also trigger
    // the durable queue save after every controller has been mutated.
    for (auto *button : {mQueuePauseButton,
                         mQueueCancelButton,
                         mQueueRetryButton,
                         mQueueRemoveButton,
                         mQueueClearButton})
    {
      button->Disable();
    }
  }
} // namespace havremote::ui
