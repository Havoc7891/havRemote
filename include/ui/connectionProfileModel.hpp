// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UI_CONNECTION_PROFILE_MODEL_HPP
#define HAVREMOTE_INCLUDE_UI_CONNECTION_PROFILE_MODEL_HPP

#include "config/configTypes.hpp"
#include "core/types.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace havremote::ui
{
  struct ConnectionProfileResolution final
  {
    SiteProfile site;
    bool savedSite{};
  };

  // A saved profile is selected only by its stable ID and an unchanged
  // endpoint. The complete profile is copied so protocol-specific
  // authentication and Credential Manager references survive the launch.
  // Every other request is an independent password-only Quick Connect
  // profile and cannot inherit authentication metadata from a saved site.
  [[nodiscard]] ConnectionProfileResolution ResolveConnectionProfile(
      std::span<const SiteProfile> savedSites,
      std::string_view selectedSavedSiteId,
      const SiteEndpointIdentity &requestedEndpoint,
      std::string runtimeSiteId,
      std::filesystem::path quickInitialLocalDirectory);

  // A tab represents a saved site only while both its stable ID and endpoint
  // match. This keeps cloned profiles distinct and prevents an old active
  // endpoint from being mistaken for a saved site that was edited later.
  [[nodiscard]] bool ConnectionTabMatchesSavedSite(
      std::string_view tabSiteId,
      const SiteEndpointIdentity &tabEndpoint,
      const SiteProfile &savedSite);

  // A queue group may reuse a restored workspace tab only when connecting
  // that tab can claim the group's immutable site and endpoint identity.
  // Otherwise the queue needs its own recovery tab so an edited saved site or
  // a recycled connection ID cannot silently retarget queued work.
  [[nodiscard]] bool WorkspaceConnectionMatchesQueuedSite(
      const config::WorkspaceConnectionIdentity &workspaceConnection,
      std::string_view queuedSiteId,
      const SiteEndpointIdentity &queuedEndpoint,
      std::span<const SiteProfile> savedSites);

  // Only reusable secrets have Credential Manager identifiers. In particular,
  // keyboard-interactive answers may be one-time codes and must always remain
  // transient even when a malformed profile contains a password reference.
  [[nodiscard]] std::string_view CredentialIdForRequest(
      const Authentication &authentication,
      CredentialKind requestKind) noexcept;
} // namespace havremote::ui

#endif // HAVREMOTE_INCLUDE_UI_CONNECTION_PROFILE_MODEL_HPP
