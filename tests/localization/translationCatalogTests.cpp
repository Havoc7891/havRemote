// SPDX-License-Identifier: MIT

#include "localization/translationCatalog.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <string_view>

namespace havremote::localization
{
  namespace
  {
    class TemporaryDirectory final
    {
    public:
      TemporaryDirectory()
      {
        static std::atomic_uint64_t sequence{};

        const auto id = sequence.fetch_add(1, std::memory_order_relaxed);

        // Use CTest's writable working directory: the environment's temporary
        // directory may be read-only to the current process.
        mPath = std::filesystem::current_path() /
                ("havremote-translations-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 '-' + std::to_string(id));

        std::filesystem::create_directories(mPath);
      }

      ~TemporaryDirectory()
      {
        std::error_code ignored;
        std::filesystem::remove_all(mPath, ignored);
      }

      TemporaryDirectory(const TemporaryDirectory &) = delete;
      TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

      [[nodiscard]] const std::filesystem::path &Path() const noexcept { return mPath; }

      void Write(const std::string_view filename, const std::string_view contents) const
      {
        std::ofstream output(mPath / filename, std::ios::binary | std::ios::trunc);

        REQUIRE(output);

        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));

        REQUIRE(output.good());
      }

    private:
      std::filesystem::path mPath;
    };

    std::string MinimalCatalog(const std::string_view code,
                               const std::string_view name,
                               const std::string_view greeting = "Hello {0}",
                               const std::string_view key = "test.greeting")
    {
      return "formatVersion: 1\n"
             "language:\n"
             "  code: \"" +
             std::string{code} + "\"\n"
                                 "  displayName: \"" +
             std::string{name} + "\"\n"
                                 "strings:\n"
                                 "  \"" +
             std::string{key} + "\": \"" +
             std::string{greeting} + "\"\n";
    }

    TEST_CASE("local deletion confirmations warn about permanent deletion")
    {
      const auto directory = std::filesystem::path{HAVREMOTE_TRANSLATIONS_SOURCE_DIR};

      for (const std::string_view code : {"en", "de"})
      {
        const auto catalog = TranslationCatalog::Load(directory, code);

        REQUIRE(catalog);

        const auto warning = code == "en" ? "permanently deleted" : "endgültig gelöscht";

        for (const auto key : {"local.deleteConfirm", "local.deleteSelectedConfirm"})
        {
          const auto text = catalog->Text(key);

          CHECK(text.contains("\n\n"));
          CHECK(text.contains(warning));
        }

        const auto multiple = catalog->Format("local.deleteSelectedConfirm", {"3"});

        CHECK(multiple.contains("3"));
        CHECK_FALSE(multiple.contains("{0}"));
      }
    }

