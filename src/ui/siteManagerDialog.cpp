// SPDX-License-Identifier: MIT

#include "ui/siteManagerDialog.hpp"

#include "localization/translationCatalog.hpp"
#include "platform/reportExport.hpp"
#include "ui/pickerLabel.hpp"
#include "ui/siteManagerModel.hpp"

#include <wx/arrstr.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/display.h>
#include <wx/filedlg.h>
#include <wx/filepicker.h>
#include <wx/imaglist.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

#include "platform/toolkitPaths.hpp"
#include "protocol/ftpSession.hpp"
#include "havRemoteImages.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <ranges>
#include <span>
#include <unordered_set>
#include <utility>

namespace havremote::ui
{
  namespace
  {
    constexpr int kSiteTreeId = wxID_HIGHEST + 100;
    constexpr int kNewSiteId = wxID_HIGHEST + 101;
    constexpr int kDeleteSiteId = wxID_HIGHEST + 102;
    constexpr int kProtocolId = wxID_HIGHEST + 103;
    constexpr int kAuthenticationId = wxID_HIGHEST + 104;
    constexpr int kNewFolderId = wxID_HIGHEST + 105;
    constexpr int kBrowseKeyId = wxID_HIGHEST + 106;
    constexpr int kDuplicateSiteId = wxID_HIGHEST + 107;
    constexpr int kFtpDataConnectionModeId = wxID_HIGHEST + 108;
    constexpr int kConnectSiteId = wxID_HIGHEST + 109;
    constexpr int kImportSitesId = wxID_HIGHEST + 110;
    constexpr int kExportSitesId = wxID_HIGHEST + 111;

    enum class TreeItemKind
    {
      Folder,
      Site,
    };

    class SiteTreeItemData final : public wxTreeItemData
    {
    public:
      SiteTreeItemData(const TreeItemKind itemKind, std::string stableId)
          : kind(itemKind), id(std::move(stableId)) {}

      TreeItemKind kind;
      std::string id;
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
        dialog.SetYesNoLabels(Translated(catalog, "common.yes"),
                              Translated(catalog, "common.no"));
      }
      else if ((style & wxOK) != 0)
      {
        dialog.SetOKLabel(Translated(catalog, "common.ok"));
      }

      dialog.CentreOnParent();

      return dialog.ShowModal();
    }

    std::string ToUtf8(const wxString &value)
    {
      const auto buffer = value.ToUTF8();
      return buffer ? std::string(buffer.data(), buffer.length()) : std::string{};
    }

    wxString SiteTransferErrorMessage(const config::ConfigError &error)
    {
      wxString message{platform::ToToolkitPath(error.path)};

      if (error.line)
      {
        message += ": " + std::to_string(*error.line);

        if (error.column)
        {
          message += ":" + std::to_string(*error.column);
        }
      }

      if (!message.empty())
      {
        message += ": ";
      }

      message += FromUtf8(error.message);

      return message;
    }

    std::vector<std::byte> SecretBytes(const std::string &value)
    {
      const auto bytes = std::as_bytes(std::span{value.data(), value.size()});
      return {bytes.begin(), bytes.end()};
    }

    void ClearSecret(std::string &value) noexcept
    {
      if (!value.empty())
      {
        wxSecureZeroMemory(value.data(), value.size());

        value.clear();
      }
    }

    ProtocolKind ProtocolAt(const int index)
    {
      switch (index)
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

    FtpDataConnectionMode FtpDataConnectionModeAt(const int index)
    {
      return index == 1 ? FtpDataConnectionMode::Active
                        : FtpDataConnectionMode::Passive;
    }

    int FtpDataConnectionModeIndex(const FtpDataConnectionMode mode)
    {
      return mode == FtpDataConnectionMode::Active ? 1 : 0;
    }

    bool IsAssignedLocalIpAddress(const std::string_view address)
    {
      return ftp::IsAssignedLocalAddress(address).value_or(false);
    }

    AuthenticationKind AuthenticationAt(const int index)
    {
      switch (index)
      {
      case 1:
        return AuthenticationKind::PrivateKey;

      case 2:
        return AuthenticationKind::Agent;

      case 3:
        return AuthenticationKind::KeyboardInteractive;

      case 4:
        return AuthenticationKind::PasswordKeyboardInteractive;

      default:
        return AuthenticationKind::Password;
      }
    }

    int AuthenticationIndex(const AuthenticationKind authentication)
    {
      switch (authentication)
      {
      case AuthenticationKind::Password:
        return 0;

      case AuthenticationKind::PrivateKey:
        return 1;

      case AuthenticationKind::Agent:
        return 2;

      case AuthenticationKind::KeyboardInteractive:
        return 3;

      case AuthenticationKind::PasswordKeyboardInteractive:
        return 4;
      }

      return 0;
    }
  } // namespace

  PendingCredentialUpdate::PendingCredentialUpdate(std::string credentialId,
                                                   std::string username,
                                                   const CredentialKind kind,
                                                   std::vector<std::byte> secret)
      : mCredentialId(std::move(credentialId)),
        mUsername(std::move(username)),
        mKind(kind),
        mSecret(std::move(secret)) {}

  PendingCredentialUpdate::~PendingCredentialUpdate() { Clear(); }

  PendingCredentialUpdate::PendingCredentialUpdate(PendingCredentialUpdate &&other) noexcept
      : mCredentialId(std::move(other.mCredentialId)),
        mUsername(std::move(other.mUsername)),
        mKind(other.mKind),
        mSecret(std::move(other.mSecret)) {}

  PendingCredentialUpdate &PendingCredentialUpdate::operator=(
      PendingCredentialUpdate &&other) noexcept
  {
    if (this != &other)
    {
      Clear();

      mCredentialId = std::move(other.mCredentialId);
      mUsername = std::move(other.mUsername);
      mKind = other.mKind;
      mSecret = std::move(other.mSecret);
    }

    return *this;
  }

  void PendingCredentialUpdate::Clear() noexcept
  {
    if (!mSecret.empty())
    {
      wxSecureZeroMemory(mSecret.data(), mSecret.size());

      mSecret.clear();
    }
  }

