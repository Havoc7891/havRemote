// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_PLATFORM_RECYCLE_BIN_HPP
#define HAVREMOTE_INCLUDE_PLATFORM_RECYCLE_BIN_HPP

#include "platform/platformError.hpp"

#include <filesystem>
#include <memory>
#include <span>

namespace havremote::platform
{
  class IRecycleBin
  {
  public:
    virtual ~IRecycleBin() = default;
    [[nodiscard]] virtual Result<void> MoveToRecycleBin(
        std::span<const std::filesystem::path> paths) = 0;
  };

  [[nodiscard]] std::unique_ptr<IRecycleBin> MakeRecycleBin();
} // namespace havremote::platform

#endif // HAVREMOTE_INCLUDE_PLATFORM_RECYCLE_BIN_HPP
