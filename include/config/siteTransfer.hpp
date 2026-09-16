// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_CONFIG_SITE_TRANSFER_HPP
#define HAVREMOTE_INCLUDE_CONFIG_SITE_TRANSFER_HPP

#include "config/configTypes.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace havremote::config
{
  struct SiteTransferData final
  {
    std::vector<SiteProfile> sites;
    std::vector<SiteFolder> folders;
    std::vector<std::string> siteManagerOrder;
  };

  // A portable, strictly validated site tree, never an application config or
  // credential backup. Imports receive fresh IDs. Private-key authentication
  // retains its kind but requires the caller to select a local key before use.
  [[nodiscard]] std::expected<SiteTransferData, ConfigError> ReadSiteExport(
      const std::filesystem::path &file);

  // The caller obtains explicit overwrite approval before invoking this writer.
  // A selected folder includes its complete subtree. No selection exports all.
  [[nodiscard]] std::expected<void, ConfigError> WriteSiteExport(
      const std::filesystem::path &file,
      const SiteTransferData &data,
      std::optional<std::string> selectedRootId = std::nullopt);
} // namespace havremote::config

#endif // HAVREMOTE_INCLUDE_CONFIG_SITE_TRANSFER_HPP
