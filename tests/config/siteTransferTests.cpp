// SPDX-License-Identifier: MIT

#include "configTestSupport.hpp"
#include "config/siteTransfer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <unordered_set>

namespace havremote::config
{
  namespace
  {
    SiteProfile Site(std::string id, std::string name)
    {
      SiteProfile result;
      result.id = std::move(id);
      result.name = std::move(name);
      result.host = "example.test";
      result.username = "alice";
      result.initialRemoteDirectory = RemotePath{"/資料/Grüße"};

      return result;
    }

    SiteTransferData Tree()
    {
      return SiteTransferData{
          {Site("a", "Root first"), Site("b", "Folder first"),
           Site("c", "Nested site"), Site("d", "Folder last")},
          {{"outer", "Projects", "", {"b", "d"}},
           {"inner", "Nested", "outer", {"c"}},
           {"empty", "Empty", "", {}}},
          {"a", "outer", "b", "inner", "c", "d", "empty"}};
    }

    std::string ValidSite()
    {
      return R"({kind: "site", name: "Test", protocol: "sftp",
host: "example.test", port: 22, username: "alice",
authentication: "password", initialRemoteDirectory: "/data",
ftpEncoding: "UTF-8", ftpDataConnectionMode: "passive"})";
    }

    std::string Document(const std::string &entries)
    {
      return "formatVersion: 1\nkind: \"havRemoteSites\"\nentries: [" + entries + "]\n";
    }

    std::string Replaced(std::string text, const std::string_view oldText,
                         const std::string_view newText)
    {
      const auto position = text.find(oldText);

      REQUIRE(position != std::string::npos);

      text.replace(position, oldText.size(), newText);

      return text;
    }

    std::vector<std::string> OrderedNames(const SiteTransferData &data)
    {
      std::vector<std::string> names;

      for (const auto &id : data.siteManagerOrder)
      {
        const auto saved = std::ranges::find(data.sites, id, &SiteProfile::id);
        if (saved != data.sites.end())
        {
          names.push_back(saved->name);
        }
        else
        {
          const auto folder = std::ranges::find(data.folders, id, &SiteFolder::id);

          REQUIRE(folder != data.folders.end());

          names.push_back(folder->name);
        }
      }

      return names;
    }
  } // namespace

  TEST_CASE("portable site exports retain mixed nested order and regenerate identities")
  {
    test::TempDirectory temporary;

    const auto file = temporary.Path() / std::filesystem::path{u8"Übertragung-資料.cson"};

    const auto source = Tree();

    REQUIRE(WriteSiteExport(file, source));

    const auto serialized = test::ReadText(file);

    CHECK(serialized.starts_with("formatVersion: 1.0\nkind: \"havRemoteSites\"\nentries:"));
    CHECK(serialized.find("  kind:") != std::string::npos);

    const auto first = ReadSiteExport(file);

    const auto second = ReadSiteExport(file);

    INFO((first ? "first import succeeded" : first.error().message));
    INFO((second ? "second import succeeded" : second.error().message));
    REQUIRE(first);
    REQUIRE(second);
    CHECK(OrderedNames(*first) == OrderedNames(source));
    CHECK(first->sites.size() == 4U);
    REQUIRE(first->folders.size() == 3U);
    CHECK(first->folders[0].parentId.empty());
    CHECK(first->folders[1].parentId == first->folders[0].id);
    CHECK(first->folders[0].siteIds ==
          std::vector<std::string>{first->sites[1].id, first->sites[3].id});
    CHECK(first->folders[1].siteIds == std::vector<std::string>{first->sites[2].id});
    CHECK(first->folders[2].siteIds.empty());
    CHECK(first->sites[2].initialRemoteDirectory.Bytes() == "/資料/Grüße");

    std::unordered_set<std::string> allIds(source.siteManagerOrder.begin(),
                                           source.siteManagerOrder.end());

    for (const auto *copy : {&*first, &*second})
    {
      for (const auto &id : copy->siteManagerOrder)
      {
        CHECK(id.size() == 36U);
        CHECK(allIds.insert(id).second);
      }
    }

    const auto another = temporary.Path() / "again.cson";

    REQUIRE(WriteSiteExport(another, *first));
    CHECK(test::ReadText(another) == serialized);
  }

  TEST_CASE("portable site export selects exactly one site or an entire folder subtree")
  {
    test::TempDirectory temporary;

    const auto file = temporary.Path() / "subset.cson";

    const auto source = Tree();

    REQUIRE(WriteSiteExport(file, source, "b"));

    auto loaded = ReadSiteExport(file);

    INFO((loaded ? "subset import succeeded" : loaded.error().message));
    REQUIRE(loaded);
    REQUIRE(loaded->sites.size() == 1U);
    CHECK(loaded->sites[0].name == "Folder first");
    CHECK(loaded->folders.empty());
    REQUIRE(WriteSiteExport(file, source, "inner"));

    loaded = ReadSiteExport(file);

    REQUIRE(loaded);
    CHECK(OrderedNames(*loaded) == std::vector<std::string>{"Nested", "Nested site"});
    REQUIRE(loaded->folders.size() == 1U);
    CHECK(loaded->folders[0].parentId.empty());
    REQUIRE(WriteSiteExport(file, source, "outer"));

    loaded = ReadSiteExport(file);

    REQUIRE(loaded);
    CHECK(OrderedNames(*loaded) == std::vector<std::string>{
                                       "Projects", "Folder first", "Nested", "Nested site", "Folder last"});
    REQUIRE(WriteSiteExport(file, source, "empty"));

    loaded = ReadSiteExport(file);

    REQUIRE(loaded);
    CHECK(loaded->sites.empty());
    CHECK(loaded->folders.size() == 1U);

    const auto prior = test::ReadText(file);

    CHECK_FALSE(WriteSiteExport(file, source, "missing"));
    CHECK(test::ReadText(file) == prior);
    CHECK(OrderedNames(source) == OrderedNames(Tree()));
  }

  TEST_CASE("portable sites never include credentials machine paths or local network overrides")
  {
    test::TempDirectory temporary;

    const auto file = temporary.Path() / "portable.cson";

    auto saved = Site("private-original-id", "Portable");
    saved.authentication.kind = AuthenticationKind::PrivateKey;
    saved.authentication.credentialId = "private-password-credential";
    saved.authentication.passphraseCredentialId = "private-passphrase-credential";
    saved.authentication.privateKeyFile = "private-machine-private-key";
    saved.authentication.publicKeyFile = "private-machine-public-key";
    saved.initialLocalDirectory = "private-local-directory";
    saved.ftpActiveAddress = "192.0.2.123";

    const SiteTransferData data{{saved}, {}, {saved.id}};

    REQUIRE(WriteSiteExport(file, data));

    const auto exported = test::ReadText(file);

    for (const auto forbidden : {"private-original-id", "private-password-credential",
                                 "private-passphrase-credential", "private-machine-private-key",
                                 "private-machine-public-key", "private-local-directory",
                                 "192.0.2.123", "credentialId", "privateKeyFile", "publicKeyFile",
                                 "initialLocalDirectory", "ftpActiveAddress", "tlsTrust"})
    {
      CHECK(exported.find(forbidden) == std::string::npos);
    }

    const auto loaded = ReadSiteExport(file);

    INFO((loaded ? "portable import succeeded" : loaded.error().message));
    REQUIRE(loaded);
    REQUIRE(loaded->sites.size() == 1U);

    const auto &imported = loaded->sites[0];

    CHECK(imported.authentication.kind == AuthenticationKind::PrivateKey);
    CHECK(imported.authentication.credentialId.empty());
    CHECK(imported.authentication.passphraseCredentialId.empty());
    CHECK(imported.authentication.privateKeyFile.empty());
    CHECK(imported.authentication.publicKeyFile.empty());
    CHECK(imported.initialLocalDirectory.empty());
    CHECK(imported.ftpActiveAddress.empty());
    CHECK(saved.authentication.credentialId == "private-password-credential");

    // Re-exporting a staged key-auth import does not require a local key
    REQUIRE(WriteSiteExport(file, *loaded));
  }

  TEST_CASE("portable site imports accept every current protocol and SSH authentication mode")
  {
    test::TempDirectory temporary;

    const auto file = temporary.Path() / "modes.cson";

    for (const auto protocol : {ProtocolKind::Ftp, ProtocolKind::FtpsExplicit,
                                ProtocolKind::FtpsImplicit, ProtocolKind::Sftp})
    {
      for (const auto authentication : {AuthenticationKind::Password,
                                        AuthenticationKind::PrivateKey,
                                        AuthenticationKind::Agent,
                                        AuthenticationKind::KeyboardInteractive,
                                        AuthenticationKind::PasswordKeyboardInteractive})
      {
        if (protocol != ProtocolKind::Sftp && authentication != AuthenticationKind::Password)
        {
          continue;
        }

        for (const auto mode : {FtpDataConnectionMode::Passive, FtpDataConnectionMode::Active})
        {
          if (protocol == ProtocolKind::Sftp && mode == FtpDataConnectionMode::Active)
          {
            continue;
          }

          auto saved = Site("test", "Protocol");
          saved.protocol = protocol;
          saved.authentication.kind = authentication;
          saved.ftpDataConnectionMode = mode;
          saved.port = 65535U;
          saved.username.clear();
          saved.initialRemoteDirectory = RemotePath{""};

          REQUIRE(WriteSiteExport(file, {{saved}, {}, {saved.id}}));

          const auto loaded = ReadSiteExport(file);

          INFO((loaded ? "mode import succeeded" : loaded.error().message));
          REQUIRE(loaded);
          REQUIRE(loaded->sites.size() == 1U);
          CHECK(loaded->sites[0].protocol == protocol);
          CHECK(loaded->sites[0].authentication.kind == authentication);
          CHECK(loaded->sites[0].ftpDataConnectionMode == mode);
          CHECK(loaded->sites[0].port == 65535U);
        }
      }
    }
  }

  TEST_CASE("portable site import rejects malformed duplicate unsupported and mistyped fields")
  {
    test::TempDirectory temporary;

    const auto file = temporary.Path() / "invalid.cson";

    const auto valid = Document(ValidSite());

    const std::vector<std::string> invalid{
        "this: [",
        "[]",
        "",
        "formatVersion: 1\nkind: \"havRemoteSites\"",
        Replaced(valid, "formatVersion: 1", "formatVersion: 2"),
        Replaced(valid, "formatVersion: 1", "formatVersion: 0"),
        Replaced(valid, "formatVersion: 1", "formatVersion: 1.5"),
        Replaced(valid, "formatVersion: 1", "formatVersion: true"),
        Replaced(valid, "havRemoteSites", "havRemoteQueue"),
        valid + "unknown: true\n",
        Replaced(valid, "port: 22", "port: 22, port: 23"),
        Replaced(valid, "port: 22", "port: 0"),
        Replaced(valid, "port: 22", "port: -1"),
        Replaced(valid, "port: 22", "port: 65536"),
        Replaced(valid, "port: 22", "port: 22.5"),
        Replaced(valid, "port: 22", "port: \"22\""),
        Replaced(valid, "port: 22", "port: true"),
        Replaced(valid, "port: 22", "port: null"),
        Replaced(valid, "name: \"Test\"", "name: \"\""),
        Replaced(valid, "name: \"Test\"", "name: \"   \""),
        Replaced(valid, "name: \"Test\"", "name: false"),
        Replaced(valid, "example.test", "ftp://example.test"),
        Replaced(valid, "example.test", "example.test:22"),
        Replaced(valid, "example.test", "host name"),
        Replaced(valid, "protocol: \"sftp\"", "protocol: \"future\""),
        Replaced(valid, "authentication: \"password\"", "authentication: \"future\""),
        Replaced(valid, "authentication: \"password\"", "authentication: {}"),
        Replaced(valid, "ftpDataConnectionMode: \"passive\"", "ftpDataConnectionMode: \"active\""),
        Replaced(valid, "ftpDataConnectionMode: \"passive\"", "ftpDataConnectionMode: \"future\""),
        Replaced(valid, "ftpEncoding: \"UTF-8\"", "ftpEncoding: \"\""),
        Replaced(valid, "initialRemoteDirectory: \"/data\"", "initialRemoteDirectory: false"),
        Replaced(valid, "username: \"alice\"", "username: \"bad\\u0000user\""),
        Replaced(valid, "initialRemoteDirectory: \"/data\"", "initialRemoteDirectory: \"/bad\\npath\""),
        Replaced(valid, "name: \"Test\"", "name: \"bad\\u007f\""),
        Replaced(valid, "name: \"Test\"", "name: \"Test\", credentialId: \"secret-id\""),
        Document("{kind: \"folder\", name: \"Folder\", entries: {}}"),
        Document("{kind: \"future\", name: \"Unknown\"}"),
        Document(ValidSite() + ", " + ValidSite()),
        Document(ValidSite() + ", {kind: \"folder\", name: \"Test\", entries: []}"),
        Document("{kind: \"folder\", name: \"Folder\", entries: [], entries: []}"),
        Replaced(Replaced(valid, "protocol: \"sftp\"", "protocol: \"ftp\""),
                 "authentication: \"password\"", "authentication: \"agent\""),
    };

    for (std::size_t index = 0; index < invalid.size(); ++index)
    {
      CAPTURE(index);

      test::WriteText(file, invalid[index]);

      const auto loaded = ReadSiteExport(file);

      REQUIRE_FALSE(loaded);
      CHECK(loaded.error().path == file);
      CHECK_FALSE(loaded.error().message.empty());
      CHECK(test::ReadText(file) == invalid[index]);
    }

    test::WriteText(file, Replaced(valid, "port: 22", "port: 22, port: 23"));

    auto loaded = ReadSiteExport(file);

    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().kind == ConfigErrorKind::Parse);
    CHECK(loaded.error().line.has_value());
    CHECK(loaded.error().column.has_value());

    test::WriteText(file, Replaced(valid, "formatVersion: 1", "formatVersion: 2"));

    loaded = ReadSiteExport(file);

    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().kind == ConfigErrorKind::UnsupportedVersion);

    test::WriteText(file, Replaced(valid, "Test", std::string{"\xc0\xaf", 2}));

    loaded = ReadSiteExport(file);

    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().kind == ConfigErrorKind::Parse);
    CHECK(loaded.error().line.has_value());
    CHECK(loaded.error().column.has_value());
  }

  TEST_CASE("portable site imports allow repeated names in different folders and comments")
  {
    test::TempDirectory temporary;

    const auto file = temporary.Path() / "comments.cson";

    test::WriteText(file, "# [ ignored delimiter\n" + Document("{kind: \"folder\", name: \"One\", entries: [" + ValidSite() + "]},\n" + "{kind: \"folder\", name: \"Two\", entries: [" + ValidSite() + "]}") +
                              "###\n# ignored [[{{\n###\n");

    const auto loaded = ReadSiteExport(file);

    REQUIRE(loaded);
    CHECK(loaded->sites.size() == 2U);
    CHECK(loaded->folders.size() == 2U);
  }

  TEST_CASE("portable site export validates topology ordering names and subset requests before writing")
  {
    test::TempDirectory temporary;

    const auto file = temporary.Path() / "unchanged.cson";

    const std::string original = "previous export\n";

    std::vector<SiteTransferData> invalid;

    auto add = [&](auto change)
    {
      auto data = Tree();

      change(data);

      invalid.push_back(std::move(data));
    };

    add([](auto &d)
        { d.sites[1].id = d.sites[0].id; });
    add([](auto &d)
        { d.folders[0].id = d.sites[0].id; });
    add([](auto &d)
        { d.folders[1].id = d.folders[0].id; });
    add([](auto &d)
        { d.folders[0].parentId = "missing"; });
    add([](auto &d)
        { d.folders[0].parentId = d.folders[0].id; });
    add([](auto &d)
        { d.folders[0].parentId = d.folders[1].id; });
    add([](auto &d)
        { d.folders[0].siteIds.push_back("missing"); });
    add([](auto &d)
        { d.folders[0].siteIds.push_back("b"); });
    add([](auto &d)
        { d.folders[1].siteIds.push_back("b"); });
    add([](auto &d)
        { d.siteManagerOrder.pop_back(); });
    add([](auto &d)
        { d.siteManagerOrder.push_back("missing"); });
    add([](auto &d)
        { d.siteManagerOrder.push_back("a"); });
    add([](auto &d)
        { d.sites[3].name = d.sites[1].name; });
    add([](auto &d)
        { d.folders[0].name = d.sites[0].name; });
    add([](auto &d)
        { d.sites[0].protocol = static_cast<ProtocolKind>(99); });
    add([](auto &d)
        { d.sites[0].authentication.kind = static_cast<AuthenticationKind>(99); });
    add([](auto &d)
        { d.sites[0].ftpDataConnectionMode = static_cast<FtpDataConnectionMode>(99); });
    add([](auto &d)
        { d.sites[0].host = "ftp://example.test"; });
    add([](auto &d)
        { d.sites[0].initialRemoteDirectory = RemotePath{"/bad\npath"}; });
    add([](auto &d)
        { d.sites[0].name = std::string{"\xed\xa0\x80", 3}; });

    for (std::size_t index = 0; index < invalid.size(); ++index)
    {
      CAPTURE(index);

      test::WriteText(file, original);

      // Full source topology is checked even for a valid-looking subset
      const auto written = WriteSiteExport(file, invalid[index], "a");

      REQUIRE_FALSE(written);
      CHECK(written.error().kind == ConfigErrorKind::Validation);
      CHECK(test::ReadText(file) == original);
    }
  }

  TEST_CASE("portable site export bounds file size text size entry count and nesting")
  {
    test::TempDirectory temporary;

    const auto file = temporary.Path() / "bounded.cson";

    test::WriteText(file, std::string(4U * 1024U * 1024U + 1U, ' '));

    CHECK_FALSE(ReadSiteExport(file));

    test::WriteText(file, Document(Replaced(ValidSite(), "Test", std::string(16'385U, 'x'))));

    CHECK_FALSE(ReadSiteExport(file));

    test::WriteText(file, "entries: " + std::string(100'000U, '['));

    CHECK_FALSE(ReadSiteExport(file));

    // ### remains a line comment. Subsequent syntax still consumes the
    // parser's real container-depth budget.
    test::WriteText(file, "###\nentries: " + std::string(100'000U, '['));

    CHECK_FALSE(ReadSiteExport(file));

    test::WriteText(file, std::string(300U, ' ') + "entries: []\n");

    CHECK_FALSE(ReadSiteExport(file));

    std::string nested = ValidSite();

    for (int depth = 0; depth < 33; ++depth)
    {
      nested = "{kind: \"folder\", name: \"Folder\", entries: [" + nested + "]}";
    }

    test::WriteText(file, Document(nested));

    CHECK_FALSE(ReadSiteExport(file));

    SiteTransferData deep;

    for (int depth = 0; depth < 33; ++depth)
    {
      const auto id = std::to_string(depth);

      deep.folders.push_back({id, "Folder", depth == 0 ? "" : std::to_string(depth - 1), {}});
      deep.siteManagerOrder.push_back(id);
    }

    CHECK_FALSE(WriteSiteExport(file, deep));

    deep.folders.pop_back();
    deep.siteManagerOrder.pop_back();

    REQUIRE(WriteSiteExport(file, deep));

    const auto deepImport = ReadSiteExport(file);

    INFO((deepImport ? "depth-boundary import succeeded" : deepImport.error().message));
    REQUIRE(deepImport);

    std::string many;

    SiteTransferData tooMany;

    for (int index = 0; index < 10'001; ++index)
    {
      const auto id = std::to_string(index);

      tooMany.folders.push_back({id, id, "", {}});
      tooMany.siteManagerOrder.push_back(id);

      if (index != 0)
      {
        many += ',';
      }

      many += "{kind: \"folder\", name: \"" + id + "\", entries: []}";
    }

    test::WriteText(file, Document(many));

    CHECK_FALSE(ReadSiteExport(file));
    CHECK_FALSE(WriteSiteExport(file, tooMany));

    SiteTransferData large;

    for (int index = 0; index < 260; ++index)
    {
      auto item = Site(std::to_string(index), std::to_string(index));
      item.initialRemoteDirectory = RemotePath{"/" + std::string(16'000U, 'x')};

      large.siteManagerOrder.push_back(item.id);
      large.sites.push_back(std::move(item));
    }

    CHECK_FALSE(WriteSiteExport(file, large));
  }

  TEST_CASE("empty portable site trees and IO failures are explicit")
  {
    test::TempDirectory temporary;

    const auto file = temporary.Path() / "empty.cson";

    auto loaded = ReadSiteExport(file);

    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().kind == ConfigErrorKind::Io);
    REQUIRE(WriteSiteExport(file, {}));

    loaded = ReadSiteExport(file);

    REQUIRE(loaded);
    CHECK(loaded->sites.empty());
    CHECK(loaded->folders.empty());
    CHECK(loaded->siteManagerOrder.empty());
    CHECK_FALSE(WriteSiteExport(temporary.Path() / "absent" / "sites.cson", Tree()));
  }

  TEST_CASE("site transfer filenames cannot be truncated at an embedded NUL")
  {
    test::TempDirectory temporary;

    const auto file = temporary.Path() / "unchanged.cson";

    REQUIRE(WriteSiteExport(file, Tree()));

    const auto original = test::ReadText(file);

    auto native = file.native();
    native.push_back(std::filesystem::path::value_type{});
    native += std::filesystem::path{"suffix"}.native();

    const std::filesystem::path invalid{native};

    CHECK_FALSE(ReadSiteExport(invalid));
    CHECK_FALSE(WriteSiteExport(invalid, {}));
    CHECK(test::ReadText(file) == original);
    CHECK_FALSE(ReadSiteExport({}));
    CHECK_FALSE(WriteSiteExport({}, {}));
  }

  TEST_CASE("site imports use the parser depth limit for explicit and implicit objects")
  {
    test::TempDirectory temporary;

    const auto file = temporary.Path() / "implicit.cson";

    const auto repeated = [](const std::string_view value)
    {
      std::string text;

      for (int index = 0; index < 10'000; ++index)
      {
        text += value;
      }

      return text;
    };

    for (const auto &source : {
             repeated("a:") + "0",
             repeated("'a':") + "0",
             repeated("\"a\":") + "0",
             repeated("a:b:0, ") + "z:0",
             repeated("a:b:0\n") + "z:0",
             "###\r# ignored quote\r" + repeated("a:") + "0",
             "[" + repeated("a:") + "0]",
             "{\"field\":" + repeated("'a':") + "0}",
             std::string(10'000U, '[') + "0" + std::string(10'000U, ']')})
    {
      test::WriteText(file, source);

      const auto loaded = ReadSiteExport(file);

      REQUIRE_FALSE(loaded);
      CHECK(loaded.error().kind == ConfigErrorKind::Validation);
      CHECK(loaded.error().message.find("depth") != std::string::npos);
      CHECK(loaded.error().line.has_value());
      CHECK(loaded.error().column.has_value());
    }

    // A wide, shallow document must reach normal duplicate-key validation.
    // Counting every colon as a permanently active container rejects it early.
    for (const auto &source : {repeated("a:0, ") + "z:0",
                               repeated("a:0\n") + "z:0"})
    {
      test::WriteText(file, source);

      const auto loaded = ReadSiteExport(file);

      REQUIRE_FALSE(loaded);
      CHECK(loaded.error().kind == ConfigErrorKind::Parse);
      CHECK(loaded.error().message.find("depth") == std::string::npos);
    }

    // Colon-like text in a valid string/comment consumes no parser stack
    const auto safe = Document(Replaced(ValidSite(), "/data", repeated("a:").substr(0, 16000)));

    test::WriteText(file, "# " + repeated("a:") + "\n" + safe);

    const auto loaded = ReadSiteExport(file);

    INFO((loaded ? "quoted colons imported" : loaded.error().message));
    REQUIRE(loaded);
  }

  TEST_CASE("site import byte limits include comments and accept the exact boundary")
  {
    test::TempDirectory temporary;

    const auto file = temporary.Path() / "limit.cson";

    constexpr std::size_t maximumBytes = 4U * 1024U * 1024U;

    const auto valid = Document(ValidSite());

    const auto source = "#" + std::string(maximumBytes - valid.size() - 2U, 'x') +
                        "\n" + valid;

    REQUIRE(source.size() == maximumBytes);

    test::WriteText(file, source);

    REQUIRE(ReadSiteExport(file));

    test::WriteText(file, source + "\n");

    const auto oversized = ReadSiteExport(file);

    REQUIRE_FALSE(oversized);
    CHECK(oversized.error().kind == ConfigErrorKind::Validation);
    CHECK(oversized.error().message.find("input byte") != std::string::npos);
    CHECK(oversized.error().path == file);
    CHECK(test::ReadText(file) == source + "\n");
  }

  TEST_CASE("site imports report exact source locations for nested checked values")
  {
    test::TempDirectory temporary;

    const auto file = temporary.Path() / std::filesystem::path{u8"Prüfung-資料.cson"};

    for (const auto invalidPort : {"\"22\"", "22.5", "0", "65536", "true"})
    {
      CAPTURE(invalidPort);

      const auto portField = "port: " + std::string{invalidPort};
      const auto badSite = Replaced(Replaced(ValidSite(), "name: \"Test\"",
                                             "name: \"Second\""),
                                    "port: 22", portField);
      const auto source = "# port: 0 is not the failing field\n" + Document(
                                                                       ValidSite() + ", {kind: \"folder\", name: \"Folder\", entries: [" + badSite + "]}");

      test::WriteText(file, source);

      const auto loaded = ReadSiteExport(file);

      REQUIRE_FALSE(loaded);
      CHECK(loaded.error().kind == ConfigErrorKind::Validation);
      CHECK(loaded.error().message.find("$[\"entries\"][1][\"entries\"][0][\"port\"]") !=
            std::string::npos);

      const auto offset = source.find(portField, source.find("name: \"Second\"")) + 6U;

      const auto lineStart = source.rfind('\n', offset);

      CHECK(loaded.error().line == static_cast<std::size_t>(
                                       std::count(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(offset), '\n')) +
                                       1U);
      CHECK(loaded.error().column == offset - lineStart);
      CHECK(loaded.error().path == file);
      CHECK(test::ReadText(file) == source);
    }
  }

  TEST_CASE("site import missing members refer to their containing object")
  {
    test::TempDirectory temporary;

    const auto file = temporary.Path() / "missing-member.cson";

    auto source = Document(ValidSite());
    source = Replaced(source, "username: \"alice\",", "");

    test::WriteText(file, source);

    const auto loaded = ReadSiteExport(file);

    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().kind == ConfigErrorKind::Validation);
    CHECK(loaded.error().message.find("$[\"entries\"][0][\"username\"]") != std::string::npos);
    CHECK(loaded.error().line == 3U);
    CHECK(loaded.error().column == 11U);
  }

  TEST_CASE("site imports distinguish syntax failures from inaccessible file targets")
  {
    test::TempDirectory temporary;

    for (const auto &file : {temporary.Path(), temporary.Path() / "absent.cson",
                             temporary.Path() / "missing" / "sites.cson"})
    {
      const auto loaded = ReadSiteExport(file);

      REQUIRE_FALSE(loaded);
      CHECK(loaded.error().kind == ConfigErrorKind::Io);
      CHECK(loaded.error().path == file);
      CHECK_FALSE(loaded.error().line.has_value());
      CHECK_FALSE(loaded.error().column.has_value());
    }

    const auto empty = temporary.Path() / "empty.cson";

    test::WriteText(empty, "");

    const auto malformed = ReadSiteExport(empty);

    REQUIRE_FALSE(malformed);
    CHECK(malformed.error().kind == ConfigErrorKind::Parse);
    CHECK(malformed.error().line.has_value());
    CHECK(malformed.error().column.has_value());
  }
} // namespace havremote::config
