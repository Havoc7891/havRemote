// SPDX-License-Identifier: MIT

#include "platform/credentialStore.hpp"
#include "platform/credentialStoreInternal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <wx/log.h>

#include <algorithm>
#include <array>
#include <string>
#include <type_traits>
#include <thread>
#include <utility>

using namespace havremote;

namespace
{
  class FakeSecretStoreBackend final : public platform::detail::ISecretStoreBackend
  {
  public:
    bool available{true};
    bool operationSucceeds{true};
    bool reportError{};
    mutable unsigned availabilityChecks{};
    mutable unsigned operationCalls{};
    mutable wxString serviceName;
    wxString savedUsername;
    std::vector<std::byte> savedSecret;

    bool IsOk(wxString &reason) const override
    {
      ++availabilityChecks;

      if (!available)
      {
        reason = "No desktop secret service is running";
      }

      return available;
    }

    bool Save(const wxString &service,
              const wxString &username,
              const wxSecretValue &secret) override
    {
      if (!Operation(service))
      {
        return false;
      }

      savedUsername = username;

      if (secret.GetSize() != 0U)
      {
        const auto *bytes = static_cast<const std::byte *>(secret.GetData());

        savedSecret.assign(bytes, bytes + secret.GetSize());
      }

      return true;
    }

    bool Load(const wxString &service,
              wxString &username,
              wxSecretValue &secret) const override
    {
      if (!Operation(service))
      {
        return false;
      }

      username = savedUsername;

      const std::byte empty{};

      secret = wxSecretValue{savedSecret.size(), savedSecret.empty() ? &empty : savedSecret.data()};

      return true;
    }

    bool Delete(const wxString &service) override { return Operation(service); }

  private:
    bool Operation(const wxString &service) const
    {
      ++operationCalls;

      serviceName = service;

      if (reportError)
      {
        wxLogError("The secure store denied access");
      }

      return operationSucceeds;
    }
  };

  struct CredentialFixture final
  {
    FakeSecretStoreBackend *backend{};
    std::unique_ptr<platform::ICredentialStore> store;

    CredentialFixture()
    {
      auto owned = std::make_unique<FakeSecretStoreBackend>();

      backend = owned.get();

      store = platform::detail::MakeCredentialStoreWithBackend(std::move(owned));
    }
  };
}

TEST_CASE("Credential payloads preserve binary data across moves",
          "[platform][credentials]")
{
  STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<platform::CredentialPayload>);
  STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<platform::CredentialPayload>);

  const std::vector bytes{std::byte{0}, std::byte{0xff}, std::byte{42}};

  platform::CredentialPayload source{"user", bytes};

  platform::CredentialPayload moved{std::move(source)};

  CHECK(source.Username().empty());
  CHECK(source.Secret().empty());
  CHECK(moved.Username() == "user");
  CHECK(std::ranges::equal(moved.Secret(), bytes));

  platform::CredentialPayload destination{"previous", {std::byte{12}}};

  destination = std::move(moved);

  CHECK(moved.Username().empty());
  CHECK(moved.Secret().empty());
  CHECK(destination.Username() == "user");
  CHECK(std::ranges::equal(destination.Secret(), bytes));

  auto &same = destination;

  destination = std::move(same);

  CHECK(destination.Username() == "user");
  CHECK(std::ranges::equal(destination.Secret(), bytes));
}

TEST_CASE("Credential identifiers are validated before accessing the OS store",
          "[platform][credentials]")
{
  CredentialFixture fixture;

  const std::array invalid{
      std::string{}, std::string{"bad\nkey"}, std::string{"bad\rkey"},
      std::string{"prefix\0suffix", 13}, std::string{"\xc0\xaf"},
      std::string{"\xed\xa0\x80"}, std::string{"\xf4\x90\x80\x80"}};

  for (const auto &identifier : invalid)
  {
    const auto loaded = fixture.store->Load(identifier);
    const auto saved = fixture.store->Store(identifier, "user", {});
    const auto erased = fixture.store->Erase(identifier);

    REQUIRE_FALSE(loaded);
    REQUIRE_FALSE(saved);
    REQUIRE_FALSE(erased);
    CHECK(loaded.error().code == platform::PlatformErrorCode::InvalidArgument);
    CHECK(saved.error().code == platform::PlatformErrorCode::InvalidArgument);
    CHECK(erased.error().code == platform::PlatformErrorCode::InvalidArgument);
  }

  CHECK(fixture.backend->availabilityChecks == 0);
  CHECK(fixture.backend->operationCalls == 0);
}

TEST_CASE("Credential usernames cannot silently truncate or change encoding",
          "[platform][credentials]")
{
  CredentialFixture fixture;

  for (const auto &username : {std::string{}, std::string{"a\0b", 3},
                               std::string{"\xff"}})
  {
    const auto result = fixture.store->Store("site-id", username, {});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == platform::PlatformErrorCode::InvalidArgument);
  }

  CHECK(fixture.backend->availabilityChecks == 0);
}

