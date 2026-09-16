// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_CONFIG_CONFIG_REPOSITORY_HPP
#define HAVREMOTE_INCLUDE_CONFIG_CONFIG_REPOSITORY_HPP

#include "config/configTypes.hpp"

#include <expected>
#include <filesystem>

namespace havremote::config
{
  class IConfigRepository
  {
  public:
    virtual ~IConfigRepository() = default;

    // If the file is absent, load creates and atomically persists defaults.
    // Parse, validation, and future-version failures never overwrite it.
    [[nodiscard]] virtual std::expected<ConfigLoadResult, ConfigError> Load() = 0;
    [[nodiscard]] virtual std::expected<void, ConfigError> Save(const ConfigData &data) = 0;
    [[nodiscard]] virtual std::expected<std::filesystem::path, ConfigError> ConfigPath() const = 0;
  };
} // namespace havremote::config

#endif // HAVREMOTE_INCLUDE_CONFIG_CONFIG_REPOSITORY_HPP
