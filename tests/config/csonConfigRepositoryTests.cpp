// SPDX-License-Identifier: MIT

#include "configTestSupport.hpp"

#include "config/configPathProvider.hpp"
#include "config/csonConfigRepository.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace havremote::config
{
  using test::ReadText;
  using test::TempDirectory;
  using test::ValidEmptyConfig;
  using test::WriteText;

  namespace
  {
    constexpr std::string_view DefaultFileListsBlock = R"(  fileLists:
    local:
      sortColumn: "name"
      sortAscending: true
    remote:
      sortColumn: "name"
      sortAscending: true
)";

    constexpr std::string_view DefaultLocalFileListBlock = R"(    local:
      sortColumn: "name"
      sortAscending: true
)";

    constexpr std::string_view DefaultRemoteFileListBlock = R"(    remote:
      sortColumn: "name"
      sortAscending: true
)";

    constexpr std::string_view DefaultUpdatesBlock = R"(  updates:
    checkAutomatically: true
    lastCheckUnixSeconds: ""
    skippedVersion: ""
)";

    constexpr std::string_view DefaultWorkspaceBlock = R"(workspace:
  localDirectory: ""
  connectionDirectories: []
)";

    std::string SiteNode(std::string_view id,
                         std::string_view name,
                         std::string_view host)
    {
      return "  {\n"
             "    id: \"" +
             std::string{id} + "\"\n"
                               "    name: \"" +
             std::string{name} + "\"\n"
                                 "    protocol: \"sftp\"\n"
                                 "    host: \"" +
             std::string{host} + "\"\n"
                                 "    port: 22\n"
                                 "    username: \"user\"\n"
                                 "    authentication:\n"
                                 "      kind: \"password\"\n"
                                 "      credentialId: \"credential-reference\"\n"
                                 "      privateKeyFile: \"\"\n"
                                 "      publicKeyFile: \"\"\n"
                                 "      passphraseCredentialId: \"\"\n"
                                 "    initialRemoteDirectory: \"/\"\n"
                                 "    initialLocalDirectory: \"C:/Downloads\"\n"
                                 "    ftpEncoding: \"UTF-8\"\n"
                                 "  }\n";
    }

    std::string ConfigWithSites(const std::vector<std::string> &nodes)
    {
      std::string result = ValidEmptyConfig();

      std::string replacement = "sites: [\n";

      for (const auto &node : nodes)
      {
        replacement += node;
      }

      replacement += ']';

      result.replace(result.find("sites: []"), std::string("sites: []").size(),
                     replacement);

      return result;
    }

    std::string ConfigWithTlsTrust(std::string_view host)
    {
      std::string result = ValidEmptyConfig();

      const std::string replacement =
          "tlsTrust: [\n"
          "  {\n"
          "    host: \"" +
          std::string{host} + "\"\n"
                              "    port: 21\n"
                              "    publicKeyPin: \"sha256//YWJjZA==\"\n"
                              "  }\n"
                              "]";

      result.replace(result.find("tlsTrust: []"),
                     std::string("tlsTrust: []").size(), replacement);

      return result;
    }

    std::string QuickConnectHistoryNode(std::string_view protocol,
                                        std::string_view host,
                                        std::uint16_t port,
                                        std::string_view username)
    {
      return "  {\n"
             "    protocol: \"" +
             std::string{protocol} + "\"\n"
                                     "    host: \"" +
             std::string{host} + "\"\n"
                                 "    port: " +
             std::to_string(port) + "\n"
                                    "    username: \"" +
             std::string{username} + "\"\n"
                                     "  }\n";
    }

    std::string ConfigWithQuickConnectHistory(
        const std::vector<std::string> &nodes)
    {
      std::string result = ValidEmptyConfig();

      std::string replacement = "quickConnectHistory: [\n";

      for (const auto &node : nodes)
      {
        replacement += node;
      }

      replacement += ']';

      result.replace(result.find("quickConnectHistory: []"),
                     std::string("quickConnectHistory: []").size(), replacement);

      return result;
    }

    std::string ConfigWithWorkspaceConnectionDirectories(
        const std::vector<std::string> &nodes,
        const std::string_view localDirectory = "C:/Downloads")
    {
      std::string result = ValidEmptyConfig();

      std::string replacement = "workspace:\n  localDirectory: \"";
      replacement += localDirectory;
      replacement += "\"\n  connectionDirectories: [\n";

      for (const auto &node : nodes)
      {
        replacement += node;
      }

      replacement += "  ]\n";

      result.replace(result.find(DefaultWorkspaceBlock),
                     DefaultWorkspaceBlock.size(), replacement);

      return result;
    }

    std::string ConfigWithExternalEditor(std::string_view mode,
                                         std::string_view executable,
                                         std::string_view arguments)
    {
      std::string result = ValidEmptyConfig();

      std::string replacement{DefaultFileListsBlock};
      replacement += "  externalEditor:\n"
                     "    mode: \"" +
                     std::string{mode} + "\"\n"
                                         "    executable: \"" +
                     std::string{executable} + "\"\n"
                                               "    arguments: \"" +
                     std::string{arguments} + "\"\n";

      result.replace(result.find(DefaultFileListsBlock),
                     DefaultFileListsBlock.size(), replacement);

      return result;
    }

    std::string SavedWorkspaceDirectoriesNode(std::string_view siteId,
                                              std::string_view localDirectory,
                                              std::string_view remoteDirectory)
    {
      return "    {\n"
             "      siteId: \"" +
             std::string{siteId} + "\"\n"
                                   "      localDirectory: \"" +
             std::string{localDirectory} + "\"\n"
                                           "      remoteDirectory: \"" +
             std::string{remoteDirectory} + "\"\n"
                                            "    }\n";
    }

    std::string QuickWorkspaceDirectoriesNode(std::string_view protocol,
                                              std::string_view host,
                                              std::uint16_t port,
                                              std::string_view username,
                                              std::string_view localDirectory,
                                              std::string_view remoteDirectory)
    {
      return "    {\n"
             "      protocol: \"" +
             std::string{protocol} + "\"\n"
                                     "      host: \"" +
             std::string{host} + "\"\n"
                                 "      port: " +
             std::to_string(port) + "\n"
                                    "      username: \"" +
             std::string{username} + "\"\n"
                                     "      localDirectory: \"" +
             std::string{localDirectory} + "\"\n"
                                           "      remoteDirectory: \"" +
             std::string{remoteDirectory} + "\"\n"
                                            "    }\n";
    }

    void ReplaceOnce(std::string &text,
                     std::string_view original,
                     std::string_view replacement)
    {
      const auto position = text.find(original);

      REQUIRE(position != std::string::npos);

      text.replace(position, original.size(), replacement);
    }
  } // namespace

  TEST_CASE("A missing configuration is created atomically with defaults")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "nested" / "havRemote.cson";

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->createdDefaults);
    REQUIRE(loaded->data.formatVersion == CurrentFormatVersion);
    REQUIRE(loaded->data.settings == AppSettings{});
    REQUIRE(loaded->data.settings.theme == AppearanceTheme::Dark);
    REQUIRE(loaded->data.settings.language == "en");
    REQUIRE(loaded->data.settings.fileLists == FileListSettings{});
    REQUIRE(loaded->data.settings.updates == UpdateSettings{});
    REQUIRE(loaded->data.settings.externalEditor == ExternalEditorSettings{});
    REQUIRE(loaded->data.workspace == WorkspaceState{});
    REQUIRE_FALSE(loaded->data.workspace.mainWindow.position.has_value());
    REQUIRE(loaded->data.workspace.mainWindow.width ==
            DefaultMainWindowWidth);
    REQUIRE(loaded->data.workspace.mainWindow.height ==
            DefaultMainWindowHeight);
    REQUIRE_FALSE(loaded->data.workspace.mainWindow.maximized);
    REQUIRE(loaded->data.quickConnectHistory.empty());
    REQUIRE(loaded->data.sites.empty());
    REQUIRE(loaded->data.siteFolders.empty());
    REQUIRE(loaded->data.siteManagerOrder.empty());
    REQUIRE(loaded->data.pendingCredentialDeletions.empty());
    REQUIRE(loaded->data.tlsTrust.empty());
    REQUIRE(std::filesystem::exists(file));

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("formatVersion: 1.0") < saved.find("settings:"));
    REQUIRE(saved.find("defaultConflictPolicy: \"ask\"") <
            saved.find("theme: \"dark\""));
    REQUIRE(saved.find("theme: \"dark\"") < saved.find("language: \"en\""));
    REQUIRE(saved.find("language: \"en\"") < saved.find("fileLists:"));
    REQUIRE(saved.find("fileLists:") < saved.find("local:"));
    REQUIRE(saved.find("local:") < saved.find("remote:"));
    REQUIRE(saved.find("local:") < saved.find("sortColumn: \"name\""));
    REQUIRE(saved.find("sortColumn: \"name\"") <
            saved.find("sortAscending: true"));
    REQUIRE(saved.find("fileLists:") < saved.find("updates:"));
    REQUIRE(saved.find("updates:") <
            saved.find("checkAutomatically: true"));
    REQUIRE(saved.find("checkAutomatically: true") <
            saved.find("lastCheckUnixSeconds: \"\""));
    REQUIRE(saved.find("lastCheckUnixSeconds: \"\"") <
            saved.find("skippedVersion: \"\""));
    REQUIRE(saved.find("skippedVersion: \"\"") <
            saved.find("externalEditor:"));
    REQUIRE(saved.find("externalEditor:") < saved.find("mode: \"system\""));
    REQUIRE(saved.find("mode: \"system\"") <
            saved.find("executable: \"\""));
    REQUIRE(saved.find("executable: \"\"") <
            saved.find("arguments: \"{file}\""));
    REQUIRE(saved.find("settings:") < saved.find("workspace:"));
    REQUIRE(saved.find("workspace:") <
            saved.find("quickConnectHistory:\n  []"));
    REQUIRE(saved.find("localDirectory: \"\"") <
            saved.find("connectionDirectories:\n    []"));
    REQUIRE(saved.find("connectionDirectories:\n    []") <
            saved.find("mainWindow:"));
    REQUIRE(saved.find("x: null") < saved.find("y: null"));
    REQUIRE(saved.find("y: null") < saved.find("width: 1410.0"));
    REQUIRE(saved.find("width: 1410.0") < saved.find("height: 930.0"));
    REQUIRE(saved.find("height: 930.0") < saved.find("maximized: false"));
    REQUIRE(saved.find("quickConnectHistory:\n  []") <
            saved.find("sites:\n  []"));
    REQUIRE(saved.find("sites:\n  []") <
            saved.find("siteFolders:\n  []"));
    REQUIRE(saved.find("siteFolders:\n  []") <
            saved.find("siteManagerOrder:\n  []"));
    REQUIRE(saved.find("siteManagerOrder:\n  []") <
            saved.find("pendingCredentialDeletions:\n  []"));
    REQUIRE(saved.find("pendingCredentialDeletions:\n  []") <
            saved.find("tlsTrust:\n  []"));
  }

  TEST_CASE("Site folders are optional in format version 1 and inserted on save")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    std::string text = ValidEmptyConfig();

    ReplaceOnce(text, "siteFolders: []\n", "");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.formatVersion == 1);
    REQUIRE(loaded->data.siteFolders.empty());

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("sites:") < saved.find("siteFolders:\n  []"));
    REQUIRE(saved.find("siteFolders:\n  []") <
            saved.find("siteManagerOrder:\n  []"));
    REQUIRE(saved.find("siteManagerOrder:\n  []") < saved.find("tlsTrust:"));
  }

  TEST_CASE("Pending credential deletions default empty within format version 1")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "pending-credentials.cson";

    const auto text = ValidEmptyConfig();

    WriteText(file, text);

    CsonConfigRepository repository(file);

    const auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    CHECK(loaded->data.formatVersion == 1);
    CHECK(loaded->data.pendingCredentialDeletions.empty());
    CHECK(ReadText(file) == text);

    REQUIRE(repository.Save(loaded->data).has_value());

    const auto saved = ReadText(file);

    CHECK(saved.find("siteManagerOrder:") <
          saved.find("pendingCredentialDeletions:\n  []"));
    CHECK(saved.find("pendingCredentialDeletions:\n  []") <
          saved.find("tlsTrust:"));
  }

  TEST_CASE("Pending credential deletions round trip opaque store identifiers")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "pending-credentials.cson";

    CsonConfigRepository repository(file);

    ConfigData data;
    data.pendingCredentialDeletions = {
        "removed-site-password",
        "removed-site-passphrase",
        "credential/\xc3\xa4/\xe2\x82\xac/\xf0\x9f\x94\x91",
        "id\twith-tab",
        "id\x01with-control",
        std::string(300, 'x'),
    };

    REQUIRE(repository.Save(data).has_value());

    CsonConfigRepository verification(file);

    const auto loaded = verification.Load();

    REQUIRE(loaded.has_value());
    CHECK(loaded->data.formatVersion == 1);
    CHECK(loaded->data.pendingCredentialDeletions ==
          data.pendingCredentialDeletions);
    CHECK(loaded->data.sites.empty());

    const auto saved = ReadText(file);

    REQUIRE(verification.Save(loaded->data).has_value());
    CHECK(ReadText(file) == saved);
  }

  TEST_CASE("Pending credential deletion document validation reports the offending node")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "invalid-pending-credentials.cson";
    const auto prefix = ValidEmptyConfig() + "pendingCredentialDeletions: ";
    const auto fieldLine = static_cast<std::size_t>(
                               std::count(prefix.begin(), prefix.end(), '\n')) +
                           1U;

    struct InvalidValue final
    {
      std::string_view value;
      std::string_view logicalPath;
      std::size_t lineOffset{};
    };

    constexpr std::array invalidValues{
        InvalidValue{"{}", "pendingCredentialDeletions"},
        InvalidValue{"\"credential\"", "pendingCredentialDeletions"},
        InvalidValue{"[17]", "pendingCredentialDeletions[0]"},
        InvalidValue{"[null]", "pendingCredentialDeletions[0]"},
        InvalidValue{"[\"\"]", "pendingCredentialDeletions[0]"},
        InvalidValue{"[\"bad\\u0000id\"]", "pendingCredentialDeletions[0]"},
        InvalidValue{"[\"bad\\rid\"]", "pendingCredentialDeletions[0]"},
        InvalidValue{"[\"bad\\nid\"]", "pendingCredentialDeletions[0]"},
        InvalidValue{"[\n  \"duplicate\"\n  \"duplicate\"\n]",
                     "pendingCredentialDeletions[1]", 2},
    };

    for (const auto &invalid : invalidValues)
    {
      CAPTURE(invalid.value);

      const auto text = prefix + std::string{invalid.value} + '\n';

      WriteText(file, text);

      CsonConfigRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      CHECK(loaded.error().kind == ConfigErrorKind::Validation);
      CHECK(loaded.error().message.find(invalid.logicalPath) != std::string::npos);
      CHECK(loaded.error().line == fieldLine + invalid.lineOffset);
      REQUIRE(loaded.error().column.has_value());
      CHECK(*loaded.error().column > 0U);
      CHECK(ReadText(file) == text);
    }

    const auto invalidUtf8 = prefix + "[\"invalid-\xc0\xaf\"]\n";

    WriteText(file, invalidUtf8);

    CsonConfigRepository repository(file);

    const auto loaded = repository.Load();

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().kind == ConfigErrorKind::Parse);
    CHECK(loaded.error().line == fieldLine);
    CHECK(ReadText(file) == invalidUtf8);
  }

  TEST_CASE("Typed pending credential deletions reject invalid and duplicate identifiers")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "invalid-pending-credentials.cson";

    CsonConfigRepository repository(file);

    ConfigData persisted;
    persisted.pendingCredentialDeletions = {"retained-credential"};

    REQUIRE(repository.Save(persisted).has_value());

    const auto original = ReadText(file);

    const std::vector<std::vector<std::string>> invalidLists{
        {""},
        {std::string{"bad\0id", 6}},
        {"bad\rid"},
        {"bad\nid"},
        {"\x80"},
        {"\xc0\xaf"},
        {"\xe2\x82"},
        {"\xe2\x28\xa1"},
        {"\xe0\x80\xaf"},
        {"\xed\xa0\x80"},
        {"\xf0\x80\x80\xaf"},
        {"\xf4\x90\x80\x80"},
        {"\xf5\x80\x80\x80"},
        {"duplicate", "duplicate"},
    };

    for (std::size_t index = 0; index < invalidLists.size(); ++index)
    {
      CAPTURE(index);

      auto rejected = persisted;
      rejected.pendingCredentialDeletions = invalidLists[index];

      const auto saved = repository.Save(rejected);

      REQUIRE_FALSE(saved.has_value());
      CHECK(saved.error().kind == ConfigErrorKind::Validation);

      const auto invalidIndex = invalidLists[index].size() - 1U;

      CHECK(saved.error().message.find(
                "pendingCredentialDeletions[" + std::to_string(invalidIndex) + "]") !=
            std::string::npos);
      CHECK(ReadText(file) == original);
    }

    persisted.settings.transferConcurrency = 3;

    REQUIRE(repository.Save(persisted).has_value());

    CsonConfigRepository verification(file);

    const auto reloaded = verification.Load();

    REQUIRE(reloaded.has_value());
    CHECK(reloaded->data.pendingCredentialDeletions ==
          persisted.pendingCredentialDeletions);
  }

  TEST_CASE("Pending credential deletion edits retain comments through reorder and removal")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "pending-credential-comments.cson";

    auto text = ValidEmptyConfig();

    ReplaceOnce(text, "tlsTrust: []", R"(pluginCleanupBefore: "untouched before"
# cleanup leading
pendingCredentialDeletions: [ # cleanup inline
  # alpha leading
  "alpha", # alpha inline
  # beta leading
  "beta", # beta inline
  # gamma leading
  "gamma" # gamma inline
  # cleanup closing
]
pluginCleanupAfter: "untouched after" # extension inline
tlsTrust: [])");

    WriteText(file, text);

    constexpr std::array markers{
        "# cleanup leading",
        "# cleanup inline",
        "# alpha leading",
        "# alpha inline",
        "# beta leading",
        "# beta inline",
        "# gamma leading",
        "# gamma inline",
        "# cleanup closing",
        "# extension inline",
    };

    const std::vector<std::vector<std::string>> edits{
        {"beta", "alpha", "gamma"},
        {"beta"},
        {},
        {"delta"},
        {},
    };

    for (std::size_t index = 0; index < edits.size(); ++index)
    {
      CAPTURE(index);

      CsonConfigRepository repository(file);

      auto loaded = repository.Load();

      INFO((loaded.has_value() ? "" : loaded.error().message));
      REQUIRE(loaded.has_value());

      loaded->data.pendingCredentialDeletions = edits[index];

      REQUIRE(repository.Save(loaded->data).has_value());

      const auto saved = ReadText(file);

      for (const auto marker : markers)
      {
        CAPTURE(marker);

        const auto first = saved.find(marker);

        REQUIRE(first != std::string::npos);
        CHECK(saved.find(marker, first + 1U) == std::string::npos);
      }

      CHECK(saved.find("pluginCleanupBefore: \"untouched before\"") <
            saved.find("pendingCredentialDeletions:"));
      CHECK(saved.find("pendingCredentialDeletions:") <
            saved.find("pluginCleanupAfter: \"untouched after\""));

      if (index == 0)
      {
        CHECK(saved.find("# beta leading") < saved.find("\"beta\""));
        CHECK(saved.find("\"beta\" # beta inline") <
              saved.find("# alpha leading"));
        CHECK(saved.find("# alpha leading") <
              saved.find("\"alpha\" # alpha inline"));
      }

      CsonConfigRepository verification(file);

      const auto reloaded = verification.Load();

      REQUIRE(reloaded.has_value());
      CHECK(reloaded->data.pendingCredentialDeletions == edits[index]);
      REQUIRE(verification.Save(reloaded->data).has_value());
      CHECK(ReadText(file) == saved);
    }
  }

  TEST_CASE("Workspace is optional in format version 1 and inserted after settings")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    std::string text = ValidEmptyConfig();

    ReplaceOnce(text, DefaultWorkspaceBlock,
                "# extension between settings and history\n"
                "pluginWorkspaceHint: true\n");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.formatVersion == 1);
    REQUIRE(loaded->data.workspace == WorkspaceState{});

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("settings:") < saved.find("workspace:"));
    REQUIRE(saved.find("workspace:") <
            saved.find("# extension between settings and history"));
    REQUIRE(saved.find("pluginWorkspaceHint: true") <
            saved.find("quickConnectHistory:"));
    REQUIRE(saved.find("localDirectory: \"\"") != std::string::npos);
    REQUIRE(saved.find("connectionDirectories:") != std::string::npos);
  }

  TEST_CASE("Main-window state is optional in format version 1 and inserted on save")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    WriteText(file, ValidEmptyConfig());

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.formatVersion == 1);
    REQUIRE(loaded->data.workspace.mainWindow == MainWindowState{});

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("connectionDirectories:") < saved.find("mainWindow:"));
    REQUIRE(saved.find("x: null") < saved.find("y: null"));
    REQUIRE(saved.find("y: null") < saved.find("width: 1410.0"));
    REQUIRE(saved.find("width: 1410.0") < saved.find("height: 930.0"));
    REQUIRE(saved.find("height: 930.0") < saved.find("maximized: false"));

    CsonConfigRepository reloadedRepository(file);

    const auto reloaded = reloadedRepository.Load();

    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->data.workspace.mainWindow == MainWindowState{});
  }

  TEST_CASE("Main-window state round trips negative coordinates and preserves lossless trivia")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    std::string text = ValidEmptyConfig();

    ReplaceOnce(text, DefaultWorkspaceBlock, R"(workspace:
  localDirectory: ""
  connectionDirectories: []
  # placement leading comment
  mainWindow: # placement block comment
    x: -1920 # x inline
    extensionBetweenPosition: "kept"
    y: 40 # y inline
    width: 1410 # width inline
    height: 930 # height inline
    maximized: false # maximized inline
)");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.formatVersion == 1);
    REQUIRE(loaded->data.workspace.mainWindow.position.has_value());
    REQUIRE((loaded->data.workspace.mainWindow.position.value() ==
             WindowPosition{.x = -1920, .y = 40}));
    REQUIRE(loaded->data.workspace.mainWindow.width == 1410);
    REQUIRE(loaded->data.workspace.mainWindow.height == 930);
    REQUIRE_FALSE(loaded->data.workspace.mainWindow.maximized);

    loaded->data.workspace.mainWindow = MainWindowState{
        .position = WindowPosition{.x = 125, .y = -75},
        .width = 1600,
        .height = 1000,
        .maximized = true,
    };

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("# placement leading comment") <
            saved.find("mainWindow: # placement block comment"));
    REQUIRE(saved.find("x: 125.0 # x inline") != std::string::npos);
    REQUIRE(saved.find("extensionBetweenPosition: \"kept\"") <
            saved.find("y: -75.0 # y inline"));
    REQUIRE(saved.find("width: 1600.0 # width inline") !=
            std::string::npos);
    REQUIRE(saved.find("height: 1000.0 # height inline") !=
            std::string::npos);
    REQUIRE(saved.find("maximized: true # maximized inline") !=
            std::string::npos);

    CsonConfigRepository reloadedRepository(file);

    const auto reloaded = reloadedRepository.Load();

    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->data.workspace.mainWindow ==
            loaded->data.workspace.mainWindow);
  }

  TEST_CASE("Workspace directories round trip for saved sites and Quick Connect endpoints")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    CsonConfigRepository repository(file);

    ConfigData data;
    data.workspace.lastLocalDirectory =
        std::filesystem::path(std::u8string(u8"D:/Téléchargements"));
    data.workspace.connectionDirectories = {
        RememberedConnectionDirectories{
            .connection = SavedSiteWorkspaceIdentity{
                "10f81b67-02b1-4a69-bf3e-42c90d67ce26"},
            .localDirectory = std::filesystem::path(std::u8string(u8"D:/Lokale Übertragung")),
            .remoteDirectory = RemotePath{"/Übertragung"},
        },
        RememberedConnectionDirectories{
            .connection = QuickConnectWorkspaceIdentity{SiteEndpointIdentity{ProtocolKind::Sftp, "quick.example", 2222, "rené"}},
            .localDirectory = std::filesystem::path(std::u8string(u8"E:/René/Projekte")),
            .remoteDirectory = RemotePath{"/home/rené/Projekte"},
        },
    };

    REQUIRE(repository.Save(data).has_value());

    CsonConfigRepository reloadedRepository(file);

    const auto loaded = reloadedRepository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.workspace == data.workspace);
    REQUIRE(loaded->data.formatVersion == 1);

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("workspace:") < saved.find("quickConnectHistory:"));
    REQUIRE(saved.find("siteId: \"10f81b67-02b1-4a69-bf3e-42c90d67ce26\"") !=
            std::string::npos);
    REQUIRE(saved.find("protocol: \"sftp\"") != std::string::npos);
    REQUIRE(saved.find("localDirectory: \"E:/René/Projekte\"") !=
            std::string::npos);
    REQUIRE(saved.find("remoteDirectory: \"/home/rené/Projekte\"") !=
            std::string::npos);
  }

  TEST_CASE("Workspace remote directories persist UTF-8 display text rather than protocol bytes")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    CsonConfigRepository repository(file);

    std::string latin1Path{"/caf"};
    latin1Path.push_back(static_cast<char>(0xe9));

    ConfigData data;
    data.workspace.connectionDirectories.push_back({
        .connection = SavedSiteWorkspaceIdentity{"encoding-test-site"},
        .localDirectory = "C:/Downloads",
        .remoteDirectory = RemotePath{latin1Path, "/café"},
    });

    REQUIRE(repository.Save(data).has_value());

    std::string saved = ReadText(file);

    REQUIRE(saved.find("remoteDirectory: \"/café\"") != std::string::npos);
    REQUIRE(saved.find(latin1Path) == std::string::npos);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.workspace.connectionDirectories.size() == 1);

    auto &remoteDirectory =
        loaded->data.workspace.connectionDirectories.front().remoteDirectory;

    REQUIRE(remoteDirectory.Bytes() == "/café");
    REQUIRE(remoteDirectory.DisplayUtf8() == "/café");

    std::string cp1252Path{"/gr"};
    cp1252Path.push_back(static_cast<char>(0xfc));
    cp1252Path += "sse";

    remoteDirectory = RemotePath{cp1252Path, "/grüße"};

    REQUIRE(repository.Save(loaded->data).has_value());

    saved = ReadText(file);

    REQUIRE(saved.find("remoteDirectory: \"/grüße\"") !=
            std::string::npos);
    REQUIRE(saved.find(cp1252Path) == std::string::npos);
  }

  TEST_CASE("Workspace edits preserve comments unknown members and complete record nodes")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    std::string text = ValidEmptyConfig();

    ReplaceOnce(text, DefaultWorkspaceBlock, R"(# workspace leading
workspace: # workspace inline
  # local leading
  localDirectory: "C:/Old" # local inline
  extensionBeforeDirectories: "kept"
  connectionDirectories: [
    # saved node
    {
      siteId: "11111111-1111-4111-8111-111111111111"
      localDirectory: "C:/Saved old" # saved local inline
      remoteDirectory: "/old" # saved remote inline
      savedExtension: true # saved extension inline
      savedTail: "kept"
    }
    # quick node
    {
      protocol: "sftp"
      host: "quick.example" # quick host inline
      port: 22
      username: "alice"
      localDirectory: "C:/Quick old"
      remoteDirectory: "/home/alice"
      quickExtension: "kept"
    }
    # directories closing
  ]
  extensionAfterDirectories: 42
)");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.workspace.connectionDirectories.size() == 2);

    loaded->data.workspace.lastLocalDirectory = "D:/Current";

    std::swap(loaded->data.workspace.connectionDirectories[0],
              loaded->data.workspace.connectionDirectories[1]);

    loaded->data.workspace.connectionDirectories[0].localDirectory =
        "D:/Quick current";
    loaded->data.workspace.connectionDirectories[0].remoteDirectory =
        RemotePath{"/home/alice/current"};
    loaded->data.workspace.connectionDirectories[1].localDirectory =
        "D:/Saved current";
    loaded->data.workspace.connectionDirectories[1].remoteDirectory =
        RemotePath{"/saved/current"};

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("# workspace leading") < saved.find("workspace:"));
    REQUIRE(saved.find("workspace: # workspace inline") != std::string::npos);
    REQUIRE(saved.find("# local leading") <
            saved.find("localDirectory: \"D:/Current\""));
    REQUIRE(saved.find("localDirectory: \"D:/Current\" # local inline") !=
            std::string::npos);
    REQUIRE(saved.find("extensionBeforeDirectories: \"kept\"") <
            saved.find("connectionDirectories: ["));
    REQUIRE(saved.find("# quick node") < saved.find("quick.example"));
    REQUIRE(saved.find("quick.example") < saved.find("# saved node"));
    REQUIRE(saved.find("localDirectory: \"D:/Quick current\"") !=
            std::string::npos);
    REQUIRE(saved.find("remoteDirectory: \"/home/alice/current\"") !=
            std::string::npos);
    REQUIRE(saved.find("localDirectory: \"D:/Saved current\" # saved local inline") !=
            std::string::npos);
    REQUIRE(saved.find("remoteDirectory: \"/saved/current\" # saved remote inline") !=
            std::string::npos);
    REQUIRE(saved.find("savedExtension: true # saved extension inline") !=
            std::string::npos);
    REQUIRE(saved.find("savedTail: \"kept\"") != std::string::npos);
    REQUIRE(saved.find("quickExtension: \"kept\"") != std::string::npos);
    REQUIRE(saved.find("# directories closing") != std::string::npos);
    REQUIRE(saved.find("extensionAfterDirectories: 42.0") != std::string::npos);

    CsonConfigRepository reloadedRepository(file);

    const auto reloaded = reloadedRepository.Load();

    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->data.workspace == loaded->data.workspace);
  }

  TEST_CASE("The earlier format-version-1 workspace directory shape migrates losslessly")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    std::string text = ValidEmptyConfig();

    ReplaceOnce(text, DefaultWorkspaceBlock, R"(workspace:
  localDirectory: "C:/Legacy default"
  # array leading
  remoteDirectories: [
    {
      siteId: "11111111-1111-4111-8111-111111111111"
      # remote leading
      directory: "/legacy" # remote inline
      extension: "kept"
    }
  ]
)");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    INFO((loaded.has_value() ? std::string{} : loaded.error().message));
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.formatVersion == 1);
    REQUIRE(loaded->data.workspace.connectionDirectories.size() == 1);

    const auto &remembered =
        loaded->data.workspace.connectionDirectories.front();

    REQUIRE(remembered.localDirectory == "C:/Legacy default");
    REQUIRE(remembered.remoteDirectory == RemotePath{"/legacy"});

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("remoteDirectories:") == std::string::npos);
    REQUIRE(saved.find("directory: \"/legacy\"") == std::string::npos);
    REQUIRE(saved.find("# array leading") <
            saved.find("connectionDirectories: ["));
    REQUIRE(saved.find("# remote leading") <
            saved.find("remoteDirectory: \"/legacy\" # remote inline"));
    REQUIRE(saved.find("extension: \"kept\"") != std::string::npos);
    REQUIRE(saved.find("localDirectory: \"C:/Legacy default\"") !=
            std::string::npos);

    CsonConfigRepository reloadedRepository(file);

    const auto reloaded = reloadedRepository.Load();

    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->data.workspace == loaded->data.workspace);
  }

  TEST_CASE("Quick workspace identity promotion reuses its complete lossless node")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    constexpr std::string_view SiteId =
        "11111111-1111-4111-8111-111111111111";

    std::string text = ConfigWithSites(
        {SiteNode(SiteId, "Promoted", "quick.example")});

    ReplaceOnce(text, DefaultWorkspaceBlock, R"(workspace:
  localDirectory: "C:/Default"
  connectionDirectories: [
    # promoted record leading
    {
      protocol: "sftp" # identity inline
      host: "quick.example" # removed host comment
      port: 22
      username: "user"
      localDirectory: "C:/Quick" # local inline
      remoteDirectory: "/quick" # remote inline
      pluginMetadata: "keep me"
    }
    # other record leading
    {
      protocol: "sftp"
      host: "other.example"
      port: 22
      username: "other"
      localDirectory: "D:/Other"
      remoteDirectory: "/other"
    }
  ]
)");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.workspace.connectionDirectories.size() == 2);

    auto &promoted = loaded->data.workspace.connectionDirectories.front();
    promoted.connection = SavedSiteWorkspaceIdentity{std::string{SiteId}};
    promoted.localDirectory = "C:/Promoted";
    promoted.remoteDirectory = RemotePath{"/promoted"};

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    const auto workspaceStart = saved.find("workspace:");
    const auto workspaceEnd = saved.find("quickConnectHistory:");

    REQUIRE(workspaceStart != std::string::npos);
    REQUIRE(workspaceEnd != std::string::npos);

    const std::string workspace =
        saved.substr(workspaceStart, workspaceEnd - workspaceStart);

    const auto promotedStart = workspace.find("# promoted record leading");
    const auto promotedEnd = workspace.find("# other record leading");

    REQUIRE(promotedStart != std::string::npos);
    REQUIRE(promotedEnd != std::string::npos);
    REQUIRE(promotedStart < promotedEnd);

    const std::string promotedNode =
        workspace.substr(promotedStart, promotedEnd - promotedStart);

    REQUIRE(promotedNode.find("siteId: \"" + std::string{SiteId} +
                              "\" # identity inline") != std::string::npos);
    REQUIRE(promotedNode.find("protocol:") == std::string::npos);
    REQUIRE(promotedNode.find("host:") == std::string::npos);
    REQUIRE(promotedNode.find("port:") == std::string::npos);
    REQUIRE(promotedNode.find("username:") == std::string::npos);
    REQUIRE(promotedNode.find("# removed host comment") != std::string::npos);
    REQUIRE(promotedNode.find("localDirectory: \"C:/Promoted\" # local inline") !=
            std::string::npos);
    REQUIRE(promotedNode.find("remoteDirectory: \"/promoted\" # remote inline") !=
            std::string::npos);
    REQUIRE(promotedNode.find("pluginMetadata: \"keep me\"") !=
            std::string::npos);
    REQUIRE(workspace.find("other.example") != std::string::npos);

    CsonConfigRepository reloadedRepository(file);

    const auto reloaded = reloadedRepository.Load();

    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->data.workspace == loaded->data.workspace);
  }

  TEST_CASE("Quick workspace identity promotion rejects ambiguous saved endpoints")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    constexpr std::string_view FirstSiteId =
        "11111111-1111-4111-8111-111111111111";
    constexpr std::string_view SecondSiteId =
        "22222222-2222-4222-8222-222222222222";

    std::string text = ConfigWithSites({
        SiteNode(FirstSiteId, "First", "duplicate.example"),
        SiteNode(SecondSiteId, "Second", "duplicate.example"),
    });

    ReplaceOnce(text, DefaultWorkspaceBlock, R"(workspace:
  localDirectory: "C:/Default"
  connectionDirectories: [
    {
      protocol: "sftp"
      host: "duplicate.example"
      port: 22
      username: "user"
      localDirectory: "C:/Quick"
      remoteDirectory: "/quick"
      ambiguousExtension: "must not be promoted"
    }
  ]
)");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());

    loaded->data.workspace.connectionDirectories.front().connection =
        SavedSiteWorkspaceIdentity{std::string{FirstSiteId}};

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    const auto workspaceStart = saved.find("workspace:");
    const auto workspaceEnd = saved.find("quickConnectHistory:");

    REQUIRE(workspaceStart != std::string::npos);
    REQUIRE(workspaceEnd != std::string::npos);

    const std::string workspace =
        saved.substr(workspaceStart, workspaceEnd - workspaceStart);

    REQUIRE(workspace.find("siteId: \"" + std::string{FirstSiteId} + "\"") !=
            std::string::npos);
    REQUIRE(workspace.find("ambiguousExtension") == std::string::npos);
  }

  TEST_CASE("File-list settings are optional in format version 1 and inserted on save")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    std::string text = ValidEmptyConfig();

    ReplaceOnce(text, DefaultFileListsBlock, "");
    ReplaceOnce(text, "  language: \"en\"\n",
                "  language: \"en\"\n"
                "  # extension remains ahead of appended settings\n"
                "  pluginAfterLanguage: true\n");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.settings.fileLists == FileListSettings{});

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("# extension remains ahead of appended settings") !=
            std::string::npos);
    REQUIRE(saved.find("pluginAfterLanguage: true") < saved.find("fileLists:"));
    REQUIRE(saved.find("fileLists:") < saved.find("quickConnectHistory:"));
    REQUIRE(saved.find("local:") < saved.find("remote:"));
  }

  TEST_CASE("Local and remote file-list sort settings round trip independently")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    std::string text = ValidEmptyConfig();

    ReplaceOnce(text, "      sortColumn: \"name\"",
                "      sortColumn: \"size\"");
    ReplaceOnce(text, "      sortAscending: true",
                "      sortAscending: false");
    ReplaceOnce(text, "      sortColumn: \"name\"",
                "      sortColumn: \"owner\"");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE((loaded->data.settings.fileLists.local == FileListSortSettings{
                                                          FileListSortColumn::Size, false, {}}));
    REQUIRE((loaded->data.settings.fileLists.remote == FileListSortSettings{
                                                           FileListSortColumn::Owner, true, {}}));

    loaded->data.settings.fileLists.local = {
        FileListSortColumn::Type, true, {}};
    loaded->data.settings.fileLists.remote = {
        FileListSortColumn::Permissions, false, {}};

    REQUIRE(repository.Save(loaded->data).has_value());

    CsonConfigRepository reloadedRepository(file);

    const auto reloaded = reloadedRepository.Load();

    REQUIRE(reloaded.has_value());
    REQUIRE((reloaded->data.settings.fileLists.local == FileListSortSettings{
                                                            FileListSortColumn::Type, true, {}}));
    REQUIRE((reloaded->data.settings.fileLists.remote == FileListSortSettings{
                                                             FileListSortColumn::Permissions, false, {}}));
  }

  TEST_CASE("File-list column widths round trip in device-independent pixels")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "column-widths.cson";

    CsonConfigRepository repository(file);

    ConfigData data;
    data.settings.fileLists.local.columnWidths = {260, 90, 120, 170};
    data.settings.fileLists.remote.columnWidths = {
        280, 95, 110, 175, 125, 140};

    REQUIRE(repository.Save(data).has_value());

    CsonConfigRepository reloadedRepository(file);

    const auto loaded = reloadedRepository.Load();

    REQUIRE(loaded.has_value());
    CHECK(loaded->data.settings.fileLists.local.columnWidths ==
          std::vector<std::uint32_t>{260, 90, 120, 170});
    CHECK(loaded->data.settings.fileLists.remote.columnWidths ==
          std::vector<std::uint32_t>{280, 95, 110, 175, 125, 140});
  }

  TEST_CASE("Invalid file-list column width arrays are rejected")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "invalid-column-widths.cson";

    std::string text = ValidEmptyConfig();

    ReplaceOnce(text,
                "      sortAscending: true\n",
                "      sortAscending: true\n"
                "      columnWidths: [210, 85]\n");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    const auto loaded = repository.Load();

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().kind == ConfigErrorKind::Validation);
    CHECK(loaded.error().message.find("columnWidths") != std::string::npos);
    CHECK(ReadText(file) == text);
  }

  TEST_CASE("Open connection tabs retain identity order paths and comments")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "open-tabs.cson";

    std::string text = ValidEmptyConfig();

    ReplaceOnce(text, DefaultWorkspaceBlock, R"(workspace:
  localDirectory: "C:/Downloads"
  connectionDirectories: []
  # tabs leading comment
  openTabs: [
    {
      connectionId: "tab-saved"
      siteId: "saved-site"
      localDirectory: "C:/Saved"
      remoteDirectory: "/saved" # saved path comment
      extension: "keep me"
    }
    {
      connectionId: "tab-quick"
      protocol: "sftp"
      host: "quick.example"
      port: 22
      username: "user"
      localDirectory: "C:/Quick"
      remoteDirectory: "/quick"
    }
  ]
  selectedConnectionId: "tab-quick"
)");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.workspace.openTabs.size() == 2U);
    CHECK(loaded->data.workspace.selectedConnectionId == "tab-quick");

    std::ranges::reverse(loaded->data.workspace.openTabs);

    loaded->data.workspace.openTabs.back().remoteDirectory =
        RemotePath{"/saved/changed"};

    REQUIRE(repository.Save(loaded->data).has_value());

    const auto saved = ReadText(file);

    CHECK(saved.find("# tabs leading comment") != std::string::npos);
    CHECK(saved.find("remoteDirectory: \"/saved/changed\" # saved path comment") !=
          std::string::npos);
    CHECK(saved.find("extension: \"keep me\"") != std::string::npos);
    CHECK(saved.find("connectionId: \"tab-quick\"") <
          saved.find("connectionId: \"tab-saved\""));

    CsonConfigRepository reloadedRepository(file);

    const auto reloaded = reloadedRepository.Load();

    REQUIRE(reloaded.has_value());
    CHECK(reloaded->data.workspace.openTabs ==
          loaded->data.workspace.openTabs);
    CHECK(reloaded->data.workspace.selectedConnectionId == "tab-quick");
  }

  TEST_CASE("Clearing a tab identity preserves removed comments inside that tab")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "open-tabs.cson";

    std::string text = ValidEmptyConfig();

    ReplaceOnce(text, DefaultWorkspaceBlock, R"(workspace:
  localDirectory: ""
  connectionDirectories: []
  openTabs: [
    {
      connectionId: "tab-quick"
      protocol: "sftp"
      host: "quick.example" # host explanation
      port: 22
      username: "user"
      localDirectory: "/local"
      remoteDirectory: "/remote"
    }
  ]
)");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());

    loaded->data.workspace.openTabs.front().connection.reset();

    REQUIRE(repository.Save(loaded->data).has_value());

    const auto saved = ReadText(file);

    REQUIRE(saved.find("        # host explanation\n        localDirectory:") != std::string::npos);
    REQUIRE(saved.find("host explanation") == saved.rfind("host explanation"));

    loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE_FALSE(loaded->data.workspace.openTabs.front().connection.has_value());
  }

  TEST_CASE("Open connection tab ids and selected tab are validated")
  {
    TempDirectory directory;

    const auto duplicateFile = directory.Path() / "duplicate-tabs.cson";

    ConfigData duplicate;
    duplicate.workspace.openTabs = {
        {.connectionId = "same",
         .connection = std::nullopt,
         .localDirectory = {},
         .remoteDirectory = RemotePath::Root()},
        {.connectionId = "same",
         .connection = std::nullopt,
         .localDirectory = {},
         .remoteDirectory = RemotePath::Root()},
    };

    CsonConfigRepository duplicateRepository(duplicateFile);

    const auto duplicateSaved = duplicateRepository.Save(duplicate);

    REQUIRE_FALSE(duplicateSaved.has_value());
    CHECK(duplicateSaved.error().kind == ConfigErrorKind::Validation);

    const auto selectedFile = directory.Path() / "missing-selected-tab.cson";

    ConfigData selected;
    selected.workspace.openTabs = {
        {.connectionId = "present",
         .connection = std::nullopt,
         .localDirectory = {},
         .remoteDirectory = RemotePath::Root()},
    };
    selected.workspace.selectedConnectionId = "missing";

    CsonConfigRepository selectedRepository(selectedFile);

    const auto selectedSaved = selectedRepository.Save(selected);

    REQUIRE_FALSE(selectedSaved.has_value());
    CHECK(selectedSaved.error().kind == ConfigErrorKind::Validation);

    const auto emptyIdFile = directory.Path() / "empty-tab-id.cson";

    std::string emptyId = ValidEmptyConfig();

    ReplaceOnce(emptyId, DefaultWorkspaceBlock, R"(workspace:
  localDirectory: ""
  connectionDirectories: []
  openTabs: [
    {
      connectionId: ""
      localDirectory: ""
      remoteDirectory: "/"
    }
  ]
  selectedConnectionId: ""
)");

    WriteText(emptyIdFile, emptyId);

    CsonConfigRepository emptyIdRepository(emptyIdFile);

    const auto emptyIdLoaded = emptyIdRepository.Load();

    REQUIRE_FALSE(emptyIdLoaded.has_value());
    CHECK(emptyIdLoaded.error().kind == ConfigErrorKind::Validation);
  }

  TEST_CASE("File-list setting edits preserve nested comments and unknown members")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    std::string text = ValidEmptyConfig();

    ReplaceOnce(text, DefaultFileListsBlock, R"(  # file-list leading
  fileLists: # file-list block
    extensionBeforeLocal: "kept"
    local: # local block
      # local column leading
      sortColumn: "name" # local column inline
      sortAscending: true # local direction inline
      columnWidths: [
        # name width leading
        210 # name width inline
        85
        100
        145
      ]
      localExtension: 42
    remote: # remote block
      sortColumn: "name" # remote column inline
      sortAscending: true # remote direction inline
      remoteExtension: false
    extensionAfterRemote: "kept too"
)");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());

    loaded->data.settings.fileLists.local = {
        FileListSortColumn::Modified, false, {260, 90, 120, 170}};

    loaded->data.settings.fileLists.remote = {
        FileListSortColumn::Size, false, {}};

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("# file-list leading") < saved.find("fileLists:"));
    REQUIRE(saved.find("fileLists: # file-list block") != std::string::npos);
    REQUIRE(saved.find("extensionBeforeLocal: \"kept\"") <
            saved.find("local: # local block"));
    REQUIRE(saved.find("# local column leading") <
            saved.find("sortColumn: \"modified\""));
    REQUIRE(saved.find("sortColumn: \"modified\" # local column inline") !=
            std::string::npos);
    REQUIRE(saved.find("sortAscending: false # local direction inline") !=
            std::string::npos);
    REQUIRE(saved.find("# name width leading") != std::string::npos);
    REQUIRE(saved.find("# name width inline") != std::string::npos);
    REQUIRE(saved.find("localExtension: 42.0") != std::string::npos);
    REQUIRE(saved.find("sortColumn: \"size\" # remote column inline") !=
            std::string::npos);
    REQUIRE(saved.find("sortAscending: false # remote direction inline") !=
            std::string::npos);
    REQUIRE(saved.find("remoteExtension: false") != std::string::npos);
    REQUIRE(saved.find("extensionAfterRemote: \"kept too\"") !=
            std::string::npos);

    CsonConfigRepository reloadedRepository(file);

    const auto reloaded = reloadedRepository.Load();

    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->data.settings.fileLists.local.sortColumn ==
            FileListSortColumn::Modified);
    REQUIRE_FALSE(reloaded->data.settings.fileLists.local.sortAscending);
    REQUIRE(reloaded->data.settings.fileLists.local.columnWidths ==
            std::vector<std::uint32_t>{260, 90, 120, 170});
    REQUIRE(reloaded->data.settings.fileLists.remote.sortColumn ==
            FileListSortColumn::Size);
    REQUIRE_FALSE(reloaded->data.settings.fileLists.remote.sortAscending);
  }

  TEST_CASE("Quick Connect history is an optional format version 1 section")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    std::string text = ValidEmptyConfig();

    ReplaceOnce(text, "quickConnectHistory: []\n", "");
    ReplaceOnce(text, "sites: []", "# existing sites comment\nsites: []");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.formatVersion == 1);
    REQUIRE(loaded->data.quickConnectHistory.empty());

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("quickConnectHistory:\n  []") != std::string::npos);
    REQUIRE(saved.find("quickConnectHistory:") < saved.find("sites:"));
    REQUIRE(saved.find("# existing sites comment") != std::string::npos);
  }

  TEST_CASE("Quick Connect history round trips in MRU order without secrets")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    WriteText(file, ConfigWithQuickConnectHistory({
                        QuickConnectHistoryNode("sftp", "new.example", 22,
                                                "alice"),
                        QuickConnectHistoryNode("ftp", "old.example", 21,
                                                "anonymous"),
                    }));

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.quickConnectHistory ==
            std::vector<QuickConnectHistoryEntry>{
                {ProtocolKind::Sftp, "new.example", 22, "alice"},
                {ProtocolKind::Ftp, "old.example", 21, "anonymous"},
            });
    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("new.example") < saved.find("old.example"));
    REQUIRE(saved.find("password") == std::string::npos);
    REQUIRE(saved.find("passphrase") == std::string::npos);
  }

  TEST_CASE("Quick Connect history reordering preserves complete lossless nodes")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    std::string text = ValidEmptyConfig();

    ReplaceOnce(text, "quickConnectHistory: []", R"(quickConnectHistory: [
  # alpha node
  {
    protocol: "sftp"
    host: "alpha.example" # alpha host
    port: 22
    username: "alice"
    extension: "alpha metadata" # extension comment
    extensionAfterInline: true
  }
  # beta node
  {
    protocol: "ftpsExplicit"
    host: "beta.example"
    port: 21
    username: "bob"
  }
])");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());

    std::swap(loaded->data.quickConnectHistory[0],
              loaded->data.quickConnectHistory[1]);

    loaded->data.quickConnectHistory[1].host = "ALPHA.EXAMPLE";

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("beta.example") < saved.find("ALPHA.EXAMPLE"));
    REQUIRE(saved.find("# alpha node") < saved.find("ALPHA.EXAMPLE"));
    REQUIRE(saved.find("ALPHA.EXAMPLE\" # alpha host") != std::string::npos);
    REQUIRE(saved.find("extension: \"alpha metadata\" # extension comment") !=
            std::string::npos);
    REQUIRE(saved.find("extension: \"alpha metadata\"") <
            saved.find("extensionAfterInline: true"));
    REQUIRE(saved.find("# beta node") < saved.find("beta.example"));
  }

  TEST_CASE("Saving preserves comments blank lines unknown fields and member order")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    WriteText(file, R"(# root comment
formatVersion: 1
settings:
  # concurrency comment
  transferConcurrency: 2 # workers

  connectionTimeoutSeconds: 20
  commandIdleTimeoutSeconds: 60
  defaultConflictPolicy: "ask"
  theme: "dark"
  language: "en"
  pluginSetting: "untouched" # plugin comment
  updates:
    checkAutomatically: true
    lastCheckUnixSeconds: ""
    skippedVersion: ""
sites: []
tlsTrust: []
futureSection:
  enabled: true
)");

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());

    loaded->data.settings.transferConcurrency = 4;

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("# root comment") != std::string::npos);
    REQUIRE(saved.find("# concurrency comment") != std::string::npos);
    REQUIRE(saved.find("transferConcurrency: 4.0 # workers") != std::string::npos);
    REQUIRE(saved.find("pluginSetting: \"untouched\" # plugin comment") !=
            std::string::npos);
    REQUIRE(saved.find("futureSection:") != std::string::npos);
    REQUIRE(saved.find("pluginSetting") > saved.find("theme"));
    REQUIRE(saved.find("\n\n  connectionTimeoutSeconds") != std::string::npos);
  }

  TEST_CASE("Theme edits round trip without losing comments or member order")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    std::string text = ValidEmptyConfig();

    ReplaceOnce(text, "  theme: \"dark\"",
                "  # native appearance\n"
                "  theme: \"dark\" # restart required\n"
                "  pluginAfterTheme: true");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.settings.theme == AppearanceTheme::Dark);

    loaded->data.settings.theme = AppearanceTheme::Light;

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("# native appearance") < saved.find("theme: \"light\""));
    REQUIRE(saved.find("theme: \"light\" # restart required") !=
            std::string::npos);
    REQUIRE(saved.find("theme: \"light\"") < saved.find("pluginAfterTheme: true"));

    CsonConfigRepository reloadedRepository(file);

    auto reloaded = reloadedRepository.Load();

    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->data.settings.theme == AppearanceTheme::Light);
    REQUIRE(reloadedRepository.Save(reloaded->data).has_value());
    REQUIRE(ReadText(file).find("theme: \"light\" # restart required") !=
            std::string::npos);
  }

  TEST_CASE("Language edits round trip without losing comments or member order")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    std::string text = ValidEmptyConfig();

    ReplaceOnce(text, "  language: \"en\"",
                "  # interface language\n"
                "  language: \"en\" # discovered catalog code\n"
                "  pluginAfterLanguage: true");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.settings.language == "en");

    loaded->data.settings.language = "de";

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("# interface language") < saved.find("language: \"de\""));
    REQUIRE(saved.find("language: \"de\" # discovered catalog code") !=
            std::string::npos);
    REQUIRE(saved.find("language: \"de\"") <
            saved.find("pluginAfterLanguage: true"));

    CsonConfigRepository reloadedRepository(file);

    const auto reloaded = reloadedRepository.Load();

    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->data.settings.language == "de");
  }

  TEST_CASE("Update settings round trip without losing comments unknown fields or order")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    std::string text = ValidEmptyConfig();

    ReplaceOnce(text, DefaultUpdatesBlock, R"(  # update preferences
  updates: # update settings object
    # automatic checks can be disabled
    checkAutomatically: true # check on startup

    pluginBeforeLastCheck: "kept"
    lastCheckUnixSeconds: "" # exact timestamp string
    skippedVersion: "" # normalized release version
    pluginAfterSkippedVersion: true # extension setting
    # update settings closing comment
)");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.settings.updates == UpdateSettings{});

    loaded->data.settings.updates = UpdateSettings{
        .checkAutomatically = false,
        .lastCheckUnixSeconds = "18446744073709551615",
        .skippedVersion = "0.2.0",
    };

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("# update preferences") <
            saved.find("updates: # update settings object"));
    REQUIRE(saved.find("# automatic checks can be disabled") <
            saved.find("checkAutomatically: false # check on startup"));
    REQUIRE(saved.find("checkAutomatically: false # check on startup") !=
            std::string::npos);
    REQUIRE(saved.find("\n\n    pluginBeforeLastCheck: \"kept\"") !=
            std::string::npos);
    REQUIRE(saved.find("pluginBeforeLastCheck: \"kept\"") <
            saved.find(
                "lastCheckUnixSeconds: \"18446744073709551615\" # exact timestamp string"));
    REQUIRE(saved.find(
                "skippedVersion: \"0.2.0\" # normalized release version") <
            saved.find(
                "pluginAfterSkippedVersion: true # extension setting"));
    REQUIRE(saved.find("# update settings closing comment") !=
            std::string::npos);

    CsonConfigRepository reloadedRepository(file);

    auto reloaded = reloadedRepository.Load();

    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->data.settings.updates ==
            loaded->data.settings.updates);
    REQUIRE(reloadedRepository.Save(reloaded->data).has_value());

    const std::string savedAgain = ReadText(file);

    REQUIRE(savedAgain.find("# update preferences") != std::string::npos);
    REQUIRE(savedAgain.find(
                "pluginAfterSkippedVersion: true # extension setting") !=
            std::string::npos);
    REQUIRE(savedAgain.find("# update settings closing comment") !=
            std::string::npos);
  }

  TEST_CASE("Update settings require the complete current schema and valid exact strings")
  {
    TempDirectory directory;

    std::vector<std::string> invalidDocuments;

    auto missingUpdates = ValidEmptyConfig();

    ReplaceOnce(missingUpdates, DefaultUpdatesBlock, "");

    invalidDocuments.push_back(std::move(missingUpdates));

    auto updatesNotObject = ValidEmptyConfig();

    ReplaceOnce(updatesNotObject, DefaultUpdatesBlock, "  updates: []\n");

    invalidDocuments.push_back(std::move(updatesNotObject));

    constexpr std::array requiredFields{
        std::string_view{"    checkAutomatically: true\n"},
        std::string_view{"    lastCheckUnixSeconds: \"\"\n"},
        std::string_view{"    skippedVersion: \"\"\n"},
    };

    for (const auto field : requiredFields)
    {
      auto missingField = ValidEmptyConfig();

      ReplaceOnce(missingField, field, "");

      invalidDocuments.push_back(std::move(missingField));
    }

    auto automaticWrongType = ValidEmptyConfig();

    ReplaceOnce(automaticWrongType, "checkAutomatically: true",
                "checkAutomatically: \"true\"");

    invalidDocuments.push_back(std::move(automaticWrongType));

    auto timestampWrongType = ValidEmptyConfig();

    ReplaceOnce(timestampWrongType, "lastCheckUnixSeconds: \"\"",
                "lastCheckUnixSeconds: 123");

    invalidDocuments.push_back(std::move(timestampWrongType));

    constexpr std::array invalidTimestamps{
        std::string_view{"00"},
        std::string_view{"-1"},
        std::string_view{"+1"},
        std::string_view{"1.0"},
        std::string_view{"12x"},
        std::string_view{" 1"},
        std::string_view{"18446744073709551616"},
    };

    for (const auto value : invalidTimestamps)
    {
      auto invalidTimestamp = ValidEmptyConfig();

      ReplaceOnce(invalidTimestamp, "lastCheckUnixSeconds: \"\"",
                  "lastCheckUnixSeconds: \"" + std::string{value} + "\"");

      invalidDocuments.push_back(std::move(invalidTimestamp));
    }

    auto skippedVersionWrongType = ValidEmptyConfig();

    ReplaceOnce(skippedVersionWrongType, "skippedVersion: \"\"",
                "skippedVersion: 1");

    invalidDocuments.push_back(std::move(skippedVersionWrongType));

    constexpr std::array invalidVersions{
        std::string_view{"v1.2.3"},
        std::string_view{"1.2"},
        std::string_view{"1.2.3.4"},
        std::string_view{"01.2.3"},
        std::string_view{"1.02.3"},
        std::string_view{"1.2.03"},
        std::string_view{"1.2.3-beta"},
        std::string_view{"4294967296.2.3"},
    };

    for (const auto value : invalidVersions)
    {
      auto invalidVersion = ValidEmptyConfig();

      ReplaceOnce(invalidVersion, "skippedVersion: \"\"",
                  "skippedVersion: \"" + std::string{value} + "\"");

      invalidDocuments.push_back(std::move(invalidVersion));
    }

    for (std::size_t index = 0; index < invalidDocuments.size(); ++index)
    {
      CAPTURE(index);

      const auto file = directory.Path() /
                        ("invalid-update-settings-" +
                         std::to_string(index) + ".cson");

      WriteText(file, invalidDocuments[index]);

      CsonConfigRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind == ConfigErrorKind::Validation);
      REQUIRE(loaded.error().message.find("updates") != std::string::npos);
      REQUIRE(loaded.error().line.has_value());
      REQUIRE(loaded.error().column.has_value());
      REQUIRE(ReadText(file) == invalidDocuments[index]);
    }
  }

  TEST_CASE("Typed saves reject invalid update timestamps and skipped versions")
  {
    TempDirectory directory;

    constexpr std::array invalidTimestamps{
        std::string_view{"00"},
        std::string_view{"-1"},
        std::string_view{"18446744073709551616"},
    };

    for (std::size_t index = 0; index < invalidTimestamps.size(); ++index)
    {
      CAPTURE(invalidTimestamps[index]);

      const auto file = directory.Path() /
                        ("typed-invalid-update-timestamp-" +
                         std::to_string(index) + ".cson");

      ConfigData data;
      data.settings.updates.lastCheckUnixSeconds = invalidTimestamps[index];

      CsonConfigRepository repository(file);

      const auto saved = repository.Save(data);

      REQUIRE_FALSE(saved.has_value());
      REQUIRE(saved.error().kind == ConfigErrorKind::Validation);
      REQUIRE_FALSE(std::filesystem::exists(file));
    }

    constexpr std::array invalidVersions{
        std::string_view{"v1.2.3"},
        std::string_view{"1.2"},
        std::string_view{"01.2.3"},
        std::string_view{"1.2.3-beta"},
        std::string_view{"4294967296.2.3"},
    };

    for (std::size_t index = 0; index < invalidVersions.size(); ++index)
    {
      CAPTURE(invalidVersions[index]);

      const auto file = directory.Path() /
                        ("typed-invalid-skipped-version-" +
                         std::to_string(index) + ".cson");

      ConfigData data;
      data.settings.updates.skippedVersion = invalidVersions[index];

      CsonConfigRepository repository(file);

      const auto saved = repository.Save(data);

      REQUIRE_FALSE(saved.has_value());
      REQUIRE(saved.error().kind == ConfigErrorKind::Validation);
      REQUIRE_FALSE(std::filesystem::exists(file));
    }

    const auto boundaryFile = directory.Path() / "typed-update-boundaries.cson";

    ConfigData boundaryData;

    boundaryData.settings.updates = UpdateSettings{
        .checkAutomatically = false,
        .lastCheckUnixSeconds = "18446744073709551615",
        .skippedVersion = "4294967295.4294967295.4294967295",
    };

    CsonConfigRepository boundaryRepository(boundaryFile);

    REQUIRE(boundaryRepository.Save(boundaryData).has_value());

    CsonConfigRepository boundaryReloadRepository(boundaryFile);

    const auto boundaryReloaded = boundaryReloadRepository.Load();

    REQUIRE(boundaryReloaded.has_value());
    REQUIRE(boundaryReloaded->data.settings.updates ==
            boundaryData.settings.updates);
  }

  TEST_CASE(
      "External editor defaults are loaded from older version 1 settings and appended on save")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    WriteText(file, ValidEmptyConfig());

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.formatVersion == 1);
    REQUIRE(loaded->data.settings.externalEditor == ExternalEditorSettings{});

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("fileLists:") < saved.find("externalEditor:"));
    REQUIRE(saved.find("externalEditor:") < saved.find("mode: \"system\""));
    REQUIRE(saved.find("mode: \"system\"") <
            saved.find("executable: \"\""));
    REQUIRE(saved.find("executable: \"\"") <
            saved.find("arguments: \"{file}\""));

    CsonConfigRepository reloadedRepository(file);

    const auto reloaded = reloadedRepository.Load();

    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->data.settings.externalEditor ==
            ExternalEditorSettings{});
  }

  TEST_CASE("External editor edits preserve nested comments unknown fields and order")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    std::string text = ValidEmptyConfig();

    const std::string replacement =
        std::string{DefaultFileListsBlock} +
        R"(  # editor selection
  externalEditor: # editor object
    # editor mode
    mode: "custom" # selected mode
    executable: "C:/Tools/editor.exe" # selected executable
    pluginEditorSetting: "untouched" # extension setting
    arguments: "--wait {file}" # argument template
)";

    ReplaceOnce(text, DefaultFileListsBlock, replacement);

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.settings.externalEditor.mode ==
            ExternalEditorMode::Custom);
    REQUIRE(loaded->data.settings.externalEditor.executable ==
            std::filesystem::path{"C:/Tools/editor.exe"});
    REQUIRE(loaded->data.settings.externalEditor.arguments ==
            "--wait {file}");

    loaded->data.settings.externalEditor.executable =
        std::filesystem::path{"C:/Tools/new-editor.exe"};
    loaded->data.settings.externalEditor.arguments = "--reuse {file}";

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("# editor selection") < saved.find("externalEditor:"));
    REQUIRE(saved.find("externalEditor: # editor object") !=
            std::string::npos);
    REQUIRE(saved.find("# editor mode") < saved.find("mode: \"custom\""));
    REQUIRE(saved.find("mode: \"custom\" # selected mode") !=
            std::string::npos);
    REQUIRE(saved.find(
                "executable: \"C:/Tools/new-editor.exe\" # selected executable") !=
            std::string::npos);
    REQUIRE(saved.find("executable: \"C:/Tools/new-editor.exe\"") <
            saved.find("pluginEditorSetting: \"untouched\""));
    REQUIRE(
        saved.find(
            "pluginEditorSetting: \"untouched\" # extension setting") <
        saved.find("arguments: \"--reuse {file}\" # argument template"));

    CsonConfigRepository reloadedRepository(file);

    auto reloaded = reloadedRepository.Load();

    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->data.settings.externalEditor ==
            loaded->data.settings.externalEditor);
    REQUIRE(reloadedRepository.Save(reloaded->data).has_value());
    REQUIRE(ReadText(file).find(
                "pluginEditorSetting: \"untouched\" # extension setting") !=
            std::string::npos);
  }

  TEST_CASE("Repeated load and save cycles retain every comment position and unknown data")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    constexpr std::string_view stableId = "a4f30157-fef6-4cc3-b23d-b662f4648619";

    WriteText(file, R"(# root leading
formatVersion: 1 # version inline
extensionBeforeSettings: "kept"
settings: # settings block
  # setting leading
  transferConcurrency: 2 # workers inline

  connectionTimeoutSeconds: 20
  commandIdleTimeoutSeconds: 60
  defaultConflictPolicy: "ask"
  theme: "dark"
  language: "en"
  updates:
    checkAutomatically: true
    lastCheckUnixSeconds: ""
    skippedVersion: ""
sites: [
  # site leading
  {
    id: "a4f30157-fef6-4cc3-b23d-b662f4648619"
    name: "Alpha" # site inline
    protocol: "sftp"
    host: "alpha.example"
    port: 22
    username: "alice"
    authentication:
      kind: "password"
      credentialId: "credential-reference"
      privateKeyFile: ""
      publicKeyFile: ""
      passphraseCredentialId: ""
    initialRemoteDirectory: "/"
    initialLocalDirectory: "C:/Downloads"
    ftpEncoding: "UTF-8"
    pluginExactCount: "18446744073709551615"
    pluginObject:
      enabled: true # nested unknown inline
  }
  # sites closing
]
tlsTrust: []
# root trailing
)");

    constexpr std::array commentMarkers{
        "# root leading",
        "# version inline",
        "# settings block",
        "# setting leading",
        "# workers inline",
        "# site leading",
        "# site inline",
        "# nested unknown inline",
        "# sites closing",
        "# root trailing",
    };

    for (std::uint32_t cycle = 0; cycle < 3; ++cycle)
    {
      CAPTURE(cycle);

      CsonConfigRepository repository(file);

      auto loaded = repository.Load();

      REQUIRE(loaded.has_value());
      REQUIRE(loaded->data.sites.size() == 1);
      REQUIRE(loaded->data.sites.front().id == stableId);

      loaded->data.settings.transferConcurrency = 3 + cycle;
      loaded->data.sites.front().name = "Alpha cycle " + std::to_string(cycle);

      REQUIRE(repository.Save(loaded->data).has_value());

      const std::string saved = ReadText(file);

      for (const auto marker : commentMarkers)
      {
        REQUIRE(saved.find(marker) != std::string::npos);
      }

      REQUIRE(saved.find("\n\n  connectionTimeoutSeconds") != std::string::npos);
      REQUIRE(saved.find("extensionBeforeSettings") < saved.find("settings:"));
      REQUIRE(saved.find("pluginExactCount: \"18446744073709551615\"") !=
              std::string::npos);
      REQUIRE(saved.find("pluginObject:") != std::string::npos);
    }
  }

  TEST_CASE("Sites are matched by stable id and retain comments when reordered")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    WriteText(file, R"(formatVersion: 1
settings:
  transferConcurrency: 2
  connectionTimeoutSeconds: 20
  commandIdleTimeoutSeconds: 60
  defaultConflictPolicy: "ask"
  theme: "dark"
  language: "en"
  updates:
    checkAutomatically: true
    lastCheckUnixSeconds: ""
    skippedVersion: ""
sites: [
  # alpha site
  {
    id: "11111111-1111-4111-8111-111111111111"
    name: "Alpha" # display name
    protocol: "sftp"
    host: "alpha.example"
    port: 22
    username: "alice"
    authentication:
      kind: "password"
      credentialId: "cred-alpha"
      privateKeyFile: ""
      publicKeyFile: ""
      passphraseCredentialId: ""
    initialRemoteDirectory: "/"
    initialLocalDirectory: "C:/Downloads"
    ftpEncoding: "UTF-8"
    pluginSiteField: true
  }
  # beta site
  {
    id: "22222222-2222-4222-8222-222222222222"
    name: "Beta"
    protocol: "ftp"
    host: "beta.example"
    port: 21
    username: "bob"
    authentication:
      kind: "password"
      credentialId: ""
      privateKeyFile: ""
      publicKeyFile: ""
      passphraseCredentialId: ""
    initialRemoteDirectory: "/pub"
    initialLocalDirectory: "D:/Incoming"
    ftpEncoding: "UTF-8"
  }
]
tlsTrust: []
)");

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.sites.size() == 2);

    loaded->data.sites[0].name = "Alpha edited";

    std::swap(loaded->data.sites[0], loaded->data.sites[1]);

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("# beta site") <
            saved.find("id: \"22222222-2222-4222-8222-222222222222\""));
    REQUIRE(saved.find("id: \"22222222-2222-4222-8222-222222222222\"") <
            saved.find("# alpha site"));
    REQUIRE(saved.find("# alpha site") <
            saved.find("id: \"11111111-1111-4111-8111-111111111111\""));
    REQUIRE(saved.find("name: \"Alpha edited\" # display name") !=
            std::string::npos);
    REQUIRE(saved.find("pluginSiteField: true") != std::string::npos);
  }

  TEST_CASE("Nested site folders round trip and retain lossless nodes when reorganized")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    constexpr std::string_view alphaId =
        "11111111-1111-4111-8111-111111111111";
    constexpr std::string_view betaId =
        "22222222-2222-4222-8222-222222222222";
    constexpr std::string_view rootFolderId =
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
    constexpr std::string_view childFolderId =
        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";

    std::string text = ConfigWithSites({
        SiteNode(alphaId, "Alpha", "alpha.example"),
        SiteNode(betaId, "Beta", "beta.example"),
    });

    ReplaceOnce(text, "siteFolders: []", R"(siteFolders: [
  # root folder leading
  {
    id: "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
    name: "Servers" # folder name inline
    parentId: ""
    siteIds: [
      # alpha placement
      "11111111-1111-4111-8111-111111111111" # placement inline
    ]
    pluginFolderField: "untouched"
  }
  # child folder leading
  {
    id: "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
    name: "Archive"
    parentId: "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
    siteIds: [
      "22222222-2222-4222-8222-222222222222"
    ]
  }
  # folders closing
])");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.siteFolders.size() == 2);
    REQUIRE(loaded->data.siteFolders[0].id == rootFolderId);
    REQUIRE(loaded->data.siteFolders[0].siteIds ==
            std::vector<std::string>{std::string{alphaId}});
    REQUIRE(loaded->data.siteFolders[1].id == childFolderId);
    REQUIRE(loaded->data.siteFolders[1].parentId == rootFolderId);

    auto rootFolder = loaded->data.siteFolders[0];
    auto childFolder = loaded->data.siteFolders[1];

    rootFolder.name = "Connections";
    rootFolder.siteIds.clear();

    childFolder.siteIds.insert(childFolder.siteIds.begin(),
                               std::string{alphaId});

    loaded->data.siteFolders = {std::move(childFolder),
                                std::move(rootFolder)};

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("id: \"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb\"") <
            saved.find("id: \"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa\""));
    REQUIRE(saved.find("name: \"Connections\" # folder name inline") !=
            std::string::npos);
    REQUIRE(saved.find("# root folder leading") != std::string::npos);
    REQUIRE(saved.find("# child folder leading") != std::string::npos);
    REQUIRE(saved.find("# alpha placement") != std::string::npos);
    REQUIRE(saved.find("# placement inline") != std::string::npos);
    REQUIRE(saved.find("pluginFolderField: \"untouched\"") !=
            std::string::npos);
    REQUIRE(saved.find("# folders closing") != std::string::npos);

    CsonConfigRepository reloadedRepository(file);

    auto reloaded = reloadedRepository.Load();

    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->data.siteFolders == loaded->data.siteFolders);
  }

  TEST_CASE("Same-named sites in different folders retain their own lossless nodes")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    const std::string rootSiteId = "11111111-1111-4111-8111-111111111111";
    const std::string workSiteId = "22222222-2222-4222-8222-222222222222";
    const std::string personalSiteId = "33333333-3333-4333-8333-333333333333";
    const std::string workFolderId = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
    const std::string personalFolderId = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";

    WriteText(file, ConfigWithSites({
                        "  # root site\n" + SiteNode(rootSiteId, "Production", "root.example"),
                        "  # work site\n" + SiteNode(workSiteId, "Production", "work.example"),
                        "  # personal site\n" + SiteNode(personalSiteId, "Production", "personal.example"),
                    }));

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());

    loaded->data.siteFolders = {
        {workFolderId, "Work", {}, {workSiteId}},
        {personalFolderId, "Personal", {}, {personalSiteId}},
    };
    loaded->data.siteManagerOrder = {rootSiteId, workFolderId, workSiteId,
                                     personalFolderId, personalSiteId};

    REQUIRE(repository.Save(loaded->data).has_value());

    for (int cycle = 0; cycle < 2; ++cycle)
    {
      CsonConfigRepository reopened(file);

      auto current = reopened.Load();

      REQUIRE(current.has_value());
      REQUIRE(current->data.siteFolders == loaded->data.siteFolders);
      REQUIRE(current->data.siteManagerOrder == loaded->data.siteManagerOrder);
      REQUIRE(current->data.sites.size() == 3);

      for (const auto &site : current->data.sites)
      {
        REQUIRE(site.name == "Production");
      }

      // Edit and reorder only the work profile. Identical display names must
      // not merge sites or attach a sibling's comments to it.
      auto work = std::ranges::find(current->data.sites, workSiteId, &SiteProfile::id);

      REQUIRE(work != current->data.sites.end());

      work->host = "edited-work.example";

      std::ranges::reverse(current->data.sites);

      REQUIRE(reopened.Save(current->data).has_value());

      const auto saved = ReadText(file);

      auto previousNodeEnd = saved.find("sites:");

      REQUIRE(previousNodeEnd != std::string::npos);

      for (const auto &site : current->data.sites)
      {
        const auto label = site.id == workSiteId ? "work" : site.id == personalSiteId ? "personal"
                                                                                      : "root";
        const auto commentPosition = saved.find(std::string{"# "} + label + " site");
        const auto idPosition = saved.find("id: \"" + site.id + '"');

        REQUIRE(commentPosition != std::string::npos);
        REQUIRE(idPosition != std::string::npos);
        REQUIRE(previousNodeEnd < commentPosition);
        REQUIRE(commentPosition < idPosition);

        previousNodeEnd = saved.find('}', idPosition);

        REQUIRE(previousNodeEnd != std::string::npos);
        REQUIRE(site.host == (site.id == workSiteId ? "edited-work.example" : site.id == personalSiteId ? "personal.example"
                                                                                                        : "root.example"));
      }
    }
  }

  TEST_CASE("Missing Site Manager order preserves the legacy tree presentation")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "legacy-site-manager-order.cson";

    constexpr std::string_view alphaId =
        "11111111-1111-4111-8111-111111111111";
    constexpr std::string_view betaId =
        "22222222-2222-4222-8222-222222222222";
    constexpr std::string_view rootSiteId =
        "33333333-3333-4333-8333-333333333333";
    constexpr std::string_view rootFolderId =
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
    constexpr std::string_view childFolderId =
        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
    constexpr std::string_view siblingFolderId =
        "cccccccc-cccc-4ccc-8ccc-cccccccccccc";

    std::string text = ConfigWithSites({
        SiteNode(alphaId, "Alpha", "alpha.example"),
        SiteNode(betaId, "Beta", "beta.example"),
        SiteNode(rootSiteId, "Root", "root.example"),
    });

    ReplaceOnce(text, "siteFolders: []", R"(siteFolders: [
  { id: "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", name: "Servers", parentId: "", siteIds: ["11111111-1111-4111-8111-111111111111"] }
  { id: "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", name: "Archive", parentId: "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", siteIds: ["22222222-2222-4222-8222-222222222222"] }
  { id: "cccccccc-cccc-4ccc-8ccc-cccccccccccc", name: "Other", parentId: "", siteIds: [] }
])");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.siteManagerOrder == std::vector<std::string>{
                                                 std::string{rootFolderId},
                                                 std::string{childFolderId},
                                                 std::string{betaId},
                                                 std::string{alphaId},
                                                 std::string{siblingFolderId},
                                                 std::string{rootSiteId},
                                             });

    REQUIRE(repository.Save(loaded->data).has_value());

    const auto saved = ReadText(file);

    REQUIRE(saved.find("siteFolders:") < saved.find("siteManagerOrder:"));
    REQUIRE(saved.find("siteManagerOrder:") < saved.find("tlsTrust:"));

    CsonConfigRepository reloadedRepository(file);

    const auto reloaded = reloadedRepository.Load();

    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->data.siteManagerOrder == loaded->data.siteManagerOrder);
  }

  TEST_CASE("Explicit Site Manager order must be a complete unique id permutation")
  {
    TempDirectory directory;

    constexpr std::string_view siteId =
        "11111111-1111-4111-8111-111111111111";
    constexpr std::string_view folderId =
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";

    std::string base = ConfigWithSites(
        {SiteNode(siteId, "Alpha", "alpha.example")});

    ReplaceOnce(base, "siteFolders: []", R"(siteFolders: [
  { id: "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", name: "Servers", parentId: "", siteIds: ["11111111-1111-4111-8111-111111111111"] }
])");

    const std::array invalidOrders{
        std::string{"siteManagerOrder: {}\n"},
        std::string{"siteManagerOrder: [\n  \""} + std::string{folderId} +
            "\"\n]\n",
        std::string{"siteManagerOrder: [\n  \""} + std::string{folderId} +
            "\"\n  \"" + std::string{siteId} + "\"\n  \"" +
            std::string{siteId} + "\"\n]\n",
        std::string{"siteManagerOrder: [\n  \""} + std::string{folderId} +
            "\"\n  42\n]\n",
        std::string{"siteManagerOrder: [\n  \""} + std::string{folderId} +
            "\"\n  \"missing\"\n]\n",
    };

    for (std::size_t index = 0; index < invalidOrders.size(); ++index)
    {
      CAPTURE(index);

      auto text = base;

      ReplaceOnce(text, "tlsTrust: []", invalidOrders[index] + "tlsTrust: []");

      const auto file = directory.Path() /
                        ("invalid-site-manager-order-" +
                         std::to_string(index) + ".cson");

      WriteText(file, text);

      CsonConfigRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind == ConfigErrorKind::Validation);
      REQUIRE(loaded.error().message.find("siteManagerOrder") !=
              std::string::npos);
      REQUIRE(ReadText(file) == text);
    }
  }

  TEST_CASE("Site Manager order edits retain comments across repeated saves")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "site-manager-order-comments.cson";

    constexpr std::string_view alphaId =
        "11111111-1111-4111-8111-111111111111";
    constexpr std::string_view betaId =
        "22222222-2222-4222-8222-222222222222";

    std::string text = ConfigWithSites({
        SiteNode(alphaId, "Alpha", "alpha.example"),
        SiteNode(betaId, "Beta", "beta.example"),
    });

    ReplaceOnce(text, "tlsTrust: []", R"(siteManagerOrder: [
  # beta leading
  "22222222-2222-4222-8222-222222222222" # beta inline
  # alpha leading
  "11111111-1111-4111-8111-111111111111"
  # order closing
]
tlsTrust: [])");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());

    std::swap(loaded->data.siteManagerOrder[0],
              loaded->data.siteManagerOrder[1]);

    REQUIRE(repository.Save(loaded->data).has_value());

    const auto saved = ReadText(file);

    const auto orderStart = saved.find("siteManagerOrder:");
    const auto orderEnd = saved.find("tlsTrust:", orderStart);

    REQUIRE(orderStart != std::string::npos);
    REQUIRE(orderEnd != std::string::npos);

    const auto orderBlock = saved.substr(orderStart, orderEnd - orderStart);

    REQUIRE(orderBlock.find("# alpha leading") <
            orderBlock.find(alphaId));
    REQUIRE(orderBlock.find(alphaId) <
            orderBlock.find("# beta leading"));
    REQUIRE(orderBlock.find("# beta leading") <
            orderBlock.find(betaId));
    REQUIRE(orderBlock.find("# beta inline") != std::string::npos);
    REQUIRE(orderBlock.find("# order closing") != std::string::npos);

    CsonConfigRepository reloadedRepository(file);

    auto reloaded = reloadedRepository.Load();

    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->data.siteManagerOrder == loaded->data.siteManagerOrder);
    REQUIRE(reloadedRepository.Save(reloaded->data).has_value());
    REQUIRE(ReadText(file) == saved);
  }

  TEST_CASE("Site folder relationships reject missing references duplicates and cycles")
  {
    TempDirectory directory;

    constexpr std::string_view siteId =
        "11111111-1111-4111-8111-111111111111";

    const auto base = ConfigWithSites(
        {SiteNode(siteId, "Alpha", "alpha.example")});

    const std::array<std::string_view, 4> invalidFolders{
        R"(siteFolders: [
  { id: "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", name: "Missing parent", parentId: "missing", siteIds: [] }
])",
        R"(siteFolders: [
  { id: "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", name: "A", parentId: "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", siteIds: [] }
  { id: "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", name: "B", parentId: "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", siteIds: [] }
])",
        R"(siteFolders: [
  { id: "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", name: "A", parentId: "", siteIds: ["11111111-1111-4111-8111-111111111111"] }
  { id: "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", name: "B", parentId: "", siteIds: ["11111111-1111-4111-8111-111111111111"] }
])",
        R"(siteFolders: [
  { id: "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", name: "Unknown site", parentId: "", siteIds: ["99999999-9999-4999-8999-999999999999"] }
])",
    };

    for (std::size_t index = 0; index < invalidFolders.size(); ++index)
    {
      CAPTURE(index);

      std::string text = base;

      ReplaceOnce(text, "siteFolders: []", invalidFolders[index]);

      const auto file = directory.Path() /
                        ("invalid-site-folders-" + std::to_string(index) +
                         ".cson");

      WriteText(file, text);

      CsonConfigRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind == ConfigErrorKind::Validation);
      REQUIRE(ReadText(file) == text);
    }
  }

  TEST_CASE("Unicode paths and credential references round trip without secrets")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    CsonConfigRepository repository(file);

    ConfigData data;

    SiteProfile site;
    site.id = "10f81b67-02b1-4a69-bf3e-42c90d67ce26";
    site.name = "München";
    site.protocol = ProtocolKind::Sftp;
    site.host = "sftp.example";
    site.port = 22;
    site.username = "rené";
    site.authentication.kind = AuthenticationKind::PrivateKey;
    site.authentication.privateKeyFile = std::filesystem::path(
        std::u8string(u8"C:/Schlüssel/id_ed25519"));
    site.authentication.credentialId = "havRemote/site/10f81/password";
    site.authentication.passphraseCredentialId = "havRemote/site/10f81/key";
    site.initialRemoteDirectory = RemotePath("/Übertragung");
    site.initialLocalDirectory =
        std::filesystem::path(std::u8string(u8"D:/Téléchargements"));

    data.sites.push_back(site);
    data.siteManagerOrder.push_back(site.id);

    REQUIRE(repository.Save(data).has_value());

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.sites.size() == 1);
    REQUIRE(loaded->data.sites[0].name == site.name);
    REQUIRE(loaded->data.sites[0].initialRemoteDirectory ==
            site.initialRemoteDirectory);
    REQUIRE(loaded->data.sites[0].initialLocalDirectory ==
            site.initialLocalDirectory);
    REQUIRE(loaded->data.sites[0].authentication.privateKeyFile ==
            site.authentication.privateKeyFile);

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("credentialId") != std::string::npos);
    REQUIRE(saved.find("passphraseCredentialId") != std::string::npos);
    REQUIRE(saved.find("password:") == std::string::npos);
    REQUIRE(saved.find("passphrase:") == std::string::npos);
  }

  TEST_CASE("FTP data connection modes round trip without changing format version")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    auto text = ConfigWithSites({SiteNode(
        "d79fbf76-b4bf-4fbd-832f-ceebf82dc7db", "Active FTP",
        "ftp.example")});

    ReplaceOnce(text, "    protocol: \"sftp\"", "    protocol: \"ftp\"");
    ReplaceOnce(text, "    port: 22", "    port: 21");
    ReplaceOnce(text, "    ftpEncoding: \"UTF-8\"",
                "    ftpEncoding: \"UTF-8\"\n"
                "    # The server connects back to this client.\n"
                "    # Local interface override for a multi-homed client.\n"
                "    ftpDataConnectionMode: \"active\" # keep active\n"
                "    ftpActiveAddress: \"2001:db8::25\" # keep address\n"
                "    pluginSiteOption: true");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    std::string loadError;

    if (!loaded)
    {
      loadError = loaded.error().message;

      if (loaded.error().line && loaded.error().column)
      {
        loadError += " at " + std::to_string(*loaded.error().line) + ":" +
                     std::to_string(*loaded.error().column);
      }
    }

    INFO(loadError);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.formatVersion == 1);
    REQUIRE(loaded->data.sites.size() == 1);
    CHECK(loaded->data.sites[0].ftpDataConnectionMode ==
          FtpDataConnectionMode::Active);
    CHECK(loaded->data.sites[0].ftpActiveAddress ==
          "2001:db8::25");

    REQUIRE(repository.Save(loaded->data).has_value());

    const auto saved = ReadText(file);

    CHECK(saved.find("formatVersion: 1") != std::string::npos);
    CHECK(saved.find("# The server connects back to this client.") !=
          std::string::npos);
    CHECK(saved.find("ftpDataConnectionMode: \"active\" # keep active") !=
          std::string::npos);
    CHECK(saved.find("# Local interface override for a multi-homed client.") !=
          std::string::npos);
    CHECK(saved.find(
              "ftpActiveAddress: \"2001:db8::25\" # keep address") !=
          std::string::npos);
    CHECK(saved.find("pluginSiteOption: true") != std::string::npos);
  }

  TEST_CASE("Missing FTP data connection mode defaults to passive and is added losslessly")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    auto text = ConfigWithSites({SiteNode(
        "c9283e42-9e65-4776-86e5-aa694bd98564", "Existing site",
        "sftp.example")});

    ReplaceOnce(text, "    ftpEncoding: \"UTF-8\"",
                "    # encoding comment\n"
                "    ftpEncoding: \"UTF-8\"\n"
                "    # extension comment\n"
                "    pluginSiteOption: true");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    std::string loadError;

    if (!loaded)
    {
      loadError = loaded.error().message;

      if (loaded.error().line && loaded.error().column)
      {
        loadError += " at " + std::to_string(*loaded.error().line) + ":" +
                     std::to_string(*loaded.error().column);
      }
    }

    INFO(loadError);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.sites.size() == 1);
    CHECK(loaded->data.sites[0].ftpDataConnectionMode ==
          FtpDataConnectionMode::Passive);
    CHECK(loaded->data.sites[0].ftpActiveAddress.empty());

    REQUIRE(repository.Save(loaded->data).has_value());

    const auto saved = ReadText(file);

    CHECK(saved.find("# encoding comment") !=
          std::string::npos);
    CHECK(saved.find("# extension comment") !=
          std::string::npos);
    CHECK(saved.find("pluginSiteOption: true") !=
          std::string::npos);
    CHECK(saved.find("ftpDataConnectionMode: \"passive\"") !=
          std::string::npos);
    CHECK(saved.find("ftpActiveAddress: \"\"") !=
          std::string::npos);
    CHECK(saved.find("pluginSiteOption:") <
          saved.find("ftpDataConnectionMode:"));
    CHECK(saved.find("ftpDataConnectionMode:") <
          saved.find("ftpActiveAddress:"));
  }

  TEST_CASE("FTP data connection mode rejects wrong types unknown values and active SFTP")
  {
    TempDirectory directory;

    const auto base = ConfigWithSites({SiteNode(
        "20c3a238-194b-46ec-972c-f6024320affd", "Mode validation",
        "files.example")});

    constexpr std::array invalidFields{
        "    ftpDataConnectionMode: true\n",
        "    ftpDataConnectionMode: \"automatic\"\n",
        "    ftpDataConnectionMode: \"active\"\n",
    };

    for (std::size_t index = 0; index < invalidFields.size(); ++index)
    {
      CAPTURE(index);

      auto text = base;

      ReplaceOnce(text, "    ftpEncoding: \"UTF-8\"\n",
                  "    ftpEncoding: \"UTF-8\"\n" +
                      std::string{invalidFields[index]});

      const auto file = directory.Path() /
                        ("invalid-ftp-mode-" + std::to_string(index) +
                         ".cson");

      WriteText(file, text);

      CsonConfigRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      CHECK(loaded.error().kind == ConfigErrorKind::Validation);
      CHECK(loaded.error().message.find("ftpDataConnectionMode") !=
            std::string::npos);
      CHECK(ReadText(file) == text);
    }
  }

  TEST_CASE("FTP active local address accepts only bare IP literals")
  {
    TempDirectory directory;

    const auto base = ConfigWithSites({SiteNode(
        "8855c34a-bc30-4a75-88c7-469697fc59e8", "Address validation",
        "files.example")});

    struct InvalidAddress
    {
      std::string_view field;
      bool sftp{};
    };

    constexpr std::array invalidAddresses{
        InvalidAddress{"    ftpActiveAddress: true\n", false},
        InvalidAddress{"    ftpActiveAddress: \"client.example\"\n", false},
        InvalidAddress{"    ftpActiveAddress: \"[2001:db8::25]\"\n", false},
        InvalidAddress{"    ftpActiveAddress: \"fe80::1%12\"\n", false},
        InvalidAddress{"    ftpActiveAddress: \" 192.0.2.25\"\n", false},
        InvalidAddress{"    ftpActiveAddress: \"192.0.2.25:2020\"\n", false},
        InvalidAddress{"    ftpActiveAddress: \"ftp://192.0.2.25\"\n", false},
        InvalidAddress{"    ftpActiveAddress: \"192.0.2.25\"\n", true},
    };

    for (std::size_t index = 0; index < invalidAddresses.size(); ++index)
    {
      CAPTURE(index);

      auto text = base;

      if (!invalidAddresses[index].sftp)
      {
        ReplaceOnce(text, "    protocol: \"sftp\"", "    protocol: \"ftp\"");
        ReplaceOnce(text, "    port: 22", "    port: 21");
      }

      ReplaceOnce(text, "    ftpEncoding: \"UTF-8\"\n",
                  "    ftpEncoding: \"UTF-8\"\n" +
                      std::string{invalidAddresses[index].field});

      const auto file = directory.Path() /
                        ("invalid-active-address-" + std::to_string(index) +
                         ".cson");

      WriteText(file, text);

      CsonConfigRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      CHECK(loaded.error().kind == ConfigErrorKind::Validation);
      CHECK(loaded.error().message.find("ftpActiveAddress") !=
            std::string::npos);
      CHECK(ReadText(file) == text);
    }
  }

  TEST_CASE("Password and keyboard-interactive authentication round trips in format version one")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    CsonConfigRepository repository(file);

    ConfigData data;

    SiteProfile site;
    site.id = "c991381d-68e2-4ef0-9f33-b68c4f7cf6f1";
    site.name = "MFA server";
    site.protocol = ProtocolKind::Sftp;
    site.host = "mfa.example";
    site.port = 22;
    site.username = "mfa-user";
    site.authentication.kind =
        AuthenticationKind::PasswordKeyboardInteractive;
    site.authentication.credentialId = "site-mfa-password";
    site.initialRemoteDirectory = RemotePath::Root();
    site.initialLocalDirectory = "C:/Downloads";
    data.sites.push_back(site);
    data.siteManagerOrder.push_back(site.id);

    REQUIRE(repository.Save(data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("formatVersion: 1") != std::string::npos);
    REQUIRE(saved.find("kind: \"passwordKeyboardInteractive\"") !=
            std::string::npos);
    REQUIRE(saved.find("credentialId: \"site-mfa-password\"") !=
            std::string::npos);
    REQUIRE(saved.find("password:") == std::string::npos);
    REQUIRE(saved.find("totp") == std::string::npos);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.sites.size() == 1);
    CHECK(loaded->data.sites[0].authentication.kind ==
          AuthenticationKind::PasswordKeyboardInteractive);
    CHECK(loaded->data.sites[0].authentication.credentialId ==
          "site-mfa-password");
  }

  TEST_CASE("Decoded endpoint hosts reject URL userinfo and embedded-port syntax")
  {
    TempDirectory directory;

    constexpr std::array invalidHosts{
        "sftp://example.com",
        "display-name@example.com",
        "example.com:22",
    };

    for (std::size_t index = 0; index < invalidHosts.size(); ++index)
    {
      CAPTURE(invalidHosts[index]);

      const auto siteFile = directory.Path() /
                            ("invalid-site-host-" + std::to_string(index) + ".cson");

      const std::string siteText = ConfigWithSites({SiteNode(
          "32c0f4df-ec14-4c87-a510-65dad7a83bca", "Endpoint", invalidHosts[index])});

      WriteText(siteFile, siteText);

      CsonConfigRepository siteRepository(siteFile);

      const auto siteResult = siteRepository.Load();

      REQUIRE_FALSE(siteResult.has_value());
      REQUIRE(siteResult.error().kind == ConfigErrorKind::Validation);
      REQUIRE(siteResult.error().message.find("sites[0].host") != std::string::npos);
      REQUIRE(ReadText(siteFile) == siteText);

      const auto trustFile = directory.Path() /
                             ("invalid-trust-host-" + std::to_string(index) + ".cson");

      const std::string trustText = ConfigWithTlsTrust(invalidHosts[index]);

      WriteText(trustFile, trustText);

      CsonConfigRepository trustRepository(trustFile);

      const auto trustResult = trustRepository.Load();

      REQUIRE_FALSE(trustResult.has_value());
      REQUIRE(trustResult.error().kind == ConfigErrorKind::Validation);
      REQUIRE(trustResult.error().message.find("tlsTrust[0].host") != std::string::npos);
      REQUIRE(ReadText(trustFile) == trustText);
    }
  }

  TEST_CASE("Typed saves reject unsafe endpoint hosts")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    CsonConfigRepository repository(file);

    ConfigData data;

    SiteProfile site;
    site.id = "cb95744a-8dd5-4660-81ee-af0273bed090";
    site.name = "Unsafe endpoint";
    site.protocol = ProtocolKind::Sftp;
    site.host = "sftp://example.com";
    site.port = 22;
    site.ftpEncoding = "UTF-8";

    data.sites.push_back(site);
    data.siteManagerOrder.push_back(site.id);

    auto siteSave = repository.Save(data);

    REQUIRE_FALSE(siteSave.has_value());
    REQUIRE(siteSave.error().kind == ConfigErrorKind::Validation);
    REQUIRE_FALSE(std::filesystem::exists(file));

    data.sites.clear();
    data.siteManagerOrder.clear();
    data.tlsTrust.push_back({"user@example.com", 21, "sha256//YWJjZA=="});

    auto trustSave = repository.Save(data);

    REQUIRE_FALSE(trustSave.has_value());
    REQUIRE(trustSave.error().kind == ConfigErrorKind::Validation);
    REQUIRE_FALSE(std::filesystem::exists(file));

    data.tlsTrust.clear();
    data.quickConnectHistory.push_back(
        {ProtocolKind::Sftp, "sftp://example.com", 22, "alice"});

    auto historySave = repository.Save(data);

    REQUIRE_FALSE(historySave.has_value());
    REQUIRE(historySave.error().kind == ConfigErrorKind::Validation);
    REQUIRE_FALSE(std::filesystem::exists(file));
  }

  TEST_CASE("TLS trust edits preserve record comments unknown fields and ordering")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    std::string text = ConfigWithTlsTrust("secure.example");

    ReplaceOnce(text, "    host: \"secure.example\"",
                "    # trust leading\n    host: \"secure.example\"");
    ReplaceOnce(text, "    publicKeyPin: \"sha256//YWJjZA==\"",
                "    publicKeyPin: \"sha256//YWJjZA==\" # PIN inline\n"
                "    pluginTrustField: \"untouched\"");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->data.tlsTrust.size() == 1);

    loaded->data.tlsTrust.front().publicKeyPin = "sha256//ZWZnaA==";

    REQUIRE(repository.Save(loaded->data).has_value());

    const std::string saved = ReadText(file);

    REQUIRE(saved.find("# trust leading") < saved.find("host: \"secure.example\""));
    REQUIRE(saved.find("publicKeyPin: \"sha256//ZWZnaA==\" # PIN inline") !=
            std::string::npos);
    REQUIRE(saved.find("pluginTrustField: \"untouched\"") != std::string::npos);
    REQUIRE(saved.find("publicKeyPin") < saved.find("pluginTrustField"));
  }

  TEST_CASE("Malformed duplicate and future-version files are never overwritten")
  {
    TempDirectory directory;

    const auto malformed = directory.Path() / "malformed.cson";

    const std::string malformedText = "formatVersion: 1\nsettings: \"\\q\"\n";

    WriteText(malformed, malformedText);

    CsonConfigRepository malformedRepository(malformed);

    auto malformedResult = malformedRepository.Load();

    REQUIRE_FALSE(malformedResult.has_value());
    REQUIRE(malformedResult.error().kind == ConfigErrorKind::Parse);
    REQUIRE(malformedResult.error().path == malformed);
    REQUIRE(malformedResult.error().path.filename() == "malformed.cson");
    REQUIRE(malformedResult.error().line.has_value());
    REQUIRE(*malformedResult.error().line == 2);
    REQUIRE(malformedResult.error().column.has_value());
    REQUIRE(ReadText(malformed) == malformedText);

    const auto duplicate = directory.Path() / "duplicate.cson";
    const std::string duplicateText = ValidEmptyConfig() + "formatVersion: 1\n";

    WriteText(duplicate, duplicateText);

    CsonConfigRepository duplicateRepository(duplicate);

    auto duplicateResult = duplicateRepository.Load();

    REQUIRE_FALSE(duplicateResult.has_value());
    REQUIRE(duplicateResult.error().kind == ConfigErrorKind::Parse);
    REQUIRE(duplicateResult.error().path == duplicate);
    REQUIRE(duplicateResult.error().line.has_value());
    REQUIRE(duplicateResult.error().column.has_value());
    REQUIRE(ReadText(duplicate) == duplicateText);

    const auto future = directory.Path() / "future.cson";
    std::string futureRoot = ValidEmptyConfig();
    futureRoot.replace(futureRoot.find("formatVersion: 1"),
                       std::string("formatVersion: 1").size(),
                       "formatVersion: 999");

    std::string futureText =
        "# formatVersion: 777 is only a comment\n"
        "plugin:\n"
        "  note: \"formatVersion: 888 is only text\"\n"
        "  formatVersion: 123\n" +
        futureRoot;

    WriteText(future, futureText);

    CsonConfigRepository futureRepository(future);

    auto futureResult = futureRepository.Load();

    REQUIRE_FALSE(futureResult.has_value());
    REQUIRE(futureResult.error().kind == ConfigErrorKind::UnsupportedVersion);
    REQUIRE(futureResult.error().path == future);
    REQUIRE(futureResult.error().path.filename() == "future.cson");
    REQUIRE(futureResult.error().line.has_value());
    REQUIRE(*futureResult.error().line == 5);
    REQUIRE(futureResult.error().column.has_value());
    REQUIRE(*futureResult.error().column == 1);
    REQUIRE(ReadText(future) == futureText);
  }

  TEST_CASE("Schema rejects fractions ranges duplicate ids and secret fields")
  {
    TempDirectory directory;

    SECTION("fractional bounded setting")
    {
      const auto file = directory.Path() / "fraction.cson";

      std::string text = ValidEmptyConfig();
      text.replace(text.find("transferConcurrency: 2"),
                   std::string("transferConcurrency: 2").size(),
                   "transferConcurrency: 2.5");

      WriteText(file, text);

      CsonConfigRepository repository(file);

      auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind == ConfigErrorKind::Validation);
    }

    SECTION("future unknown field cannot smuggle a secret")
    {
      const auto file = directory.Path() / "secret.cson";

      std::string text = ValidEmptyConfig() + "plugin:\n  password: \"plaintext\"\n";

      WriteText(file, text);

      CsonConfigRepository repository(file);

      auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind == ConfigErrorKind::Validation);
      REQUIRE(loaded.error().message.find("plaintext") == std::string::npos);
      REQUIRE(ReadText(file) == text);
    }

    SECTION("typed save range")
    {
      const auto file = directory.Path() / "typed.cson";

      CsonConfigRepository repository(file);

      ConfigData data;
      data.settings.transferConcurrency = 0;

      auto saved = repository.Save(data);

      REQUIRE_FALSE(saved.has_value());
      REQUIRE(saved.error().kind == ConfigErrorKind::Validation);
      REQUIRE_FALSE(std::filesystem::exists(file));
    }

    SECTION("duplicate stable site ids")
    {
      const auto file = directory.Path() / "duplicate-sites.cson";

      const auto duplicate = SiteNode("5ee80921-c826-498f-94c4-9c3725ef8863",
                                      "Duplicate", "one.example");

      const std::string text = ConfigWithSites({duplicate, duplicate});

      WriteText(file, text);

      CsonConfigRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind == ConfigErrorKind::Validation);
      REQUIRE(loaded.error().message.find("duplicate stable id") != std::string::npos);
      REQUIRE(ReadText(file) == text);
    }

    SECTION("duplicate Quick Connect entries use case-insensitive hosts")
    {
      const auto file = directory.Path() / "duplicate-quick-connect.cson";

      const std::string text = ConfigWithQuickConnectHistory({
          QuickConnectHistoryNode("sftp", "EXAMPLE.com", 22, "alice"),
          QuickConnectHistoryNode("sftp", "example.COM", 22, "alice"),
      });

      WriteText(file, text);

      CsonConfigRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind == ConfigErrorKind::Validation);
      REQUIRE(loaded.error().message.find("duplicate connection") !=
              std::string::npos);
      REQUIRE(ReadText(file) == text);
    }

    SECTION("Quick Connect history has a bounded size")
    {
      const auto file = directory.Path() / "oversized-quick-connect.cson";

      std::vector<std::string> nodes;

      for (std::size_t index = 0; index <= MaxQuickConnectHistoryEntries; ++index)
      {
        nodes.push_back(QuickConnectHistoryNode(
            "sftp", "host" + std::to_string(index) + ".example", 22,
            "alice"));
      }

      const std::string text = ConfigWithQuickConnectHistory(nodes);

      WriteText(file, text);

      CsonConfigRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind == ConfigErrorKind::Validation);
      REQUIRE(loaded.error().message.find("no more than") != std::string::npos);
      REQUIRE(ReadText(file) == text);
    }

    SECTION("duplicate TLS endpoints are case insensitive")
    {
      const auto file = directory.Path() / "duplicate-trust.cson";

      auto text = ConfigWithTlsTrust("EXAMPLE.com");

      const auto closing = text.rfind(']');

      REQUIRE(closing != std::string::npos);

      text.insert(closing,
                  "  {\n"
                  "    host: \"example.COM\"\n"
                  "    port: 21\n"
                  "    publicKeyPin: \"sha256//ZWZnaA==\"\n"
                  "  }\n");

      WriteText(file, text);

      CsonConfigRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind == ConfigErrorKind::Validation);
      REQUIRE(loaded.error().message.find("duplicate host/port") != std::string::npos);
      REQUIRE(ReadText(file) == text);
    }
  }

  TEST_CASE("Every schema number is integral and range checked")
  {
    TempDirectory directory;

    struct InvalidNumber final
    {
      std::string_view original;
      std::string_view replacement;
    };

    constexpr std::array invalidSettings{
        InvalidNumber{"formatVersion: 1", "formatVersion: 1.5"},
        InvalidNumber{"transferConcurrency: 2", "transferConcurrency: 0"},
        InvalidNumber{"transferConcurrency: 2", "transferConcurrency: 17"},
        InvalidNumber{"connectionTimeoutSeconds: 20", "connectionTimeoutSeconds: 0"},
        InvalidNumber{"connectionTimeoutSeconds: 20", "connectionTimeoutSeconds: 601"},
        InvalidNumber{"commandIdleTimeoutSeconds: 60", "commandIdleTimeoutSeconds: 1.25"},
        InvalidNumber{"commandIdleTimeoutSeconds: 60", "commandIdleTimeoutSeconds: 3601"},
    };

    for (std::size_t index = 0; index < invalidSettings.size(); ++index)
    {
      CAPTURE(invalidSettings[index].replacement);

      auto text = ValidEmptyConfig();

      ReplaceOnce(text, invalidSettings[index].original,
                  invalidSettings[index].replacement);

      const auto file = directory.Path() /
                        ("invalid-setting-number-" + std::to_string(index) + ".cson");

      WriteText(file, text);

      CsonConfigRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind == ConfigErrorKind::Validation);
      REQUIRE(ReadText(file) == text);
    }

    constexpr std::array invalidPorts{"port: 0", "port: 65536", "port: 22.5"};

    for (std::size_t index = 0; index < invalidPorts.size(); ++index)
    {
      CAPTURE(invalidPorts[index]);

      auto text = ConfigWithSites({SiteNode(
          "d78df8ae-2201-43e9-9791-5a379c98b859", "Port", "port.example")});

      ReplaceOnce(text, "port: 22", invalidPorts[index]);

      const auto file = directory.Path() /
                        ("invalid-site-port-" + std::to_string(index) + ".cson");

      WriteText(file, text);

      CsonConfigRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind == ConfigErrorKind::Validation);
    }

    auto historyText = ConfigWithQuickConnectHistory(
        {QuickConnectHistoryNode("sftp", "history.example", 22, "alice")});

    ReplaceOnce(historyText, "port: 22", "port: 22.5");

    const auto historyFile = directory.Path() / "fractional-history-port.cson";

    WriteText(historyFile, historyText);

    CsonConfigRepository historyRepository(historyFile);

    const auto historyResult = historyRepository.Load();

    REQUIRE_FALSE(historyResult.has_value());
    REQUIRE(historyResult.error().kind == ConfigErrorKind::Validation);

    auto trustText = ConfigWithTlsTrust("trust.example");

    ReplaceOnce(trustText, "port: 21", "port: 21.5");

    const auto trustFile = directory.Path() / "fractional-trust-port.cson";

    WriteText(trustFile, trustText);

    CsonConfigRepository trustRepository(trustFile);

    const auto trustResult = trustRepository.Load();

    REQUIRE_FALSE(trustResult.has_value());
    REQUIRE(trustResult.error().kind == ConfigErrorKind::Validation);
  }

  TEST_CASE("External editor documents reject invalid modes paths and argument templates")
  {
    TempDirectory directory;

    std::vector<std::string> invalidDocuments;
    invalidDocuments.push_back(ConfigWithExternalEditor(
        "automatic", "", "{file}"));
    invalidDocuments.push_back(ConfigWithExternalEditor(
        "custom", "", "{file}"));
    invalidDocuments.push_back(ConfigWithExternalEditor(
        "custom", "C:/Tools/editor.exe", "--wait"));
    invalidDocuments.push_back(ConfigWithExternalEditor(
        "custom", "C:/Tools/editor.exe", "{file} --diff {file}"));
    invalidDocuments.push_back(ConfigWithExternalEditor(
        "custom", "C:/Tools/editor.exe", "--wait \\\"{file}"));
    invalidDocuments.push_back(ConfigWithExternalEditor(
        "custom", "C:/Tools/edi\\ntor.exe", "{file}"));
    invalidDocuments.push_back(ConfigWithExternalEditor(
        "system", "", "{file}\\t--wait"));

    auto wrongShape = ValidEmptyConfig();

    std::string wrongShapeReplacement{DefaultFileListsBlock};

    wrongShapeReplacement += "  externalEditor: true\n";

    ReplaceOnce(wrongShape, DefaultFileListsBlock,
                wrongShapeReplacement);

    invalidDocuments.push_back(std::move(wrongShape));

    auto wrongArgumentsType = ConfigWithExternalEditor(
        "system", "", "{file}");

    ReplaceOnce(wrongArgumentsType, "arguments: \"{file}\"",
                "arguments: false");

    invalidDocuments.push_back(std::move(wrongArgumentsType));

    for (std::size_t index = 0; index < invalidDocuments.size(); ++index)
    {
      CAPTURE(index);

      const auto file = directory.Path() /
                        ("invalid-external-editor-" +
                         std::to_string(index) + ".cson");

      WriteText(file, invalidDocuments[index]);

      CsonConfigRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind == ConfigErrorKind::Validation);
      REQUIRE(ReadText(file) == invalidDocuments[index]);
    }
  }

  TEST_CASE("Schema validates object shapes field types and enum values")
  {
    TempDirectory directory;

    std::vector<std::string> invalidDocuments;

    invalidDocuments.push_back(R"(formatVersion: 1
settings: []
sites: []
tlsTrust: []
)");

    auto wrongSites = ValidEmptyConfig();

    ReplaceOnce(wrongSites, "sites: []", "sites: {}");

    invalidDocuments.push_back(std::move(wrongSites));

    auto wrongSiteFolders = ValidEmptyConfig();

    ReplaceOnce(wrongSiteFolders, "siteFolders: []", "siteFolders: {}");

    invalidDocuments.push_back(std::move(wrongSiteFolders));

    auto wrongFolderSiteIds = ValidEmptyConfig();

    ReplaceOnce(wrongFolderSiteIds, "siteFolders: []", R"(siteFolders: [
  { id: "folder", name: "Folder", parentId: "", siteIds: "site" }
])");

    invalidDocuments.push_back(std::move(wrongFolderSiteIds));

    auto wrongHistory = ValidEmptyConfig();

    ReplaceOnce(wrongHistory, "quickConnectHistory: []",
                "quickConnectHistory: {}");

    invalidDocuments.push_back(std::move(wrongHistory));

    auto wrongHistoryProtocol = ConfigWithQuickConnectHistory(
        {QuickConnectHistoryNode("gopher", "history.example", 70, "alice")});

    invalidDocuments.push_back(std::move(wrongHistoryProtocol));

    auto missingHistoryUsername = ConfigWithQuickConnectHistory(
        {QuickConnectHistoryNode("sftp", "history.example", 22, "alice")});

    ReplaceOnce(missingHistoryUsername, "    username: \"alice\"\n", "");

    invalidDocuments.push_back(std::move(missingHistoryUsername));

    auto wrongTrust = ValidEmptyConfig();

    ReplaceOnce(wrongTrust, "tlsTrust: []", "tlsTrust: false");

    invalidDocuments.push_back(std::move(wrongTrust));

    auto wrongPolicy = ValidEmptyConfig();

    ReplaceOnce(wrongPolicy, "defaultConflictPolicy: \"ask\"",
                "defaultConflictPolicy: \"surprise\"");

    invalidDocuments.push_back(std::move(wrongPolicy));

    auto wrongTheme = ValidEmptyConfig();

    ReplaceOnce(wrongTheme, "theme: \"dark\"", "theme: \"system\"");

    invalidDocuments.push_back(std::move(wrongTheme));

    auto wrongThemeType = ValidEmptyConfig();

    ReplaceOnce(wrongThemeType, "theme: \"dark\"", "theme: true");

    invalidDocuments.push_back(std::move(wrongThemeType));

    auto missingTheme = ValidEmptyConfig();

    ReplaceOnce(missingTheme, "  theme: \"dark\"\n", "");

    invalidDocuments.push_back(std::move(missingTheme));

    auto wrongLanguageType = ValidEmptyConfig();

    ReplaceOnce(wrongLanguageType, "language: \"en\"", "language: true");

    invalidDocuments.push_back(std::move(wrongLanguageType));

    auto missingLanguage = ValidEmptyConfig();

    ReplaceOnce(missingLanguage, "  language: \"en\"\n", "");

    invalidDocuments.push_back(std::move(missingLanguage));

    constexpr std::array unsafeLanguageCodes{
        "",
        "../de",
        "de_DE",
        "-de",
        "de-",
        "de--DE",
        "de DE",
    };

    for (const std::string_view code : unsafeLanguageCodes)
    {
      auto unsafeLanguage = ValidEmptyConfig();

      ReplaceOnce(unsafeLanguage, "language: \"en\"",
                  "language: \"" + std::string{code} + "\"");

      invalidDocuments.push_back(std::move(unsafeLanguage));
    }

    auto wrongProtocol = ConfigWithSites({SiteNode(
        "978a93a5-03cd-446f-8384-4d6c2145d42e", "Enum", "enum.example")});

    ReplaceOnce(wrongProtocol, "protocol: \"sftp\"", "protocol: \"gopher\"");

    invalidDocuments.push_back(std::move(wrongProtocol));

    auto wrongAuthentication = ConfigWithSites({SiteNode(
        "0b31b293-b49f-4775-8430-62b42c3ae919", "Auth", "auth.example")});

    ReplaceOnce(wrongAuthentication, "kind: \"password\"", "kind: \"token\"");

    invalidDocuments.push_back(std::move(wrongAuthentication));

    auto ftpWithSshAuthentication = ConfigWithSites({SiteNode(
        "d40e73f5-671e-4e5c-b0eb-e60e3df6fc58", "FTP Auth", "ftp-auth.example")});

    ReplaceOnce(ftpWithSshAuthentication, "protocol: \"sftp\"", "protocol: \"ftps-explicit\"");
    ReplaceOnce(ftpWithSshAuthentication, "kind: \"password\"", "kind: \"agent\"");

    invalidDocuments.push_back(std::move(ftpWithSshAuthentication));

    for (std::size_t index = 0; index < invalidDocuments.size(); ++index)
    {
      CAPTURE(index);

      const auto file = directory.Path() /
                        ("invalid-shape-" + std::to_string(index) + ".cson");

      WriteText(file, invalidDocuments[index]);

      CsonConfigRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind == ConfigErrorKind::Validation);
      REQUIRE(loaded.error().line.has_value());
      REQUIRE(*loaded.error().line > 0);
      REQUIRE(loaded.error().column.has_value());
      REQUIRE(*loaded.error().column > 0);
      REQUIRE(ReadText(file) == invalidDocuments[index]);
    }
  }

  TEST_CASE("Main-window documents validate every field type and range")
  {
    TempDirectory directory;

    const auto validMainWindowConfig = []
    {
      std::string text = ValidEmptyConfig();

      ReplaceOnce(text, "  connectionDirectories: []\n", R"(  connectionDirectories: []
  mainWindow:
    x: -1920
    y: 40
    width: 1410
    height: 930
    maximized: false
)");

      return text;
    };

    std::vector<std::string> invalidDocuments;

    auto wrongShape = validMainWindowConfig();

    ReplaceOnce(wrongShape, R"(  mainWindow:
    x: -1920
    y: 40
    width: 1410
    height: 930
    maximized: false
)",
                "  mainWindow: []\n");

    invalidDocuments.push_back(std::move(wrongShape));

    auto missingX = validMainWindowConfig();

    ReplaceOnce(missingX, "    x: -1920\n", "");

    invalidDocuments.push_back(std::move(missingX));

    auto wrongXType = validMainWindowConfig();

    ReplaceOnce(wrongXType, "x: -1920", "x: \"-1920\"");

    invalidDocuments.push_back(std::move(wrongXType));

    auto fractionalY = validMainWindowConfig();

    ReplaceOnce(fractionalY, "y: 40", "y: 40.5");

    invalidDocuments.push_back(std::move(fractionalY));

    auto outOfRangeX = validMainWindowConfig();

    ReplaceOnce(outOfRangeX, "x: -1920", "x: -1000001");

    invalidDocuments.push_back(std::move(outOfRangeX));

    auto halfNullPosition = validMainWindowConfig();

    ReplaceOnce(halfNullPosition, "x: -1920", "x: null");

    invalidDocuments.push_back(std::move(halfNullPosition));

    auto tooNarrow = validMainWindowConfig();

    ReplaceOnce(tooNarrow, "width: 1410", "width: 319");

    invalidDocuments.push_back(std::move(tooNarrow));

    auto fractionalWidth = validMainWindowConfig();

    ReplaceOnce(fractionalWidth, "width: 1410", "width: 1410.5");

    invalidDocuments.push_back(std::move(fractionalWidth));

    auto tooTall = validMainWindowConfig();

    ReplaceOnce(tooTall, "height: 930", "height: 100001");

    invalidDocuments.push_back(std::move(tooTall));

    auto wrongMaximizedType = validMainWindowConfig();

    ReplaceOnce(wrongMaximizedType, "maximized: false",
                "maximized: \"false\"");

    invalidDocuments.push_back(std::move(wrongMaximizedType));

    for (std::size_t index = 0; index < invalidDocuments.size(); ++index)
    {
      CAPTURE(index);

      const auto file = directory.Path() /
                        ("invalid-main-window-" + std::to_string(index) + ".cson");

      WriteText(file, invalidDocuments[index]);

      CsonConfigRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind == ConfigErrorKind::Validation);
      REQUIRE(ReadText(file) == invalidDocuments[index]);
    }
  }

  TEST_CASE("Typed saves reject invalid main-window placement ranges")
  {
    TempDirectory directory;

    const auto requireRejected = [&](std::string_view name, ConfigData data)
    {
      const auto file = directory.Path() / (std::string{name} + ".cson");

      CsonConfigRepository repository(file);

      const auto saved = repository.Save(data);

      REQUIRE_FALSE(saved.has_value());
      REQUIRE(saved.error().kind == ConfigErrorKind::Validation);
      REQUIRE_FALSE(std::filesystem::exists(file));
    };

    ConfigData invalidCoordinate;
    invalidCoordinate.workspace.mainWindow.position =
        WindowPosition{.x = -1'000'001, .y = 0};

    requireRejected("invalid-coordinate", std::move(invalidCoordinate));

    ConfigData invalidWidth;
    invalidWidth.workspace.mainWindow.width = 319;

    requireRejected("invalid-width", std::move(invalidWidth));

    ConfigData invalidHeight;
    invalidHeight.workspace.mainWindow.height = 100'001;

    requireRejected("invalid-height", std::move(invalidHeight));
  }

  TEST_CASE("Workspace validates nested shapes identities duplicates and safe paths")
  {
    TempDirectory directory;

    std::vector<std::string> invalidDocuments;

    auto workspaceNotObject = ValidEmptyConfig();

    ReplaceOnce(workspaceNotObject, DefaultWorkspaceBlock,
                "workspace: []\n");

    invalidDocuments.push_back(std::move(workspaceNotObject));

    auto missingLocalDirectory = ValidEmptyConfig();

    ReplaceOnce(missingLocalDirectory, "  localDirectory: \"\"\n", "");

    invalidDocuments.push_back(std::move(missingLocalDirectory));

    auto localDirectoryWrongType = ValidEmptyConfig();

    ReplaceOnce(localDirectoryWrongType, "localDirectory: \"\"",
                "localDirectory: 42");

    invalidDocuments.push_back(std::move(localDirectoryWrongType));

    auto unsafeLocalDirectory = ValidEmptyConfig();

    ReplaceOnce(unsafeLocalDirectory, "localDirectory: \"\"",
                "localDirectory: \"C:/bad\\npath\"");

    invalidDocuments.push_back(std::move(unsafeLocalDirectory));

    auto missingConnectionDirectories = ValidEmptyConfig();

    ReplaceOnce(missingConnectionDirectories,
                "  connectionDirectories: []\n", "");

    invalidDocuments.push_back(std::move(missingConnectionDirectories));

    auto connectionDirectoriesWrongType = ValidEmptyConfig();

    ReplaceOnce(connectionDirectoriesWrongType, "connectionDirectories: []",
                "connectionDirectories: {}");

    invalidDocuments.push_back(std::move(connectionDirectoriesWrongType));

    auto bothDirectoryArrays = ValidEmptyConfig();

    ReplaceOnce(bothDirectoryArrays, "  connectionDirectories: []\n",
                "  connectionDirectories: []\n"
                "  remoteDirectories: []\n");

    invalidDocuments.push_back(std::move(bothDirectoryArrays));

    invalidDocuments.push_back(ConfigWithWorkspaceConnectionDirectories(
        {"    \"not an object\"\n"}));

    invalidDocuments.push_back(ConfigWithWorkspaceConnectionDirectories({R"(    {
      localDirectory: "C:/Local"
      remoteDirectory: "/missing-identity"
    }
)"}));

    auto mixedIdentity = ConfigWithWorkspaceConnectionDirectories(
        {SavedWorkspaceDirectoriesNode(
            "11111111-1111-4111-8111-111111111111", "C:/Local",
            "/mixed")});

    ReplaceOnce(mixedIdentity,
                "      remoteDirectory: \"/mixed\"",
                "      protocol: \"sftp\"\n"
                "      host: \"mixed.example\"\n"
                "      port: 22\n"
                "      username: \"alice\"\n"
                "      remoteDirectory: \"/mixed\"");

    invalidDocuments.push_back(std::move(mixedIdentity));

    invalidDocuments.push_back(ConfigWithWorkspaceConnectionDirectories(
        {SavedWorkspaceDirectoriesNode("", "C:/Local", "/empty-site-id")}));

    auto siteIdWrongType = ConfigWithWorkspaceConnectionDirectories(
        {SavedWorkspaceDirectoriesNode("typed-site", "C:/Local", "/typed")});

    ReplaceOnce(siteIdWrongType, "siteId: \"typed-site\"", "siteId: 42");

    invalidDocuments.push_back(std::move(siteIdWrongType));

    const auto validSavedNode = SavedWorkspaceDirectoriesNode(
        "22222222-2222-4222-8222-222222222222", "C:/Local", "/remote");

    auto missingPerConnectionLocal = ConfigWithWorkspaceConnectionDirectories(
        {validSavedNode});

    ReplaceOnce(missingPerConnectionLocal,
                "      localDirectory: \"C:/Local\"\n", "");

    invalidDocuments.push_back(std::move(missingPerConnectionLocal));

    invalidDocuments.push_back(ConfigWithWorkspaceConnectionDirectories(
        {SavedWorkspaceDirectoriesNode(
            "33333333-3333-4333-8333-333333333333", "", "/remote")}));

    auto localDirectoryWrongTypeInRecord =
        ConfigWithWorkspaceConnectionDirectories({validSavedNode});

    ReplaceOnce(localDirectoryWrongTypeInRecord,
                "localDirectory: \"C:/Local\"", "localDirectory: false");

    invalidDocuments.push_back(std::move(localDirectoryWrongTypeInRecord));

    auto unsafePerConnectionLocal =
        ConfigWithWorkspaceConnectionDirectories({validSavedNode});

    ReplaceOnce(unsafePerConnectionLocal, "localDirectory: \"C:/Local\"",
                "localDirectory: \"C:/bad\\npath\"");

    invalidDocuments.push_back(std::move(unsafePerConnectionLocal));

    auto missingRemoteDirectory = ConfigWithWorkspaceConnectionDirectories(
        {validSavedNode});

    ReplaceOnce(missingRemoteDirectory,
                "      remoteDirectory: \"/remote\"\n", "");

    invalidDocuments.push_back(std::move(missingRemoteDirectory));

    invalidDocuments.push_back(ConfigWithWorkspaceConnectionDirectories(
        {SavedWorkspaceDirectoriesNode(
            "44444444-4444-4444-8444-444444444444", "C:/Local", "")}));

    auto remoteDirectoryWrongType =
        ConfigWithWorkspaceConnectionDirectories({validSavedNode});

    ReplaceOnce(remoteDirectoryWrongType, "remoteDirectory: \"/remote\"",
                "remoteDirectory: false");

    invalidDocuments.push_back(std::move(remoteDirectoryWrongType));

    auto unsafeRemoteDirectory =
        ConfigWithWorkspaceConnectionDirectories({validSavedNode});

    ReplaceOnce(unsafeRemoteDirectory, "remoteDirectory: \"/remote\"",
                "remoteDirectory: \"/bad\\tpath\"");

    invalidDocuments.push_back(std::move(unsafeRemoteDirectory));

    auto partialQuickIdentity = ConfigWithWorkspaceConnectionDirectories(
        {QuickWorkspaceDirectoriesNode("sftp", "partial.example", 22,
                                       "alice", "C:/Local", "/home/alice")});

    ReplaceOnce(partialQuickIdentity,
                "      username: \"alice\"\n", "");

    invalidDocuments.push_back(std::move(partialQuickIdentity));

    invalidDocuments.push_back(ConfigWithWorkspaceConnectionDirectories(
        {QuickWorkspaceDirectoriesNode("gopher", "unknown.example", 70,
                                       "alice", "C:/Local", "/")}));

    auto quickProtocolWrongType = ConfigWithWorkspaceConnectionDirectories(
        {QuickWorkspaceDirectoriesNode("sftp", "typed.example", 22,
                                       "alice", "C:/Local", "/")});

    ReplaceOnce(quickProtocolWrongType, "protocol: \"sftp\"",
                "protocol: true");

    invalidDocuments.push_back(std::move(quickProtocolWrongType));

    invalidDocuments.push_back(ConfigWithWorkspaceConnectionDirectories(
        {QuickWorkspaceDirectoriesNode(
            "sftp", "sftp://invalid.example", 22, "alice", "C:/Local",
            "/")}));

    auto invalidQuickPort = ConfigWithWorkspaceConnectionDirectories(
        {QuickWorkspaceDirectoriesNode("sftp", "port.example", 22,
                                       "alice", "C:/Local", "/")});

    ReplaceOnce(invalidQuickPort, "      port: 22", "      port: 22.5");

    invalidDocuments.push_back(std::move(invalidQuickPort));

    invalidDocuments.push_back(ConfigWithWorkspaceConnectionDirectories({
        SavedWorkspaceDirectoriesNode(
            "55555555-5555-4555-8555-555555555555", "C:/First", "/first"),
        SavedWorkspaceDirectoriesNode(
            "55555555-5555-4555-8555-555555555555", "C:/Second", "/second"),
    }));

    invalidDocuments.push_back(ConfigWithWorkspaceConnectionDirectories({
        QuickWorkspaceDirectoriesNode("sftp", "EXAMPLE.com", 22, "alice",
                                      "C:/First", "/first"),
        QuickWorkspaceDirectoriesNode("sftp", "example.COM", 22, "alice",
                                      "C:/Second", "/second"),
    }));

    for (std::size_t index = 0; index < invalidDocuments.size(); ++index)
    {
      CAPTURE(index);

      const auto file = directory.Path() /
                        ("invalid-workspace-" + std::to_string(index) + ".cson");

      WriteText(file, invalidDocuments[index]);

      CsonConfigRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind == ConfigErrorKind::Validation);
      REQUIRE(loaded.error().line.has_value());
      REQUIRE(*loaded.error().line > 0);
      REQUIRE(loaded.error().column.has_value());
      REQUIRE(*loaded.error().column > 0);
      REQUIRE(ReadText(file) == invalidDocuments[index]);
    }
  }

  TEST_CASE("Typed saves reject invalid and duplicate workspace directory state")
  {
    TempDirectory directory;

    const auto requireRejected = [&](const std::string_view name,
                                     ConfigData data)
    {
      const auto file = directory.Path() / (std::string{name} + ".cson");

      CsonConfigRepository repository(file);

      const auto saved = repository.Save(data);

      REQUIRE_FALSE(saved.has_value());
      REQUIRE(saved.error().kind == ConfigErrorKind::Validation);
      REQUIRE_FALSE(std::filesystem::exists(file));
    };

    ConfigData unsafeLocal;
    unsafeLocal.workspace.lastLocalDirectory =
        std::filesystem::path(std::u8string(u8"C:/bad\npath"));

    requireRejected("unsafe-local", std::move(unsafeLocal));

    ConfigData emptySiteIdentity;
    emptySiteIdentity.workspace.connectionDirectories.push_back({
        .connection = SavedSiteWorkspaceIdentity{""},
        .localDirectory = "C:/Valid",
        .remoteDirectory = RemotePath{"/valid"},
    });

    requireRejected("empty-site-id", std::move(emptySiteIdentity));

    ConfigData unsafeConnectionLocal;
    unsafeConnectionLocal.workspace.connectionDirectories.push_back({
        .connection = SavedSiteWorkspaceIdentity{"site-id"},
        .localDirectory =
            std::filesystem::path(std::u8string(u8"C:/bad\npath")),
        .remoteDirectory = RemotePath{"/valid"},
    });

    requireRejected("unsafe-connection-local",
                    std::move(unsafeConnectionLocal));

    ConfigData unsafeRemote;
    unsafeRemote.workspace.connectionDirectories.push_back({
        .connection = SavedSiteWorkspaceIdentity{"site-id"},
        .localDirectory = "C:/Valid",
        .remoteDirectory = RemotePath{"/bad\rpath"},
    });

    requireRejected("unsafe-remote", std::move(unsafeRemote));

    ConfigData invalidQuickEndpoint;
    invalidQuickEndpoint.workspace.connectionDirectories.push_back({
        .connection = QuickConnectWorkspaceIdentity{
            SiteEndpointIdentity{ProtocolKind::Sftp,
                                 "sftp://invalid.example", 22, "alice"}},
        .localDirectory = "C:/Valid",
        .remoteDirectory = RemotePath{"/valid"},
    });

    requireRejected("invalid-quick-endpoint",
                    std::move(invalidQuickEndpoint));

    ConfigData duplicate;
    duplicate.workspace.connectionDirectories = {
        RememberedConnectionDirectories{
            .connection = QuickConnectWorkspaceIdentity{
                SiteEndpointIdentity{ProtocolKind::Sftp,
                                     "EXAMPLE.com", 22, "alice"}},
            .localDirectory = "C:/First",
            .remoteDirectory = RemotePath{"/first"},
        },
        RememberedConnectionDirectories{
            .connection = QuickConnectWorkspaceIdentity{SiteEndpointIdentity{ProtocolKind::Sftp, "example.COM", 22, "alice"}},
            .localDirectory = "C:/Second",
            .remoteDirectory = RemotePath{"/second"},
        },
    };

    requireRejected("duplicate-quick", std::move(duplicate));
  }

  TEST_CASE("File-list settings validate every nested shape type and enum")
  {
    TempDirectory directory;

    std::vector<std::string> invalidDocuments;

    auto fileListsNotObject = ValidEmptyConfig();

    ReplaceOnce(fileListsNotObject, DefaultFileListsBlock,
                "  fileLists: []\n");

    invalidDocuments.push_back(std::move(fileListsNotObject));

    auto localNotObject = ValidEmptyConfig();

    ReplaceOnce(localNotObject, DefaultLocalFileListBlock,
                "    local: false\n");

    invalidDocuments.push_back(std::move(localNotObject));

    auto remoteNotObject = ValidEmptyConfig();

    ReplaceOnce(remoteNotObject, DefaultRemoteFileListBlock,
                "    remote: []\n");

    invalidDocuments.push_back(std::move(remoteNotObject));

    auto missingLocal = ValidEmptyConfig();

    ReplaceOnce(missingLocal, DefaultLocalFileListBlock, "");

    invalidDocuments.push_back(std::move(missingLocal));

    auto missingRemote = ValidEmptyConfig();

    ReplaceOnce(missingRemote, DefaultRemoteFileListBlock, "");

    invalidDocuments.push_back(std::move(missingRemote));

    auto missingColumn = ValidEmptyConfig();

    ReplaceOnce(missingColumn, "      sortColumn: \"name\"\n", "");

    invalidDocuments.push_back(std::move(missingColumn));

    auto missingDirection = ValidEmptyConfig();

    ReplaceOnce(missingDirection, "      sortAscending: true\n", "");

    invalidDocuments.push_back(std::move(missingDirection));

    auto columnWrongType = ValidEmptyConfig();

    ReplaceOnce(columnWrongType, "sortColumn: \"name\"", "sortColumn: true");

    invalidDocuments.push_back(std::move(columnWrongType));

    auto columnUnknown = ValidEmptyConfig();

    ReplaceOnce(columnUnknown, "sortColumn: \"name\"",
                "sortColumn: \"checksum\"");

    invalidDocuments.push_back(std::move(columnUnknown));

    auto localRemoteOnlyColumn = ValidEmptyConfig();

    ReplaceOnce(localRemoteOnlyColumn, "sortColumn: \"name\"",
                "sortColumn: \"owner\"");

    invalidDocuments.push_back(std::move(localRemoteOnlyColumn));

    auto directionWrongType = ValidEmptyConfig();

    ReplaceOnce(directionWrongType, "sortAscending: true",
                "sortAscending: \"true\"");

    invalidDocuments.push_back(std::move(directionWrongType));

    for (std::size_t index = 0; index < invalidDocuments.size(); ++index)
    {
      CAPTURE(index);

      const auto file = directory.Path() /
                        ("invalid-file-list-setting-" + std::to_string(index) + ".cson");

      WriteText(file, invalidDocuments[index]);

      CsonConfigRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind == ConfigErrorKind::Validation);
      REQUIRE(ReadText(file) == invalidDocuments[index]);
    }
  }

  TEST_CASE("Typed saves reject invalid file-list sort columns")
  {
    TempDirectory directory;

    SECTION("local")
    {
      const auto file = directory.Path() / "invalid-local-sort.cson";

      CsonConfigRepository repository(file);

      ConfigData data;
      data.settings.fileLists.local.sortColumn =
          static_cast<FileListSortColumn>(999);

      const auto saved = repository.Save(data);

      REQUIRE_FALSE(saved.has_value());
      REQUIRE(saved.error().kind == ConfigErrorKind::Validation);
      REQUIRE_FALSE(std::filesystem::exists(file));
    }

    SECTION("local remote-only column")
    {
      const auto file = directory.Path() / "invalid-local-owner-sort.cson";

      CsonConfigRepository repository(file);

      ConfigData data;
      data.settings.fileLists.local.sortColumn =
          FileListSortColumn::Owner;

      const auto saved = repository.Save(data);

      REQUIRE_FALSE(saved.has_value());
      REQUIRE(saved.error().kind == ConfigErrorKind::Validation);
      REQUIRE_FALSE(std::filesystem::exists(file));
    }

    SECTION("remote")
    {
      const auto file = directory.Path() / "invalid-remote-sort.cson";

      CsonConfigRepository repository(file);

      ConfigData data;
      data.settings.fileLists.remote.sortColumn =
          static_cast<FileListSortColumn>(999);

      const auto saved = repository.Save(data);

      REQUIRE_FALSE(saved.has_value());
      REQUIRE(saved.error().kind == ConfigErrorKind::Validation);
      REQUIRE_FALSE(std::filesystem::exists(file));
    }
  }

  TEST_CASE("Typed saves reject unsafe language codes")
  {
    TempDirectory directory;

    constexpr std::array unsafeLanguageCodes{
        "",
        "../de",
        "de_DE",
        "-de",
        "de-",
        "de--DE",
        "de DE",
    };

    for (std::size_t index = 0; index < unsafeLanguageCodes.size(); ++index)
    {
      CAPTURE(unsafeLanguageCodes[index]);

      const auto file = directory.Path() /
                        ("invalid-language-" + std::to_string(index) + ".cson");

      CsonConfigRepository repository(file);

      ConfigData data;
      data.settings.language = unsafeLanguageCodes[index];

      const auto saved = repository.Save(data);

      REQUIRE_FALSE(saved.has_value());
      REQUIRE(saved.error().kind == ConfigErrorKind::Validation);
      REQUIRE_FALSE(std::filesystem::exists(file));
    }
  }

  TEST_CASE("Typed saves reject unsafe external editor settings")
  {
    TempDirectory directory;

    SECTION("unknown mode")
    {
      const auto file = directory.Path() / "invalid-editor-mode.cson";

      CsonConfigRepository repository(file);

      ConfigData data;
      data.settings.externalEditor.mode =
          static_cast<ExternalEditorMode>(999);

      const auto saved = repository.Save(data);

      REQUIRE_FALSE(saved.has_value());
      REQUIRE(saved.error().kind == ConfigErrorKind::Validation);
      REQUIRE_FALSE(std::filesystem::exists(file));
    }

    SECTION("custom mode requires an executable")
    {
      const auto file = directory.Path() / "missing-editor-path.cson";

      CsonConfigRepository repository(file);

      ConfigData data;
      data.settings.externalEditor.mode = ExternalEditorMode::Custom;

      const auto saved = repository.Save(data);

      REQUIRE_FALSE(saved.has_value());
      REQUIRE(saved.error().kind == ConfigErrorKind::Validation);
      REQUIRE_FALSE(std::filesystem::exists(file));
    }

    SECTION("executable path rejects controls")
    {
      const auto file = directory.Path() / "unsafe-editor-path.cson";

      CsonConfigRepository repository(file);

      ConfigData data;
      data.settings.externalEditor.mode = ExternalEditorMode::Custom;
      data.settings.externalEditor.executable =
          std::filesystem::path{std::string{"C:/Tools/editor\n.exe"}};

      const auto saved = repository.Save(data);

      REQUIRE_FALSE(saved.has_value());
      REQUIRE(saved.error().kind == ConfigErrorKind::Validation);
      REQUIRE_FALSE(std::filesystem::exists(file));
    }

    SECTION("argument template requires exactly one placeholder")
    {
      constexpr std::array<std::string_view, 3> invalidArguments{
          "--wait",
          "{file} --diff {file}",
          "{file}\t--wait",
      };

      for (std::size_t index = 0; index < invalidArguments.size(); ++index)
      {
        CAPTURE(invalidArguments[index]);

        const auto file = directory.Path() /
                          ("unsafe-editor-arguments-" +
                           std::to_string(index) + ".cson");

        CsonConfigRepository repository(file);

        ConfigData data;
        data.settings.externalEditor.arguments = invalidArguments[index];

        const auto saved = repository.Save(data);

        REQUIRE_FALSE(saved.has_value());
        REQUIRE(saved.error().kind == ConfigErrorKind::Validation);
        REQUIRE_FALSE(std::filesystem::exists(file));
      }
    }
  }

  TEST_CASE("Non-current and unversioned configurations are rejected without modification")
  {
    TempDirectory directory;

    const std::array<std::string, 2> incompatibleDocuments{
        R"(# non-current version
formatVersion: 0
settings:
  transferConcurrency: 2
  connectionTimeoutSeconds: 20
  commandIdleTimeoutSeconds: 60
  defaultConflictPolicy: "ask"
  theme: "dark"
  language: "en"
sites: []
tlsTrust: []
)",
        R"(# no format version
settings:
  transferConcurrency: 2
  connectionTimeoutSeconds: 20
  commandIdleTimeoutSeconds: 60
  defaultConflictPolicy: "ask"
  theme: "dark"
  language: "en"
sites: []
tlsTrust: []
)",
    };

    for (std::size_t index = 0; index < incompatibleDocuments.size(); ++index)
    {
      CAPTURE(index);

      const auto file = directory.Path() /
                        ("incompatible-" + std::to_string(index) + ".cson");

      WriteText(file, incompatibleDocuments[index]);

      CsonConfigRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      REQUIRE(loaded.error().kind == ConfigErrorKind::UnsupportedVersion);
      REQUIRE(loaded.error().message.find("Expected version 1") !=
              std::string::npos);
      REQUIRE(ReadText(file) == incompatibleDocuments[index]);
    }
  }

  TEST_CASE("AppData and fixed path providers keep path lookup injectable")
  {
    TempDirectory directory;

    const auto appData = directory.Path() / std::filesystem::path(
                                                std::u8string{u8"Roaming-Ünicode"});

    AppDataConfigPathProvider appDataProvider(appData);

    const auto appDataPath = appDataProvider.ConfigPath();

    REQUIRE(appDataPath.has_value());
    REQUIRE(*appDataPath == appData / "havRemote" / "havRemote.cson");

    FixedConfigPathProvider fixedProvider(directory.Path() / "custom.cson");

    const auto fixedPath = fixedProvider.ConfigPath();

    REQUIRE(fixedPath.has_value());
    REQUIRE(*fixedPath == directory.Path() / "custom.cson");

    AppDataConfigPathProvider missingAppData({});

    const auto missingAppDataPath = missingAppData.ConfigPath();

    REQUIRE_FALSE(missingAppDataPath.has_value());
    REQUIRE(missingAppDataPath.error().kind == ConfigErrorKind::PathUnavailable);

    FixedConfigPathProvider missingFixedPath({});

    const auto missingFixed = missingFixedPath.ConfigPath();

    REQUIRE_FALSE(missingFixed.has_value());
    REQUIRE(missingFixed.error().kind == ConfigErrorKind::PathUnavailable);
  }
  TEST_CASE("Configuration diagnostics identify the exact repeated nested member")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "nested.cson";

    std::string text;
    std::string invalidMember;
    std::size_t expectedColumn = 1;

    SECTION("second site's port instead of the first site or an extension")
    {
      auto second = SiteNode("20f81b67-02b1-4a69-bf3e-42c90d67ce26", "Second", "second.example");
      second.replace(second.find("port: 22"), std::string("port: 22").size(), "port: 22.5");

      text = "plugin:\n  port: 88\n" + ConfigWithSites({SiteNode("10f81b67-02b1-4a69-bf3e-42c90d67ce26", "First", "first.example"), second});

      invalidMember = "    port: 22.5";

      expectedColumn = 5;
    }

    SECTION("remote list boolean instead of local list boolean")
    {
      text = ValidEmptyConfig();

      const auto remote = text.find("    remote:");
      const auto setting = text.find("sortAscending: true", remote);

      REQUIRE(setting != std::string::npos);

      text.replace(setting, std::string("sortAscending: true").size(), "sortAscending: 1");

      invalidMember = "      sortAscending: 1";

      expectedColumn = 7;
    }

    const auto offset = text.find(invalidMember);

    REQUIRE(offset != std::string::npos);

    const auto expectedLine = 1U + static_cast<std::size_t>(
                                       std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));

    WriteText(file, text);

    CsonConfigRepository repository(file);

    const auto loaded = repository.Load();

    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().kind == ConfigErrorKind::Validation);
    REQUIRE(loaded.error().line == expectedLine);
    REQUIRE(loaded.error().column == expectedColumn);
    REQUIRE(ReadText(file) == text);
  }

  TEST_CASE("Library parser limits reject excessive configuration nesting without overwriting")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "deep.cson";
    const auto text = ValidEmptyConfig() + "plugin: " + std::string(300, '[') +
                      "0" + std::string(300, ']') + "\n";

    WriteText(file, text);

    CsonConfigRepository repository(file);

    const auto loaded = repository.Load();

    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().kind == ConfigErrorKind::Parse);
    REQUIRE(loaded.error().line.has_value());
    REQUIRE(loaded.error().message.find("depth") != std::string::npos);
    REQUIRE(ReadText(file) == text);
  }

  TEST_CASE("Updating nested authentication and subsequent site fields commits every edit")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    WriteText(file, ConfigWithSites({SiteNode(
                        "10f81b67-02b1-4a69-bf3e-42c90d67ce26", "Test", "sftp.example")}));

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());

    auto &site = loaded->data.sites.front();

    site.authentication.kind = AuthenticationKind::PrivateKey;
    site.authentication.privateKeyFile = "/keys/id_ed25519";
    site.authentication.passphraseCredentialId = "havRemote/key/reference";
    site.initialRemoteDirectory = RemotePath("/changed-remote");
    site.initialLocalDirectory = "/changed-local";
    site.ftpEncoding = "ISO-8859-1";

    const auto expected = site;

    REQUIRE(repository.Save(loaded->data).has_value());

    for (int cycle = 0; cycle < 3; ++cycle)
    {
      loaded = repository.Load();

      REQUIRE(loaded.has_value());

      const auto &actual = loaded->data.sites.front();

      REQUIRE(actual.authentication.kind == expected.authentication.kind);
      REQUIRE(actual.authentication.privateKeyFile == expected.authentication.privateKeyFile);
      REQUIRE(actual.authentication.passphraseCredentialId == expected.authentication.passphraseCredentialId);
      REQUIRE(actual.initialRemoteDirectory == expected.initialRemoteDirectory);
      REQUIRE(actual.initialLocalDirectory == expected.initialLocalDirectory);
      REQUIRE(actual.ftpEncoding == expected.ftpEncoding);
      REQUIRE(repository.Save(loaded->data).has_value());
    }
  }

  TEST_CASE("Small endpoint spelling edits persist even when connection identities compare equal")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    CsonConfigRepository repository(file);

    ConfigData data;

    const SiteEndpointIdentity endpoint{
        ProtocolKind::Sftp, "quick.example", 22, "user"};

    data.quickConnectHistory = {endpoint};
    data.workspace.connectionDirectories = {{
        .connection = QuickConnectWorkspaceIdentity{endpoint},
        .localDirectory = "/local",
        .remoteDirectory = RemotePath{"/remote"},
    }};
    data.workspace.openTabs = {{
        .connectionId = "quick-tab",
        .connection = QuickConnectWorkspaceIdentity{endpoint},
        .localDirectory = "/local",
        .remoteDirectory = RemotePath{"/remote"},
    }};
    data.workspace.selectedConnectionId = "quick-tab";

    REQUIRE(repository.Save(data).has_value());

    for (int edit = 0; edit < 3; ++edit)
    {
      CAPTURE(edit);

      if (edit == 0)
      {
        data.quickConnectHistory.front().host = "QUICK.example";
      }
      else if (edit == 1)
      {
        std::get<QuickConnectWorkspaceIdentity>(
            data.workspace.connectionDirectories.front().connection)
            .endpoint.host =
            "Quick.EXAMPLE";
      }
      else
      {
        std::get<QuickConnectWorkspaceIdentity>(
            *data.workspace.openTabs.front().connection)
            .endpoint.host =
            "QUICK.EXAMPLE";
      }

      REQUIRE(repository.Save(data).has_value());

      CsonConfigRepository verification(file);

      const auto reloaded = verification.Load();

      REQUIRE(reloaded.has_value());
      REQUIRE(reloaded->data.quickConnectHistory.size() == 1);
      CHECK(reloaded->data.quickConnectHistory.front().host ==
            data.quickConnectHistory.front().host);
      REQUIRE(reloaded->data.workspace.connectionDirectories.size() == 1);
      CHECK(std::get<QuickConnectWorkspaceIdentity>(
                reloaded->data.workspace.connectionDirectories.front().connection)
                .endpoint.host ==
            std::get<QuickConnectWorkspaceIdentity>(
                data.workspace.connectionDirectories.front().connection)
                .endpoint.host);
      REQUIRE(reloaded->data.workspace.openTabs.size() == 1);
      REQUIRE(reloaded->data.workspace.openTabs.front().connection.has_value());
      CHECK(std::get<QuickConnectWorkspaceIdentity>(
                *reloaded->data.workspace.openTabs.front().connection)
                .endpoint.host ==
            std::get<QuickConnectWorkspaceIdentity>(
                *data.workspace.openTabs.front().connection)
                .endpoint.host);
    }
  }

  TEST_CASE("Repeated small settings and directory saves leave unrelated sections intact")
  {
    TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    constexpr std::string_view alphaId =
        "11111111-1111-4111-8111-111111111111";
    constexpr std::string_view betaId =
        "22222222-2222-4222-8222-222222222222";

    std::string text = ConfigWithSites({
        SiteNode(betaId, "Beta", "beta.example"),
        SiteNode(alphaId, "Alpha", "alpha.example"),
    });

    ReplaceOnce(text, "    name: \"Beta\"",
                "    # saved site leading\n"
                "    pluginSite: \"untouched\"\n"
                "\n"
                "    name: \"Beta\" # saved site inline");
    ReplaceOnce(text, "  language: \"en\"",
                "  language: \"en\" # selected language\n"
                "  pluginSettings: \"untouched\"");
    ReplaceOnce(text, DefaultWorkspaceBlock, R"(workspace:
  localDirectory: "/local" # browser location
  connectionDirectories: [
    {
      siteId: "11111111-1111-4111-8111-111111111111"
      localDirectory: "/saved"
      remoteDirectory: "/remote" # saved remote location
      pluginDirectory: "untouched"
    }
  ]
  openTabs: [
    {
      connectionId: "tab-alpha"
      siteId: "11111111-1111-4111-8111-111111111111"
      localDirectory: "/saved"
      remoteDirectory: "/remote" # tab remote location
      pluginTab: "untouched"
    }
    {
      connectionId: "tab-new"
      localDirectory: "/new"
      remoteDirectory: "/"
    }
  ]
  selectedConnectionId: "tab-alpha"
)");
    ReplaceOnce(text, "quickConnectHistory: []", R"(quickConnectHistory: [
  # history leading
  {
    protocol: "sftp"
    host: "quick.example" # history endpoint
    port: 22
    username: "user"
    pluginHistory: "untouched"
  }
  # history closing
])");
    ReplaceOnce(text, "siteFolders: []", R"(siteFolders: [
  # folder leading
  {
    id: "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
    name: "Servers" # folder name
    parentId: ""
    siteIds: ["11111111-1111-4111-8111-111111111111"]
    pluginFolder: "untouched"
  }
]
siteManagerOrder: [
  "22222222-2222-4222-8222-222222222222"
  "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
  "11111111-1111-4111-8111-111111111111"
])");
    ReplaceOnce(text, "tlsTrust: []", R"(tlsTrust: [
  {
    host: "secure.example"
    port: 21
    publicKeyPin: "sha256//YWJjZA==" # trusted key
    pluginTrust: "untouched"
  }
]

