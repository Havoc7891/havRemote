// SPDX-License-Identifier: MIT

#include "ui/connectionProfileModel.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace havremote::ui
{
  ConnectionProfileResolution ResolveConnectionProfile(
      const std::span<const SiteProfile> savedSites,
      const std::string_view selectedSavedSiteId,
      const SiteEndpointIdentity &requestedEndpoint,
      std::string runtimeSiteId,
      std::filesystem::path quickInitialLocalDirectory)
  {
    const auto saved = std::ranges::find(savedSites, selectedSavedSiteId, &SiteProfile::id);
    if (saved != savedSites.end() &&
        EndpointIdentity(*saved) == requestedEndpoint)
    {
      return ConnectionProfileResolution{.site = *saved,
                                         .savedSite = true};
    }

    SiteProfile quick;
    quick.id = std::move(runtimeSiteId);
    quick.name = requestedEndpoint.host;
    quick.protocol = requestedEndpoint.protocol;
    quick.host = requestedEndpoint.host;
    quick.port = requestedEndpoint.port;
    quick.username = requestedEndpoint.username;
    quick.authentication.kind = AuthenticationKind::Password;
    quick.initialRemoteDirectory = RemotePath::Root();
    quick.initialLocalDirectory =
        std::move(quickInitialLocalDirectory);

    return ConnectionProfileResolution{.site = std::move(quick),
                                       .savedSite = false};
  }

  bool ConnectionTabMatchesSavedSite(
      const std::string_view tabSiteId,
      const SiteEndpointIdentity &tabEndpoint,
      const SiteProfile &savedSite)
  {
    return tabSiteId == savedSite.id &&
           tabEndpoint == EndpointIdentity(savedSite);
  }

  bool WorkspaceConnectionMatchesQueuedSite(
      const config::WorkspaceConnectionIdentity &workspaceConnection,
      const std::string_view queuedSiteId,
      const SiteEndpointIdentity &queuedEndpoint,
      const std::span<const SiteProfile> savedSites)
  {
    return std::visit(
        [&](const auto &identity)
        {
          using Identity = std::decay_t<decltype(identity)>;

          if constexpr (std::is_same_v<
                            Identity,
                            config::SavedSiteWorkspaceIdentity>)
          {
            const auto saved = std::ranges::find(savedSites, identity.siteId, &SiteProfile::id);

            return identity.siteId == queuedSiteId &&
                   saved != savedSites.end() &&
                   queuedEndpoint == EndpointIdentity(*saved);
          }
          else
          {
            return identity.endpoint == queuedEndpoint;
          }
        },
        workspaceConnection);
  }

  std::string_view CredentialIdForRequest(
      const Authentication &authentication,
      const CredentialKind requestKind) noexcept
  {
    switch (requestKind)
    {
    case CredentialKind::Password:
      return authentication.credentialId;

    case CredentialKind::PrivateKeyPassphrase:
      return authentication.passphraseCredentialId;

    case CredentialKind::KeyboardInteractive:
      return {};
    }

    return {};
  }
} // namespace havremote::ui
