// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_PROTOCOL_SFTP_KEY_FILES_HPP
#define HAVREMOTE_INCLUDE_PROTOCOL_SFTP_KEY_FILES_HPP

#include <string_view>

namespace havremote::sftp
{
  enum class PrivateKeyEncryption
  {
    Unknown,
    Unencrypted,
    Encrypted,
  };

  // Detects whether a supported private-key container requires a passphrase.
  // The detector does not parse secret key fields. Structurally invalid,
  // truncated, oversized, or unfamiliar input is deliberately Unknown.
  [[nodiscard]] PrivateKeyEncryption DetectPrivateKeyEncryption(
      std::string_view contents) noexcept;
} // namespace havremote::sftp

#endif // HAVREMOTE_INCLUDE_PROTOCOL_SFTP_KEY_FILES_HPP
