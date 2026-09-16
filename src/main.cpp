// SPDX-License-Identifier: MIT

#include "config/csonConfigRepository.hpp"
#include "config/csonQueueRepository.hpp"
#include "localization/translationCatalog.hpp"
#include "platform/credentialStore.hpp"
#include "platform/recycleBin.hpp"
#include "ui/mainFrame.hpp"
#include "platform/toolkitPaths.hpp"
#include "havRemoteImages.hpp"

#if defined(HAVREMOTE_UPDATE_SIMULATION)
#include "update/updateSimulationWorkspace.hpp"
#include <wx/cmdline.h>
#endif

#include <wx/app.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/imagpng.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/stdpaths.h>

#include <array>
#include <filesystem>
#include <memory>
#include <string>

namespace
{
  std::string TranslationErrorMessage(
      const havremote::localization::TranslationError &error)
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

  class HavRemoteApp final : public wxApp
  {
  public:
#if defined(HAVREMOTE_UPDATE_SIMULATION)
    void OnInitCmdLine(wxCmdLineParser &parser) override
    {
      wxApp::OnInitCmdLine(parser);

      parser.AddOption({}, "simulate-update",
                       "Offline update scenario: available, up-to-date, "
                       "no-release, network-error, or server-error");
    }

    bool OnCmdLineParsed(wxCmdLineParser &parser) override
    {
      if (!wxApp::OnCmdLineParsed(parser))
      {
        return false;
      }

      wxString scenario;

      if (parser.Found("simulate-update", &scenario))
      {
        const auto parsed = havremote::updates::ParseUpdateSimulationScenario(
            scenario.ToStdString());
        if (!parsed)
        {
          wxMessageBox(wxString::FromUTF8(parsed.error()),
                       "havRemote update simulation", wxOK | wxICON_ERROR);

          return false;
        }

        mUpdateSimulation = *parsed;
      }

      return true;
    }
#endif

