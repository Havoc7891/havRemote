// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_CORE_LOG_SANITIZER_HPP
#define HAVREMOTE_INCLUDE_CORE_LOG_SANITIZER_HPP

#include <string>
#include <string_view>

namespace havremote
{
  // Sanitizes untrusted protocol, filesystem, and configuration text immediately
  // before it reaches a diagnostic sink. The function is intentionally
  // conservative: once a credential-shaped field is found, the remainder of
  // that logical line is redacted rather than risking a partial secret leak.
  [[nodiscard]] std::string SanitizeDiagnosticText(std::string_view text);
} // namespace havremote

#endif // HAVREMOTE_INCLUDE_CORE_LOG_SANITIZER_HPP
