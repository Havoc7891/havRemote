// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_PROTOCOL_SFTP_ERRORS_HPP
#define HAVREMOTE_INCLUDE_PROTOCOL_SFTP_ERRORS_HPP

#include "core/types.hpp"

#include <optional>
#include <string_view>

namespace havremote::sftp
{
  // Only use the SFTP status when the library reports a parsed failure reply,
  // not an authentication/transport error or malformed packet. Keep its readable
  // description in the shared message used by logs, dialogs and the queue.
  [[nodiscard]] RemoteError MakeOperationError(
      std::string_view operation,
      int nativeCode,
      std::string_view libraryDetail,
      std::optional<unsigned long> status,
      bool mutating);
} // namespace havremote::sftp

#endif // HAVREMOTE_INCLUDE_PROTOCOL_SFTP_ERRORS_HPP
