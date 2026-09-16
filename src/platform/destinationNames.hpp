// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_SRC_PLATFORM_DESTINATION_NAMES_HPP
#define HAVREMOTE_SRC_PLATFORM_DESTINATION_NAMES_HPP

#include "platform/platformError.hpp"

#include <filesystem>
#include <memory>

namespace havremote::platform::detail
{
  // Checks planned names using private entries on the destination filesystem.
  // This does not reserve the final destination against other writers.
  class DestinationNames final
  {
  public:
    explicit DestinationNames(std::filesystem::path absoluteDestinationRoot);
    ~DestinationNames();
    DestinationNames(DestinationNames &&) noexcept;
    DestinationNames &operator=(DestinationNames &&) noexcept;
    DestinationNames(const DestinationNames &) = delete;
    DestinationNames &operator=(const DestinationNames &) = delete;

    [[nodiscard]] Result<void> Reserve(const std::filesystem::path &relative,
                                       bool allowAliasPrefixes = false);

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
  };
} // namespace havremote::platform::detail

#endif // HAVREMOTE_SRC_PLATFORM_DESTINATION_NAMES_HPP
