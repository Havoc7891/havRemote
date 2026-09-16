// SPDX-License-Identifier: MIT

#include "ui/remotePermissionsDialog.hpp"

#include "localization/translationCatalog.hpp"
#include "ui/fileListModel.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <array>
#include <string>
#include <string_view>

namespace havremote::ui
{
  namespace
  {
    constexpr std::array<std::uint32_t, 12> PermissionBits{
        0400U,
        0200U,
        0100U,
        0040U,
        0020U,
        0010U,
        0004U,
        0002U,
        0001U,
        04000U,
        02000U,
        01000U,
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

    std::string ToUtf8(const wxString &value)
    {
      const auto utf8 = value.ToUTF8();

      return utf8.data() ? std::string{utf8.data(), utf8.length()}
                         : std::string{};
    }
  } // namespace

  RemotePermissionsDialog::RemotePermissionsDialog(
      wxWindow *const parent,
      const std::size_t selectedItemCount,
      const std::optional<std::uint32_t> initialPermissions,
      const localization::TranslationCatalog &translations)
      : wxDialog(parent,
                 wxID_ANY,
                 Translated(translations, "permissions.title"),
                 wxDefaultPosition,
                 wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE)
  {
    auto *root = new wxBoxSizer(wxVERTICAL);

    const auto selectionText =
        selectedItemCount == 1U
            ? Translated(translations, "permissions.selectionOne")
            : FromUtf8(translations.Format(
                  "permissions.selectionMultiple",
                  std::array<std::string_view, 1>{
                      std::to_string(selectedItemCount)}));

    auto *selection = new wxStaticText(this, wxID_ANY, selectionText);
    selection->Wrap(FromDIP(430));
    root->Add(selection, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

    auto *ordinary = new wxStaticBoxSizer(
        wxVERTICAL,
        this,
        Translated(translations, "permissions.accessTitle"));

    auto *grid = new wxFlexGridSizer(4, 4, FromDIP(6), FromDIP(16));
    grid->Add(new wxStaticText(this, wxID_ANY, wxString{}));
    grid->Add(new wxStaticText(
        this, wxID_ANY, Translated(translations, "permissions.read")));
    grid->Add(new wxStaticText(
        this, wxID_ANY, Translated(translations, "permissions.write")));
    grid->Add(new wxStaticText(
        this, wxID_ANY, Translated(translations, "permissions.execute")));

    constexpr std::array<std::string_view, 3> categoryKeys{
        "permissions.owner",
        "permissions.group",
        "permissions.others",
    };

    constexpr std::array<std::string_view, 3> ordinaryPermissionKeys{
        "permissions.read",
        "permissions.write",
        "permissions.execute",
    };

    for (std::size_t category = 0; category < categoryKeys.size(); ++category)
    {
      grid->Add(new wxStaticText(
                    this,
                    wxID_ANY,
                    Translated(translations, categoryKeys[category])),
                0,
                wxALIGN_CENTER_VERTICAL);

      for (std::size_t permission = 0; permission < 3U; ++permission)
      {
        const auto index = category * 3U + permission;

        mPermissionBits[index] = new wxCheckBox(this, wxID_ANY, wxString{});

        const auto accessibleName =
            Translated(translations, categoryKeys[category]) + " " +
            Translated(translations, ordinaryPermissionKeys[permission]);

        mPermissionBits[index]->SetName(accessibleName);
        mPermissionBits[index]->SetToolTip(accessibleName);

        grid->Add(mPermissionBits[index], 0, wxALIGN_CENTER);
      }
    }

    ordinary->Add(grid, 0, wxEXPAND | wxALL, 8);
    root->Add(ordinary, 0, wxEXPAND | wxALL, 12);

    auto *special = new wxStaticBoxSizer(
        wxVERTICAL,
        this,
        Translated(translations, "permissions.specialTitle"));

    constexpr std::array<std::string_view, 3> specialKeys{
        "permissions.setUserId",
        "permissions.setGroupId",
        "permissions.sticky",
    };

    for (std::size_t index = 0; index < specialKeys.size(); ++index)
    {
      mPermissionBits[9U + index] = new wxCheckBox(
          this,
          wxID_ANY,
          Translated(translations, specialKeys[index]));

      special->Add(mPermissionBits[9U + index], 0, wxBOTTOM, 4);
    }

    root->Add(special, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    auto *octalRow = new wxBoxSizer(wxHORIZONTAL);
    octalRow->Add(new wxStaticText(
                      this,
                      wxID_ANY,
                      Translated(translations, "permissions.octalLabel")),
                  0,
                  wxALIGN_CENTER_VERTICAL | wxRIGHT,
                  8);
    mOctal = new wxTextCtrl(this, wxID_ANY);
    mOctal->SetHint(Translated(translations, "permissions.octalHint"));
    octalRow->Add(mOctal, 1, wxEXPAND);
    root->Add(octalRow, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    auto *validation = new wxStaticText(
        this,
        wxID_ANY,
        Translated(translations,
                   initialPermissions ? "permissions.validationHint"
                                      : "permissions.mixedHint"));
    validation->Wrap(FromDIP(430));
    root->Add(validation, 0, wxEXPAND | wxALL, 12);

    auto *buttons = CreateButtonSizer(wxOK | wxCANCEL);
    root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    SetSizer(root);

    mOkButton = dynamic_cast<wxButton *>(wxWindow::FindWindowById(wxID_OK, this));
    if (mOkButton)
    {
      mOkButton->SetLabel(Translated(translations, "common.ok"));
      mOkButton->SetDefault();
    }

    if (auto *cancel = wxWindow::FindWindowById(wxID_CANCEL, this))
    {
      cancel->SetLabel(Translated(translations, "common.cancel"));
    }

    auto minimumSize = FromDIP(wxSize{460, 440});
    minimumSize.IncTo(root->ComputeFittingWindowSize(this));

    SetMinSize(minimumSize);
    SetSize(minimumSize);

    mOctal->Bind(wxEVT_TEXT,
                 &RemotePermissionsDialog::OnOctalChanged,
                 this);

    for (auto *checkbox : mPermissionBits)
    {
      checkbox->Bind(wxEVT_CHECKBOX,
                     &RemotePermissionsDialog::OnPermissionBitChanged,
                     this);
    }

    if (initialPermissions)
    {
      mOctal->ChangeValue(FromUtf8(
          FormatFilePermissionsOctal(*initialPermissions)));
    }

    UpdateFromOctal();

    mOctal->SetFocus();

    CentreOnParent();
  }

  std::uint32_t RemotePermissionsDialog::Permissions() const noexcept
  {
    return mPermissions.value_or(0U);
  }

  void RemotePermissionsDialog::OnOctalChanged(wxCommandEvent &event)
  {
    UpdateFromOctal();

    event.Skip();
  }

  void RemotePermissionsDialog::OnPermissionBitChanged(wxCommandEvent &event)
  {
    UpdateOctalFromBits();

    event.Skip();
  }

  void RemotePermissionsDialog::UpdateFromOctal()
  {
    if (mSynchronizing)
    {
      return;
    }

    mSynchronizing = true;

    mPermissions = ParseFilePermissionsOctal(ToUtf8(mOctal->GetValue()));

    for (std::size_t index = 0; index < mPermissionBits.size(); ++index)
    {
      mPermissionBits[index]->Enable(mPermissions.has_value());
      mPermissionBits[index]->SetValue(
          mPermissions && ((*mPermissions & PermissionBits[index]) != 0U));
    }

    if (mOkButton)
    {
      mOkButton->Enable(mPermissions.has_value());
    }

    mSynchronizing = false;
  }

  void RemotePermissionsDialog::UpdateOctalFromBits()
  {
    if (mSynchronizing || !mPermissions)
    {
      return;
    }

    std::uint32_t mode{};

    for (std::size_t index = 0; index < mPermissionBits.size(); ++index)
    {
      if (mPermissionBits[index]->GetValue())
      {
        mode |= PermissionBits[index];
      }
    }

    mPermissions = mode;

    mSynchronizing = true;

    mOctal->ChangeValue(FromUtf8(FormatFilePermissionsOctal(mode)));

    mSynchronizing = false;

    if (mOkButton)
    {
      mOkButton->Enable(true);
    }
  }
} // namespace havremote::ui
