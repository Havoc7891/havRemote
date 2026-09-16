// SPDX-License-Identifier: MIT

#include "platform/credentialCleanup.hpp"

#include "../config/configTestSupport.hpp"
#include "config/csonConfigRepository.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace havremote;

namespace
{
  struct CredentialStoreState final
  {
    std::unordered_set<std::string> identifiers;
    std::unordered_map<std::string, platform::PlatformError> eraseFailures;
  };

  class FakeCredentialStore final : public platform::ICredentialStore
  {
  public:
    explicit FakeCredentialStore(CredentialStoreState &state) : mState(state) {}

    platform::Result<void> Store(std::string_view,
                                 std::string_view,
                                 std::span<const std::byte>) override
    {
      FAIL("Credential cleanup must not store secrets");

      return {};
    }

    platform::Result<platform::CredentialPayload> Load(std::string_view) const override
    {
      FAIL("Credential cleanup must not load secrets");

      return std::unexpected(platform::PlatformError{
          platform::PlatformErrorCode::NotFound, "No such credential"});
    }

    platform::Result<void> Erase(const std::string_view credentialId) override
    {
      const std::string id{credentialId};

      mEraseCalls.push_back(id);

      const auto failure = mState.eraseFailures.find(id);

      if (failure != mState.eraseFailures.end())
      {
        return std::unexpected(failure->second);
      }

      if (mState.identifiers.erase(id) == 0)
      {
        return std::unexpected(platform::PlatformError{
            platform::PlatformErrorCode::NotFound, "No such credential"});
      }

      return {};
    }

    [[nodiscard]] const std::vector<std::string> &EraseCalls() const
    {
      return mEraseCalls;
    }

  private:
    CredentialStoreState &mState;
    std::vector<std::string> mEraseCalls;
  };

  SiteProfile Site(std::string id,
                   std::string passwordId,
                   std::string passphraseId = {})
  {
    SiteProfile site;
    site.id = std::move(id);
    site.name = site.id;
    site.host = "example.test";
    site.username = "user";
    site.authentication.credentialId = std::move(passwordId);
    site.authentication.passphraseCredentialId = std::move(passphraseId);

    return site;
  }

  void RequireSaved(config::CsonConfigRepository &repository,
                    const config::ConfigData &data)
  {
    const auto saved = repository.Save(data);

    INFO((saved.has_value() ? std::string{} : saved.error().message));
    REQUIRE(saved);
  }
} // namespace

TEST_CASE("Removed and replaced passwords and passphrases are queued in order",
          "[platform][credentials][cleanup]")
{
  const std::vector pending{std::string{"earlier-removal"}};

  const std::vector previousSites{
      Site("removed", "removed-password", "removed-passphrase"),
      Site("changed", "old-password", "old-passphrase"),
      Site("unchanged", "kept-password", "kept-passphrase")};

  const std::vector currentSites{
      Site("changed", "new-password", "new-passphrase"),
      previousSites.back()};

  CHECK(platform::QueueRemovedCredentials(pending, previousSites, currentSites) ==
        std::vector<std::string>{"earlier-removal", "removed-password",
                                 "removed-passphrase", "old-password",
                                 "old-passphrase"});
}

TEST_CASE("Saved references protect credentials shared across sites and fields",
          "[platform][credentials][cleanup]")
{
  const std::vector previousSites{
      Site("removed", "shared-password", "shared-passphrase"),
      Site("retained", "shared-password", "shared-passphrase")};

  const std::vector currentSites{
      Site("retained", "shared-passphrase", "shared-password")};

  CHECK(platform::QueueRemovedCredentials({}, previousSites, currentSites).empty());

  // A marker loaded from an earlier cleanup remains durable even if a saved
  // site currently references it. Erasure checks the references again.
  const std::vector pending{std::string{"shared-password"},
                            std::string{"shared-passphrase"}};

  CHECK(platform::QueueRemovedCredentials(pending, previousSites, currentSites) ==
        pending);

  CredentialStoreState state{.identifiers = {"shared-password", "shared-passphrase"},
                             .eraseFailures = {}};

  FakeCredentialStore store{state};

  const auto cleaned = platform::EraseReleasedCredentials(pending, currentSites, {}, store);

  CHECK(cleaned.pendingIds == pending);
  CHECK(cleaned.errors.empty());
  CHECK(store.EraseCalls().empty());
  CHECK(state.identifiers.size() == 2);
}

