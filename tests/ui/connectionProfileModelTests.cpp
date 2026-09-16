// SPDX-License-Identifier: MIT

#include "ui/connectionProfileModel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace havremote;
using namespace havremote::ui;

namespace
{
  SiteProfile PrivateKeySite()
  {
    SiteProfile site;
    site.id = "saved-key-site";
    site.name = "Production";
    site.protocol = ProtocolKind::Sftp;
    site.host = "sftp.example";
    site.port = 2222;
    site.username = "alice";
    site.authentication.kind = AuthenticationKind::PrivateKey;
    site.authentication.credentialId = "unused-password-reference";
    site.authentication.privateKeyFile = "C:/Keys/id_ed25519";
    site.authentication.publicKeyFile = "C:/Keys/id_ed25519.pub";
    site.authentication.passphraseCredentialId =
        "saved-key-passphrase";
    site.initialRemoteDirectory = RemotePath{"/srv/files"};
    site.initialLocalDirectory = "D:/Downloads/Production";
    site.ftpEncoding = "ISO-8859-1";

    return site;
  }

  void CheckPasswordOnlyQuickProfile(
      const ConnectionProfileResolution &resolution,
      const SiteEndpointIdentity &endpoint,
      const std::filesystem::path &localDirectory)
  {
    CHECK_FALSE(resolution.savedSite);
    CHECK(resolution.site.id == "runtime-site");
    CHECK(resolution.site.name == endpoint.host);
    CHECK(EndpointIdentity(resolution.site) == endpoint);
    CHECK(resolution.site.authentication.kind ==
          AuthenticationKind::Password);
    CHECK(resolution.site.authentication.credentialId.empty());
    CHECK(resolution.site.authentication.privateKeyFile.empty());
    CHECK(resolution.site.authentication.publicKeyFile.empty());
    CHECK(resolution.site.authentication.passphraseCredentialId.empty());
    CHECK(resolution.site.initialRemoteDirectory == RemotePath::Root());
    CHECK(resolution.site.initialLocalDirectory == localDirectory);
    CHECK(resolution.site.ftpDataConnectionMode ==
          FtpDataConnectionMode::Passive);
    CHECK(resolution.site.ftpActiveAddress.empty());
  }
} // namespace

TEST_CASE("saved private-key connection profiles retain complete authentication metadata")
{
  const auto saved = PrivateKeySite();

  const std::vector sites{saved};

  auto requested = EndpointIdentity(saved);
  requested.host = "SFTP.EXAMPLE";

  const auto resolution = ResolveConnectionProfile(
      sites,
      saved.id,
      requested,
      "runtime-site",
      "C:/Quick");

  REQUIRE(resolution.savedSite);
  CHECK(resolution.site.id == saved.id);
  CHECK(resolution.site.name == saved.name);
  CHECK(resolution.site.protocol == saved.protocol);
  CHECK(resolution.site.host == saved.host);
  CHECK(resolution.site.port == saved.port);
  CHECK(resolution.site.username == saved.username);
  CHECK(resolution.site.authentication.kind ==
        AuthenticationKind::PrivateKey);
  CHECK(resolution.site.authentication.credentialId ==
        saved.authentication.credentialId);
  CHECK(resolution.site.authentication.privateKeyFile ==
        saved.authentication.privateKeyFile);
  CHECK(resolution.site.authentication.publicKeyFile ==
        saved.authentication.publicKeyFile);
  CHECK(resolution.site.authentication.passphraseCredentialId ==
        saved.authentication.passphraseCredentialId);
  CHECK(resolution.site.initialRemoteDirectory ==
        saved.initialRemoteDirectory);
  CHECK(resolution.site.initialLocalDirectory ==
        saved.initialLocalDirectory);
  CHECK(resolution.site.ftpEncoding == saved.ftpEncoding);
  CHECK(resolution.site.ftpDataConnectionMode ==
        FtpDataConnectionMode::Passive);
}

TEST_CASE("saved active FTP profiles retain their data connection mode")
{
  SiteProfile saved;
  saved.id = "saved-active-ftp";
  saved.name = "Active FTP";
  saved.protocol = ProtocolKind::Ftp;
  saved.host = "ftp.example";
  saved.port = 21;
  saved.username = "alice";
  saved.authentication.kind = AuthenticationKind::Password;
  saved.ftpDataConnectionMode = FtpDataConnectionMode::Active;
  saved.ftpActiveAddress = "198.51.100.42";

  const std::vector sites{saved};

  const auto resolution = ResolveConnectionProfile(
      sites,
      saved.id,
      EndpointIdentity(saved),
      "runtime-site",
      "C:/Quick");

  REQUIRE(resolution.savedSite);
  CHECK(resolution.site.ftpDataConnectionMode ==
        FtpDataConnectionMode::Active);
  CHECK(resolution.site.ftpActiveAddress == "198.51.100.42");
}

TEST_CASE("saved password profiles retain their Credential Manager reference")
{
  auto saved = PrivateKeySite();
  saved.id = "saved-password-site";
  saved.authentication = Authentication{};
  saved.authentication.kind = AuthenticationKind::Password;
  saved.authentication.credentialId = "saved-password-reference";

  const std::vector sites{saved};

  const auto resolution = ResolveConnectionProfile(
      sites,
      saved.id,
      EndpointIdentity(saved),
      "runtime-site",
      "C:/Quick");

  REQUIRE(resolution.savedSite);
  CHECK(resolution.site.authentication.kind ==
        AuthenticationKind::Password);
  CHECK(resolution.site.authentication.credentialId ==
        "saved-password-reference");
}

