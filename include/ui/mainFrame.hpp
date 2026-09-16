// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UI_MAIN_FRAME_HPP
#define HAVREMOTE_INCLUDE_UI_MAIN_FRAME_HPP

#include "config/configRepository.hpp"
#include "config/queueRepository.hpp"
#include "platform/credentialStore.hpp"
#include "platform/recycleBin.hpp"
#include "ui/localDirectoryLoader.hpp"
#include "ui/remoteController.hpp"

#if defined(HAVREMOTE_UPDATE_SIMULATION)
#include "update/updateSimulation.hpp"
#endif

#include <wx/frame.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

class wxButton;
class wxBitmapButton;
class wxAuiNotebook;
class wxAuiNotebookEvent;
class wxBookCtrlEvent;
class wxChoice;
class wxCloseEvent;
class wxContextMenuEvent;
class wxDirPickerCtrl;
class wxFileDirPickerEvent;
class wxKeyEvent;
class wxListCtrl;
class wxListEvent;
class wxMenu;
class wxMoveEvent;
class wxNotebook;
class wxSizeEvent;
class wxSpinCtrl;
class wxStaticText;
class wxTextCtrl;
class wxTimer;
class wxTimerEvent;

namespace havremote::localization
{
  class TranslationCatalog;
}

namespace havremote::updates
{
  class IUpdateService;
}

namespace havremote::ui
{
  class FileListCtrl;
  class SplitButton;

  class MainFrame final : public wxFrame
  {
  public:
    MainFrame(std::unique_ptr<config::IConfigRepository> configRepository,
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
              std::optional<updates::UpdateSimulationScenario> updateSimulation = std::nullopt
#endif
    );
    ~MainFrame() override;

    // Establish the persisted native placement while the frame is hidden so
    // its first visible state already has the correct monitor, size, and
    // maximized state.
    void ShowWithRestoredGeometry();

  private:
    enum class ConnectionState
    {
      Disconnected,
      Connecting,
      Connected,
    };

    struct ConnectionTab;
    struct ExternalEditSession;
    struct PendingFileDrag;
    struct QueueJobRef final
    {
      std::string connectionId;
      std::string jobId;

      friend bool operator==(const QueueJobRef &, const QueueJobRef &) = default;
    };

    struct PendingTransferFailure final
    {
      QueueJobRef job;
      std::uint32_t attempt{};
    };

