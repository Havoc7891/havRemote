// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_PROTOCOL_SFTP_SESSION_HPP
#define HAVREMOTE_INCLUDE_PROTOCOL_SFTP_SESSION_HPP

#include "core/session.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace havremote
{
  [[nodiscard]] RemoteSessionPtr MakeSftpSession();

  namespace sftp
  {
    // SSH servers return authentication methods as an exact comma-delimited
    // token list. Do not use substring matching here: for example, a vendor
    // extension containing "password" is not password authentication.
    [[nodiscard]] bool AuthenticationMethodOffered(
        std::string_view methods,
        std::string_view method) noexcept;

    struct OwnerGroup final
    {
      std::string owner;
      std::string group;

      friend bool operator==(const OwnerGroup &, const OwnerGroup &) = default;
    };

    // SFTP v3 servers may omit numeric UID/GID attributes but still provide the
    // conventional `ls -l`-style long-name field. Parsing is deliberately strict:
    // the field is display metadata supplied by the server and is never used to
    // identify or operate on a remote path.
    [[nodiscard]] std::optional<OwnerGroup> ParseLongEntryOwnerGroup(
        std::string_view longEntry);
  } // namespace sftp
} // namespace havremote

#endif // HAVREMOTE_INCLUDE_PROTOCOL_SFTP_SESSION_HPP
