// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_CONFIG_QUEUE_REPOSITORY_HPP
#define HAVREMOTE_INCLUDE_CONFIG_QUEUE_REPOSITORY_HPP

#include "config/configTypes.hpp"
#include "config/queueTypes.hpp"

#include <expected>
#include <filesystem>
#include <vector>

namespace havremote::config
{
  class IQueueRepository
  {
  public:
    virtual ~IQueueRepository() = default;

    // A missing file represents an empty queue. Invalid existing state is
    // never overwritten by a later save unless a subsequent load succeeds.
    [[nodiscard]] virtual std::expected<QueueLoadResult, ConfigError> Load() = 0;
    [[nodiscard]] virtual std::expected<void, ConfigError> Save(
        const std::vector<PersistentQueueItem> &items) = 0;
    [[nodiscard]] virtual std::expected<std::filesystem::path, ConfigError>
    QueuePath() const = 0;
  };
} // namespace havremote::config

#endif // HAVREMOTE_INCLUDE_CONFIG_QUEUE_REPOSITORY_HPP