    TEST_CASE("shipped translation catalogs are discovered and selectable")
    {
      const auto directory = std::filesystem::path{HAVREMOTE_TRANSLATIONS_SOURCE_DIR};

      const auto german = TranslationCatalog::Load(directory, "DE");

      REQUIRE(german);
      CHECK(german->SelectedLanguageCode() == "de");
      CHECK_FALSE(german->UsedFallback());
      CHECK(german->Text("about.website") == "Website");
      CHECK(german->Text("about.sourceCode") == "Quellcode");
      CHECK(german->Text("about.email") == "E-Mail");
      CHECK(german->Text("quick.connect") == "Verbinden");
      CHECK(german->Format("entry.kind.fileWithExtension", {"TXT"}) == "TXT-Datei");
      CHECK(german->Text("entry.kind.file") == "Datei");
      CHECK(german->Text("siteManager.connect") == "Verbinden");
      CHECK(german->Text("quick.disconnect") == "Trennen");
      CHECK(german->Text("status.disconnected") == "Verbindung getrennt");
      CHECK(german->Text("connection.canceledLog") ==
            "Verbindungsversuch abgebrochen");
      CHECK(german->Text("quick.clear") == "Leeren");
      CHECK(german->Text("quick.history") == "Verlauf");
      CHECK(german->Text("quick.historyEmpty") ==
            "Keine kürzlich verwendeten Verbindungen");
      CHECK(german->Text("quick.clearHistory") == "Verlauf löschen");
      CHECK(german->Text("settings.restoreWindowSize") ==
            "Standardgröße wiederherstellen");
      CHECK(german->Format("status.queuedActive", {"7"}) == "7 wartend/aktiv");
      CHECK(german->Format("connection.connectingLog",
                           {"example.com", "22", "SFTP"}) ==
            "Verbindung zu example.com:22 über SFTP wird hergestellt...");
      CHECK(german->Format("connection.transferConnectingLog",
                           {"2", "example.com", "22", "SFTP", "3"}) ==
            "Übertragungs-Worker 2 stellt eine Verbindung zu example.com:22 über SFTP her (Versuch 3)...");
      CHECK(german->Text("transfer.failedTitle") ==
            "Übertragung fehlgeschlagen");
      CHECK(german->Text("transfer.failuresTitle") ==
            "Übertragungen fehlgeschlagen");
      CHECK(german->Format("transfer.failureItem",
                           {"Büro", german->Text("transfer.direction.upload"),
                            "/Berichte/Entwurf.txt", "Zugriff verweigert"}) ==
            "Büro\nHochladen: /Berichte/Entwurf.txt\nZugriff verweigert");
      CHECK(german->Format("transfer.failureItem",
                           {"Büro", german->Text("transfer.direction.download"),
                            "/Berichte/Entwurf.txt", "Verbindung unterbrochen"}) ==
            "Büro\nHerunterladen: /Berichte/Entwurf.txt\nVerbindung unterbrochen");
      CHECK(german->Format("transfer.failuresMore", {"7"}) ==
            "7 weitere fehlgeschlagene Übertragungen. Einzelheiten finden Sie im Tab Fehlgeschlagen.");
      CHECK(german->Text("transfer.failureRetry") ==
            "Fehlgeschlagene Übertragungen bleiben im Tab Fehlgeschlagen. Markieren Sie einen Eintrag und wählen Sie Erneut versuchen, nachdem Sie das Problem behoben haben.");

      const auto languages = german->Languages();

      REQUIRE(languages.size() == 2);
      CHECK(std::ranges::any_of(languages, [](const LanguageInfo &language)
                                { return language.code == "en" && language.displayName == "English"; }));
      CHECK(std::ranges::any_of(languages, [](const LanguageInfo &language)
                                { return language.code == "de" && language.displayName == "Deutsch"; }));

      const auto english = german->WithLanguage("eN");

      CHECK(english.SelectedLanguageCode() == "en");
      CHECK_FALSE(english.UsedFallback());
      CHECK(english.Text("about.website") == "Website");
      CHECK(english.Text("about.sourceCode") == "Source Code");
      CHECK(english.Text("about.email") == "Email");
      CHECK(english.Text("quick.connect") == "Connect");
      CHECK(english.Text("siteManager.connect") == "Connect");
      CHECK(english.Text("quick.disconnect") == "Disconnect");
      CHECK(english.Text("status.disconnected") == "Disconnected");
      CHECK(english.Text("connection.canceledLog") ==
            "Connection attempt canceled");
      CHECK(english.Format("entry.kind.fileWithExtension", {"TXT"}) == "TXT file");
      CHECK(english.Text("entry.kind.file") == "File");
      CHECK(english.Text("quick.clear") == "Clear");
      CHECK(english.Text("quick.history") == "History");
      CHECK(english.Text("quick.historyEmpty") == "No recent connections");
      CHECK(english.Text("quick.clearHistory") == "Clear History");
      CHECK(english.Text("settings.restoreWindowSize") ==
            "Restore default size");
      CHECK(english.Format("connection.connectingLog",
                           {"example.com", "22", "SFTP"}) ==
            "Connecting to example.com:22 using SFTP...");
      CHECK(english.Format("connection.transferConnectingLog",
                           {"2", "example.com", "22", "SFTP", "3"}) ==
            "Transfer worker 2 connecting to example.com:22 using SFTP (attempt 3)...");
      CHECK(english.Text("transfer.failedTitle") == "Transfer failed");
      CHECK(english.Text("transfer.failuresTitle") == "Transfers failed");
      CHECK(english.Format("transfer.failureItem",
                           {"Office", english.Text("transfer.direction.upload"),
                            "/Reports/Draft.txt", "Permission denied"}) ==
            "Office\nUpload: /Reports/Draft.txt\nPermission denied");
      CHECK(english.Format("transfer.failureItem",
                           {"Office", english.Text("transfer.direction.download"),
                            "/Reports/Draft.txt", "Connection interrupted"}) ==
            "Office\nDownload: /Reports/Draft.txt\nConnection interrupted");
      CHECK(english.Format("transfer.failuresMore", {"7"}) ==
            "7 more failed transfers. See the Failed tab for details.");
      CHECK(english.Text("transfer.failureRetry") ==
            "Failed transfers remain in the Failed tab. Select an item and choose Retry after resolving the problem.");
    }

