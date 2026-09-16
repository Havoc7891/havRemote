// SPDX-License-Identifier: MIT

#include "configTestSupport.hpp"

#include "config/csonConfigRepository.hpp"

#include <havCSON.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

namespace havremote::config
{
  TEST_CASE("Every atomic write failure stage keeps the prior configuration recoverable")
  {
    test::TempDirectory directory;

    constexpr std::array failureStages{
        havCSON::testing::AtomicWriteStage::TemporaryOpen,
        havCSON::testing::AtomicWriteStage::Write,
        havCSON::testing::AtomicWriteStage::Flush,
        havCSON::testing::AtomicWriteStage::Sync,
        havCSON::testing::AtomicWriteStage::Close,
        havCSON::testing::AtomicWriteStage::Replace,
    };

    for (std::size_t index = 0; index < failureStages.size(); ++index)
    {
      CAPTURE(index);

      const auto file = directory.Path() /
                        ("havRemote-" + std::to_string(index) + ".cson");

      CsonConfigRepository repository(file);

      auto loaded = repository.Load();

      REQUIRE(loaded.has_value());

      loaded->data.pendingCredentialDeletions = {
          "retained-password", "retained-passphrase"};

      REQUIRE(repository.Save(loaded->data).has_value());

      const std::string original = test::ReadText(file);

      loaded->data.settings.transferConcurrency = 5;
      loaded->data.settings.language = "de";
      loaded->data.workspace.lastLocalDirectory = "/changed-local";
      loaded->data.workspace.openTabs = {{
          .connectionId = "changed-tab",
          .localDirectory = "/changed-tab-local",
          .remoteDirectory = RemotePath{"/changed-tab-remote"},
      }};
      loaded->data.workspace.selectedConnectionId = "changed-tab";
      loaded->data.quickConnectHistory = {
          {ProtocolKind::Sftp, "changed.example", 22, "user"}};
      loaded->data.tlsTrust = {
          {"secure.example", 21, "sha256//YWJjZA=="}};
      loaded->data.pendingCredentialDeletions.clear();

      havCSON::testing::FailAtomicWriteAt(failureStages[index]);

      auto saved = repository.Save(loaded->data);

      havCSON::testing::FailAtomicWriteAt(havCSON::testing::AtomicWriteStage::None);

      REQUIRE_FALSE(saved.has_value());
      REQUIRE(saved.error().kind == ConfigErrorKind::AtomicWrite);
      REQUIRE(test::ReadText(file) == original);

      CsonConfigRepository failedWriteVerification(file);

      const auto afterFailure = failedWriteVerification.Load();

      REQUIRE(afterFailure.has_value());
      CHECK(afterFailure->data.pendingCredentialDeletions ==
            std::vector<std::string>{"retained-password", "retained-passphrase"});

      REQUIRE(repository.Save(loaded->data).has_value());
      REQUIRE(test::ReadText(file).find("transferConcurrency: 5.0") !=
              std::string::npos);

      CsonConfigRepository verification(file);

      const auto reloaded = verification.Load();

      REQUIRE(reloaded.has_value());
      CHECK(reloaded->data.settings == loaded->data.settings);
      CHECK(reloaded->data.workspace == loaded->data.workspace);
      CHECK(reloaded->data.quickConnectHistory == loaded->data.quickConnectHistory);
      CHECK(reloaded->data.tlsTrust == loaded->data.tlsTrust);
      CHECK(reloaded->data.pendingCredentialDeletions.empty());
    }
  }