# root extension
pluginRoot: "untouched" # root trailing
)");

    WriteText(file, text);

    CsonConfigRepository repository(file);

    auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    // Complete optional defaults once before comparing normalized output
    REQUIRE(repository.Save(loaded->data).has_value());

    const auto baseline = ReadText(file);

    const auto suffixPosition = baseline.find("quickConnectHistory:");

    REQUIRE(suffixPosition != std::string::npos);

    const auto unrelatedSections = baseline.substr(suffixPosition);

    for (int cycle = 0; cycle < 3; ++cycle)
    {
      for (int edit = 0; edit < 4; ++edit)
      {
        CAPTURE(cycle, edit);

        const auto suffix = std::to_string(cycle);

        if (edit == 0)
        {
          loaded->data.settings.language = cycle % 2 == 0 ? "de" : "en";
        }
        else if (edit == 1)
        {
          loaded->data.workspace.lastLocalDirectory = "/local/" + suffix;
        }
        else if (edit == 2)
        {
          loaded->data.workspace.connectionDirectories.front().remoteDirectory =
              RemotePath{"/remote/" + suffix};
        }
        else
        {
          auto &tab = loaded->data.workspace.openTabs.front();

          tab.localDirectory = "/tab/" + suffix;
          tab.remoteDirectory = RemotePath{"/tab/" + suffix};

          loaded->data.workspace.selectedConnectionId =
              cycle % 2 == 0 ? "tab-new" : "tab-alpha";
        }

        REQUIRE(repository.Save(loaded->data).has_value());

        const auto saved = ReadText(file);

        const auto actualSuffix = saved.find("quickConnectHistory:");

        REQUIRE(actualSuffix != std::string::npos);
        CHECK(saved.substr(actualSuffix) == unrelatedSections);

        for (const auto marker : {"# selected language", "# browser location",
                                  "# saved remote location", "# tab remote location",
                                  "pluginSettings:", "pluginDirectory:", "pluginTab:"})
        {
          CAPTURE(marker);

          const auto position = saved.find(marker);

          REQUIRE(position != std::string::npos);
          CHECK(position == saved.rfind(marker));
        }

        CsonConfigRepository verification(file);

        const auto reloaded = verification.Load();

        REQUIRE(reloaded.has_value());
        CHECK(reloaded->data.settings == loaded->data.settings);
        CHECK(reloaded->data.workspace == loaded->data.workspace);
        CHECK(reloaded->data.quickConnectHistory == loaded->data.quickConnectHistory);
        CHECK(reloaded->data.siteFolders == loaded->data.siteFolders);
        CHECK(reloaded->data.siteManagerOrder == loaded->data.siteManagerOrder);
        CHECK(reloaded->data.tlsTrust == loaded->data.tlsTrust);
        REQUIRE(reloaded->data.sites.size() == 2);
        CHECK(reloaded->data.sites[0].id == betaId);
        CHECK(reloaded->data.sites[1].id == alphaId);
      }
    }
  }
} // namespace havremote::config
