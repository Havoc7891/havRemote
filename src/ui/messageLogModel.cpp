// SPDX-License-Identifier: MIT

#include "ui/messageLogModel.hpp"

namespace havremote::ui
{
  RgbColour MessageLogColour(const DiagnosticLevel level,
                             const config::AppearanceTheme theme) noexcept
  {
    if (theme == config::AppearanceTheme::Dark)
    {
      switch (level)
      {
      case DiagnosticLevel::Debug:
        return {197, 197, 197};

      case DiagnosticLevel::Information:
        return {246, 246, 246};

      case DiagnosticLevel::Warning:
        return {202, 169, 78};

      case DiagnosticLevel::Error:
        return {255, 114, 126};
      }

      return {246, 246, 246};
    }

    switch (level)
    {
    case DiagnosticLevel::Debug:
      return {85, 85, 85};

    case DiagnosticLevel::Information:
      return {17, 17, 17};

    case DiagnosticLevel::Warning:
      return {138, 101, 0};

    case DiagnosticLevel::Error:
      return {176, 0, 32};
    }

    return {17, 17, 17};
  }
} // namespace havremote::ui
