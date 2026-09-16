// SPDX-License-Identifier: MIT

#include "ui/siteManagerModel.hpp"

#include <algorithm>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace havremote::ui
{
  Authentication PrepareCredentialIdentifiers(const Authentication &current,
                                              const bool updatePassword,
                                              const bool updatePassphrase)
  {
    auto prepared = current;

    if (updatePassword || updatePassphrase)
    {
      const auto revision = GenerateId();

      if (updatePassword)
      {
        prepared.credentialId = "credential-" + revision + "-password";
      }

      if (updatePassphrase)
      {
        prepared.passphraseCredentialId = "credential-" + revision + "-passphrase";
      }
    }

    return prepared;
  }

  std::optional<std::size_t> DuplicateSiteNameIndex(
      const std::span<const SiteProfile> sites,
      const std::span<const config::SiteFolder> folders)
  {
    for (std::size_t right = 1U; right < sites.size(); ++right)
    {
      for (std::size_t left = 0U; left < right; ++left)
      {
        if (sites[left].name == sites[right].name &&
            SiteFolderId(folders, sites[left].id) ==
                SiteFolderId(folders, sites[right].id))
        {
          return right;
        }
      }
    }

    return std::nullopt;
  }

  std::string SiteFolderId(
      const std::span<const config::SiteFolder> folders,
      const std::string_view siteId)
  {
    for (const auto &folder : folders)
    {
      if (std::ranges::find(folder.siteIds, siteId) != folder.siteIds.end())
      {
        return folder.id;
      }
    }

    return {};
  }

  void AssignSiteToFolder(std::vector<config::SiteFolder> &folders,
                          const std::string_view siteId,
                          const std::string_view folderId)
  {
    // Callers commonly pass IDs stored inside folders. Copy them before any
    // erase operation can invalidate the source strings.
    const std::string siteIdCopy{siteId};
    const std::string folderIdCopy{folderId};

    if (!folderIdCopy.empty() &&
        std::ranges::find(folders, folderIdCopy,
                          &config::SiteFolder::id) == folders.end())
    {
      return;
    }

    for (auto &folder : folders)
    {
      std::erase(folder.siteIds, siteIdCopy);
    }

    if (folderIdCopy.empty())
    {
      return;
    }

    const auto destination = std::ranges::find(folders, folderIdCopy,
                                               &config::SiteFolder::id);
    if (destination != folders.end())
    {
      destination->siteIds.push_back(siteIdCopy);
    }
  }

  bool IsFolderDescendantOf(
      const std::span<const config::SiteFolder> folders,
      const std::string_view candidateFolderId,
      const std::string_view ancestorFolderId) noexcept
  {
    if (candidateFolderId.empty() || ancestorFolderId.empty())
    {
      return false;
    }

    const auto candidate = std::ranges::find(
        folders, candidateFolderId, &config::SiteFolder::id);
    if (candidate == folders.end())
    {
      return false;
    }

    std::string_view current = candidate->id;

    for (std::size_t depth = 0; depth <= folders.size(); ++depth)
    {
      if (current == ancestorFolderId)
      {
        return true;
      }

      const auto folder = std::ranges::find(folders, current,
                                            &config::SiteFolder::id);
      if (folder == folders.end() || folder->parentId.empty())
      {
        return false;
      }

      current = folder->parentId;
    }

    return false;
  }

  bool IsFolderNameUnique(
      const std::span<const config::SiteFolder> folders,
      const std::span<const SiteProfile> sites,
      const std::string_view candidate,
      const std::string_view parentFolderId,
      const std::string_view excludedFolderId)
  {
    for (const auto &folder : folders)
    {
      if (folder.id != excludedFolderId && folder.parentId == parentFolderId &&
          folder.name == candidate)
      {
        return false;
      }
    }

    return std::ranges::none_of(
        sites,
        [&](const SiteProfile &site)
        {
          return site.name == candidate &&
                 SiteFolderId(folders, site.id) == parentFolderId;
        });
  }

  bool IsSiteNameUniqueInTree(
      const std::span<const SiteProfile> sites,
      const std::span<const config::SiteFolder> folders,
      const std::string_view candidate,
      const std::string_view parentFolderId,
      const std::string_view excludedSiteId)
  {
    if (std::ranges::any_of(
            sites,
            [&](const SiteProfile &site)
            {
              return (excludedSiteId.empty() || site.id != excludedSiteId) &&
                     site.name == candidate &&
                     SiteFolderId(folders, site.id) == parentFolderId;
            }))
    {
      return false;
    }

    return std::ranges::none_of(
        folders,
        [&](const config::SiteFolder &folder)
        {
          return folder.parentId == parentFolderId && folder.name == candidate;
        });
  }

  std::string NextAvailableFolderName(
      const std::span<const config::SiteFolder> folders,
      const std::span<const SiteProfile> sites,
      const std::string_view parentFolderId,
      const std::string_view base)
  {
    if (IsFolderNameUnique(folders, sites, base, parentFolderId))
    {
      return std::string{base};
    }

    for (std::size_t suffix = 2U;; ++suffix)
    {
      std::string candidate{base};
      candidate += ' ';
      candidate += std::to_string(suffix);

      if (IsFolderNameUnique(folders, sites, candidate, parentFolderId))
      {
        return candidate;
      }
    }
  }

  std::string NextAvailableSiteNameInTree(
      const std::span<const SiteProfile> sites,
      const std::span<const config::SiteFolder> folders,
      const std::string_view parentFolderId,
      const std::string_view base)
  {
    if (IsSiteNameUniqueInTree(sites, folders, base, parentFolderId))
    {
      return std::string{base};
    }

    for (std::size_t suffix = 2U;; ++suffix)
    {
      std::string candidate{base};
      candidate += ' ';
      candidate += std::to_string(suffix);

      if (IsSiteNameUniqueInTree(sites, folders, candidate, parentFolderId))
      {
        return candidate;
      }
    }
  }

  std::vector<std::string> SynthesizeSiteManagerOrder(
      const std::span<const SiteProfile> sites,
      const std::span<const config::SiteFolder> folders)
  {
    std::vector<std::string> result;
    result.reserve(folders.size() + sites.size());

    std::unordered_set<std::string_view> knownSiteIds;
    knownSiteIds.reserve(sites.size());

    for (const auto &site : sites)
    {
      knownSiteIds.insert(site.id);
    }

    std::unordered_set<std::string_view> insertedFolders;
    std::unordered_set<std::string_view> insertedSites;

    insertedFolders.reserve(folders.size());
    insertedSites.reserve(sites.size());

    // Append child folders before direct sites, preserving SiteFolder::siteIds order
    const auto appendFolder = [&](this const auto &self,
                                  const config::SiteFolder &folder) -> void
    {
      if (!insertedFolders.insert(folder.id).second)
      {
        return;
      }

      result.push_back(folder.id);

      for (const auto &child : folders)
      {
        if (child.parentId == folder.id)
        {
          self(child);
        }
      }

      for (const auto &siteId : folder.siteIds)
      {
        if (knownSiteIds.contains(siteId) &&
            insertedSites.insert(siteId).second)
        {
          result.push_back(siteId);
        }
      }
    };

    for (const auto &folder : folders)
    {
      if (folder.parentId.empty())
      {
        appendFolder(folder);
      }
    }

    for (const auto &folder : folders)
    {
      appendFolder(folder);
    }

    for (const auto &site : sites)
    {
      if (insertedSites.insert(site.id).second)
      {
        result.push_back(site.id);
      }
    }

    return result;
  }

  namespace
  {
    std::optional<SiteManagerEntryKind> FindSiteManagerEntryKind(
        const std::span<const SiteProfile> sites,
        const std::span<const config::SiteFolder> folders,
        const std::string_view id)
    {
      const bool isFolder = std::ranges::find(folders, id,
                                              &config::SiteFolder::id) !=
                            folders.end();
      const bool isSite =
          std::ranges::find(sites, id, &SiteProfile::id) != sites.end();
      if (isFolder == isSite)
      {
        return std::nullopt;
      }

      return isFolder ? SiteManagerEntryKind::Folder
                      : SiteManagerEntryKind::Site;
    }

    std::string SiteManagerEntryParent(
        const std::span<const SiteProfile> sites,
        const std::span<const config::SiteFolder> folders,
        const std::string_view id,
        const SiteManagerEntryKind kind)
    {
      if (kind == SiteManagerEntryKind::Folder)
      {
        const auto folder = std::ranges::find(folders, id,
                                              &config::SiteFolder::id);
        return folder == folders.end() ? std::string{} : folder->parentId;
      }

      (void)sites;

      return SiteFolderId(folders, id);
    }

    bool IsValidSiteManagerOrder(
        const std::span<const SiteProfile> sites,
        const std::span<const config::SiteFolder> folders,
        const std::span<const std::string> order)
    {
      std::unordered_set<std::string_view> known;
      known.reserve(sites.size() + folders.size());

      for (const auto &folder : folders)
      {
        if (folder.id.empty() || !known.insert(folder.id).second)
        {
          return false;
        }
      }

      for (const auto &site : sites)
      {
        if (site.id.empty() || !known.insert(site.id).second)
        {
          return false;
        }
      }

      if (order.size() != known.size())
      {
        return false;
      }

      std::unordered_set<std::string_view> ordered;
      ordered.reserve(order.size());

      for (const auto &id : order)
      {
        if (!known.contains(id) || !ordered.insert(id).second)
        {
          return false;
        }
      }

      return true;
    }

    bool IsDirectSiteManagerChild(
        const std::span<const SiteProfile> sites,
        const std::span<const config::SiteFolder> folders,
        const std::string_view id,
        const std::string_view parentFolderId)
    {
      const auto kind = FindSiteManagerEntryKind(sites, folders, id);
      return kind && SiteManagerEntryParent(sites, folders, id, *kind) ==
                         parentFolderId;
    }

    bool ValidImportTree(const config::SiteTransferData &tree)
    {
      if (!IsValidSiteManagerOrder(tree.sites, tree.folders,
                                   tree.siteManagerOrder))
      {
        return false;
      }

      std::unordered_set<std::string_view> siteIds;
      std::unordered_map<std::string_view, const config::SiteFolder *> folders;
      std::unordered_map<std::string_view, std::string_view> siteParents;
      std::unordered_map<std::string_view, std::unordered_set<std::string_view>> names;

      for (const auto &site : tree.sites)
      {
        siteIds.insert(site.id);
      }

      for (const auto &folder : tree.folders)
      {
        folders.emplace(folder.id, &folder);
      }

      for (const auto &folder : tree.folders)
      {
        if (folder.name.empty() ||
            (!folder.parentId.empty() && !folders.contains(folder.parentId)) ||
            !names[folder.parentId].insert(folder.name).second)
        {
          return false;
        }

        std::string_view ancestor{folder.id};

        for (std::size_t depth = 0; !ancestor.empty(); ++depth)
        {
          if (depth >= tree.folders.size())
          {
            return false;
          }

          const auto parent = folders.find(ancestor);
          if (parent == folders.end())
          {
            return false;
          }

          ancestor = parent->second->parentId;
        }

        for (const auto &id : folder.siteIds)
        {
          if (!siteIds.contains(id) || !siteParents.emplace(id, folder.id).second)
          {
            return false;
          }
        }
      }

      for (const auto &site : tree.sites)
      {
        const auto parent = siteParents.find(site.id);
        const auto parentId = parent == siteParents.end() ? std::string_view{}
                                                          : parent->second;

        if (site.name.empty() || !names[parentId].insert(site.name).second)
        {
          return false;
        }
      }

      return true;
    }
  } // namespace

  std::vector<SiteManagerEntry> OrderedSiteManagerChildren(
      const std::span<const SiteProfile> sites,
      const std::span<const config::SiteFolder> folders,
      const std::span<const std::string> siteManagerOrder,
      const std::string_view parentFolderId)
  {
    std::vector<SiteManagerEntry> result;

    std::unordered_set<std::string> inserted;
    inserted.reserve(sites.size() + folders.size());

    const auto append = [&](const std::string_view id)
    {
      const auto kind = FindSiteManagerEntryKind(sites, folders, id);
      if (!kind ||
          SiteManagerEntryParent(sites, folders, id, *kind) !=
              parentFolderId ||
          !inserted.insert(std::string{id}).second)
      {
        return;
      }

      result.push_back(SiteManagerEntry{.kind = *kind,
                                        .id = std::string{id}});
    };

    for (const auto &id : siteManagerOrder)
    {
      append(id);
    }

    for (const auto &id : SynthesizeSiteManagerOrder(sites, folders))
    {
      append(id);
    }

    return result;
  }

  std::expected<SiteManagerImportPlan, SiteManagerImportError>
  PlanSiteImport(const config::SiteTransferData &existing,
                 const config::SiteTransferData &incoming,
                 const std::string_view destinationFolderId)
  {
    if (!ValidImportTree(existing) || !ValidImportTree(incoming))
    {
      return std::unexpected(SiteManagerImportError::InvalidTree);
    }

    if (!destinationFolderId.empty() &&
        std::ranges::find(existing.folders, destinationFolderId,
                          &config::SiteFolder::id) == existing.folders.end())
    {
      return std::unexpected(SiteManagerImportError::InvalidDestination);
    }

    std::unordered_set<std::string_view> ids;

    for (const auto &id : existing.siteManagerOrder)
    {
      ids.insert(id);
    }

    for (const auto &id : incoming.siteManagerOrder)
    {
      if (!ids.insert(id).second)
      {
        return std::unexpected(SiteManagerImportError::IdentityCollision);
      }
    }

    SiteManagerImportPlan result;
    result.merged = existing;
    result.importedRoots = OrderedSiteManagerChildren(
        incoming.sites, incoming.folders, incoming.siteManagerOrder, {});
    result.merged.sites.insert(result.merged.sites.end(),
                               incoming.sites.begin(), incoming.sites.end());
    result.merged.folders.insert(result.merged.folders.end(),
                                 incoming.folders.begin(), incoming.folders.end());
    result.merged.siteManagerOrder.insert(result.merged.siteManagerOrder.end(),
                                          incoming.siteManagerOrder.begin(),
                                          incoming.siteManagerOrder.end());

    std::unordered_map<std::string_view, SiteProfile *> sites;
    std::unordered_map<std::string_view, config::SiteFolder *> folders;

    for (auto &site : result.merged.sites)
    {
      sites.emplace(site.id, &site);
    }

    for (auto &folder : result.merged.folders)
    {
      folders.emplace(folder.id, &folder);
    }

    std::unordered_set<std::string> siblingNames;

    for (const auto &entry : OrderedSiteManagerChildren(
             existing.sites, existing.folders, existing.siteManagerOrder,
             destinationFolderId))
    {
      siblingNames.insert(entry.kind == SiteManagerEntryKind::Folder
                              ? folders.at(entry.id)->name
                              : sites.at(entry.id)->name);
    }

    auto occupiedNames = siblingNames;

    for (const auto &entry : result.importedRoots)
    {
      occupiedNames.insert(entry.kind == SiteManagerEntryKind::Folder
                               ? folders.at(entry.id)->name
                               : sites.at(entry.id)->name);
    }

    for (const auto &entry : result.importedRoots)
    {
      auto &name = entry.kind == SiteManagerEntryKind::Folder
                       ? folders.at(entry.id)->name
                       : sites.at(entry.id)->name;

      const auto originalName = name;

      if (siblingNames.contains(name))
      {
        for (std::size_t suffix = 2;; ++suffix)
        {
          name = originalName + " " + std::to_string(suffix);

          if (!occupiedNames.contains(name))
          {
            break;
          }
        }
      }

      occupiedNames.insert(name);

      if (name != originalName)
      {
        result.renames.push_back({entry.id, originalName, name});
      }

      if (entry.kind == SiteManagerEntryKind::Folder)
      {
        folders.at(entry.id)->parentId = destinationFolderId;
      }
      else if (!destinationFolderId.empty())
      {
        folders.at(destinationFolderId)->siteIds.push_back(entry.id);
      }
    }

    return result;
  }

  SiteManagerMoveResult MoveSiteManagerEntry(
      const std::span<const SiteProfile> sites,
      std::vector<config::SiteFolder> &folders,
      std::vector<std::string> &siteManagerOrder,
      const std::string_view sourceId,
      const std::string_view targetId,
      const SiteManagerDropPosition position)
  {
    const auto sourceKind = FindSiteManagerEntryKind(sites, folders, sourceId);
    if (!sourceKind)
    {
      return SiteManagerMoveResult::InvalidSource;
    }

    if (!IsValidSiteManagerOrder(sites, folders, siteManagerOrder))
    {
      return SiteManagerMoveResult::InvalidOrder;
    }

    std::optional<SiteManagerEntryKind> targetKind;

    if (!targetId.empty())
    {
      targetKind = FindSiteManagerEntryKind(sites, folders, targetId);
      if (!targetKind)
      {
        return SiteManagerMoveResult::InvalidTarget;
      }
    }

    if (targetId.empty() && position != SiteManagerDropPosition::Into)
    {
      return SiteManagerMoveResult::InvalidTarget;
    }

    if (position == SiteManagerDropPosition::Into && !targetId.empty() &&
        targetKind != SiteManagerEntryKind::Folder)
    {
      return SiteManagerMoveResult::InvalidTarget;
    }

    if (sourceId == targetId && position != SiteManagerDropPosition::Into)
    {
      return SiteManagerMoveResult::NoChange;
    }

    const auto sourceParent = SiteManagerEntryParent(
        sites, folders, sourceId, *sourceKind);

    std::string destinationParent;

    if (position == SiteManagerDropPosition::Into)
    {
      destinationParent = targetId;
    }
    else
    {
      destinationParent = SiteManagerEntryParent(
          sites, folders, targetId, *targetKind);
    }

    if (*sourceKind == SiteManagerEntryKind::Folder &&
        IsFolderDescendantOf(folders, destinationParent, sourceId))
    {
      return SiteManagerMoveResult::Cycle;
    }

    if (sourceParent != destinationParent)
    {
      if (*sourceKind == SiteManagerEntryKind::Folder)
      {
        const auto source = std::ranges::find(
            folders, sourceId, &config::SiteFolder::id);

        if (!IsFolderNameUnique(folders, sites, source->name,
                                destinationParent, sourceId))
        {
          return SiteManagerMoveResult::NameConflict;
        }
      }
      else
      {
        const auto source = std::ranges::find(sites, sourceId,
                                              &SiteProfile::id);

        if (!IsSiteNameUniqueInTree(sites, folders, source->name,
                                    destinationParent, sourceId))
        {
          return SiteManagerMoveResult::NameConflict;
        }
      }
    }

    if (sourceParent == destinationParent)
    {
      const auto siblings = OrderedSiteManagerChildren(
          sites, folders, siteManagerOrder, sourceParent);

      const auto source = std::ranges::find(siblings, sourceId,
                                            &SiteManagerEntry::id);

      if (position == SiteManagerDropPosition::Into)
      {
        if (source != siblings.end() && std::next(source) == siblings.end())
        {
          return SiteManagerMoveResult::NoChange;
        }
      }
      else
      {
        const auto target = std::ranges::find(siblings, targetId,
                                              &SiteManagerEntry::id);

        if (source != siblings.end() && target != siblings.end())
        {
          if (position == SiteManagerDropPosition::Before &&
              std::next(source) == target)
          {
            return SiteManagerMoveResult::NoChange;
          }

          if (position == SiteManagerDropPosition::After &&
              std::next(target) == source)
          {
            return SiteManagerMoveResult::NoChange;
          }
        }
      }
    }

    auto nextFolders = folders;
    auto nextOrder = siteManagerOrder;

    if (*sourceKind == SiteManagerEntryKind::Folder)
    {
      const auto source = std::ranges::find(
          nextFolders, sourceId, &config::SiteFolder::id);
      source->parentId = destinationParent;
    }
    else
    {
      AssignSiteToFolder(nextFolders, sourceId, destinationParent);
    }

    const auto sourceOrder = std::ranges::find(nextOrder, sourceId);
    if (sourceOrder == nextOrder.end())
    {
      return SiteManagerMoveResult::InvalidOrder;
    }

    const std::string sourceIdCopy{sourceId};

    nextOrder.erase(sourceOrder);

    auto insertion = nextOrder.end();

    if (position == SiteManagerDropPosition::Before ||
        position == SiteManagerDropPosition::After)
    {
      insertion = std::ranges::find(nextOrder, targetId);

      if (insertion == nextOrder.end())
      {
        return SiteManagerMoveResult::InvalidOrder;
      }

      if (position == SiteManagerDropPosition::After)
      {
        ++insertion;
      }
    }
    else
    {
      auto lastChild = nextOrder.end();

      for (auto candidate = nextOrder.begin(); candidate != nextOrder.end();
           ++candidate)
      {
        if (IsDirectSiteManagerChild(sites, nextFolders, *candidate,
                                     destinationParent))
        {
          lastChild = candidate;
        }
      }

      if (lastChild != nextOrder.end())
      {
        insertion = std::next(lastChild);
      }
      else if (!destinationParent.empty())
      {
        insertion = std::ranges::find(nextOrder, destinationParent);

        if (insertion == nextOrder.end())
        {
          return SiteManagerMoveResult::InvalidOrder;
        }

        ++insertion;
      }
    }

    nextOrder.insert(insertion, sourceIdCopy);

    if (nextFolders == folders && nextOrder == siteManagerOrder)
    {
      return SiteManagerMoveResult::NoChange;
    }

    folders = std::move(nextFolders);
    siteManagerOrder = std::move(nextOrder);

    return SiteManagerMoveResult::Moved;
  }

  SiteProfile CloneSiteProfile(const SiteProfile &source,
                               std::string newId,
                               std::string newName)
  {
    SiteProfile clone = source;
    clone.id = std::move(newId);
    clone.name = std::move(newName);
    clone.authentication.credentialId.clear();
    clone.authentication.passphraseCredentialId.clear();

    return clone;
  }

  std::optional<SiteFolderTreeClone> CloneSiteFolderTree(
      const std::span<const SiteProfile> sites,
      const std::span<const config::SiteFolder> folders,
      const std::string_view sourceFolderId,
      const std::string_view copySuffix,
      const std::span<const std::string> sourceOrder)
  {
    const auto sourceRoot = std::ranges::find(
        folders, sourceFolderId, &config::SiteFolder::id);
    if (sourceRoot == folders.end())
    {
      return std::nullopt;
    }

    std::unordered_set<std::string> usedIds;
    usedIds.reserve(sites.size() + folders.size());

    for (const auto &site : sites)
    {
      usedIds.insert(site.id);
    }

    for (const auto &folder : folders)
    {
      usedIds.insert(folder.id);
    }

    const auto makeFreshId = [&usedIds]
    {
      while (true)
      {
        auto id = GenerateId();
        if (!id.empty() && usedIds.insert(id).second)
        {
          return id;
        }
      }
    };

    std::vector<const config::SiteFolder *> sourceFolders;
    sourceFolders.reserve(folders.size());

    std::unordered_set<std::string> visitedFolderIds;
    visitedFolderIds.reserve(folders.size());

    const auto collectFolder = [&](this const auto &self,
                                   const config::SiteFolder &folder) -> void
    {
      if (!visitedFolderIds.insert(folder.id).second)
      {
        return;
      }

      sourceFolders.push_back(&folder);

      for (const auto &candidate : folders)
      {
        if (candidate.parentId == folder.id)
        {
          self(candidate);
        }
      }
    };

    collectFolder(*sourceRoot);

    SiteFolderTreeClone result;
    result.folders.reserve(sourceFolders.size());

    std::unordered_map<std::string, std::string> clonedFolderIds;
    clonedFolderIds.reserve(sourceFolders.size());

    for (const auto *sourceFolder : sourceFolders)
    {
      clonedFolderIds.emplace(sourceFolder->id, makeFreshId());
    }

    std::string rootNameBase = sourceRoot->name;
    rootNameBase += copySuffix;

    const auto rootName = NextAvailableFolderName(
        folders, sites, sourceRoot->parentId, rootNameBase);

    for (const auto *sourceFolder : sourceFolders)
    {
      config::SiteFolder clone;
      clone.id = clonedFolderIds.at(sourceFolder->id);
      clone.name = sourceFolder == &*sourceRoot ? rootName : sourceFolder->name;

      if (sourceFolder == &*sourceRoot)
      {
        clone.parentId = sourceRoot->parentId;
      }
      else
      {
        clone.parentId = clonedFolderIds.at(sourceFolder->parentId);
      }

      result.folders.push_back(std::move(clone));
    }

    result.rootFolderId = result.folders.front().id;

    std::unordered_map<std::string, std::string> clonedSiteIds;
    clonedSiteIds.reserve(sites.size());

    for (std::size_t folderIndex = 0; folderIndex < sourceFolders.size(); ++folderIndex)
    {
      const auto &sourceFolder = *sourceFolders[folderIndex];

      auto &clonedFolder = result.folders[folderIndex];
      clonedFolder.siteIds.reserve(sourceFolder.siteIds.size());

      for (const auto &sourceSiteId : sourceFolder.siteIds)
      {
        if (const auto existingClone = clonedSiteIds.find(sourceSiteId);
            existingClone != clonedSiteIds.end())
        {
          clonedFolder.siteIds.push_back(existingClone->second);

          continue;
        }

        const auto sourceSite = std::ranges::find(
            sites, sourceSiteId, &SiteProfile::id);
        if (sourceSite == sites.end())
        {
          continue;
        }

        auto clone = CloneSiteProfile(*sourceSite, makeFreshId(), sourceSite->name);
        const auto cloneId = clone.id;

        clonedSiteIds.emplace(sourceSiteId, cloneId);
        clonedFolder.siteIds.push_back(cloneId);

        result.sites.push_back(std::move(clone));
      }
    }

    std::unordered_set<std::string> orderedCloneIds;
    orderedCloneIds.reserve(clonedFolderIds.size() + clonedSiteIds.size());

    const auto appendClonedOrder = [&](const std::string_view sourceId)
    {
      if (const auto folder = clonedFolderIds.find(std::string{sourceId});
          folder != clonedFolderIds.end() &&
          orderedCloneIds.insert(folder->second).second)
      {
        result.siteManagerOrder.push_back(folder->second);

        return;
      }

      if (const auto site = clonedSiteIds.find(std::string{sourceId});
          site != clonedSiteIds.end() &&
          orderedCloneIds.insert(site->second).second)
      {
        result.siteManagerOrder.push_back(site->second);
      }
    };

    result.siteManagerOrder.reserve(clonedFolderIds.size() +
                                    clonedSiteIds.size());

    for (const auto &id : sourceOrder)
    {
      appendClonedOrder(id);
    }

    for (const auto &id : SynthesizeSiteManagerOrder(sites, folders))
    {
      appendClonedOrder(id);
    }

    return result;
  }
} // namespace havremote::ui
