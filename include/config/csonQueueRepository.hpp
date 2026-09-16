// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_CONFIG_CSON_QUEUE_REPOSITORY_HPP
#define HAVREMOTE_INCLUDE_CONFIG_CSON_QUEUE_REPOSITORY_HPP

#include "config/queueRepository.hpp"

#include <memory>

namespace havremote::config
{
  class CsonQueueRepository final : public IQueueRepository
  {
  public:
    explicit CsonQueueRepository(std::filesystem::path path);
    ~CsonQueueRepository() override;

    CsonQueueRepository(const CsonQueueRepository &) = delete;
    CsonQueueRepository &operator=(const CsonQueueRepository &) = delete;
    CsonQueueRepository(CsonQueueRepository &&) noexcept;
    CsonQueueRepository &operator=(CsonQueueRepository &&) noexcept;

    [[nodiscard]] std::expected<QueueLoadResult, ConfigError> Load() override;
    [[nodiscard]] std::expected<void, ConfigError> Save(
        const std::vector<PersistentQueueItem> &items) override;
    [[nodiscard]] std::expected<std::filesystem::path, ConfigError>
    QueuePath() const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
  };
} // namespace havremote::config

#endif // HAVREMOTE_INCLUDE_CONFIG_CSON_QUEUE_REPOSITORY_HPP
