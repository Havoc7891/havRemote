// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UI_SITE_MANAGER_DIALOG_HPP
#define HAVREMOTE_INCLUDE_UI_SITE_MANAGER_DIALOG_HPP

#include "config/configTypes.hpp"

#include <wx/dialog.h>
#include <wx/treectrl.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

class wxButton;
class wxChoice;
class wxDirPickerCtrl;
class wxSpinCtrl;
class wxStaticText;
class wxTextCtrl;
class wxTreeEvent;

namespace havremote::localization
{
  class TranslationCatalog;
}

namespace havremote::ui
{
  class PendingCredentialUpdate final
  {
  public:
    PendingCredentialUpdate(std::string credentialId,
                            std::string username,
                            CredentialKind kind,
                            std::vector<std::byte> secret);
    ~PendingCredentialUpdate();

    PendingCredentialUpdate(const PendingCredentialUpdate &) = delete;
    PendingCredentialUpdate &operator=(const PendingCredentialUpdate &) = delete;
    PendingCredentialUpdate(PendingCredentialUpdate &&other) noexcept;
    PendingCredentialUpdate &operator=(PendingCredentialUpdate &&other) noexcept;

    [[nodiscard]] const std::string &CredentialId() const noexcept { return mCredentialId; }
    [[nodiscard]] const std::string &Username() const noexcept { return mUsername; }
    [[nodiscard]] CredentialKind Kind() const noexcept { return mKind; }
    [[nodiscard]] std::span<const std::byte> Secret() const noexcept { return mSecret; }

  private:
    void Clear() noexcept;

    std::string mCredentialId;
    std::string mUsername;
    CredentialKind mKind{CredentialKind::Password};
    std::vector<std::byte> mSecret;
  };

  class SiteManagerDialog final : public wxDialog
  {
  public:
    SiteManagerDialog(wxWindow *parent,
                      std::vector<SiteProfile> sites,
                      std::vector<config::SiteFolder> folders,
                      std::vector<std::string> siteManagerOrder,
                      const localization::TranslationCatalog &translations,
                      std::vector<std::filesystem::path> protectedExportPaths = {});
    ~SiteManagerDialog() override;

    [[nodiscard]] const std::vector<SiteProfile> &Sites() const noexcept { return mSites; }
    [[nodiscard]] const std::vector<config::SiteFolder> &SiteFolders() const noexcept
    {
      return mFolders;
    }
    [[nodiscard]] const std::vector<std::string> &SiteManagerOrder() const noexcept
    {
      return mSiteManagerOrder;
    }
    [[nodiscard]] std::vector<PendingCredentialUpdate> TakePendingCredentialUpdates();
    // Present only when Connect accepted the dialog. The caller must persist
    // the edited sites and credentials successfully before connecting this ID.
    [[nodiscard]] const std::optional<std::string> &ConnectionSiteId() const noexcept
    {
      return mConnectionSiteId;
    }

  private:
    struct SelectedNode
    {
      bool folder{};
      std::string id;

      friend bool operator==(const SelectedNode &,
                             const SelectedNode &) = default;
    };

    struct PendingSecrets
    {
      std::string password;
      std::string passphrase;
    };

    void BuildLayout();
    void RefreshSiteTree(std::optional<SelectedNode> select = std::nullopt);
    void LoadSelection(const SelectedNode &selection);
    bool CaptureSelection(bool showErrors);
    void ClearSiteFields();
    void SetSiteControlsVisible(bool visible);
    void UpdateActionButtons();
    [[nodiscard]] std::optional<SelectedNode> NodeForItem(
        const wxTreeItemId &item) const;
    [[nodiscard]] std::string SelectedParentFolderId() const;
    void PreparePendingCredentialIds();
    void ClearPendingSecrets() noexcept;
    void UpdateAuthenticationControls();
    void UpdateProtocolControls(bool chooseDefaultPort);

    void OnSelectionChanging(wxTreeEvent &event);
    void OnSelectionChanged(wxTreeEvent &event);
    void OnBeginDrag(wxTreeEvent &event);
    void OnEndDrag(wxTreeEvent &event);
    void OnNewSite(wxCommandEvent &event);
    void OnNewFolder(wxCommandEvent &event);
    void OnDuplicate(wxCommandEvent &event);
    void OnDeleteSite(wxCommandEvent &event);
    void OnProtocolChanged(wxCommandEvent &event);
    void OnFtpDataConnectionModeChanged(wxCommandEvent &event);
    void OnAuthenticationChanged(wxCommandEvent &event);
    void OnBrowseKey(wxCommandEvent &event);
    void OnImport(wxCommandEvent &event);
    void OnExport(wxCommandEvent &event);
    void ExportSites(std::optional<std::string> selectedRootId);
    void OnAccept(wxCommandEvent &event);

    std::vector<SiteProfile> mSites;
    std::vector<config::SiteFolder> mFolders;
    std::vector<std::string> mSiteManagerOrder;
    const localization::TranslationCatalog &mTranslations;
    std::vector<std::filesystem::path> mProtectedExportPaths;
    std::unordered_map<std::string, PendingSecrets> mPendingSecrets;
    std::optional<SelectedNode> mSelectedNode;
    std::optional<SelectedNode> mDraggedNode;
    std::optional<std::string> mConnectionSiteId;
    bool mLoading{};

    wxTreeCtrl *mSiteTree{};
    wxButton *mDuplicateButton{};
    wxButton *mDeleteButton{};
    wxButton *mConnectButton{};
    wxButton *mExportButton{};
    std::vector<wxWindow *> mSiteOnlyControls;
    wxTextCtrl *mName{};
    wxChoice *mProtocol{};
    wxTextCtrl *mHost{};
    wxSpinCtrl *mPort{};
    wxTextCtrl *mUsername{};
    wxChoice *mAuthentication{};
    wxTextCtrl *mPassword{};
    wxStaticText *mPasswordLabel{};
    wxTextCtrl *mPrivateKey{};
    wxButton *mBrowseKey{};
    wxStaticText *mPrivateKeyLabel{};
    wxTextCtrl *mPassphrase{};
    wxStaticText *mPassphraseLabel{};
    wxDirPickerCtrl *mLocalDirectory{};
    wxTextCtrl *mRemoteDirectory{};
    wxTextCtrl *mFtpEncoding{};
    wxStaticText *mFtpEncodingLabel{};
    wxChoice *mFtpDataConnectionMode{};
    wxStaticText *mFtpDataConnectionModeLabel{};
    wxTextCtrl *mFtpActiveAddress{};
    wxStaticText *mFtpActiveAddressLabel{};
  };
} // namespace havremote::ui

#endif // HAVREMOTE_INCLUDE_UI_SITE_MANAGER_DIALOG_HPP
