// SPDX-License-Identifier: MIT

#include "ui/updateDialog.hpp"

#include "havRemoteImages.hpp"
#include "localization/translationCatalog.hpp"
#include "update/updateService.hpp"

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/icon.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <array>

namespace havremote::ui
{
  namespace
  {
    wxString FromUtf8(const std::string_view value)
    {
      return wxString::FromUTF8(value.data(), value.size());
    }

    wxString Translated(const localization::TranslationCatalog &catalog,
                        const std::string_view key)
    {
      return FromUtf8(catalog.Text(key));
    }

    class UpdateAvailableDialog final : public wxDialog
    {
    public:
      UpdateAvailableDialog(
          wxWindow *parent,
          const localization::TranslationCatalog &translations,
          const std::string_view installedVersion,
          const updates::Release &release)
          : wxDialog(parent,
                     wxID_ANY,
                     Translated(translations, "update.availableTitle"),
                     wxDefaultPosition,
                     wxDefaultSize,
                     wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
      {
        const auto applicationIcon = LoadApplicationIcon();
        if (applicationIcon.IsOk())
        {
          SetIcon(applicationIcon);
        }

        auto *root = new wxBoxSizer(wxVERTICAL);
        const std::array<std::string_view, 2> versionArguments{
            installedVersion,
            release.canonicalVersion,
        };

        auto *message = new wxStaticText(
            this,
            wxID_ANY,
            FromUtf8(translations.Format("update.availableMessage",
                                         versionArguments)));
        root->Add(message, 0, wxEXPAND | wxALL, 14);

        auto *notesHeading = new wxStaticText(
            this,
            wxID_ANY,
            Translated(translations, "update.releaseNotes"));

        auto headingFont = notesHeading->GetFont();
        headingFont.SetWeight(wxFONTWEIGHT_BOLD);

        notesHeading->SetFont(headingFont);

        root->Add(notesHeading, 0, wxLEFT | wxRIGHT | wxBOTTOM, 14);

        auto *notes = new wxTextCtrl(
            this,
            wxID_ANY,
            release.notes.empty()
                ? Translated(translations, "update.noReleaseNotes")
                : FromUtf8(release.notes),
            wxDefaultPosition,
            FromDIP(wxSize{590, 250}),
            wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
        root->Add(notes, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);
        root->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

        auto *buttons = new wxBoxSizer(wxHORIZONTAL);
        auto *download = new wxButton(
            this, wxWindow::NewControlId(),
            Translated(translations, "update.download"));
        download->Enable(release.runtimeAsset.has_value());

        auto *viewRelease = new wxButton(
            this, wxWindow::NewControlId(),
            Translated(translations, "update.viewRelease"));

        auto *skip = new wxButton(
            this, wxWindow::NewControlId(),
            Translated(translations, "update.skipVersion"));

        auto *later = new wxButton(
            this, wxID_CANCEL,
            Translated(translations, "update.later"));

        buttons->Add(download, 0, wxRIGHT, 6);
        buttons->Add(viewRelease, 0, wxRIGHT, 6);
        buttons->Add(skip, 0, wxRIGHT, 6);
        buttons->Add(later, 0);

        root->Add(buttons, 0, wxALIGN_RIGHT | wxALL, 12);

        const auto finish = [this](const UpdateDialogAction action)
        {
          mAction = action;
          EndModal(wxID_OK);
        };

        download->Bind(
            wxEVT_BUTTON,
            [finish](wxCommandEvent &)
            { finish(UpdateDialogAction::Download); });
        viewRelease->Bind(
            wxEVT_BUTTON,
            [finish](wxCommandEvent &)
            { finish(UpdateDialogAction::ViewRelease); });
        skip->Bind(
            wxEVT_BUTTON,
            [finish](wxCommandEvent &)
            { finish(UpdateDialogAction::SkipVersion); });

        SetSizer(root);

        auto minimumSize = FromDIP(wxSize{640, 430});
        minimumSize.IncTo(root->ComputeFittingWindowSize(this));

        auto initialSize = FromDIP(wxSize{680, 500});
        initialSize.IncTo(minimumSize);

        SetMinSize(minimumSize);
        SetSize(initialSize);

        SetEscapeId(wxID_CANCEL);

        auto *primary = release.runtimeAsset ? download : viewRelease;
        primary->SetDefault();
        primary->SetFocus();

        CentreOnParent();
      }

      [[nodiscard]] UpdateDialogAction Action() const noexcept
      {
        return mAction;
      }

    private:
      UpdateDialogAction mAction{UpdateDialogAction::Later};
    };
  } // namespace

  UpdateDialogAction ShowUpdateAvailableDialog(
      wxWindow *parent,
      const localization::TranslationCatalog &translations,
      const std::string_view installedVersion,
      const updates::Release &release)
  {
    UpdateAvailableDialog dialog{parent, translations, installedVersion, release};

    (void)dialog.ShowModal();

    return dialog.Action();
  }
} // namespace havremote::ui
