// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_SRC_PLATFORM_RECYCLE_BIN_INTERNAL_HPP
#define HAVREMOTE_SRC_PLATFORM_RECYCLE_BIN_INTERNAL_HPP

#include "platform/recycleBin.hpp"

namespace havremote::platform::detail
{
  [[nodiscard]] Result<void> MoveToSystemTrash(
      std::span<const std::filesystem::path> paths);
}

#endif // HAVREMOTE_SRC_PLATFORM_RECYCLE_BIN_INTERNAL_HPP