TEST_CASE("Cleanup queues normalize empty and duplicate credential identifiers",
          "[platform][credentials][cleanup]")
{
  const std::vector pending{std::string{}, std::string{"old"}, std::string{"old"},
                            std::string{"other"}, std::string{}};

  const std::vector previousSites{
      Site("first", "old", "new"),
      Site("second", "new", "new"),
      Site("empty", "")};

  CHECK(platform::QueueRemovedCredentials(pending, previousSites, {}) ==
        std::vector<std::string>{"old", "other", "new"});

  CredentialStoreState state{.identifiers = {"old", "other"}, .eraseFailures = {}};

  FakeCredentialStore store{state};

  const auto cleaned = platform::EraseReleasedCredentials(pending, {}, {}, store);

  CHECK(store.EraseCalls() == std::vector<std::string>{"old", "other"});
  CHECK(cleaned.pendingIds.empty());
  CHECK(cleaned.errors.empty());
}

TEST_CASE("Active authentications defer queued credential erasure until released",
          "[platform][credentials][cleanup]")
{
  const std::vector previousSites{
      Site("removed", "active-password", "active-passphrase"),
      Site("also-removed", "released-password")};

  const std::vector active{previousSites.front().authentication};

  const auto pending = platform::QueueRemovedCredentials({}, previousSites, {});

  CHECK(pending == std::vector<std::string>{"active-password", "active-passphrase",
                                            "released-password"});

  CredentialStoreState state{
      .identifiers = {"active-password", "active-passphrase", "released-password"},
      .eraseFailures = {}};

  FakeCredentialStore store{state};

  const auto deferred = platform::EraseReleasedCredentials(pending, {}, active, store);

  CHECK(deferred.pendingIds ==
        std::vector<std::string>{"active-password", "active-passphrase"});
  CHECK(deferred.errors.empty());
  CHECK(store.EraseCalls() == std::vector<std::string>{"released-password"});

  const auto released = platform::EraseReleasedCredentials(deferred.pendingIds, {}, {}, store);

  CHECK(released.pendingIds.empty());
  CHECK(released.errors.empty());
  CHECK(store.EraseCalls() ==
        std::vector<std::string>{"released-password", "active-password", "active-passphrase"});
  CHECK(state.identifiers.empty());
}

TEST_CASE("Credential failures and references retain pending order for retry",
          "[platform][credentials][cleanup]")
{
  const std::vector pending{std::string{"unavailable"}, std::string{"referenced"},
                            std::string{"successful"}, std::string{"denied"},
                            std::string{"unavailable"}, std::string{"absent"}};

  const std::vector sites{Site("retained", "referenced")};

  CredentialStoreState state{
      .identifiers = {"unavailable", "referenced", "successful", "denied"},
      .eraseFailures = {
          {"unavailable", {platform::PlatformErrorCode::Unavailable, "Store is locked", 17}},
          {"denied", {platform::PlatformErrorCode::AccessDenied, "Access denied", 23}}}};

  FakeCredentialStore store{state};

  const auto failed = platform::EraseReleasedCredentials(pending, sites, {}, store);

  CHECK(failed.pendingIds ==
        std::vector<std::string>{"unavailable", "referenced", "denied"});
  CHECK(store.EraseCalls() ==
        std::vector<std::string>{"unavailable", "successful", "denied", "absent"});
  REQUIRE(failed.errors.size() == 2);
  CHECK(failed.errors[0].code == platform::PlatformErrorCode::Unavailable);
  CHECK(failed.errors[0].message == "Store is locked");
  CHECK(failed.errors[0].nativeCode == 17);
  CHECK(failed.errors[1].code == platform::PlatformErrorCode::AccessDenied);
  CHECK(failed.errors[1].message == "Access denied");
  CHECK(failed.errors[1].nativeCode == 23);

  state.eraseFailures.clear();

  FakeCredentialStore retryStore{state};

  const auto retried = platform::EraseReleasedCredentials(failed.pendingIds, sites, {}, retryStore);

  CHECK(retried.pendingIds == std::vector<std::string>{"referenced"});
  CHECK(retried.errors.empty());
  CHECK(retryStore.EraseCalls() == std::vector<std::string>{"unavailable", "denied"});
  CHECK(state.identifiers == std::unordered_set<std::string>{"referenced"});
}