  TEST_CASE("Site removal and pending credential deletions commit atomically")
  {
    test::TempDirectory directory;

    constexpr std::array failureStages{
        havCSON::testing::AtomicWriteStage::Write,
        havCSON::testing::AtomicWriteStage::Flush,
        havCSON::testing::AtomicWriteStage::Sync,
        havCSON::testing::AtomicWriteStage::Close,
        havCSON::testing::AtomicWriteStage::Replace,
    };

    SiteProfile site;
    site.id = "11111111-1111-4111-8111-111111111111";
    site.name = "Removed site";
    site.host = "removed.example";
    site.username = "user";
    site.authentication.credentialId = "removed-site-password";
    site.authentication.passphraseCredentialId = "removed-site-passphrase";

    for (std::size_t index = 0; index < failureStages.size(); ++index)
    {
      CAPTURE(index);

      const auto file = directory.Path() /
                        ("site-removal-" + std::to_string(index) + ".cson");

      CsonConfigRepository repository(file);

      ConfigData initial;
      initial.sites = {site};
      initial.siteManagerOrder = {site.id};

      REQUIRE(repository.Save(initial).has_value());

      const auto original = test::ReadText(file);

      auto removed = initial;
      removed.sites.clear();
      removed.siteManagerOrder.clear();
      removed.pendingCredentialDeletions = {
          site.authentication.credentialId,
          site.authentication.passphraseCredentialId,
      };

      havCSON::testing::FailAtomicWriteAt(failureStages[index]);

      const auto failure = repository.Save(removed);

      havCSON::testing::FailAtomicWriteAt(havCSON::testing::AtomicWriteStage::None);

      REQUIRE_FALSE(failure.has_value());
      CHECK(failure.error().kind == ConfigErrorKind::AtomicWrite);
      CHECK(test::ReadText(file) == original);

      CsonConfigRepository failedWriteVerification(file);

      const auto afterFailure = failedWriteVerification.Load();

      REQUIRE(afterFailure.has_value());
      REQUIRE(afterFailure->data.sites.size() == 1U);

      const auto &retainedSite = afterFailure->data.sites.front();

      CHECK(retainedSite.id == site.id);
      CHECK(retainedSite.authentication.credentialId ==
            site.authentication.credentialId);
      CHECK(retainedSite.authentication.passphraseCredentialId ==
            site.authentication.passphraseCredentialId);
      CHECK(afterFailure->data.siteManagerOrder == initial.siteManagerOrder);
      CHECK(afterFailure->data.pendingCredentialDeletions.empty());

      REQUIRE(repository.Save(removed).has_value());

      CsonConfigRepository verification(file);

      const auto reloaded = verification.Load();

      REQUIRE(reloaded.has_value());
      CHECK(reloaded->data.sites.empty());
      CHECK(reloaded->data.siteManagerOrder.empty());
      CHECK(reloaded->data.pendingCredentialDeletions ==
            removed.pendingCredentialDeletions);
    }
  }

  TEST_CASE("A failed configuration candidate is not reused by a different subsequent save")
  {
    test::TempDirectory directory;

    const auto file = directory.Path() / "havRemote.cson";

    CsonConfigRepository repository(file);

    const auto loaded = repository.Load();

    REQUIRE(loaded.has_value());

    const auto original = test::ReadText(file);

    auto rejected = loaded->data;
    rejected.settings.language = "de";
    rejected.workspace.lastLocalDirectory = "/rejected-local";
    rejected.quickConnectHistory = {
        {ProtocolKind::Sftp, "rejected.example", 22, "user"}};
    rejected.pendingCredentialDeletions = {"rejected-credential"};

    havCSON::testing::FailAtomicWriteAt(havCSON::testing::AtomicWriteStage::Replace);

    const auto failure = repository.Save(rejected);
    havCSON::testing::FailAtomicWriteAt(havCSON::testing::AtomicWriteStage::None);

    REQUIRE_FALSE(failure.has_value());
    REQUIRE(test::ReadText(file) == original);

    auto accepted = loaded->data;
    accepted.settings.transferConcurrency = 3;

    REQUIRE(repository.Save(accepted).has_value());

    CsonConfigRepository verification(file);

    const auto reloaded = verification.Load();

    REQUIRE(reloaded.has_value());
    CHECK(reloaded->data.settings == accepted.settings);
    CHECK(reloaded->data.workspace == accepted.workspace);
    CHECK(reloaded->data.quickConnectHistory.empty());
    CHECK(reloaded->data.pendingCredentialDeletions.empty());
    CHECK(test::ReadText(file).find("rejected") == std::string::npos);
  }
} // namespace havremote::config
