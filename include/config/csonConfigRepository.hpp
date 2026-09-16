// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_CONFIG_CSON_CONFIG_REPOSITORY_HPP
#define HAVREMOTE_INCLUDE_CONFIG_CSON_CONFIG_REPOSITORY_HPP

#include "config/configPathProvider.hpp"
#include "config/configRepository.hpp"

#include <memory>

namespace havremote::config
{
  class CsonConfigRepository final : public IConfigRepository
  {
  public:
    explicit CsonConfigRepository(std::shared_ptr<const IConfigPathProvider> pathProvider);
    explicit CsonConfigRepository(std::filesystem::path path);
    ~CsonConfigRepository() override;

    CsonConfigRepository(const CsonConfigRepository &) = delete;
    CsonConfigRepository &operator=(const CsonConfigRepository &) = delete;
    CsonConfigRepository(CsonConfigRepository &&) noexcept;
    CsonConfigRepository &operator=(CsonConfigRepository &&) noexcept;

    [[nodiscard]] std::expected<ConfigLoadResult, ConfigError> Load() override;
    [[nodiscard]] std::expected<void, ConfigError> Save(const ConfigData &data) override;
    [[nodiscard]] std::expected<std::filesystem::path, ConfigError>
    ConfigPath() const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
  };
} // namespace havremote::config

#endif // HAVREMOTE_INCLUDE_CONFIG_CSON_CONFIG_REPOSITORY_HPP