  SiteManagerDialog::SiteManagerDialog(
      wxWindow *parent,
      std::vector<SiteProfile> sites,
      std::vector<config::SiteFolder> folders,
      std::vector<std::string> siteManagerOrder,
      const localization::TranslationCatalog &translations,
      std::vector<std::filesystem::path> protectedExportPaths)
      : wxDialog(parent,
                 wxID_ANY,
                 Translated(translations, "siteManager.title"),
                 wxDefaultPosition,
                 wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
        mSites(std::move(sites)),
        mFolders(std::move(folders)),
        mSiteManagerOrder(std::move(siteManagerOrder)),
        mTranslations(translations),
        mProtectedExportPaths(std::move(protectedExportPaths))
  {
    if (mSiteManagerOrder.empty() && (!mSites.empty() || !mFolders.empty()))
    {
      mSiteManagerOrder = SynthesizeSiteManagerOrder(mSites, mFolders);
    }

    BuildLayout();

    RefreshSiteTree();

    CentreOnParent();
  }

  SiteManagerDialog::~SiteManagerDialog()
  {
    ClearPendingSecrets();

    if (mPassword)
    {
      mPassword->ChangeValue({});
    }

    if (mPassphrase)
    {
      mPassphrase->ChangeValue({});
    }
  }

  void SiteManagerDialog::BuildLayout()
  {
    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *body = new wxBoxSizer(wxHORIZONTAL);

    auto *sitesColumn = new wxBoxSizer(wxVERTICAL);
    sitesColumn->Add(new wxStaticText(
                         this,
                         wxID_ANY,
                         Translated(mTranslations, "siteManager.savedSites")),
                     0,
                     wxBOTTOM,
                     5);
    mSiteTree = new wxTreeCtrl(
        this,
        kSiteTreeId,
        wxDefaultPosition,
        FromDIP(wxSize{280, -1}),
        wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_HIDE_ROOT |
            wxTR_DEFAULT_STYLE);

    auto *treeImages = new wxImageList(16, 16, true);
    treeImages->Add(LoadUiBitmap("HAVREMOTE_FILE_LIST_FOLDER_ICON"));
    treeImages->Add(LoadUiBitmap("HAVREMOTE_SITE_MANAGER_ICON"));

    mSiteTree->AssignImageList(treeImages);

    sitesColumn->Add(mSiteTree, 1, wxEXPAND);

    auto *siteButtons = new wxGridSizer(3, 2, 4, 4);
    siteButtons->Add(new wxButton(
                         this,
                         kNewSiteId,
                         Translated(mTranslations, "siteManager.newSite")),
                     1,
                     wxEXPAND);
    siteButtons->Add(new wxButton(
                         this,
                         kNewFolderId,
                         Translated(mTranslations, "siteManager.newFolder")),
                     1,
                     wxEXPAND);
    mDuplicateButton = new wxButton(
        this,
        kDuplicateSiteId,
        Translated(mTranslations, "siteManager.duplicate"));
    siteButtons->Add(mDuplicateButton, 1, wxEXPAND);
    mDeleteButton = new wxButton(
        this,
        kDeleteSiteId,
        Translated(mTranslations, "siteManager.delete"));
    siteButtons->Add(mDeleteButton, 1, wxEXPAND);
    siteButtons->Add(new wxButton(this, kImportSitesId,
                                  Translated(mTranslations, "siteManager.import")),
                     1, wxEXPAND);
    mExportButton = new wxButton(
        this, kExportSitesId, Translated(mTranslations, "siteManager.export"));
    siteButtons->Add(mExportButton, 1, wxEXPAND);
    sitesColumn->Add(siteButtons, 0, wxEXPAND | wxTOP, 6);
    body->Add(sitesColumn, 0, wxEXPAND | wxALL, 10);
    body->Add(new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_VERTICAL),
              0,
              wxEXPAND | wxTOP | wxBOTTOM,
              10);

    auto *fields = new wxFlexGridSizer(2, 8, 10);
    fields->AddGrowableCol(1, 1);
    const auto addField = [&](const wxString &label,
                              wxWindow *control,
                              const bool siteOnly = true)
    {
      auto *labelControl = new wxStaticText(this, wxID_ANY, label);

      fields->Add(labelControl, 0, wxALIGN_CENTER_VERTICAL);
      fields->Add(control, 1, wxEXPAND);

      if (siteOnly)
      {
        mSiteOnlyControls.push_back(labelControl);
        mSiteOnlyControls.push_back(control);
      }
    };

    mName = new wxTextCtrl(this, wxID_ANY);

    mProtocol = new wxChoice(this, kProtocolId);
    mProtocol->Append(Translated(mTranslations, "siteManager.protocol.ftp"));
    mProtocol->Append(Translated(mTranslations, "siteManager.protocol.ftpsExplicit"));
    mProtocol->Append(Translated(mTranslations, "siteManager.protocol.ftpsImplicit"));
    mProtocol->Append(Translated(mTranslations, "siteManager.protocol.sftp"));
    mProtocol->SetSelection(ProtocolIndex(ProtocolKind::Sftp));
    mHost = new wxTextCtrl(this, wxID_ANY);
    mPort = new wxSpinCtrl(this, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535, 22);
    mUsername = new wxTextCtrl(this, wxID_ANY);
    mAuthentication = new wxChoice(this, kAuthenticationId);
    mAuthentication->Append(
        Translated(mTranslations, "siteManager.authentication.password"));
    mAuthentication->Append(
        Translated(mTranslations, "siteManager.authentication.privateKey"));
    mAuthentication->Append(
        Translated(mTranslations, "siteManager.authentication.agent"));
    mAuthentication->Append(
        Translated(mTranslations, "siteManager.authentication.keyboardInteractive"));
    mAuthentication->Append(
        Translated(mTranslations,
                   "siteManager.authentication.passwordKeyboardInteractive"));
    mAuthentication->SetSelection(
        AuthenticationIndex(AuthenticationKind::Password));
    mPassword = new wxTextCtrl(this, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
    mPassword->SetHint(
        Translated(mTranslations, "siteManager.passwordHint"));

    auto *keyPanel = new wxPanel(this);
    mPrivateKey = new wxTextCtrl(keyPanel, wxID_ANY);
    mBrowseKey = new wxButton(
        keyPanel,
        kBrowseKeyId,
        Translated(mTranslations, "common.browse"));

    auto *keyRow = new wxBoxSizer(wxHORIZONTAL);
    keyRow->Add(mPrivateKey, 1, wxEXPAND | wxRIGHT, 5);
    keyRow->Add(mBrowseKey, 0);
    keyPanel->SetSizer(keyRow);
    mPassphrase = new wxTextCtrl(this, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
    mPassphrase->SetHint(
        Translated(mTranslations, "siteManager.passphraseHint"));
    mLocalDirectory = new wxDirPickerCtrl(
        this,
        wxID_ANY,
        {},
        Translated(mTranslations, "siteManager.chooseLocalDirectory"),
        wxDefaultPosition,
        wxDefaultSize,
        wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST);

    SetPickerButtonLabel(*mLocalDirectory,
                         Translated(mTranslations, "common.browse"));

    mRemoteDirectory = new wxTextCtrl(this, wxID_ANY);
    mFtpEncoding = new wxTextCtrl(this, wxID_ANY, "UTF-8");
    mFtpDataConnectionMode = new wxChoice(this, kFtpDataConnectionModeId);
    mFtpDataConnectionMode->Append(Translated(
        mTranslations, "siteManager.ftpDataConnectionMode.passive"));
    mFtpDataConnectionMode->Append(Translated(
        mTranslations, "siteManager.ftpDataConnectionMode.active"));
    mFtpDataConnectionMode->SetSelection(
        FtpDataConnectionModeIndex(FtpDataConnectionMode::Passive));
    mFtpActiveAddress = new wxTextCtrl(this, wxID_ANY);
    mFtpActiveAddress->SetHint(Translated(
        mTranslations, "siteManager.ftpActiveAddressHint"));

    addField(Translated(mTranslations, "siteManager.field.name"), mName, false);
    addField(Translated(mTranslations, "siteManager.field.protocol"), mProtocol);
    addField(Translated(mTranslations, "siteManager.field.host"), mHost);
    addField(Translated(mTranslations, "siteManager.field.port"), mPort);
    addField(Translated(mTranslations, "siteManager.field.username"), mUsername);
    addField(Translated(mTranslations, "siteManager.field.authentication"), mAuthentication);

    mPasswordLabel = new wxStaticText(
        this,
        wxID_ANY,
        Translated(mTranslations, "siteManager.field.password"));
    fields->Add(mPasswordLabel, 0, wxALIGN_CENTER_VERTICAL);
    fields->Add(mPassword, 1, wxEXPAND);
    mSiteOnlyControls.push_back(mPasswordLabel);
    mSiteOnlyControls.push_back(mPassword);
    mPrivateKeyLabel = new wxStaticText(
        this,
        wxID_ANY,
        Translated(mTranslations, "siteManager.field.privateKey"));
    fields->Add(mPrivateKeyLabel, 0, wxALIGN_CENTER_VERTICAL);
    fields->Add(keyPanel, 1, wxEXPAND);
    mSiteOnlyControls.push_back(mPrivateKeyLabel);
    mSiteOnlyControls.push_back(keyPanel);
    mPassphraseLabel = new wxStaticText(
        this,
        wxID_ANY,
        Translated(mTranslations, "siteManager.field.keyPassphrase"));
    fields->Add(mPassphraseLabel, 0, wxALIGN_CENTER_VERTICAL);
    fields->Add(mPassphrase, 1, wxEXPAND);
    mSiteOnlyControls.push_back(mPassphraseLabel);
    mSiteOnlyControls.push_back(mPassphrase);
    addField(Translated(mTranslations, "siteManager.field.initialLocalDirectory"),
             mLocalDirectory);
    addField(Translated(mTranslations, "siteManager.field.initialRemoteDirectory"),
             mRemoteDirectory);
    mFtpEncodingLabel = new wxStaticText(
        this,
        wxID_ANY,
        Translated(mTranslations, "siteManager.field.ftpEncoding"));
    fields->Add(mFtpEncodingLabel, 0, wxALIGN_CENTER_VERTICAL);
    fields->Add(mFtpEncoding, 1, wxEXPAND);
    mSiteOnlyControls.push_back(mFtpEncodingLabel);
    mSiteOnlyControls.push_back(mFtpEncoding);
    mFtpDataConnectionModeLabel = new wxStaticText(
        this,
        wxID_ANY,
        Translated(mTranslations,
                   "siteManager.field.ftpDataConnectionMode"));
    fields->Add(mFtpDataConnectionModeLabel, 0, wxALIGN_CENTER_VERTICAL);
    fields->Add(mFtpDataConnectionMode, 1, wxEXPAND);
    mSiteOnlyControls.push_back(mFtpDataConnectionModeLabel);
    mSiteOnlyControls.push_back(mFtpDataConnectionMode);
    mFtpActiveAddressLabel = new wxStaticText(
        this,
        wxID_ANY,
        Translated(mTranslations,
                   "siteManager.field.ftpActiveAddress"));
    fields->Add(mFtpActiveAddressLabel, 0,
                wxALIGN_CENTER_VERTICAL);
    fields->Add(mFtpActiveAddress, 1, wxEXPAND);
    mSiteOnlyControls.push_back(mFtpActiveAddressLabel);
    mSiteOnlyControls.push_back(mFtpActiveAddress);

    body->Add(fields, 1, wxEXPAND | wxALL, 10);
    root->Add(body, 1, wxEXPAND);
    root->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    auto *actions = new wxBoxSizer(wxHORIZONTAL);
    mConnectButton = new wxButton(
        this, kConnectSiteId,
        Translated(mTranslations, "siteManager.connect"));

    auto *standardButtons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    const auto *buttonLayout =
        standardButtons->GetItem(standardButtons->GetAffirmativeButton());
    actions->AddStretchSpacer();
    actions->Add(mConnectButton, buttonLayout->GetProportion(),
                 buttonLayout->GetFlag(), buttonLayout->GetBorder());
    actions->Add(standardButtons, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(actions, 0, wxEXPAND | wxALL, 10);

    SetSizer(root);

    LocalizeStandardButtons(*this, mTranslations);

    // Measure the complete form before selection hides any fields, so folders
    // and different authentication methods share a stable resize limit.
    auto minimumSize = FromDIP(wxSize{790, 570});
    minimumSize.IncTo(root->ComputeFittingWindowSize(this));
    auto initialSize = FromDIP(wxSize{1100, 700});
    const wxDisplay display{GetParent() ? GetParent() : this};
    const auto workAreaSize = display.IsOk()
                                  ? display.GetClientArea().GetSize()
                                  : wxDefaultSize;

    if (workAreaSize.x > 0 && workAreaSize.y > 0)
    {
      minimumSize.DecTo(workAreaSize);
      initialSize.DecTo(workAreaSize);
    }

    initialSize.IncTo(minimumSize);

    SetMinSize(minimumSize);
    SetSize(initialSize);

    Bind(wxEVT_TREE_SEL_CHANGING,
         &SiteManagerDialog::OnSelectionChanging,
         this,
         kSiteTreeId);
    Bind(wxEVT_TREE_SEL_CHANGED,
         &SiteManagerDialog::OnSelectionChanged,
         this,
         kSiteTreeId);
    Bind(wxEVT_TREE_BEGIN_DRAG,
         &SiteManagerDialog::OnBeginDrag,
         this,
         kSiteTreeId);
    Bind(wxEVT_TREE_END_DRAG,
         &SiteManagerDialog::OnEndDrag,
         this,
         kSiteTreeId);
    Bind(wxEVT_BUTTON, &SiteManagerDialog::OnNewSite, this, kNewSiteId);
    Bind(wxEVT_BUTTON, &SiteManagerDialog::OnNewFolder, this, kNewFolderId);
    Bind(wxEVT_BUTTON, &SiteManagerDialog::OnDuplicate, this, kDuplicateSiteId);
    Bind(wxEVT_BUTTON, &SiteManagerDialog::OnDeleteSite, this, kDeleteSiteId);
    Bind(wxEVT_CHOICE, &SiteManagerDialog::OnProtocolChanged, this, kProtocolId);
    Bind(wxEVT_CHOICE,
         &SiteManagerDialog::OnFtpDataConnectionModeChanged,
         this,
         kFtpDataConnectionModeId);
    Bind(wxEVT_CHOICE, &SiteManagerDialog::OnAuthenticationChanged, this, kAuthenticationId);
    Bind(wxEVT_BUTTON, &SiteManagerDialog::OnBrowseKey, this, kBrowseKeyId);
    Bind(wxEVT_BUTTON, &SiteManagerDialog::OnImport, this, kImportSitesId);
    Bind(wxEVT_BUTTON, &SiteManagerDialog::OnExport, this, kExportSitesId);
    Bind(wxEVT_BUTTON, &SiteManagerDialog::OnAccept, this, wxID_OK);
    Bind(wxEVT_BUTTON, &SiteManagerDialog::OnAccept, this, kConnectSiteId);
  }

  void SiteManagerDialog::RefreshSiteTree(
      const std::optional<SelectedNode> select)
  {
    std::unordered_set<std::string> expandedFolders;

    const auto previousRoot = mSiteTree->GetRootItem();
    const bool firstPopulation = !previousRoot.IsOk();

    if (previousRoot.IsOk())
    {
      std::function<void(const wxTreeItemId &)> rememberExpanded;

      rememberExpanded = [&](const wxTreeItemId &parent)
      {
        wxTreeItemIdValue cookie;

        for (auto child = mSiteTree->GetFirstChild(parent, cookie);
             child.IsOk(); child = mSiteTree->GetNextChild(parent, cookie))
        {
          if (const auto node = NodeForItem(child);
              node && node->folder && mSiteTree->IsExpanded(child))
          {
            expandedFolders.insert(node->id);
          }

          rememberExpanded(child);
        }
      };

      rememberExpanded(previousRoot);
    }

    const auto wantedSelection = select ? select : mSelectedNode;

    mLoading = true;

    mSiteTree->Freeze();
    mSiteTree->DeleteAllItems();

    const auto root = mSiteTree->AddRoot({});

    wxTreeItemId selectedItem;
    std::unordered_set<std::string> insertedFolders;
    std::unordered_set<std::string> insertedSites;

    const auto appendSite = [&](const wxTreeItemId &parent,
                                const SiteProfile &site)
    {
      if (!insertedSites.insert(site.id).second)
      {
        return;
      }

      const auto item = mSiteTree->AppendItem(
          parent,
          site.name.empty()
              ? Translated(mTranslations, "siteManager.unnamedSite")
              : FromUtf8(site.name),
          1,
          1,
          new SiteTreeItemData(TreeItemKind::Site, site.id));

      if (wantedSelection && !wantedSelection->folder &&
          wantedSelection->id == site.id)
      {
        selectedItem = item;
      }
    };

    std::function<void(const wxTreeItemId &, std::string_view)> appendChildren;
    std::function<void(const wxTreeItemId &, const config::SiteFolder &)>
        appendFolder;
    appendFolder = [&](const wxTreeItemId &parent,
                       const config::SiteFolder &folder)
    {
      if (!insertedFolders.insert(folder.id).second)
      {
        return;
      }

      const auto item = mSiteTree->AppendItem(
          parent,
          FromUtf8(folder.name),
          0,
          0,
          new SiteTreeItemData(TreeItemKind::Folder, folder.id));

      if (wantedSelection && wantedSelection->folder &&
          wantedSelection->id == folder.id)
      {
        selectedItem = item;
      }

      appendChildren(item, folder.id);

      if (firstPopulation || expandedFolders.contains(folder.id))
      {
        mSiteTree->Expand(item);
      }
    };

    appendChildren = [&](const wxTreeItemId &parent,
                         const std::string_view parentFolderId)
    {
      for (const auto &entry : OrderedSiteManagerChildren(
               mSites, mFolders, mSiteManagerOrder, parentFolderId))
      {
        if (entry.kind == SiteManagerEntryKind::Folder)
        {
          const auto folder = std::ranges::find(
              mFolders, entry.id, &config::SiteFolder::id);

          if (folder != mFolders.end())
          {
            appendFolder(parent, *folder);
          }

          continue;
        }

        const auto site = std::ranges::find(mSites, entry.id,
                                            &SiteProfile::id);
        if (site != mSites.end())
        {
          appendSite(parent, *site);
        }
      }
    };

    appendChildren(root, {});

    // Show orphaned nodes at the root of the working tree
    for (const auto &folder : mFolders)
    {
      if (!insertedFolders.contains(folder.id))
      {
        appendFolder(root, folder);
      }
    }

    for (const auto &site : mSites)
    {
      if (!insertedSites.contains(site.id))
      {
        appendSite(root, site);
      }
    }

    if (!selectedItem.IsOk())
    {
      wxTreeItemIdValue cookie;
      selectedItem = mSiteTree->GetFirstChild(root, cookie);
    }

    if (selectedItem.IsOk())
    {
      mSiteTree->SelectItem(selectedItem);
      mSiteTree->EnsureVisible(selectedItem);
      mSelectedNode = NodeForItem(selectedItem);
    }
    else
    {
      mSelectedNode.reset();
    }

    mSiteTree->Thaw();

    mLoading = false;

    if (mSelectedNode)
    {
      LoadSelection(*mSelectedNode);
    }
    else
    {
      mName->ChangeValue({});
      mName->Enable(false);

      ClearSiteFields();

      SetSiteControlsVisible(false);

      SetTitle(Translated(mTranslations, "siteManager.title"));
    }

    UpdateActionButtons();
  }

  void SiteManagerDialog::LoadSelection(const SelectedNode &selection)
  {
    mLoading = true;

    mName->Enable(true);

    if (selection.folder)
    {
      const auto folder = std::ranges::find(mFolders, selection.id,
                                            &config::SiteFolder::id);

      if (folder == mFolders.end())
      {
        mLoading = false;

        return;
      }

      mName->ChangeValue(FromUtf8(folder->name));

      ClearSiteFields();

      SetSiteControlsVisible(false);
      SetTitle(Translated(mTranslations, "siteManager.title"));

      mLoading = false;

      UpdateActionButtons();

      return;
    }

    const auto found = std::ranges::find(mSites, selection.id,
                                         &SiteProfile::id);
    if (found == mSites.end())
    {
      mLoading = false;

      return;
    }

    const auto &site = *found;

    SetSiteControlsVisible(true);

    mName->ChangeValue(FromUtf8(site.name));
    mProtocol->SetSelection(ProtocolIndex(site.protocol));
    mHost->ChangeValue(FromUtf8(site.host));
    mPort->SetValue(site.port);
    mUsername->ChangeValue(FromUtf8(site.username));
    mAuthentication->SetSelection(AuthenticationIndex(site.authentication.kind));
    mPassword->ChangeValue({});
    mPrivateKey->ChangeValue(platform::ToToolkitPath(site.authentication.privateKeyFile));
    mPassphrase->ChangeValue({});
    mLocalDirectory->SetPath(platform::ToToolkitPath(site.initialLocalDirectory));
    mRemoteDirectory->ChangeValue(FromUtf8(site.initialRemoteDirectory.Bytes()));
    mFtpEncoding->ChangeValue(FromUtf8(site.ftpEncoding));
    mFtpDataConnectionMode->SetSelection(
        FtpDataConnectionModeIndex(site.ftpDataConnectionMode));
    mFtpActiveAddress->ChangeValue(FromUtf8(site.ftpActiveAddress));

    if (const auto pending = mPendingSecrets.find(site.id); pending != mPendingSecrets.end())
    {
      mPassword->ChangeValue(FromUtf8(pending->second.password));
      mPassphrase->ChangeValue(FromUtf8(pending->second.passphrase));
    }

    UpdateProtocolControls(false);
    UpdateAuthenticationControls();

    mLoading = false;

    UpdateActionButtons();
  }

  bool SiteManagerDialog::CaptureSelection(const bool showErrors)
  {
    if (!mSelectedNode)
    {
      return true;
    }

    const std::string name = ToUtf8(mName->GetValue().Trim(true).Trim(false));

    if (mSelectedNode->folder)
    {
      const auto folder = std::ranges::find(mFolders, mSelectedNode->id,
                                            &config::SiteFolder::id);
      if (folder == mFolders.end())
      {
        return true;
      }

      if (name.empty())
      {
        if (showErrors)
        {
          LocalizedMessageBox(
              mTranslations,
              Translated(mTranslations, "siteManager.folderRequiredMessage"),
              Translated(mTranslations, "siteManager.invalidTitle"),
              wxOK | wxICON_WARNING,
              this);

          mName->SetFocus();
        }

        return false;
      }

      if (!IsFolderNameUnique(mFolders, mSites, name, folder->parentId,
                              folder->id))
      {
        if (showErrors)
        {
          LocalizedMessageBox(
              mTranslations,
              Translated(mTranslations,
                         "siteManager.duplicateFolderNameMessage"),
              Translated(mTranslations, "siteManager.invalidTitle"),
              wxOK | wxICON_WARNING,
              this);

          mName->SetFocus();
          mName->SelectAll();
        }

        return false;
      }

      folder->name = name;

      const auto item = mSiteTree->GetSelection();
      if (item.IsOk())
      {
        mSiteTree->SetItemText(item, FromUtf8(name));
      }

      return true;
    }

    const auto found = std::ranges::find(mSites, mSelectedNode->id,
                                         &SiteProfile::id);
    if (found == mSites.end())
    {
      return true;
    }

    auto &site = *found;

    const std::string host = ToUtf8(mHost->GetValue().Trim(true).Trim(false));
    if (name.empty() || host.empty())
    {
      if (showErrors)
      {
        LocalizedMessageBox(mTranslations, Translated(mTranslations, "siteManager.requiredMessage"),
                            Translated(mTranslations, "siteManager.invalidTitle"),
                            wxOK | wxICON_WARNING,
                            this);
      }

      return false;
    }

    if (!IsValidEndpointHost(host))
    {
      if (showErrors)
      {
        LocalizedMessageBox(mTranslations, Translated(mTranslations, "siteManager.invalidHostMessage"),
                            Translated(mTranslations, "siteManager.invalidTitle"),
                            wxOK | wxICON_WARNING,
                            this);
      }

      return false;
    }

    const auto selectedProtocol = ProtocolAt(mProtocol->GetSelection());
    const auto selectedFtpDataConnectionMode =
        selectedProtocol == ProtocolKind::Sftp
            ? FtpDataConnectionMode::Passive
            : FtpDataConnectionModeAt(
                  mFtpDataConnectionMode->GetSelection());
    const std::string ftpActiveAddress =
        ToUtf8(mFtpActiveAddress->GetValue());

    if (selectedProtocol != ProtocolKind::Sftp &&
        !ftpActiveAddress.empty() &&
        !IsValidIpAddress(ftpActiveAddress))
    {
      if (showErrors)
      {
        LocalizedMessageBox(
            mTranslations,
            Translated(mTranslations,
                       "siteManager.invalidFtpActiveAddressMessage"),
            Translated(mTranslations, "siteManager.invalidTitle"),
            wxOK | wxICON_WARNING,
            this);

        mFtpActiveAddress->SetFocus();
        mFtpActiveAddress->SelectAll();
      }

      return false;
    }

    if (selectedFtpDataConnectionMode == FtpDataConnectionMode::Active &&
        !ftpActiveAddress.empty() &&
        !IsAssignedLocalIpAddress(ftpActiveAddress))
    {
      if (showErrors)
      {
        LocalizedMessageBox(
            mTranslations,
            Translated(mTranslations,
                       "siteManager.invalidFtpActiveAddressMessage"),
            Translated(mTranslations, "siteManager.invalidTitle"),
            wxOK | wxICON_WARNING,
            this);

        mFtpActiveAddress->SetFocus();
        mFtpActiveAddress->SelectAll();
      }

      return false;
    }

    if (!IsSiteNameUniqueInTree(mSites, mFolders, name,
                                SiteFolderId(mFolders, site.id), site.id))
    {
      if (showErrors)
      {
        LocalizedMessageBox(
            mTranslations,
            Translated(mTranslations, "siteManager.duplicateNameMessage"),
            Translated(mTranslations, "siteManager.invalidTitle"),
            wxOK | wxICON_WARNING,
            this);

        mName->SetFocus();
        mName->SelectAll();
      }

      return false;
    }

    site.name = name;
    site.protocol = selectedProtocol;
    site.host = host;
    site.port = static_cast<std::uint16_t>(mPort->GetValue());
    site.username = ToUtf8(mUsername->GetValue());
    site.authentication.kind = site.protocol == ProtocolKind::Sftp
                                   ? AuthenticationAt(mAuthentication->GetSelection())
                                   : AuthenticationKind::Password;
    site.authentication.privateKeyFile =
        platform::FromToolkitPath(mPrivateKey->GetValue());

    const bool usesPassword =
        site.authentication.kind == AuthenticationKind::Password ||
        site.authentication.kind == AuthenticationKind::PasswordKeyboardInteractive;
    if (!usesPassword)
    {
      site.authentication.credentialId.clear();
    }

    if (site.authentication.kind != AuthenticationKind::PrivateKey)
    {
      site.authentication.privateKeyFile.clear();
      site.authentication.publicKeyFile.clear();
      site.authentication.passphraseCredentialId.clear();
    }

    site.initialLocalDirectory =
        platform::FromToolkitPath(mLocalDirectory->GetPath());
    site.initialRemoteDirectory = RemotePath{ToUtf8(mRemoteDirectory->GetValue())};
    site.ftpEncoding = ToUtf8(mFtpEncoding->GetValue());

    if (site.ftpEncoding.empty())
    {
      site.ftpEncoding = "UTF-8";
    }

    site.ftpDataConnectionMode = selectedFtpDataConnectionMode;
    site.ftpActiveAddress =
        site.protocol == ProtocolKind::Sftp
            ? std::string{}
            : ftpActiveAddress;

    auto &pending = mPendingSecrets[site.id];
    auto password = usesPassword
                        ? ToUtf8(mPassword->GetValue())
                        : std::string{};
    auto passphrase = site.authentication.kind == AuthenticationKind::PrivateKey
                          ? ToUtf8(mPassphrase->GetValue())
                          : std::string{};

    ClearSecret(pending.password);
    ClearSecret(pending.passphrase);

    pending.password = std::move(password);
    pending.passphrase = std::move(passphrase);

    const auto item = mSiteTree->GetSelection();
    if (item.IsOk())
    {
      mSiteTree->SetItemText(item, FromUtf8(site.name));
    }

    return true;
  }

  void SiteManagerDialog::ClearSiteFields()
  {
    mProtocol->SetSelection(wxNOT_FOUND);
    mHost->ChangeValue({});
    mPort->SetValue(DefaultPort(ProtocolKind::Sftp));
    mUsername->ChangeValue({});
    mAuthentication->SetSelection(wxNOT_FOUND);
    mPassword->ChangeValue({});
    mPrivateKey->ChangeValue({});
    mPassphrase->ChangeValue({});
    mLocalDirectory->SetPath({});
    mRemoteDirectory->ChangeValue({});
    mFtpEncoding->ChangeValue({});
    mFtpDataConnectionMode->SetSelection(
        FtpDataConnectionModeIndex(FtpDataConnectionMode::Passive));
    mFtpActiveAddress->ChangeValue({});
  }

  void SiteManagerDialog::SetSiteControlsVisible(const bool visible)
  {
    for (auto *control : mSiteOnlyControls)
    {
      control->Show(visible);
    }

    Layout();
  }

  void SiteManagerDialog::UpdateActionButtons()
  {
    mDuplicateButton->Enable(mSelectedNode.has_value());
    mDeleteButton->Enable(mSelectedNode.has_value());
    mConnectButton->Enable(mSelectedNode && !mSelectedNode->folder);
    mExportButton->Enable(!mSites.empty() || !mFolders.empty());
  }

  std::optional<SiteManagerDialog::SelectedNode>
  SiteManagerDialog::NodeForItem(const wxTreeItemId &item) const
  {
    if (!item.IsOk())
    {
      return std::nullopt;
    }

    const auto *data = dynamic_cast<const SiteTreeItemData *>(
        mSiteTree->GetItemData(item));
    if (data == nullptr)
    {
      return std::nullopt;
    }

    return SelectedNode{data->kind == TreeItemKind::Folder, data->id};
  }

  std::string SiteManagerDialog::SelectedParentFolderId() const
  {
    if (!mSelectedNode)
    {
      return {};
    }

    if (mSelectedNode->folder)
    {
      return mSelectedNode->id;
    }

    return std::string{SiteFolderId(mFolders, mSelectedNode->id)};
  }

  void SiteManagerDialog::PreparePendingCredentialIds()
  {
    for (auto &site : mSites)
    {
      const auto pending = mPendingSecrets.find(site.id);
      if (pending == mPendingSecrets.end())
      {
        continue;
      }

      site.authentication = PrepareCredentialIdentifiers(
          site.authentication,
          !pending->second.password.empty(),
          !pending->second.passphrase.empty());
    }
  }

  void SiteManagerDialog::ClearPendingSecrets() noexcept
  {
    for (auto &[siteId, pending] : mPendingSecrets)
    {
      (void)siteId;

      ClearSecret(pending.password);
      ClearSecret(pending.passphrase);
    }

    mPendingSecrets.clear();
  }

  std::vector<PendingCredentialUpdate> SiteManagerDialog::TakePendingCredentialUpdates()
  {
    std::vector<PendingCredentialUpdate> updates;

    for (const auto &site : mSites)
    {
      const auto pending = mPendingSecrets.find(site.id);
      if (pending == mPendingSecrets.end())
      {
        continue;
      }

      if (!pending->second.password.empty())
      {
        updates.emplace_back(
            site.authentication.credentialId,
            site.username.empty() && site.protocol != ProtocolKind::Sftp ? "anonymous"
                                                                         : site.username,
            CredentialKind::Password,
            SecretBytes(pending->second.password));
      }

      if (!pending->second.passphrase.empty())
      {
        updates.emplace_back(site.authentication.passphraseCredentialId,
                             site.username,
                             CredentialKind::PrivateKeyPassphrase,
                             SecretBytes(pending->second.passphrase));
      }
    }

    ClearPendingSecrets();

    return updates;
  }

  void SiteManagerDialog::UpdateAuthenticationControls()
  {
    if (!mSelectedNode || mSelectedNode->folder)
    {
      return;
    }

    const auto authentication = AuthenticationAt(mAuthentication->GetSelection());
    const bool password =
        authentication == AuthenticationKind::Password ||
        authentication == AuthenticationKind::PasswordKeyboardInteractive;
    const bool privateKey = authentication == AuthenticationKind::PrivateKey;

    mPasswordLabel->Show(password);
    mPassword->Show(password);
    mPrivateKeyLabel->Show(privateKey);
    mPrivateKey->GetParent()->Show(privateKey);
    mPassphraseLabel->Show(privateKey);
    mPassphrase->Show(privateKey);

    Layout();
  }

  void SiteManagerDialog::UpdateProtocolControls(const bool chooseDefaultPort)
  {
    if (!mSelectedNode || mSelectedNode->folder)
    {
      return;
    }

    const auto protocol = ProtocolAt(mProtocol->GetSelection());

    const bool ftp = protocol != ProtocolKind::Sftp;
    if (ftp)
    {
      mAuthentication->SetSelection(AuthenticationIndex(AuthenticationKind::Password));
    }

    mAuthentication->Enable(!ftp);
    mFtpEncodingLabel->Show(ftp);
    mFtpEncoding->Show(ftp);
    mFtpDataConnectionModeLabel->Show(ftp);
    mFtpDataConnectionMode->Show(ftp);

    const bool active =
        ftp && FtpDataConnectionModeAt(
                   mFtpDataConnectionMode->GetSelection()) ==
                   FtpDataConnectionMode::Active;

    mFtpActiveAddressLabel->Show(active);
    mFtpActiveAddress->Show(active);

    if (!ftp)
    {
      mFtpDataConnectionMode->SetSelection(
          FtpDataConnectionModeIndex(FtpDataConnectionMode::Passive));
      mFtpActiveAddress->ChangeValue({});
    }

    if (chooseDefaultPort)
    {
      mPort->SetValue(DefaultPort(protocol));
    }

    if (!mLoading && protocol == ProtocolKind::Ftp)
    {
      SetTitle(Translated(mTranslations, "siteManager.insecureTitle"));
    }
    else
    {
      SetTitle(Translated(mTranslations, "siteManager.title"));
    }

    UpdateAuthenticationControls();

    Layout();
  }

  void SiteManagerDialog::OnFtpDataConnectionModeChanged(wxCommandEvent &event)
  {
    UpdateProtocolControls(false);

    event.Skip();
  }

  void SiteManagerDialog::OnSelectionChanging(wxTreeEvent &event)
  {
    if (mLoading)
    {
      return;
    }

    if (!CaptureSelection(true))
    {
      event.Veto();

      return;
    }

    event.Skip();
  }

  void SiteManagerDialog::OnSelectionChanged(wxTreeEvent &event)
  {
    if (mLoading)
    {
      return;
    }

    mSelectedNode = NodeForItem(event.GetItem());

    if (mSelectedNode)
    {
      LoadSelection(*mSelectedNode);
    }
    else
    {
      mName->ChangeValue({});
      mName->Enable(false);

      ClearSiteFields();

      SetSiteControlsVisible(false);

      SetTitle(Translated(mTranslations, "siteManager.title"));

      UpdateActionButtons();
    }

    event.Skip();
  }

  void SiteManagerDialog::OnBeginDrag(wxTreeEvent &event)
  {
    if (mLoading || !CaptureSelection(true))
    {
      return;
    }

    mDraggedNode = NodeForItem(event.GetItem());

    if (mDraggedNode)
    {
      event.Allow();
    }
  }

  void SiteManagerDialog::OnEndDrag(wxTreeEvent &event)
  {
    if (!mDraggedNode)
    {
      return;
    }

    const auto dragged = std::exchange(mDraggedNode, std::nullopt);

    const auto target = NodeForItem(event.GetItem());

    auto position = SiteManagerDropPosition::Into;

    if (target)
    {
      position = target->folder ? SiteManagerDropPosition::Into
                                : SiteManagerDropPosition::After;

      wxRect bounds;

      if (mSiteTree->GetBoundingRect(event.GetItem(), bounds, false) &&
          bounds.height > 0)
      {
        const auto offset = event.GetPoint().y - bounds.y;

        if (target->folder)
        {
          if (offset * 3 < bounds.height)
          {
            position = SiteManagerDropPosition::Before;
          }
          else if (offset * 3 >= bounds.height * 2)
          {
            position = SiteManagerDropPosition::After;
          }
        }
        else
        {
          position = offset * 2 < bounds.height
                         ? SiteManagerDropPosition::Before
                         : SiteManagerDropPosition::After;
        }
      }
    }

    const auto result = MoveSiteManagerEntry(
        mSites, mFolders, mSiteManagerOrder, dragged->id,
        target ? std::string_view{target->id} : std::string_view{}, position);

    if (result == SiteManagerMoveResult::Moved)
    {
      RefreshSiteTree(*dragged);

      return;
    }

    if (result == SiteManagerMoveResult::NameConflict)
    {
      LocalizedMessageBox(
          mTranslations,
          Translated(mTranslations,
                     dragged->folder
                         ? "siteManager.duplicateFolderNameMessage"
                         : "siteManager.duplicateNameMessage"),
          Translated(mTranslations, "siteManager.invalidTitle"),
          wxOK | wxICON_WARNING,
          this);

      return;
    }

    if (result != SiteManagerMoveResult::NoChange)
    {
      wxBell();
    }
  }

  void SiteManagerDialog::OnNewSite(wxCommandEvent &)
  {
    if (!CaptureSelection(true))
    {
      return;
    }

    const auto parentFolderId = SelectedParentFolderId();

    SiteProfile site;
    site.id = GenerateId();
    site.name = NextAvailableSiteNameInTree(
        mSites, mFolders, parentFolderId,
        mTranslations.Text("siteManager.defaultName"));
    site.protocol = ProtocolKind::Sftp;
    site.port = DefaultPort(site.protocol);
    site.initialRemoteDirectory = RemotePath::Root();

    const auto siteId = site.id;

    mSites.push_back(std::move(site));

    AssignSiteToFolder(mFolders, siteId, parentFolderId);

    mSiteManagerOrder.push_back(siteId);

    if (mSelectedNode)
    {
      (void)MoveSiteManagerEntry(
          mSites, mFolders, mSiteManagerOrder, siteId, mSelectedNode->id,
          mSelectedNode->folder ? SiteManagerDropPosition::Into
                                : SiteManagerDropPosition::After);
    }

    RefreshSiteTree(SelectedNode{false, siteId});

    mName->SetFocus();
    mName->SelectAll();
  }

  void SiteManagerDialog::OnNewFolder(wxCommandEvent &)
  {
    if (!CaptureSelection(true))
    {
      return;
    }

    const auto parentFolderId = SelectedParentFolderId();

    config::SiteFolder folder;
    folder.id = GenerateId();
    folder.name = NextAvailableFolderName(
        mFolders, mSites, parentFolderId,
        mTranslations.Text("siteManager.defaultFolderName"));
    folder.parentId = parentFolderId;

    const auto folderId = folder.id;

    mFolders.push_back(std::move(folder));

    mSiteManagerOrder.push_back(folderId);

    if (mSelectedNode)
    {
      (void)MoveSiteManagerEntry(
          mSites, mFolders, mSiteManagerOrder, folderId, mSelectedNode->id,
          mSelectedNode->folder ? SiteManagerDropPosition::Into
                                : SiteManagerDropPosition::After);
    }

    RefreshSiteTree(SelectedNode{true, folderId});

    mName->SetFocus();
    mName->SelectAll();
  }

  void SiteManagerDialog::OnDuplicate(wxCommandEvent &)
  {
    if (!mSelectedNode || !CaptureSelection(true))
    {
      return;
    }

    if (mSelectedNode->folder)
    {
      auto cloned = CloneSiteFolderTree(
          mSites, mFolders, mSelectedNode->id,
          mTranslations.Text("siteManager.copySuffix"),
          mSiteManagerOrder);
      if (!cloned)
      {
        wxBell();

        return;
      }

      const auto clonedRootId = cloned->rootFolderId;

      for (auto &site : cloned->sites)
      {
        mSites.push_back(std::move(site));
      }

      for (auto &folder : cloned->folders)
      {
        mFolders.push_back(std::move(folder));
      }

      const auto insertion = std::ranges::find(mSiteManagerOrder,
                                               mSelectedNode->id);
      mSiteManagerOrder.insert(
          insertion == mSiteManagerOrder.end() ? mSiteManagerOrder.end()
                                               : std::next(insertion),
          cloned->siteManagerOrder.begin(), cloned->siteManagerOrder.end());

      RefreshSiteTree(SelectedNode{true, clonedRootId});

      mName->SetFocus();
      mName->SelectAll();

      return;
    }

    const auto source = std::ranges::find(mSites, mSelectedNode->id,
                                          &SiteProfile::id);
    if (source == mSites.end())
    {
      return;
    }

    const auto parentFolderId =
        std::string{SiteFolderId(mFolders, source->id)};

    std::string copyBase = source->name;
    copyBase += mTranslations.Text("siteManager.copySuffix");
    const auto copyName = NextAvailableSiteNameInTree(
        mSites, mFolders, parentFolderId, copyBase);
    auto clone = CloneSiteProfile(*source, GenerateId(), copyName);
    const auto cloneId = clone.id;

    mSites.push_back(std::move(clone));

    AssignSiteToFolder(mFolders, cloneId, parentFolderId);

    const auto insertion = std::ranges::find(mSiteManagerOrder,
                                             mSelectedNode->id);
    mSiteManagerOrder.insert(
        insertion == mSiteManagerOrder.end() ? mSiteManagerOrder.end()
                                             : std::next(insertion),
        cloneId);

    RefreshSiteTree(SelectedNode{false, cloneId});

    mName->SetFocus();
    mName->SelectAll();
  }

  void SiteManagerDialog::OnDeleteSite(wxCommandEvent &)
  {
    if (!mSelectedNode)
    {
      return;
    }

    const bool deletingFolder = mSelectedNode->folder;

    if (LocalizedMessageBox(mTranslations,
                            Translated(mTranslations,
                                       deletingFolder
                                           ? "siteManager.deleteFolderConfirm"
                                           : "siteManager.deleteConfirm"),
                            Translated(mTranslations,
                                       deletingFolder
                                           ? "siteManager.deleteFolderTitle"
                                           : "siteManager.deleteTitle"),
                            wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
                            this) != wxID_YES)
    {
      return;
    }

    const auto wipePendingSecret = [&](const std::string &siteId)
    {
      if (const auto pending = mPendingSecrets.find(siteId);
          pending != mPendingSecrets.end())
      {
        ClearSecret(pending->second.password);
        ClearSecret(pending->second.passphrase);

        mPendingSecrets.erase(pending);
      }
    };

    std::optional<SelectedNode> nextSelection;

    if (!deletingFolder)
    {
      const auto siteId = mSelectedNode->id;
      const auto parentId = std::string{SiteFolderId(mFolders, siteId)};

      if (!parentId.empty())
      {
        nextSelection = SelectedNode{true, parentId};
      }

      wipePendingSecret(siteId);

      AssignSiteToFolder(mFolders, siteId, {});

      std::erase_if(mSites, [&](const SiteProfile &site)
                    { return site.id == siteId; });

      std::erase(mSiteManagerOrder, siteId);

      RefreshSiteTree(nextSelection);

      return;
    }

    const auto folder = std::ranges::find(mFolders, mSelectedNode->id,
                                          &config::SiteFolder::id);
    if (folder == mFolders.end())
    {
      return;
    }
    if (!folder->parentId.empty())
    {
      nextSelection = SelectedNode{true, folder->parentId};
    }

    std::unordered_set<std::string> removedFolderIds;

    for (const auto &candidate : mFolders)
    {
      if (IsFolderDescendantOf(mFolders, candidate.id, folder->id))
      {
        removedFolderIds.insert(candidate.id);
      }
    }

    std::unordered_set<std::string> removedSiteIds;

    for (const auto &candidate : mFolders)
    {
      if (!removedFolderIds.contains(candidate.id))
      {
        continue;
      }

      removedSiteIds.insert(candidate.siteIds.begin(), candidate.siteIds.end());
    }

    for (const auto &siteId : removedSiteIds)
    {
      wipePendingSecret(siteId);

      AssignSiteToFolder(mFolders, siteId, {});
    }

    std::erase_if(mSites, [&](const SiteProfile &site)
                  { return removedSiteIds.contains(site.id); });
    std::erase_if(mFolders, [&](const config::SiteFolder &candidate)
                  { return removedFolderIds.contains(candidate.id); });
    std::erase_if(mSiteManagerOrder, [&](const std::string &id)
                  { return removedFolderIds.contains(id) ||
                           removedSiteIds.contains(id); });

    RefreshSiteTree(nextSelection);
  }

  void SiteManagerDialog::OnProtocolChanged(wxCommandEvent &)
  {
    UpdateProtocolControls(true);
  }

  void SiteManagerDialog::OnAuthenticationChanged(wxCommandEvent &)
  {
    UpdateAuthenticationControls();
  }

  void SiteManagerDialog::OnBrowseKey(wxCommandEvent &)
  {
    wxFileDialog dialog(this,
                        Translated(mTranslations, "siteManager.choosePrivateKey"),
                        {},
                        {},
                        Translated(mTranslations, "siteManager.privateKeyFilter"),
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    LocalizeStandardButtons(dialog, mTranslations);

    dialog.CentreOnParent();

    if (dialog.ShowModal() == wxID_OK)
    {
      mPrivateKey->ChangeValue(dialog.GetPath());
    }
  }

  void SiteManagerDialog::OnImport(wxCommandEvent &)
  {
    if (!CaptureSelection(true))
    {
      return;
    }

    wxFileDialog fileDialog(
        this, Translated(mTranslations, "siteManager.importTitle"), {}, {},
        Translated(mTranslations, "siteManager.transferFileFilter"),
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    LocalizeStandardButtons(fileDialog, mTranslations);

    fileDialog.CentreOnParent();

    if (fileDialog.ShowModal() != wxID_OK)
    {
      return;
    }

    const auto imported = config::ReadSiteExport(
        platform::FromToolkitPath(fileDialog.GetPath()));
    if (!imported)
    {
      LocalizedMessageBox(mTranslations, SiteTransferErrorMessage(imported.error()),
                          Translated(mTranslations, "siteManager.importFailedTitle"),
                          wxOK | wxICON_ERROR, this);

      return;
    }

    const auto destination = SelectedParentFolderId();

    auto plan = PlanSiteImport({mSites, mFolders, mSiteManagerOrder},
                               *imported, destination);
    if (!plan)
    {
      const auto messageKey = plan.error() == SiteManagerImportError::IdentityCollision
                                  ? "siteManager.importIdCollision"
                              : plan.error() == SiteManagerImportError::InvalidDestination
                                  ? "siteManager.importInvalidTarget"
                                  : "siteManager.importInvalidData";

      LocalizedMessageBox(mTranslations,
                          fileDialog.GetPath() + "\n\n" + Translated(mTranslations, messageKey),
                          Translated(mTranslations, "siteManager.importFailedTitle"),
                          wxOK | wxICON_ERROR, this);

      return;
    }

    std::string destinationName{mTranslations.Text("siteManager.importRoot")};
    if (!destination.empty())
    {
      destinationName.clear();

      std::string current = destination;

      for (std::size_t depth = 0; !current.empty() && depth < mFolders.size(); ++depth)
      {
        const auto folder = std::ranges::find(mFolders, current, &config::SiteFolder::id);
        if (folder == mFolders.end())
        {
          break;
        }

        destinationName = folder->name +
                          (destinationName.empty() ? std::string{} : " / " + destinationName);

        current = folder->parentId;
      }
    }

    auto message = FromUtf8(mTranslations.Format(
        "siteManager.importConfirmMessage",
        {std::to_string(imported->sites.size()),
         std::to_string(imported->folders.size()), destinationName}));

    if (!plan->renames.empty())
    {
      message += "\n\n" + Translated(mTranslations, "siteManager.importConflicts") + "\n";

      for (const auto &rename : plan->renames)
      {
        message += FromUtf8(rename.originalName + " → " + rename.proposedName) + "\n";
      }
    }

    // Nothing from the import is applied before confirmation
    wxDialog confirmation(this, wxID_ANY,
                          Translated(mTranslations, "siteManager.importConfirmTitle"),
                          wxDefaultPosition, wxDefaultSize,
                          wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    auto *layout = new wxBoxSizer(wxVERTICAL);
    auto *preview = new wxTextCtrl(&confirmation, wxID_ANY, message,
                                   wxDefaultPosition, FromDIP(wxSize{640, 320}),
                                   wxTE_MULTILINE | wxTE_READONLY);
    preview->SetInsertionPoint(0);

    layout->Add(preview, 1, wxEXPAND | wxALL, FromDIP(10));
    layout->Add(confirmation.CreateStdDialogButtonSizer(wxOK | wxCANCEL),
                0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    LocalizeStandardButtons(confirmation, mTranslations);

    if (auto *button = wxWindow::FindWindowById(wxID_OK, &confirmation))
    {
      button->SetLabel(Translated(mTranslations, plan->renames.empty()
                                                     ? "siteManager.importConfirmAction"
                                                     : "siteManager.importKeepBoth"));
      button->SetMinSize(button->GetBestSize());
    }

    confirmation.SetSizerAndFit(layout);
    confirmation.SetMinSize(confirmation.GetSize());
    confirmation.CentreOnParent();

    if (confirmation.ShowModal() != wxID_OK)
    {
      return;
    }

    auto selection = mSelectedNode;

    if (!plan->importedRoots.empty())
    {
      selection = SelectedNode{plan->importedRoots.front().kind == SiteManagerEntryKind::Folder,
                               plan->importedRoots.front().id};
    }

    mSites = std::move(plan->merged.sites);
    mFolders = std::move(plan->merged.folders);
    mSiteManagerOrder = std::move(plan->merged.siteManagerOrder);

    RefreshSiteTree(std::move(selection));
  }

  void SiteManagerDialog::OnExport(wxCommandEvent &)
  {
    if (!CaptureSelection(true))
    {
      return;
    }

    wxMenu menu;

    auto *selected = menu.Append(
        wxID_ANY, Translated(mTranslations, "siteManager.exportSelected"));
    selected->Enable(mSelectedNode.has_value());
    const auto selection = mSelectedNode;

    menu.Bind(wxEVT_MENU, [this, selection](wxCommandEvent &)
              {
                if (selection)
                {
                  ExportSites(selection->id);
                } }, selected->GetId());

    auto *all = menu.Append(wxID_ANY,
                            Translated(mTranslations, "siteManager.exportAll"));
    all->Enable(!mSites.empty() || !mFolders.empty());

    menu.Bind(wxEVT_MENU, [this](wxCommandEvent &)
              { ExportSites(std::nullopt); }, all->GetId());

    PopupMenu(&menu, mExportButton->GetPosition() +
                         wxPoint{0, mExportButton->GetSize().y});
  }

  void SiteManagerDialog::ExportSites(std::optional<std::string> selectedRootId)
  {
    if (!CaptureSelection(true))
    {
      return;
    }

    wxFileDialog dialog(this, Translated(mTranslations, "siteManager.exportTitle"),
                        {}, "havRemote-sites.cson",
                        Translated(mTranslations, "siteManager.transferFileFilter"),
                        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

    LocalizeStandardButtons(dialog, mTranslations);

    dialog.CentreOnParent();
    if (dialog.ShowModal() != wxID_OK)
    {
      return;
    }

    const auto path = platform::FromToolkitPath(dialog.GetPath());
    if (platform::IsProtectedExportPath(path, mProtectedExportPaths))
    {
      LocalizedMessageBox(mTranslations,
                          Translated(mTranslations, "export.protectedPathMessage"),
                          Translated(mTranslations, "siteManager.exportFailedTitle"),
                          wxOK | wxICON_ERROR, this);

      return;
    }

    const auto exported = config::WriteSiteExport(
        path, {mSites, mFolders, mSiteManagerOrder}, std::move(selectedRootId));
    if (!exported)
    {
      LocalizedMessageBox(mTranslations, SiteTransferErrorMessage(exported.error()),
                          Translated(mTranslations, "siteManager.exportFailedTitle"),
                          wxOK | wxICON_ERROR, this);
    }
  }

  void SiteManagerDialog::OnAccept(wxCommandEvent &event)
  {
    mConnectionSiteId.reset();

    const bool connect = event.GetId() == kConnectSiteId;
    if (connect &&
        (!mSelectedNode || mSelectedNode->folder ||
         std::ranges::find(mSites, mSelectedNode->id, &SiteProfile::id) == mSites.end()))
    {
      return;
    }

    if (!CaptureSelection(true))
    {
      return;
    }

    for (const auto &site : mSites)
    {
      if (site.authentication.kind == AuthenticationKind::PrivateKey &&
          site.authentication.privateKeyFile.empty())
      {
        const auto id = site.id;
        const auto message = FromUtf8(mTranslations.Format(
            "siteManager.privateKeyRequiredMessage", {site.name}));

        RefreshSiteTree(SelectedNode{false, id});

        LocalizedMessageBox(mTranslations, message,
                            Translated(mTranslations, "siteManager.privateKeyRequiredTitle"),
                            wxOK | wxICON_WARNING, this);

        mPrivateKey->SetFocus();

        return;
      }
    }

    if (const auto duplicate = DuplicateSiteNameIndex(mSites, mFolders))
    {
      RefreshSiteTree(SelectedNode{false, mSites[*duplicate].id});

      LocalizedMessageBox(
          mTranslations,
          Translated(mTranslations, "siteManager.duplicateNameMessage"),
          Translated(mTranslations, "siteManager.invalidTitle"),
          wxOK | wxICON_WARNING,
          this);

      mName->SetFocus();
      mName->SelectAll();

      return;
    }

    for (const auto &folder : mFolders)
    {
      if (!IsFolderNameUnique(mFolders, mSites, folder.name,
                              folder.parentId, folder.id))
      {
        RefreshSiteTree(SelectedNode{true, folder.id});

        LocalizedMessageBox(
            mTranslations,
            Translated(mTranslations,
                       "siteManager.duplicateFolderNameMessage"),
            Translated(mTranslations, "siteManager.invalidTitle"),
            wxOK | wxICON_WARNING,
            this);

        mName->SetFocus();
        mName->SelectAll();

        return;
      }
    }

    PreparePendingCredentialIds();

    if (connect)
    {
      mConnectionSiteId = mSelectedNode->id;
    }

    // Both actions return OK through wxWidgets' normal validation path. A
    // custom button ID would not close the dialog just by skipping the event.
    AcceptAndClose();
  }
} // namespace havremote::ui
