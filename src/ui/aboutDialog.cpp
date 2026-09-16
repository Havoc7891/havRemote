// SPDX-License-Identifier: MIT

#include "ui/aboutDialog.hpp"

#include "localization/translationCatalog.hpp"
#include "havRemoteVersion.hpp"
#include "havRemoteImages.hpp"

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/hyperlink.h>
#include <wx/icon.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/statline.h>
#include <wx/stattext.h>

#include <array>
#include <string_view>

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

    class AboutDialog final : public wxDialog
    {
    public:
      AboutDialog(wxWindow *parent,
                  const localization::TranslationCatalog &translations)
          : wxDialog(parent,
                     wxID_ANY,
                     Translated(translations, "about.title"),
                     wxDefaultPosition,
                     wxDefaultSize,
                     wxDEFAULT_DIALOG_STYLE)
      {
        auto *root = new wxBoxSizer(wxVERTICAL);
        auto *content = new wxBoxSizer(wxHORIZONTAL);

        const auto iconSize = FromDIP(wxSize{48, 48});
        const auto applicationIcon = LoadApplicationIcon(iconSize);
        if (applicationIcon.IsOk())
        {
          SetIcon(applicationIcon);
          content->Add(new wxStaticBitmap(
                           this, wxID_ANY, applicationIcon),
                       0,
                       wxRIGHT | wxALIGN_TOP,
                       18);
        }

        auto *information = new wxBoxSizer(wxVERTICAL);
        auto *name = new wxStaticText(this, wxID_ANY, wxT("havRemote"));
        auto nameFont = name->GetFont();
        nameFont.MakeBold();
        nameFont.SetPointSize(nameFont.GetPointSize() + 4);
        name->SetFont(nameFont);
        information->Add(name, 0, wxBOTTOM, 2);

        const std::array<std::string_view, 1> versionArguments{ApplicationVersion};

        information->Add(
            new wxStaticText(
                this,
                wxID_ANY,
                FromUtf8(translations.Format(
                    "about.version", versionArguments))),
            0,
            wxBOTTOM,
            14);

        auto *description = new wxStaticText(
            this, wxID_ANY, Translated(translations, "about.description"));
        description->Wrap(390);
        information->Add(description, 0, wxBOTTOM, 14);
        information->Add(
            new wxStaticText(
                this,
                wxID_ANY,
                wxT("Copyright \u00a9 2026 Ren\u00e9 Nicolaus")),
            0);

        auto *links = new wxBoxSizer(wxHORIZONTAL);
        auto *website = new wxHyperlinkCtrl(
            this, wxID_ANY, Translated(translations, "about.website"),
            wxT("https://havoc.de"));
        website->SetToolTip(website->GetURL());
        links->Add(website, 0, wxRIGHT, FromDIP(18));

        const auto repositoryUrl = wxT("https://github.com/") +
                                   FromUtf8(UpdateRepositoryOwner) + wxT("/") +
                                   FromUtf8(UpdateRepositoryName);
        auto *sourceCode = new wxHyperlinkCtrl(
            this, wxID_ANY, Translated(translations, "about.sourceCode"),
            repositoryUrl);
        sourceCode->SetToolTip(sourceCode->GetURL());
        links->Add(sourceCode, 0, wxRIGHT, FromDIP(18));

        const wxString emailAddress = wxT("havremote@havoc.de");
        auto *email = new wxHyperlinkCtrl(
            this, wxID_ANY, Translated(translations, "about.email"),
            wxT("mailto:") + emailAddress);
        email->SetToolTip(emailAddress);
        links->Add(email, 0);
        information->Add(links, 0, wxTOP, FromDIP(14));
        content->Add(information, 1, wxEXPAND);

        root->Add(content, 1, wxEXPAND | wxALL, 20);
        root->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

        auto *buttons = CreateButtonSizer(wxOK);
        root->Add(buttons, 0, wxALIGN_RIGHT | wxALL, 12);

        SetSizer(root);

        if (auto *ok = wxWindow::FindWindowById(wxID_OK, this))
        {
          ok->SetLabel(Translated(translations, "common.ok"));
          ok->SetFocus();
        }

        auto minimumSize = FromDIP(wxSize{470, 260});
        minimumSize.IncTo(root->ComputeFittingWindowSize(this));

        SetMinSize(minimumSize);
        SetSize(minimumSize);

        CentreOnParent();
      }
    };
  } // namespace

  void ShowAboutDialog(
      wxWindow *parent,
      const localization::TranslationCatalog &translations)
  {
    AboutDialog dialog{parent, translations};
    dialog.ShowModal();
  }
} // namespace havremote::ui