TEST_CASE("An unavailable credential store never accepts or forgets secrets",
          "[platform][credentials]")
{
  CredentialFixture fixture;
  fixture.backend->available = false;

  const auto loaded = fixture.store->Load("site-id");
  const auto saved = fixture.store->Store("site-id", "user", {});
  const auto erased = fixture.store->Erase("site-id");

  REQUIRE_FALSE(loaded);
  REQUIRE_FALSE(saved);
  REQUIRE_FALSE(erased);

  for (const auto *error : {&loaded.error(), &saved.error(), &erased.error()})
  {
    CHECK(error->code == platform::PlatformErrorCode::Unavailable);
    CHECK(error->message.find("No desktop secret service") != std::string::npos);
  }

  CHECK(fixture.backend->operationCalls == 0);

  fixture.backend->available = true;

  REQUIRE(fixture.store->Erase("site-id"));
  CHECK(fixture.backend->operationCalls == 1);
}

TEST_CASE("Missing credentials are distinct from secure-store failures",
          "[platform][credentials]")
{
  CredentialFixture fixture;
  fixture.backend->operationSucceeds = false;

  const auto missing = fixture.store->Load("site-id");

  REQUIRE_FALSE(missing);
  CHECK(missing.error().code == platform::PlatformErrorCode::NotFound);
  CHECK(fixture.store->Erase("site-id"));

  fixture.backend->reportError = true;

  wxLogCollector outerLog;
  auto *const originalTarget = wxLog::GetActiveTarget();

  const auto failedLoad = fixture.store->Load("site-id");
  const auto failedErase = fixture.store->Erase("site-id");

  REQUIRE_FALSE(failedLoad);
  REQUIRE_FALSE(failedErase);
  CHECK(failedLoad.error().code == platform::PlatformErrorCode::OperatingSystem);
  CHECK(failedErase.error().code == platform::PlatformErrorCode::OperatingSystem);
  CHECK(failedLoad.error().message.find("denied access") != std::string::npos);
  CHECK(failedErase.error().message.find("denied access") != std::string::npos);
  CHECK(outerLog.GetMessages().empty());
  CHECK(wxLog::GetActiveTarget() == originalTarget);
}

TEST_CASE("Concurrent credential failures stay scoped to their calling threads",
          "[platform][credentials]")
{
  CredentialFixture fixture;
  fixture.backend->operationSucceeds = false;
  fixture.backend->reportError = true;

  std::array<bool, 4> classified{};

  std::array<std::jthread, 4> workers;

  for (std::size_t index = 0; index < workers.size(); ++index)
  {
    workers[index] = std::jthread([&, index]
                                  {
      const auto result = fixture.store->Load("site-id");

      classified[index] = !result &&
          result.error().code == platform::PlatformErrorCode::OperatingSystem &&
          result.error().message.find("denied access") != std::string::npos; });
  }

  for (auto &worker : workers)
  {
    worker.join();
  }

  CHECK(std::ranges::all_of(classified, [](const bool value)
                            { return value; }));
  CHECK(fixture.backend->operationCalls == workers.size());
}

#if wxUSE_SECRETSTORE
TEST_CASE("Secure-store adapter retains existing names and binary secrets",
          "[platform][credentials]")
{
  CredentialFixture fixture;

  const std::array secret{std::byte{0}, std::byte{0xff}, std::byte{'x'}};
  const auto stored = fixture.store->Store("site-id-password", "J\xc3\xb6rg", secret);

  REQUIRE(stored);
  CHECK(fixture.backend->serviceName == "havRemote/site-id-password");
  CHECK(fixture.backend->savedUsername == wxString::FromUTF8("J\xc3\xb6rg"));
  CHECK(std::ranges::equal(fixture.backend->savedSecret, secret));

  const auto loaded = fixture.store->Load("site-id-password");

  REQUIRE(loaded);
  CHECK(loaded->Username() == "J\xc3\xb6rg");
  CHECK(std::ranges::equal(loaded->Secret(), secret));
  REQUIRE(fixture.store->Erase("site-id-password"));
  CHECK(fixture.backend->serviceName == "havRemote/site-id-password");
}

TEST_CASE("Failed credential saves do not leak a second wx error message",
          "[platform][credentials]")
{
  CredentialFixture fixture;
  fixture.backend->operationSucceeds = false;
  fixture.backend->reportError = true;

  wxLogCollector outerLog;

  const auto saved = fixture.store->Store("site-id", "user", {});

  REQUIRE_FALSE(saved);
  CHECK(saved.error().code == platform::PlatformErrorCode::OperatingSystem);
  CHECK(saved.error().message.find("denied access") != std::string::npos);
  CHECK(outerLog.GetMessages().empty());
}

TEST_CASE("An empty saved password is different from a missing credential",
          "[platform][credentials]")
{
  CredentialFixture fixture;

  REQUIRE(fixture.store->Store("site-id", "user", {}));

  const auto loaded = fixture.store->Load("site-id");

  REQUIRE(loaded);
  CHECK(loaded->Secret().empty());
}
#else
TEST_CASE("Builds without secure-store support report it without a fallback",
          "[platform][credentials]")
{
  const auto store = platform::MakeCredentialStore();
  const auto result = store->Load("site-id");

  REQUIRE_FALSE(result);
  CHECK(result.error().code == platform::PlatformErrorCode::Unavailable);
  CHECK(result.error().message.find("not included in this build") != std::string::npos);
}
#endif
