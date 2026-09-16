// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UI_SITE_MANAGER_MODEL_HPP
#define HAVREMOTE_INCLUDE_UI_SITE_MANAGER_MODEL_HPP

#include "config/configTypes.hpp"
#include "config/siteTransfer.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace havremote::ui
{
  enum class SiteManagerEntryKind
  {
    Folder,
    Site,
  };

  struct SiteManagerEntry final
  {
    SiteManagerEntryKind kind{SiteManagerEntryKind::Site};
    std::string id;

    friend bool operator==(const SiteManagerEntry &,
                           const SiteManagerEntry &) = default;
  };

  enum class SiteManagerImportError
  {
    InvalidTree,
    InvalidDestination,
    IdentityCollision,
  };

  struct SiteManagerImportRename final
  {
    std::string id;
    std::string originalName;
    std::string proposedName;

    friend bool operator==(const SiteManagerImportRename &,
                           const SiteManagerImportRename &) = default;
  };

  struct SiteManagerImportPlan final
  {
    config::SiteTransferData merged;
    std::vector<SiteManagerImportRename> renames;
    std::vector<SiteManagerEntry> importedRoots;
  };

  // Builds a complete candidate without changing either input. Imported roots
  // append to the destination in file order. Conflicting sibling names receive
  // numbered proposals. The caller must obtain explicit confirmation before
  // replacing its working tree with merged. IDs must already be fresh.
  [[nodiscard]] std::expected<SiteManagerImportPlan, SiteManagerImportError>
  PlanSiteImport(const config::SiteTransferData &existing,
                 const config::SiteTransferData &incoming,
                 std::string_view destinationFolderId);

  enum class SiteManagerDropPosition
  {
    Before,
    After,
    Into,
  };

  enum class SiteManagerMoveResult
  {
    Moved,
    NoChange,
    InvalidSource,
    InvalidTarget,
    InvalidOrder,
    Cycle,
    NameConflict,
  };

  // Returns the second entry in the first pair of same-named sibling sites,
  // if one exists. Root-level sites share the empty parent folder ID.
  [[nodiscard]] std::optional<std::size_t> DuplicateSiteNameIndex(
      std::span<const SiteProfile> sites,
      std::span<const config::SiteFolder> folders);

  // Returns an empty string for a root-level site. Returning an owned value
  // keeps callers safe when they subsequently mutate the folder vectors.
  [[nodiscard]] std::string SiteFolderId(
      std::span<const config::SiteFolder> folders,
      std::string_view siteId);

  // Removes all stale assignments before optionally adding the site to its
  // new folder. An empty folder ID moves the site to the tree root. An unknown
  // non-empty destination leaves the current assignment unchanged.
  void AssignSiteToFolder(std::vector<config::SiteFolder> &folders,
                          std::string_view siteId,
                          std::string_view folderId);

  [[nodiscard]] bool IsFolderDescendantOf(
      std::span<const config::SiteFolder> folders,
      std::string_view candidateFolderId,
      std::string_view ancestorFolderId) noexcept;

  [[nodiscard]] bool IsFolderNameUnique(
      std::span<const config::SiteFolder> folders,
      std::span<const SiteProfile> sites,
      std::string_view candidate,
      std::string_view parentFolderId,
      std::string_view excludedFolderId = {});

  // Compare exactly against sites and folders in the same immediate parent,
  // after the dialog has trimmed surrounding whitespace. Exclude the current
  // site's stable ID so edits to its other fields never conflict with itself.
  [[nodiscard]] bool IsSiteNameUniqueInTree(
      std::span<const SiteProfile> sites,
      std::span<const config::SiteFolder> folders,
      std::string_view candidate,
      std::string_view parentFolderId,
      std::string_view excludedSiteId = {});

  [[nodiscard]] std::string NextAvailableFolderName(
      std::span<const config::SiteFolder> folders,
      std::span<const SiteProfile> sites,
      std::string_view parentFolderId,
      std::string_view base);

  // Returns base when available among siblings, otherwise the first available
  // numbered name: "base 2", "base 3", and so on.
  [[nodiscard]] std::string NextAvailableSiteNameInTree(
      std::span<const SiteProfile> sites,
      std::span<const config::SiteFolder> folders,
      std::string_view parentFolderId,
      std::string_view base);

  // Child folders precede sites, retaining folder-vector sibling order and
  // each folder's siteIds order.
  [[nodiscard]] std::vector<std::string> SynthesizeSiteManagerOrder(
      std::span<const SiteProfile> sites,
      std::span<const config::SiteFolder> folders);

  // Returns the direct children of parentFolderId in their persisted mixed
  // order. Missing entries are appended in SynthesizeSiteManagerOrder() order.
  [[nodiscard]] std::vector<SiteManagerEntry> OrderedSiteManagerChildren(
      std::span<const SiteProfile> sites,
      std::span<const config::SiteFolder> folders,
      std::span<const std::string> siteManagerOrder,
      std::string_view parentFolderId);

  // Moves one entry transactionally. Before and After use the target's parent.
  // Into requires a folder target and appends to it. An empty target combined
  // with Into appends at the root. Invalid drops leave folders and order
  // completely unchanged.
  [[nodiscard]] SiteManagerMoveResult MoveSiteManagerEntry(
      std::span<const SiteProfile> sites,
      std::vector<config::SiteFolder> &folders,
      std::vector<std::string> &siteManagerOrder,
      std::string_view sourceId,
      std::string_view targetId,
      SiteManagerDropPosition position);

  // Changed secrets get new store entries before the configuration is saved.
  // Unchanged fields retain their identifiers, and the input is never mutated.
  [[nodiscard]] Authentication PrepareCredentialIdentifiers(
      const Authentication &current, bool updatePassword, bool updatePassphrase);

  // Copies connection settings without sharing secure-store identifiers
  [[nodiscard]] SiteProfile CloneSiteProfile(const SiteProfile &source,
                                             std::string newId,
                                             std::string newName);

  struct SiteFolderTreeClone final
  {
    std::string rootFolderId;
    std::vector<config::SiteFolder> folders;
    std::vector<SiteProfile> sites;
    std::vector<std::string> siteManagerOrder;
  };

  // Clones a folder and all of its descendants as a sibling of the source.
  // The supplied suffix is appended to the cloned root before resolving
  // sibling collisions. All descendant labels are retained because their new
  // parents give them independent sibling namespaces.
  [[nodiscard]] std::optional<SiteFolderTreeClone> CloneSiteFolderTree(
      std::span<const SiteProfile> sites,
      std::span<const config::SiteFolder> folders,
      std::string_view sourceFolderId,
      std::string_view copySuffix,
      std::span<const std::string> sourceOrder = {});
} // namespace havremote::ui

#endif // HAVREMOTE_INCLUDE_UI_SITE_MANAGER_MODEL_HPP