TEST_CASE("Cleanup safely replays an already erased credential",
          "[platform][credentials][cleanup]")
{
  const std::vector pending{std::string{"password"}, std::string{"passphrase"}};

  CredentialStoreState state{.identifiers = {"password", "passphrase"}, .eraseFailures = {}};
  {
    FakeCredentialStore store{state};

    const auto first = platform::EraseReleasedCredentials(pending, {}, {}, store);

    REQUIRE(first.pendingIds.empty());
    REQUIRE(first.errors.empty());
  }

  FakeCredentialStore restartedStore{state};

  const auto replayed = platform::EraseReleasedCredentials(pending, {}, {}, restartedStore);

  CHECK(replayed.pendingIds.empty());
  CHECK(replayed.errors.empty());
  CHECK(restartedStore.EraseCalls() == pending);
}

TEST_CASE("Saved cleanup markers survive active references and unavailable storage across restart",
          "[platform][credentials][cleanup][config]")
{
  config::test::TempDirectory directory;

  const auto file = directory.Path() / "havRemote.cson";

  const std::vector previousSites{Site("removed", "password", "passphrase")};

  CredentialStoreState state{
      .identifiers = {"password", "passphrase"},
      .eraseFailures = {
          {"password", {platform::PlatformErrorCode::Unavailable, "Store unavailable"}},
          {"passphrase", {platform::PlatformErrorCode::Unavailable, "Store unavailable"}}}};
  {
    config::CsonConfigRepository repository{file};

    auto loaded = repository.Load();

    REQUIRE(loaded);

    loaded->data.sites = previousSites;
    loaded->data.siteManagerOrder = {previousSites.front().id};

    RequireSaved(repository, loaded->data);

    loaded->data.pendingCredentialDeletions =
        platform::QueueRemovedCredentials({}, previousSites, {});
    loaded->data.sites.clear();
    loaded->data.siteManagerOrder.clear();

    RequireSaved(repository, loaded->data);

    FakeCredentialStore store{state};

    const std::vector active{previousSites.front().authentication};
    const auto deferred = platform::EraseReleasedCredentials(
        loaded->data.pendingCredentialDeletions, loaded->data.sites, active, store);

    CHECK(deferred.pendingIds == loaded->data.pendingCredentialDeletions);
    CHECK(store.EraseCalls().empty());

    const auto unavailable = platform::EraseReleasedCredentials(
        deferred.pendingIds, loaded->data.sites, {}, store);

    CHECK(unavailable.pendingIds == loaded->data.pendingCredentialDeletions);
    REQUIRE(unavailable.errors.size() == 2);
  }

  state.eraseFailures.clear();
  {
    config::CsonConfigRepository restartedRepository{file};

    auto loaded = restartedRepository.Load();

    REQUIRE(loaded);
    REQUIRE(loaded->data.sites.empty());
    REQUIRE(loaded->data.pendingCredentialDeletions ==
            std::vector<std::string>{"password", "passphrase"});

    FakeCredentialStore restartedStore{state};

    const auto cleaned = platform::EraseReleasedCredentials(
        loaded->data.pendingCredentialDeletions, loaded->data.sites, {}, restartedStore);

    CHECK(cleaned.errors.empty());
    REQUIRE(cleaned.pendingIds.empty());
    CHECK(restartedStore.EraseCalls() == loaded->data.pendingCredentialDeletions);

    loaded->data.pendingCredentialDeletions = cleaned.pendingIds;

    RequireSaved(restartedRepository, loaded->data);
  }

  config::CsonConfigRepository verification{file};

  const auto verified = verification.Load();

  REQUIRE(verified);
  CHECK(verified->data.pendingCredentialDeletions.empty());
  CHECK(state.identifiers.empty());
}

