// SPDX-License-Identifier: MIT

#include "platform/credentialCleanup.hpp"

#include <string_view>
#include <unordered_set>

namespace havremote::platform
{
  namespace
  {
    using CredentialReferences = std::unordered_set<std::string_view>;

    void AddReferences(CredentialReferences &references,
                       const Authentication &authentication)
    {
      if (!authentication.credentialId.empty())
      {
        references.insert(authentication.credentialId);
      }

      if (!authentication.passphraseCredentialId.empty())
      {
        references.insert(authentication.passphraseCredentialId);
      }
    }

    CredentialReferences SavedReferences(const std::span<const SiteProfile> sites)
    {
      CredentialReferences references;

      for (const auto &site : sites)
      {
        AddReferences(references, site.authentication);
      }

      return references;
    }
  } // namespace

  std::vector<std::string> QueueRemovedCredentials(
      const std::span<const std::string> pendingIds,
      const std::span<const SiteProfile> previousSites,
      const std::span<const SiteProfile> currentSites)
  {
    const auto savedReferences = SavedReferences(currentSites);

    CredentialReferences queued;
    std::vector<std::string> result;

    const auto append = [&](const std::string &id)
    {
      if (!id.empty() && queued.insert(id).second)
      {
        result.push_back(id);
      }
    };

    for (const auto &id : pendingIds)
    {
      append(id);
    }

    for (const auto &site : previousSites)
    {
      const auto &authentication = site.authentication;

      if (!savedReferences.contains(authentication.credentialId))
      {
        append(authentication.credentialId);
      }

      if (!savedReferences.contains(authentication.passphraseCredentialId))
      {
        append(authentication.passphraseCredentialId);
      }
    }

    return result;
  }

  CredentialCleanupResult EraseReleasedCredentials(
      const std::span<const std::string> pendingIds,
      const std::span<const SiteProfile> sites,
      const std::span<const Authentication> activeAuthentications,
      ICredentialStore &store)
  {
    auto references = SavedReferences(sites);

    for (const auto &authentication : activeAuthentications)
    {
      AddReferences(references, authentication);
    }

    CredentialReferences visited;
    CredentialCleanupResult result;

    for (const auto &id : pendingIds)
    {
      if (id.empty() || !visited.insert(id).second)
      {
        continue;
      }

      if (references.contains(id))
      {
        result.pendingIds.push_back(id);

        continue;
      }

      const auto erased = store.Erase(id);
      if (!erased && erased.error().code != PlatformErrorCode::NotFound)
      {
        result.pendingIds.push_back(id);
        result.errors.push_back(erased.error());
      }
    }

    return result;
  }
} // namespace havremote::platform