    TEST_CASE("main-menu status help follows the selected language")
    {
      const auto directory = std::filesystem::path{HAVREMOTE_TRANSLATIONS_SOURCE_DIR};
      const auto english = TranslationCatalog::Load(directory, "en");

      REQUIRE(english);
      CHECK(english->SelectedLanguageCode() == "en");
      CHECK_FALSE(english->UsedFallback());

      const auto german = english->WithLanguage("de");

      CHECK(german.SelectedLanguageCode() == "de");
      CHECK_FALSE(german.UsedFallback());

      const auto englishAgain = german.WithLanguage("en");

      CHECK(englishAgain.SelectedLanguageCode() == "en");
      CHECK_FALSE(englishAgain.UsedFallback());

      for (const std::string_view key : {
               "menu.file.newConnectionTabHelp",
               "menu.file.closeConnectionTabHelp",
               "menu.file.siteManagerHelp",
               "menu.file.disconnectHelp",
               "menu.file.exitHelp",
               "menu.preferences.settingsHelp",
               "menu.help.contentsHelp",
               "menu.help.checkForUpdatesHelp",
               "menu.help.aboutHelp"})
      {
        CAPTURE(key);

        const auto englishText = english->Text(key);
        const auto germanText = german.Text(key);

        CHECK_FALSE(englishText.empty());
        CHECK_FALSE(germanText.empty());
        CHECK(englishText != key);
        CHECK(germanText != key);
        CHECK(englishText != germanText);
        CHECK(englishText.find_first_of("&\t") == std::string_view::npos);
        CHECK(germanText.find_first_of("&\t") == std::string_view::npos);
        CHECK(englishAgain.Text(key) == englishText);
      }
    }

    TEST_CASE("an unavailable requested language prefers English")
    {
      const auto directory = std::filesystem::path{HAVREMOTE_TRANSLATIONS_SOURCE_DIR};
      const auto catalog = TranslationCatalog::Load(directory, "fr-CA");

      REQUIRE(catalog);
      CHECK(catalog->SelectedLanguageCode() == "en");
      CHECK(catalog->UsedFallback());
      CHECK(catalog->Text("quick.connect") == "Connect");
      CHECK(catalog->Text("quick.disconnect") == "Disconnect");
      CHECK(catalog->Text("quick.clear") == "Clear");
      CHECK(catalog->Text("quick.history") == "History");
      CHECK(catalog->Text("quick.historyEmpty") == "No recent connections");
      CHECK(catalog->Text("quick.clearHistory") == "Clear History");
      CHECK(catalog->Text("missing.translation.key") == "missing.translation.key");
    }

    TEST_CASE("catalog discovery rejects duplicate case-insensitive language codes")
    {
      TemporaryDirectory directory;
      directory.Write("first.cson", MinimalCatalog("en-US", "English"));
      directory.Write("second.cson", MinimalCatalog("EN-us", "Duplicate"));

      const auto result = TranslationCatalog::Load(directory.Path(), "en-US");

      REQUIRE_FALSE(result);
      CHECK(result.error().kind == TranslationErrorKind::Validation);
      CHECK(result.error().message.find("duplicates") != std::string::npos);
    }

    TEST_CASE("catalog discovery selects a deterministic default language")
    {
      SECTION("English is preferred regardless of filename order")
      {
        TemporaryDirectory directory;
        directory.Write("01-de.cson", MinimalCatalog("de", "Deutsch"));
        directory.Write("99-en.cson", MinimalCatalog("en", "English"));

        const auto catalog = TranslationCatalog::Load(directory.Path(), "fr");

        REQUIRE(catalog);
        CHECK(catalog->SelectedLanguageCode() == "en");
        CHECK(catalog->UsedFallback());
      }

      SECTION("the first sorted catalog is used when English is absent")
      {
        TemporaryDirectory directory;
        directory.Write("20-fr.cson", MinimalCatalog("fr", "Français"));
        directory.Write("10-de.cson", MinimalCatalog("de", "Deutsch"));

        const auto catalog = TranslationCatalog::Load(directory.Path(), "it");

        REQUIRE(catalog);
        CHECK(catalog->SelectedLanguageCode() == "de");
        CHECK(catalog->UsedFallback());

        const auto switched = catalog->WithLanguage("missing");

        CHECK(switched.SelectedLanguageCode() == "de");
        CHECK(switched.UsedFallback());
      }
    }