    void BuildMenus();
    void BuildLayout();
    void SizeCompactIconButtons();
    void ApplyTranslations();
    void RestoreFileListSortSettings();
    void RestoreFileListColumnWidths();
    void InitializeConfiguration(
        std::expected<config::ConfigLoadResult, config::ConfigError>
            initialConfiguration);
    [[nodiscard]] config::MainWindowState
    DefaultWindowStateForCurrentDisplay() const;
    void ApplyWindowGeometry(const config::MainWindowState &state);
    void CaptureWindowGeometry();
    void ScheduleWindowGeometrySave();
    void PersistWindowGeometry();
    void CaptureOpenConnectionTabs();
    void PersistOpenConnectionTabs();
    void OnWindowMoved(wxMoveEvent &event);
    void OnWindowSized(wxSizeEvent &event);
    void OnWindowGeometrySaveTimer(wxTimerEvent &event);
    bool SaveConfiguration();
    void InitializeTransferQueue(
        std::expected<config::QueueLoadResult, config::ConfigError> initialQueue);
    bool PersistTransferQueue();
    void PrepareTransferQueueForShutdown();
    [[nodiscard]] bool HasUnfinishedTransfers() const;
    [[nodiscard]] ConnectionTab *SelectedConnectionTab() const;
    [[nodiscard]] ConnectionTab *FindConnectionTab(std::string_view connectionId) const;
    [[nodiscard]] ConnectionTab *ConnectionTabForWindow(const wxWindow *window) const;
    ConnectionTab &CreateConnectionTab(
        bool select = true,
        std::optional<std::string> restoredConnectionId = std::nullopt,
        std::vector<config::PersistentQueueItem> restoredItems = {},
        std::optional<config::OpenConnectionTabState> restoredTab =
            std::nullopt);
    void DestroyConnectionTab(ConnectionTab &tab);
    [[nodiscard]] bool ConfirmCloseConnectionTab(const ConnectionTab &tab);
    void EnsureConnectionTab();
    void CaptureQuickConnectDraft(ConnectionTab &tab);
    void ShowQuickConnectDraft(const ConnectionTab &tab);
    void UpdateSelectedConnectionUi();
    [[nodiscard]] wxString ConnectionTabTitle(const ConnectionTab &tab) const;
    void UpdateConnectionTabTitle(ConnectionTab &tab);
    [[nodiscard]] std::optional<std::string> IntentionalDisconnectLog(
        const ConnectionTab &tab) const;
    void SetConnectionState(ConnectionTab &tab, ConnectionState state);
    void UpdateFileActionState();
    void UpdateQueueActionState();
    void ClearRemoteView(ConnectionTab &tab);
    [[nodiscard]] bool RemoteActionsAvailable(const ConnectionTab &tab) const noexcept;
    void NavigateToLocalPath(ConnectionTab &tab, const wxString &path);
    void NavigateToLocalParent(ConnectionTab &tab);
    void NavigateToRemoteParent(ConnectionTab &tab);
    void PopulateLocalDirectory(ConnectionTab &tab,
                                const std::filesystem::path &directory,
                                std::optional<std::filesystem::path> fallback =
                                    std::nullopt);
    void RequestLocalDirectoryRefresh(ConnectionTab &tab);
    void PopulateRemoteDirectory(ConnectionTab &tab, const DirectoryEvent &directory);
    void RefreshLocalFileRows(ConnectionTab &tab);
    void RefreshRemoteFileRows(ConnectionTab &tab);
    void PopulateTransferLists();
    void AppendLog(DiagnosticLevel level, std::string_view message);
    void SetQuickSite(ConnectionTab &tab,
                      const SiteProfile &site,
                      bool navigateToInitialLocalDirectory = true);
    bool BeginConnection(ConnectionTab &tab, SiteProfile site);
    void RecordSuccessfulQuickConnection(ConnectionTab &tab);
    void RememberLocalDirectory(ConnectionTab &tab,
                                const std::filesystem::path &directory);
    void RememberRemoteDirectory(ConnectionTab &tab,
                                 const RemotePath &directory);
    void RememberConnectionDirectories(
        ConnectionTab &tab,
        const std::filesystem::path *localDirectory,
        const RemotePath *remoteDirectory);
    void EraseReleasedCredentials();
    [[nodiscard]] std::optional<std::size_t> SelectedLocalIndex(
        const ConnectionTab &tab) const;
    [[nodiscard]] std::optional<std::size_t> SelectedRemoteIndex(
        const ConnectionTab &tab) const;
    [[nodiscard]] std::vector<std::size_t> SelectedLocalIndices(
        const ConnectionTab &tab) const;
    [[nodiscard]] std::vector<std::size_t> SelectedRemoteIndices(
        const ConnectionTab &tab) const;
    [[nodiscard]] std::optional<QueueJobRef> SelectedQueueJob() const;
    [[nodiscard]] wxListCtrl *ActiveTransferList() const;
    enum class TransferTextFormat
    {
      Clipboard,
      Csv
    };
    [[nodiscard]] std::string TransferListText(
        const wxListCtrl &list, bool allRows,
        TransferTextFormat format = TransferTextFormat::Clipboard) const;
    void CopyTransferText(const std::string &text);
    void ShowQueueContextMenu(wxListCtrl &list, wxPoint position);
    void AppendQueueExportActions(wxMenu &menu, wxListCtrl &list);
    void ExportTransferRows(const wxListCtrl &list, bool allRows);
    [[nodiscard]] std::vector<std::filesystem::path> ProtectedExportPaths() const;
    void QueueUploads(ConnectionTab &tab,
                      const std::vector<std::size_t> &sourceIndices);
    void QueueUploadPaths(ConnectionTab &tab,
                          const std::vector<std::filesystem::path> &sourcePaths);
    void QueueDownloads(ConnectionTab &tab,
                        const std::vector<std::size_t> &sourceIndices);
    void RenameLocalEntry(ConnectionTab &tab,
                          std::size_t sourceIndex,
                          const wxString &newName);
    void RenameRemoteEntry(ConnectionTab &tab,
                           std::size_t sourceIndex,
                           const wxString &newName);
    void LaunchLocalEditor(const std::filesystem::path &path);
    void StartRemoteEdit(ConnectionTab &tab, std::size_t sourceIndex);
    void HandleExternalEditTransfers(
        ConnectionTab &tab,
        const std::vector<QueuedTransfer> &transfers);
    void BeginExternalEditUpload(ExternalEditSession &session);
    void MarkExternalEditDetached(std::string_view connectionId);
    [[nodiscard]] bool HasExternalEditSessions(
        std::optional<std::string_view> connectionId = std::nullopt) const;