TEST_CASE("Failed marker saves retain durable cleanup work for idempotent replay",
          "[platform][credentials][cleanup][config]")
{
  config::test::TempDirectory directory;

  const auto file = directory.Path() / "havRemote.cson";

  CredentialStoreState state{.identifiers = {"password", "passphrase"}, .eraseFailures = {}};
  {
    config::CsonConfigRepository repository{file};

    auto loaded = repository.Load();

    REQUIRE(loaded);

    loaded->data.pendingCredentialDeletions = {"password", "passphrase"};

    RequireSaved(repository, loaded->data);

    const auto durableConfig = config::test::ReadText(file);

    FakeCredentialStore store{state};

    const auto cleaned = platform::EraseReleasedCredentials(
        loaded->data.pendingCredentialDeletions, loaded->data.sites, {}, store);

    REQUIRE(cleaned.pendingIds.empty());
    REQUIRE(cleaned.errors.empty());

    auto rejected = loaded->data;
    rejected.pendingCredentialDeletions = cleaned.pendingIds;
    rejected.settings.transferConcurrency = 0;

    const auto rejectedSave = repository.Save(rejected);

    INFO((rejectedSave.has_value() ? std::string{} : rejectedSave.error().message));
    REQUIRE_FALSE(rejectedSave);
    CHECK(rejectedSave.error().kind == config::ConfigErrorKind::Validation);
    CHECK(config::test::ReadText(file) == durableConfig);
  }

  config::CsonConfigRepository restartedRepository{file};

  auto loaded = restartedRepository.Load();

  REQUIRE(loaded);
  REQUIRE(loaded->data.pendingCredentialDeletions ==
          std::vector<std::string>{"password", "passphrase"});

  FakeCredentialStore restartedStore{state};

  const auto replayed = platform::EraseReleasedCredentials(
      loaded->data.pendingCredentialDeletions, loaded->data.sites, {}, restartedStore);

  CHECK(replayed.errors.empty());
  REQUIRE(replayed.pendingIds.empty());
  CHECK(restartedStore.EraseCalls() == loaded->data.pendingCredentialDeletions);

  loaded->data.pendingCredentialDeletions = replayed.pendingIds;

  RequireSaved(restartedRepository, loaded->data);
}

TEST_CASE("Failed site-removal saves do not release the saved credentials",
          "[platform][credentials][cleanup][config]")
{
  config::test::TempDirectory directory;

  const auto file = directory.Path() / "havRemote.cson";

  config::CsonConfigRepository repository{file};

  auto loaded = repository.Load();

  REQUIRE(loaded);

  loaded->data.sites = {Site("saved", "password", "passphrase")};
  loaded->data.siteManagerOrder = {loaded->data.sites.front().id};

  RequireSaved(repository, loaded->data);

  CredentialStoreState state{.identifiers = {"password", "passphrase"}, .eraseFailures = {}};

  FakeCredentialStore store{state};

  auto candidate = loaded->data;
  candidate.sites.clear();
  candidate.siteManagerOrder.clear();
  candidate.pendingCredentialDeletions = platform::QueueRemovedCredentials(
      loaded->data.pendingCredentialDeletions, loaded->data.sites, candidate.sites);

  const auto concurrency = candidate.settings.transferConcurrency;
  candidate.settings.transferConcurrency = 0;

  const auto saved = repository.Save(candidate);

  INFO((saved.has_value() ? std::string{} : saved.error().message));

  if (saved)
  {
    (void)platform::EraseReleasedCredentials(
        candidate.pendingCredentialDeletions, candidate.sites, {}, store);
  }

  REQUIRE_FALSE(saved);
  CHECK(saved.error().kind == config::ConfigErrorKind::Validation);
  CHECK(store.EraseCalls().empty());

  config::CsonConfigRepository verification{file};

  const auto verified = verification.Load();

  REQUIRE(verified);
  REQUIRE(verified->data.sites.size() == 1);
  CHECK(verified->data.sites.front().authentication.credentialId == "password");
  CHECK(verified->data.sites.front().authentication.passphraseCredentialId == "passphrase");
  CHECK(verified->data.pendingCredentialDeletions.empty());

  candidate.settings.transferConcurrency = concurrency;

  RequireSaved(repository, candidate);

  const auto cleaned = platform::EraseReleasedCredentials(
      candidate.pendingCredentialDeletions, candidate.sites, {}, store);

  CHECK(cleaned.pendingIds.empty());
  CHECK(cleaned.errors.empty());
  CHECK(store.EraseCalls() == candidate.pendingCredentialDeletions);
}