    bool OnInit() override
    {
      // Embedded PNG resources still need their format decoder registered:
      // the MSW resource handler only retrieves the RCDATA bytes.
      wxImage::AddHandler(new wxPNGHandler);

      // Packaging smoke test: initialize the GUI executable and
      // all bundled runtime dependencies without touching user configuration or
      // creating a window. Decode every embedded UI image so this test also
      // catches missing resources and image-handler regressions.
      if (argc == 2 && wxString{argv[1]} == "--smoke-test")
      {
        mSmokeTest = true;

        constexpr std::array imageResources{
            "HAVREMOTE_CLEAR_ICON",
            "HAVREMOTE_CLEAR_HISTORY_ICON",
            "HAVREMOTE_CONNECT_ICON",
            "HAVREMOTE_DISCONNECT_ICON",
            "HAVREMOTE_DOWNLOAD_ICON",
            "HAVREMOTE_FILE_LIST_FILE_ICON",
            "HAVREMOTE_FILE_LIST_FOLDER_ICON",
            "HAVREMOTE_FILE_LIST_SYMLINK_ICON",
            "HAVREMOTE_FILE_LIST_UNKNOWN_ICON",
            "HAVREMOTE_HISTORY_ICON",
            "HAVREMOTE_NEW_CONNECTION_ICON",
            "HAVREMOTE_REFRESH_ICON",
            "HAVREMOTE_SITE_MANAGER_ICON",
            "HAVREMOTE_UP_ICON",
            "HAVREMOTE_UPLOAD_ICON",
        };

        wxLogNull suppressImageErrors;

        for (const auto *resource : imageResources)
        {
          if (!havremote::ui::LoadUiBitmap(wxString::FromAscii(resource)).IsOk())
          {
            return false;
          }
        }

        return true;
      }

      if (!wxApp::OnInit())
      {
        return false;
      }

      SetAppName("havRemote");
      SetVendorName(wxString::FromUTF8("Ren\xC3\xA9 Nicolaus"));

      auto &paths = wxStandardPaths::Get();
      paths.UseAppInfo(wxStandardPaths::AppInfo_AppName);
      paths.SetFileLayout(wxStandardPaths::FileLayout_XDG);

      const auto executable = havremote::platform::FromToolkitPath(
          paths.GetExecutablePath());

      const auto executableDirectory = executable.parent_path();

      std::filesystem::path stateDirectory;
      std::unique_ptr<havremote::platform::ICredentialStore> credentialStore;

#if defined(HAVREMOTE_UPDATE_SIMULATION)
      if (mUpdateSimulation)
      {
        const auto workspace = havremote::updates::PrepareUpdateSimulationWorkspace(
            executableDirectory, *mUpdateSimulation);
        if (!workspace)
        {
          wxMessageBox(wxString::FromUTF8(workspace.error()),
                       "havRemote update simulation", wxOK | wxICON_ERROR);

          return false;
        }

        stateDirectory = *workspace;

        credentialStore = havremote::updates::MakeUpdateSimulationCredentialStore();
      }
      else
#endif
      {
        const auto appData = havremote::platform::FromToolkitPath(
            paths.GetUserConfigDir());

        stateDirectory = appData / L"havRemote";

        credentialStore = havremote::platform::MakeCredentialStore();
      }

      auto repository =
          std::make_unique<havremote::config::CsonConfigRepository>(
              stateDirectory / L"havRemote.cson");

      auto initialConfiguration = repository->Load();

#if defined(HAVREMOTE_UPDATE_SIMULATION)
      if (mUpdateSimulation && initialConfiguration && initialConfiguration->createdDefaults)
      {
        // Start with predictable manual checks and an empty sandbox directory.
        // Subsequent launches retain the simulator's own settings and skip state.
        initialConfiguration->data.settings.updates.checkAutomatically = false;
        initialConfiguration->data.workspace.lastLocalDirectory = stateDirectory;

        if (const auto saved = repository->Save(initialConfiguration->data); !saved)
        {
          wxMessageBox(wxString::FromUTF8(saved.error().message),
                       "havRemote update simulation", wxOK | wxICON_ERROR);

          return false;
        }
      }
#endif

      auto queueRepository =
          std::make_unique<havremote::config::CsonQueueRepository>(
              stateDirectory / L"queue.cson");

      auto initialQueue = queueRepository->Load();

      const auto configuredLanguage = initialConfiguration
                                          ? initialConfiguration->data.settings.language
                                          : havremote::config::AppSettings{}.language;

      auto translations = havremote::localization::TranslationCatalog::Load(
          executableDirectory / L"translations",
          configuredLanguage);

      if (!translations)
      {
        wxMessageBox(
            wxString::FromUTF8(TranslationErrorMessage(translations.error())),
            "havRemote translation error",
            wxOK | wxICON_ERROR);

        return false;
      }

      auto translationCatalog =
          std::make_shared<havremote::localization::TranslationCatalog>(
              std::move(*translations));

      // wxMSW can only select its light/dark native control appearance
      // before the first top-level window is created. Invalid configuration
      // uses the default appearance while its load error is reported.
      const auto theme = initialConfiguration
                             ? initialConfiguration->data.settings.theme
                             : havremote::config::AppearanceTheme::Dark;
      (void)SetAppearance(
          theme == havremote::config::AppearanceTheme::Dark
              ? Appearance::Dark
              : Appearance::Light);

      const auto knownHosts = stateDirectory / L"known_hosts";

      auto *frame = new havremote::ui::MainFrame(
          std::move(repository),
          std::move(queueRepository),
          std::move(credentialStore),
          havremote::platform::MakeRecycleBin(),
          knownHosts,
          executableDirectory / L"help",
          std::move(translationCatalog),
          std::move(initialConfiguration),
          std::move(initialQueue)

#if defined(HAVREMOTE_UPDATE_SIMULATION)
              ,
          mUpdateSimulation
#endif
      );

      frame->ShowWithRestoredGeometry();

      SetTopWindow(frame);

      return true;
    }

    int OnRun() override
    {
      return mSmokeTest ? 0 : wxApp::OnRun();
    }

  private:
    bool mSmokeTest{};
#if defined(HAVREMOTE_UPDATE_SIMULATION)
    std::optional<havremote::updates::UpdateSimulationScenario> mUpdateSimulation;
#endif
  };
} // namespace

wxIMPLEMENT_APP(HavRemoteApp);