    TEST_CASE("catalogs validate language codes, key sets, and placeholders")
    {
      SECTION("parse diagnostics")
      {
        TemporaryDirectory directory;
        directory.Write("bad.cson", R"(formatVersion: 1
language:
  code: "en"
  displayName: "English"
strings: "\q"
)");

        const auto result = TranslationCatalog::Load(directory.Path(), "en");

        REQUIRE_FALSE(result);
        CHECK(result.error().kind == TranslationErrorKind::Parse);
        CHECK(result.error().line.has_value());
        CHECK(result.error().column.has_value());
      }

      SECTION("unknown schema member")
      {
        TemporaryDirectory directory;
        directory.Write(
            "bad.cson",
            MinimalCatalog("en", "English") + "extension: true\n");

        const auto result = TranslationCatalog::Load(directory.Path(), "en");

        REQUIRE_FALSE(result);
        CHECK(result.error().message.find("unknown member") != std::string::npos);
      }

      SECTION("fallback metadata is not part of the language schema")
      {
        TemporaryDirectory directory;
        directory.Write("bad.cson", R"(formatVersion: 1
language:
  code: "en"
  displayName: "English"
  fallback: true
strings:
  "test.greeting": "Hello {0}"
)");

        const auto result = TranslationCatalog::Load(directory.Path(), "en");

        REQUIRE_FALSE(result);
        CHECK(result.error().message.find("unknown member 'fallback'") !=
              std::string::npos);
      }

      SECTION("invalid language code")
      {
        TemporaryDirectory directory;
        directory.Write("bad.cson", MinimalCatalog("de--DE", "Deutsch"));

        const auto result = TranslationCatalog::Load(directory.Path(), "de-DE");

        REQUIRE_FALSE(result);
        CHECK(result.error().message.find("1-35 ASCII") != std::string::npos);
      }

      SECTION("missing translation key")
      {
        TemporaryDirectory directory;
        directory.Write("en.cson", MinimalCatalog("en", "English"));
        directory.Write(
            "de.cson",
            "formatVersion: 1\n"
            "language:\n"
            "  code: \"de\"\n"
            "  displayName: \"Deutsch\"\n"
            "strings:\n"
            "  \"test.other\": \"Sonstiges\"\n");

        const auto result = TranslationCatalog::Load(directory.Path(), "de");

        REQUIRE_FALSE(result);
        CHECK(result.error().message.find("missing") != std::string::npos);
      }

      SECTION("placeholder mismatch")
      {
        TemporaryDirectory directory;
        directory.Write("en.cson", MinimalCatalog("en", "English"));
        directory.Write("de.cson", MinimalCatalog("de", "Deutsch", "Hallo"));

        const auto result = TranslationCatalog::Load(directory.Path(), "de");

        REQUIRE_FALSE(result);
        CHECK(result.error().message.find("placeholders") != std::string::npos);
      }

      SECTION("malformed placeholder")
      {
        TemporaryDirectory directory;
        directory.Write("bad.cson", MinimalCatalog("en", "English", "Hello {name}"));

        const auto result = TranslationCatalog::Load(directory.Path(), "en");

        REQUIRE_FALSE(result);
        CHECK(result.error().message.find("placeholder") != std::string::npos);
      }
    }

    TEST_CASE("translation keys accept dot-separated camelCase segments")
    {
      TemporaryDirectory directory;

      for (const std::string_view key : {
               "a",
               "menu.file.title",
               "siteManager.defaultFolderName",
               "externalEditor.error.systemDefaultUnsafeType",
               "ftp2.protocol.ipv6",
           })
      {
        INFO("Translation key: " << key);

        directory.Write("en.cson", MinimalCatalog("en", "English", "Hello", key));

        const auto catalog = TranslationCatalog::Load(directory.Path(), "en");

        REQUIRE(catalog);
        CHECK(catalog->Text(key) == "Hello");
      }
    }

