// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_PROTOCOL_SFTP_HOST_KEY_POLICY_HPP
#define HAVREMOTE_INCLUDE_PROTOCOL_SFTP_HOST_KEY_POLICY_HPP

#include <span>
#include <string>

namespace havremote::sftp
{
  // Builds the comma-separated preference accepted by libssh2_session_method_pref().
  // The first plain Ed25519 entry is promoted ahead of libssh2's default order.
  // Every other supported algorithm retains its relative position.
  [[nodiscard]] std::string MakeHostKeyAlgorithmPreference(
      std::span<const char *const> supportedAlgorithms);
} // namespace havremote::sftp

#endif // HAVREMOTE_INCLUDE_PROTOCOL_SFTP_HOST_KEY_POLICY_HPP
