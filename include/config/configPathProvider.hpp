// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_CONFIG_CONFIG_PATH_PROVIDER_HPP
#define HAVREMOTE_INCLUDE_CONFIG_CONFIG_PATH_PROVIDER_HPP

#include "config/configTypes.hpp"

#include <expected>
#include <filesystem>

namespace havremote::config
{
  class IConfigPathProvider
  {
  public:
    virtual ~IConfigPathProvider() = default;

    [[nodiscard]] virtual std::expected<std::filesystem::path, ConfigError>
    ConfigPath() const = 0;
  };

  class FixedConfigPathProvider final : public IConfigPathProvider
  {
  public:
    explicit FixedConfigPathProvider(std::filesystem::path path);

    [[nodiscard]] std::expected<std::filesystem::path, ConfigError>
    ConfigPath() const override;

  private:
    std::filesystem::path mPath;
  };

  // Appends havRemote/havRemote.cson to an injected application-data directory
  class AppDataConfigPathProvider final : public IConfigPathProvider
  {
  public:
    explicit AppDataConfigPathProvider(std::filesystem::path appDataDirectory);

    [[nodiscard]] std::expected<std::filesystem::path, ConfigError>
    ConfigPath() const override;

  private:
    std::filesystem::path mAppDataDirectory;
  };
} // namespace havremote::config

#endif // HAVREMOTE_INCLUDE_CONFIG_CONFIG_PATH_PROVIDER_HPP
