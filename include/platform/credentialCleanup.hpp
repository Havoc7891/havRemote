// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_PLATFORM_CREDENTIAL_CLEANUP_HPP
#define HAVREMOTE_INCLUDE_PLATFORM_CREDENTIAL_CLEANUP_HPP

#include "core/types.hpp"
#include "platform/credentialStore.hpp"

#include <span>
#include <string>
#include <vector>

namespace havremote::platform
{
  // Keep existing markers and append credentials no longer referenced by saved
  // sites. Persist this result with the site changes before attempting erasure.
  // Active connections defer erasure but must not prevent durable queuing.
  [[nodiscard]] std::vector<std::string> QueueRemovedCredentials(
      std::span<const std::string> pendingIds,
      std::span<const SiteProfile> previousSites,
      std::span<const SiteProfile> currentSites);

  struct CredentialCleanupResult final
  {
    std::vector<std::string> pendingIds;
    std::vector<PlatformError> errors;
  };

  // Attempt each distinct, nonempty pending identifier once. References and
  // failures remain pending in their original order. Already absent credentials
  // are complete. The caller persists the remaining markers after cleanup.
  [[nodiscard]] CredentialCleanupResult EraseReleasedCredentials(
      std::span<const std::string> pendingIds,
      std::span<const SiteProfile> sites,
      std::span<const Authentication> activeAuthentications,
      ICredentialStore &store);
} // namespace havremote::platform

#endif // HAVREMOTE_INCLUDE_PLATFORM_CREDENTIAL_CLEANUP_HPP
