// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UI_MESSAGE_LOG_MODEL_HPP
#define HAVREMOTE_INCLUDE_UI_MESSAGE_LOG_MODEL_HPP

#include "config/configTypes.hpp"

#include <cstdint>

namespace havremote::ui
{
  struct RgbColour final
  {
    std::uint8_t red{};
    std::uint8_t green{};
    std::uint8_t blue{};

    friend bool operator==(const RgbColour &,
                           const RgbColour &) = default;
  };

  [[nodiscard]] RgbColour MessageLogColour(
      DiagnosticLevel level,
      config::AppearanceTheme theme) noexcept;
} // namespace havremote::ui

#endif // HAVREMOTE_INCLUDE_UI_MESSAGE_LOG_MODEL_HPP