    TEST_CASE("translation keys reject non-camelCase segment syntax")
    {
      TemporaryDirectory directory;

      for (const std::string_view key : {
               "",
               ".menu",
               "menu.",
               "menu..file",
               "_",
               "test_group.title",
               "menu.file_name",
               "SiteManager.title",
               "menu.File",
               "1menu.file",
               "menu.1file",
               "menu.file-name",
               "menu.file name",
               "menu/file",
               "menü.file",
               "menu.naïve",
           })
      {
        INFO("Translation key: " << key);

        directory.Write("en.cson", MinimalCatalog("en", "English", "Hello", key));

        const auto result = TranslationCatalog::Load(directory.Path(), "en");

        REQUIRE_FALSE(result);
        CHECK(result.error().kind == TranslationErrorKind::Validation);
        CHECK(result.error().message.find("invalid key") != std::string::npos);
      }
    }

    TEST_CASE("translation keys remain case-sensitive")
    {
      TemporaryDirectory directory;
      directory.Write(
          "en.cson", MinimalCatalog("en", "English", "Hello", "test.camelCase"));

      SECTION("lookups do not normalize key casing")
      {
        const auto catalog = TranslationCatalog::Load(directory.Path(), "en");

        REQUIRE(catalog);
        CHECK(catalog->Text("test.camelCase") == "Hello");
        CHECK(catalog->Text("test.camelcase") == "test.camelcase");
        CHECK(catalog->Text("test.camel_case") == "test.camel_case");
      }

      SECTION("all languages must match key casing exactly")
      {
        directory.Write(
            "de.cson", MinimalCatalog("de", "Deutsch", "Hallo", "test.camelcase"));

        const auto result = TranslationCatalog::Load(directory.Path(), "de");

        REQUIRE_FALSE(result);
        CHECK(result.error().kind == TranslationErrorKind::Validation);
        CHECK(result.error().message.find("missing") != std::string::npos);
      }
    }

    TEST_CASE("translation type errors identify the literal dotted key and source value")
    {
      TemporaryDirectory directory;
      directory.Write("en.cson",
                      "formatVersion: 1\n"
                      "language: { code: 'en', displayName: 'English' }\n"
                      "strings:\n"
                      "  'test.greeting': 42\n");

      const auto result = TranslationCatalog::Load(directory.Path(), "en");

      REQUIRE_FALSE(result);
      CHECK(result.error().kind == TranslationErrorKind::Validation);
      CHECK(result.error().path == directory.Path() / "en.cson");
      CHECK(result.error().message.find("$[\"strings\"][\"test.greeting\"]") !=
            std::string::npos);
      CHECK(result.error().line == 4);
      CHECK(result.error().column == 20);
    }

    TEST_CASE("translation parsing enforces byte and nesting limits in havCSON")
    {
      TemporaryDirectory directory;

      SECTION("input bytes")
      {
        directory.Write("en.cson", "#" + std::string(4U * 1024U * 1024U, 'x'));
      }
      SECTION("container depth")
      {
        directory.Write("en.cson", std::string(257, '[') + '0' + std::string(257, ']'));
      }

      const auto result = TranslationCatalog::Load(directory.Path(), "en");

      REQUIRE_FALSE(result);
      CHECK(result.error().kind == TranslationErrorKind::Parse);
      CHECK(result.error().message.find("Maximum") != std::string::npos);
      CHECK(result.error().line.has_value());
    }

    TEST_CASE("translation version requires a bounded integer")
    {
      TemporaryDirectory directory;

      for (const auto version : {"1.5", "-1", "4294967296"})
      {
        auto text = MinimalCatalog("en", "English");
        text.replace(text.find(": 1"), 3, std::string{": "} + version);

        directory.Write("en.cson", text);

        const auto result = TranslationCatalog::Load(directory.Path(), "en");

        REQUIRE_FALSE(result);
        CHECK(result.error().kind == TranslationErrorKind::Validation);
        CHECK(result.error().line == 1);
        CHECK(result.error().column == 16);
      }
    }

    TEST_CASE("indexed placeholders may be reordered and literal braces are escaped")
    {
      TemporaryDirectory directory;
      directory.Write(
          "en.cson",
          MinimalCatalog("en", "English", "{{{0}}} moved from {1}"));
      directory.Write(
          "de.cson",
          MinimalCatalog("de", "Deutsch", "Von {1} wurde {{{0}}}"));

      const auto catalog = TranslationCatalog::Load(directory.Path(), "de");

      REQUIRE(catalog);
      CHECK(catalog->Format("test.greeting", {"Datei", "Quelle"}) ==
            "Von Quelle wurde {Datei}");
    }
  } // namespace
} // namespace havremote::localization