TEST_CASE("keyboard-interactive answers never use a stored credential reference")
{
  Authentication authentication;
  authentication.credentialId = "saved-password-reference";
  authentication.passphraseCredentialId = "saved-passphrase-reference";

  CHECK(CredentialIdForRequest(authentication, CredentialKind::Password) ==
        "saved-password-reference");
  CHECK(CredentialIdForRequest(authentication,
                               CredentialKind::PrivateKeyPassphrase) ==
        "saved-passphrase-reference");
  CHECK(CredentialIdForRequest(authentication,
                               CredentialKind::KeyboardInteractive)
            .empty());
}

TEST_CASE("an edited saved-site endpoint becomes a credential-free Quick Connect profile")
{
  const auto saved = PrivateKeySite();

  const std::vector sites{saved};

  auto requested = EndpointIdentity(saved);
  requested.port = 22;

  const std::filesystem::path localDirectory{"C:/Quick"};

  const auto resolution = ResolveConnectionProfile(
      sites,
      saved.id,
      requested,
      "runtime-site",
      localDirectory);

  CheckPasswordOnlyQuickProfile(resolution, requested, localDirectory);
}

TEST_CASE("an unknown saved-site id cannot inherit credentials from a matching endpoint")
{
  const auto saved = PrivateKeySite();

  const std::vector sites{saved};

  const auto requested = EndpointIdentity(saved);

  const std::filesystem::path localDirectory{"C:/Quick"};

  const auto resolution = ResolveConnectionProfile(
      sites,
      "missing-site",
      requested,
      "runtime-site",
      localDirectory);

  CheckPasswordOnlyQuickProfile(resolution, requested, localDirectory);
}

TEST_CASE("a connection tab matches its saved site by stable id and endpoint")
{
  const auto saved = PrivateKeySite();

  auto endpoint = EndpointIdentity(saved);
  endpoint.host = "SFTP.EXAMPLE";

  CHECK(ConnectionTabMatchesSavedSite(
      saved.id,
      endpoint,
      saved));
}

TEST_CASE("a connection tab cannot match a different saved-site id")
{
  const auto saved = PrivateKeySite();

  CHECK_FALSE(ConnectionTabMatchesSavedSite(
      "different-active-site",
      EndpointIdentity(saved),
      saved));
}

TEST_CASE("an old active endpoint does not match a saved site edited later")
{
  auto saved = PrivateKeySite();

  const auto oldEndpoint = EndpointIdentity(saved);

  saved.host = "replacement.example";
  saved.port = 22;

  CHECK_FALSE(ConnectionTabMatchesSavedSite(
      saved.id,
      oldEndpoint,
      saved));
}

TEST_CASE("an edited disconnected draft no longer represents its saved site")
{
  const auto saved = PrivateKeySite();

  auto draftEndpoint = EndpointIdentity(saved);
  draftEndpoint.username = "bob";

  CHECK_FALSE(ConnectionTabMatchesSavedSite(
      saved.id,
      draftEndpoint,
      saved));
  CHECK_FALSE(ConnectionTabMatchesSavedSite(
      "different-draft-site",
      EndpointIdentity(saved),
      saved));
}

TEST_CASE("workspace tabs only absorb compatible persistent queue groups")
{
  const auto saved = PrivateKeySite();

  const std::vector sites{saved};

  const config::WorkspaceConnectionIdentity savedIdentity =
      config::SavedSiteWorkspaceIdentity{saved.id};

  CHECK(WorkspaceConnectionMatchesQueuedSite(
      savedIdentity, saved.id, EndpointIdentity(saved), sites));

  auto differentlyCasedEndpoint = EndpointIdentity(saved);
  differentlyCasedEndpoint.host = "SFTP.EXAMPLE";

  CHECK(WorkspaceConnectionMatchesQueuedSite(
      savedIdentity, saved.id, differentlyCasedEndpoint, sites));
  CHECK_FALSE(WorkspaceConnectionMatchesQueuedSite(
      savedIdentity, "old-runtime-id", EndpointIdentity(saved), sites));

  auto oldEndpoint = EndpointIdentity(saved);
  oldEndpoint.host = "old.example";

  CHECK_FALSE(WorkspaceConnectionMatchesQueuedSite(
      savedIdentity, saved.id, oldEndpoint, sites));

  const config::WorkspaceConnectionIdentity quickIdentity =
      config::QuickConnectWorkspaceIdentity{oldEndpoint};

  CHECK(WorkspaceConnectionMatchesQueuedSite(
      quickIdentity, "runtime-site", oldEndpoint, sites));

  oldEndpoint.host = "OLD.EXAMPLE";

  CHECK(WorkspaceConnectionMatchesQueuedSite(
      quickIdentity, "runtime-site", oldEndpoint, sites));

  oldEndpoint.port = 22;

  CHECK_FALSE(WorkspaceConnectionMatchesQueuedSite(
      quickIdentity, "runtime-site", oldEndpoint, sites));
}
