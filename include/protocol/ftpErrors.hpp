// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_PROTOCOL_FTP_ERRORS_HPP
#define HAVREMOTE_INCLUDE_PROTOCOL_FTP_ERRORS_HPP

#include "core/types.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace havremote::ftp
{
  enum class OperationKind;

  enum class UploadResumeCommand
  {
    Rest,
    Stor,
  };

  // A complete reply associated with this upload's STOR command. The caller
  // bounds and sanitizes the reply body before it reaches diagnostics.
  struct UploadReply
  {
    long code{};
    std::string text;
  };

  [[nodiscard]] std::string SocketErrorText(int error);

  // MLSD compatibility fallback is for an unsupported listing command, not a
  // second attempt at logging in or establishing a failed data connection.
  [[nodiscard]] bool CanFallbackToList(int nativeCode, long responseCode) noexcept;

  // Missing optional ownership metadata must not hide a failed session
  [[nodiscard]] bool CanIgnoreOwnershipListingFailure(
      const RemoteError &error, long responseCode) noexcept;

  // Format captured curl/FTP/OS diagnostics without reading a session handle
  [[nodiscard]] std::string FormatFailureDetail(
      int nativeCode,
      std::string_view errorBuffer,
      long responseCode,
      std::optional<long> osError = std::nullopt);

  // Callers capture the protocol results before cleanup or further requests
  [[nodiscard]] RemoteError MakeOperationError(
      int nativeCode,
      std::string_view errorBuffer,
      long responseCode,
      bool mutating,
      std::optional<long> osError,
      OperationKind operation,
      const UploadReply *uploadReply = nullptr);

  // Only for a confirmed non-350 REST reply or rejected STOR before data is
  // transferred. Transport failures still use makeOperationError instead.
  [[nodiscard]] RemoteError MakeUploadResumeError(
      int nativeCode,
      std::string_view errorBuffer,
      long responseCode,
      std::optional<long> osError,
      UploadResumeCommand command);
} // namespace havremote::ftp

#endif // HAVREMOTE_INCLUDE_PROTOCOL_FTP_ERRORS_HPP