    [[nodiscard]] Result<std::string> RequestCredential(
        ConnectionTab &tab,
        const CredentialRequest &request);
    [[nodiscard]] Result<TrustDecision> VerifyTrust(
        ConnectionTab &tab,
        const TrustChallenge &challenge);
    [[nodiscard]] Result<ConflictResolution> ResolveConflict(
        ConnectionTab &tab,
        const ConflictChallenge &challenge);

    void OnControllerEvent(wxThreadEvent &event);
    void OnLocalDirectoryLoaded(wxThreadEvent &event);
    void OnQuickConnectButton(wxCommandEvent &event);
    void OnQuickConnect(wxCommandEvent &event);
    void OnQuickClear(wxCommandEvent &event);
    void OnMessageLogClear(wxCommandEvent &event);
    void OnQuickHistory(wxCommandEvent &event);
    void OnQuickHistorySelection(wxCommandEvent &event);
    void OnQuickHistoryClear(wxCommandEvent &event);
    void PopulateSavedSitesMenu(wxMenu &menu);
    void ConnectSavedSite(std::string_view siteId);
    void OnNewConnectionTab(wxCommandEvent &event);
    void OnCloseConnectionTab(wxCommandEvent &event);
    void OnConnectionTabChanging(wxAuiNotebookEvent &event);
    void OnConnectionTabChanged(wxAuiNotebookEvent &event);
    void OnConnectionTabClose(wxAuiNotebookEvent &event);
    void OnConnectionTabClosed(wxAuiNotebookEvent &event);
    void OnSiteManager(wxCommandEvent &event);
    void OnSettings(wxCommandEvent &event);
    void OnCheckForUpdates(wxCommandEvent &event);
    void OnUpdateCheckFinished(wxThreadEvent &event);
    void StartUpdateCheck(bool manuallyRequested);
    void ScheduleAutomaticUpdateCheck();
    [[nodiscard]] bool AutomaticUpdateCheckDue() const;
    void OnHelp(wxCommandEvent &event);
    void OnAbout(wxCommandEvent &event);
    void OnDisconnect(wxCommandEvent &event);
    void OnExit(wxCommandEvent &event);
    void OnClose(wxCloseEvent &event);
    void OnLocalPathEnter(wxCommandEvent &event);
    void OnLocalDirectoryPicked(wxFileDirPickerEvent &event);
    void OnRemotePathEnter(wxCommandEvent &event);
    void OnLocalUp(wxCommandEvent &event);
    void OnLocalRefresh(wxCommandEvent &event);
    void OnRemoteUp(wxCommandEvent &event);
    void OnRemoteRefresh(wxCommandEvent &event);
    void OnLocalActivated(wxListEvent &event);
    void OnRemoteActivated(wxListEvent &event);
    void OnFileListColumnClick(wxListEvent &event);
    void OnFileListColumnResized(wxListEvent &event);
    void OnFileSelectionChanged(wxListEvent &event);
    void OnFileListKeyDown(wxKeyEvent &event);
    void OnLocalListRightClick(wxListEvent &event);
    void OnRemoteListRightClick(wxListEvent &event);
    void OnLocalContextMenu(wxContextMenuEvent &event);
    void OnRemoteContextMenu(wxContextMenuEvent &event);
    void ShowLocalContextMenu(ConnectionTab &tab);
    void ShowRemoteContextMenu(ConnectionTab &tab);
    void OnFileListBeginDrag(wxListEvent &event);
    [[nodiscard]] bool HandleInternalFileDrop(
        ConnectionTab &target,
        bool targetIsRemote,
        std::string_view token);
    [[nodiscard]] bool HandleExternalFileDrop(
        ConnectionTab &target,
        bool targetIsRemote,
        const std::vector<std::filesystem::path> &paths);
    void OnLocalBeginLabelEdit(wxListEvent &event);
    void OnLocalEndLabelEdit(wxListEvent &event);
    void OnRemoteBeginLabelEdit(wxListEvent &event);
    void OnRemoteEndLabelEdit(wxListEvent &event);
    void OnUpload(wxCommandEvent &event);
    void OnDownload(wxCommandEvent &event);
    void OnLocalCreate(wxCommandEvent &event);
    void OnLocalEdit(wxCommandEvent &event);
    void OnLocalRename(wxCommandEvent &event);
    void OnLocalDelete(wxCommandEvent &event);
    void OnRemoteCreateDirectory(wxCommandEvent &event);
    void OnRemoteCreateFile(wxCommandEvent &event);
    void OnRemoteEdit(wxCommandEvent &event);
    void OnRemoteRename(wxCommandEvent &event);
    void OnRemotePermissions(wxCommandEvent &event);
    void OnRemoteDelete(wxCommandEvent &event);
    void OnQueueSelectionChanged(wxListEvent &event);
    void OnQueueListKeyDown(wxKeyEvent &event);
    void OnQueueContextMenu(wxContextMenuEvent &event);
    void OnTransferViewChanged(wxBookCtrlEvent &event);
    void OnQueueCopy(wxCommandEvent &event);
    void OnQueueExport(wxCommandEvent &event);
    void OnQueuePause(wxCommandEvent &event);
    void OnQueueCancel(wxCommandEvent &event);
    void OnQueueRetry(wxCommandEvent &event);
    void OnQueueRemove(wxCommandEvent &event);
    void OnQueueClear(wxCommandEvent &event);
    void OnExternalEditTimer(wxTimerEvent &event);
    void OnTransferFailureTimer(wxTimerEvent &event);
    std::unique_ptr<config::IConfigRepository> mConfigRepository;
    std::unique_ptr<config::IQueueRepository> mQueueRepository;
    std::unique_ptr<platform::ICredentialStore> mCredentialStore;
    std::unique_ptr<platform::IRecycleBin> mRecycleBin;
    std::unique_ptr<updates::IUpdateService> mUpdateService;
#if defined(HAVREMOTE_UPDATE_SIMULATION)
    std::optional<updates::UpdateSimulationScenario> mUpdateSimulation;
#endif
    std::jthread mUpdateCheckWorker;
    std::filesystem::path mKnownHostsFile;
    std::filesystem::path mHelpRoot;
    std::shared_ptr<const localization::TranslationCatalog> mTranslations;
    config::ConfigData mConfiguration;
    config::AppearanceTheme mActiveAppearanceTheme{
        config::AppearanceTheme::Dark};
    config::MainWindowState mNormalWindowState;
    config::MainWindowState mLastPersistedWindowState;
    bool mWindowGeometryInitialized{};
    bool mConfigurationWritable{};
    bool mCredentialChangeInProgress{};
    bool mQueuePersistenceWritable{};
    bool mQueuePersistenceErrorReported{};
    bool mRestoringWorkspace{};
    bool mUpdateCheckInProgress{};
    bool mAutomaticUpdateCheckScheduled{};
    std::shared_ptr<TransferRuntime> mTransferRuntime;
    std::vector<std::unique_ptr<ConnectionTab>> mConnectionTabs;
    std::vector<std::unique_ptr<ExternalEditSession>> mExternalEditSessions;
    std::unique_ptr<PendingFileDrag> mPendingFileDrag;
    std::unique_ptr<wxTimer> mExternalEditTimer;
    std::unique_ptr<wxTimer> mWindowGeometrySaveTimer;
    std::unique_ptr<wxTimer> mTransferFailureTimer;
    std::vector<PendingTransferFailure> mPendingTransferFailures;
    bool mTransferFailureDialogActive{};

    std::vector<QueueJobRef> mQueuedJobIds;
    std::vector<QueueJobRef> mFailedJobIds;
    std::vector<QueueJobRef> mCompletedJobIds;
    bool mPromptActive{};
    std::optional<std::string> mPendingClosingConnectionId;

    wxChoice *mQuickProtocol{};
    wxStaticText *mQuickHeading{};
    wxTextCtrl *mQuickHost{};
    wxSpinCtrl *mQuickPort{};
    wxTextCtrl *mQuickUsername{};
    wxTextCtrl *mQuickPassword{};
    wxStaticText *mLogHeading{};
    wxTextCtrl *mMessageLog{};
    wxBitmapButton *mMessageLogClearButton{};
    wxAuiNotebook *mConnectionNotebook{};
    wxNotebook *mTransferNotebook{};
    wxListCtrl *mQueuedList{};
    wxListCtrl *mFailedList{};
    wxListCtrl *mCompletedList{};
    wxBitmapButton *mConnectButton{};
    wxBitmapButton *mQuickClearButton{};
    wxBitmapButton *mNewConnectionButton{};
    wxBitmapButton *mQuickHistoryButton{};
    SplitButton *mSiteManagerButton{};
    wxButton *mQueuePauseButton{};
    wxButton *mQueueCancelButton{};
    wxButton *mQueueRetryButton{};
    wxButton *mQueueRemoveButton{};
    wxButton *mQueueClearButton{};
    wxButton *mQueueCopyButton{};
    wxButton *mQueueExportButton{};
  };
} // namespace havremote::ui

#endif // HAVREMOTE_INCLUDE_UI_MAIN_FRAME_HPP
