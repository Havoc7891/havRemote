// SPDX-License-Identifier: MIT

#include "protocol/sftpErrors.hpp"

#include <catch2/catch_test_macros.hpp>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

using namespace havremote;

namespace
{
  std::string Lowercase(std::string text)
  {
    std::ranges::transform(text, text.begin(), [](const unsigned char character)
                           { return static_cast<char>(std::tolower(character)); });
    return text;
  }

  struct StatusCase final
  {
    unsigned long status;
    RemoteErrorCode code;
    std::string_view readableFragment;
    bool retryable{};
    bool uncertainWhenMutating{};
  };
} // namespace

TEST_CASE("SFTP operation errors expose readable server status and preserve classification",
          "[sftp][errors]")
{
  constexpr std::array cases{
      StatusCase{LIBSSH2_FX_NO_SUCH_FILE, RemoteErrorCode::NotFound, "file"},
      StatusCase{LIBSSH2_FX_NO_SUCH_PATH, RemoteErrorCode::NotFound, "path"},
      StatusCase{LIBSSH2_FX_PERMISSION_DENIED, RemoteErrorCode::PermissionDenied, "permission"},
      StatusCase{LIBSSH2_FX_FILE_ALREADY_EXISTS, RemoteErrorCode::AlreadyExists, "exists"},
      StatusCase{LIBSSH2_FX_WRITE_PROTECT, RemoteErrorCode::PermissionDenied, "protect"},
      StatusCase{LIBSSH2_FX_NO_CONNECTION, RemoteErrorCode::ConnectionLost, "connection", true, true},
      StatusCase{LIBSSH2_FX_CONNECTION_LOST, RemoteErrorCode::ConnectionLost, "connection", true, true},
      StatusCase{LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM, RemoteErrorCode::RemoteIo, "space"},
      StatusCase{LIBSSH2_FX_QUOTA_EXCEEDED, RemoteErrorCode::RemoteIo, "quota"},
      StatusCase{LIBSSH2_FX_OP_UNSUPPORTED, RemoteErrorCode::Unsupported, "support"},
      StatusCase{LIBSSH2_FX_DIR_NOT_EMPTY, RemoteErrorCode::DirectoryNotEmpty, "empty"},
      StatusCase{LIBSSH2_FX_NOT_A_DIRECTORY, RemoteErrorCode::NotDirectory, "directory"},
      StatusCase{LIBSSH2_FX_EOF, RemoteErrorCode::RemoteIo, "end", false, true},
      StatusCase{LIBSSH2_FX_BAD_MESSAGE, RemoteErrorCode::RemoteIo, "message", false, true},
      StatusCase{LIBSSH2_FX_INVALID_HANDLE, RemoteErrorCode::RemoteIo, "handle", false, true},
      StatusCase{LIBSSH2_FX_NO_MEDIA, RemoteErrorCode::RemoteIo, "media", false, true},
      StatusCase{LIBSSH2_FX_UNKNOWN_PRINCIPAL, RemoteErrorCode::RemoteIo, "principal", false, true},
      StatusCase{LIBSSH2_FX_LOCK_CONFLICT, RemoteErrorCode::RemoteIo, "lock", false, true},
      StatusCase{LIBSSH2_FX_INVALID_FILENAME, RemoteErrorCode::RemoteIo, "filename", false, true},
      StatusCase{LIBSSH2_FX_LINK_LOOP, RemoteErrorCode::RemoteIo, "loop", false, true},
  };

  for (const auto &item : cases)
  {
    for (const bool mutating : {false, true})
    {
      CAPTURE(item.status, mutating);

      const auto error = sftp::MakeOperationError(
          "Renaming a remote path", LIBSSH2_ERROR_SFTP_PROTOCOL,
          "SFTP Protocol Error", item.status, mutating);

      CHECK(error.code == item.code);
      CHECK(error.nativeCode == static_cast<int>(item.status));
      CHECK(error.retryable == item.retryable);
      CHECK(error.operationMayHaveSucceeded == (mutating && item.uncertainWhenMutating));
      CHECK(error.message.starts_with("Renaming a remote path: "));
      CHECK(error.message.ends_with(" (SFTP status " + std::to_string(item.status) + ")"));
      CHECK(Lowercase(error.message).find(item.readableFragment) != std::string::npos);
      CHECK(error.message.find("SFTP Protocol Error") == std::string::npos);
    }
  }
}

TEST_CASE("SFTP permission errors replace generic create-file details",
          "[sftp][errors]")
{
  const auto error = sftp::MakeOperationError(
      "Creating a remote file", LIBSSH2_ERROR_SFTP_PROTOCOL,
      "Failed opening remote file", LIBSSH2_FX_PERMISSION_DENIED, true);

  CHECK(error.message == "Creating a remote file: Permission denied (SFTP status 3)");
  CHECK(error.code == RemoteErrorCode::PermissionDenied);
  CHECK_FALSE(error.retryable);
  CHECK_FALSE(error.operationMayHaveSucceeded);
}

TEST_CASE("Generic and unknown SFTP status errors do not invent a specific cause",
          "[sftp][errors]")
{
  for (const auto status : {LIBSSH2_FX_FAILURE, 99UL})
  {
    for (const bool mutating : {false, true})
    {
      CAPTURE(status, mutating);

      const auto error = sftp::MakeOperationError(
          "Renaming a remote path", LIBSSH2_ERROR_SFTP_PROTOCOL,
          "SFTP Protocol Error", status, mutating);

      const auto message = Lowercase(error.message);

      CHECK(error.code == RemoteErrorCode::RemoteIo);
      CHECK(error.nativeCode == static_cast<int>(status));
      CHECK_FALSE(error.retryable);
      CHECK(error.operationMayHaveSucceeded == mutating);
      CHECK(error.message.starts_with("Renaming a remote path: "));
      CHECK(error.message.ends_with(" (SFTP status " + std::to_string(status) + ")"));
      CHECK(message.find("permission") == std::string::npos);
      CHECK(message.find("not found") == std::string::npos);
      CHECK(message.find("already exists") == std::string::npos);
      CHECK(message.find("sftp protocol error") == std::string::npos);

      if (status == LIBSSH2_FX_FAILURE)
      {
        CHECK(message.find("failure") != std::string::npos);
      }
      else
      {
        CHECK(message.find("unknown") != std::string::npos);
      }
    }
  }
}

TEST_CASE("Generic SFTP create-file failures do not imply an existing destination",
          "[sftp][errors]")
{
  const auto error = sftp::MakeOperationError(
      "Creating a remote file", LIBSSH2_ERROR_SFTP_PROTOCOL,
      "Failed opening remote file", LIBSSH2_FX_FAILURE, true);

  CHECK(error.code == RemoteErrorCode::RemoteIo);
  CHECK(error.nativeCode == static_cast<int>(LIBSSH2_FX_FAILURE));
  CHECK_FALSE(error.retryable);
  CHECK(error.operationMayHaveSucceeded);
  CHECK(Lowercase(error.message).find("already exists") == std::string::npos);
}

TEST_CASE("Non-SFTP library failures ignore stale server status and preserve safety flags",
          "[sftp][errors]")
{
  struct LibraryCase final
  {
    int nativeCode;
    RemoteErrorCode code;
    bool retryable{};
    bool uncertainWhenMutating{};
  };

  constexpr std::array cases{
      LibraryCase{LIBSSH2_ERROR_AUTHENTICATION_FAILED, RemoteErrorCode::AuthenticationFailed},
      LibraryCase{LIBSSH2_ERROR_KEYFILE_AUTH_FAILED, RemoteErrorCode::AuthenticationFailed},
      LibraryCase{LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED, RemoteErrorCode::AuthenticationFailed},
      LibraryCase{LIBSSH2_ERROR_TIMEOUT, RemoteErrorCode::TimedOut, true, true},
      LibraryCase{LIBSSH2_ERROR_SOCKET_TIMEOUT, RemoteErrorCode::TimedOut, true, true},
      LibraryCase{LIBSSH2_ERROR_SOCKET_DISCONNECT, RemoteErrorCode::ConnectionLost, true, true},
      LibraryCase{LIBSSH2_ERROR_SOCKET_RECV, RemoteErrorCode::ConnectionLost, true, true},
      LibraryCase{LIBSSH2_ERROR_SOCKET_SEND, RemoteErrorCode::ConnectionLost, true, true},
      LibraryCase{LIBSSH2_ERROR_BAD_SOCKET, RemoteErrorCode::ConnectionLost, true, true},
      LibraryCase{LIBSSH2_ERROR_METHOD_NOT_SUPPORTED, RemoteErrorCode::Unsupported},
      LibraryCase{LIBSSH2_ERROR_ALGO_UNSUPPORTED, RemoteErrorCode::Unsupported},
      LibraryCase{LIBSSH2_ERROR_PROTO, RemoteErrorCode::ProtocolError, false, true},
  };

  for (const auto &item : cases)
  {
    for (const bool mutating : {false, true})
    {
      CAPTURE(item.nativeCode, mutating);

      const auto error = sftp::MakeOperationError(
          "SSH operation", item.nativeCode, "Original library detail",
          LIBSSH2_FX_PERMISSION_DENIED, mutating);

      CHECK(error.message == "SSH operation: Original library detail");
      CHECK(error.code == item.code);
      CHECK(error.nativeCode == item.nativeCode);
      CHECK(error.retryable == item.retryable);
      CHECK(error.operationMayHaveSucceeded == (mutating && item.uncertainWhenMutating));
    }
  }
}

TEST_CASE("Missing SFTP status retains library diagnostics without fabricating server codes",
          "[sftp][errors]")
{
  for (const bool mutating : {false, true})
  {
    CAPTURE(mutating);

    const auto error = sftp::MakeOperationError(
        "Reading remote metadata", LIBSSH2_ERROR_SFTP_PROTOCOL,
        "Original library detail", std::nullopt, mutating);

    CHECK(error.message == "Reading remote metadata: Original library detail");
    CHECK(error.code == RemoteErrorCode::ProtocolError);
    CHECK(error.nativeCode == LIBSSH2_ERROR_SFTP_PROTOCOL);
    CHECK_FALSE(error.retryable);
    CHECK(error.operationMayHaveSucceeded == mutating);

    const auto withoutDetail = sftp::MakeOperationError(
        "Reading remote metadata", LIBSSH2_ERROR_SFTP_PROTOCOL,
        {}, std::nullopt, mutating);

    CHECK(withoutDetail.message == "Reading remote metadata failed");
    CHECK(withoutDetail.nativeCode == LIBSSH2_ERROR_SFTP_PROTOCOL);
  }
}

TEST_CASE("Parsed SFTP status reply diagnostics expose the supplied server error",
          "[sftp][errors]")
{
  constexpr std::array<std::string_view, 8> details{
      "SFTP Protocol Error",
      "Failed opening remote file",
      "SFTP READ error",
      "FXP write failed",
      "fsync failed",
      "posix_rename failed",
      "Operation Not Supported",
      "File already exists and SSH_FXP_RENAME_OVERWRITE not specified",
  };

  for (const auto detail : details)
  {
    CAPTURE(detail);

    const auto error = sftp::MakeOperationError(
        "Remote operation", LIBSSH2_ERROR_SFTP_PROTOCOL,
        detail, LIBSSH2_FX_PERMISSION_DENIED, true);

    CHECK(error.message == "Remote operation: Permission denied (SFTP status 3)");
    CHECK(error.code == RemoteErrorCode::PermissionDenied);
    CHECK(error.nativeCode == static_cast<int>(LIBSSH2_FX_PERMISSION_DENIED));
    CHECK_FALSE(error.retryable);
    CHECK_FALSE(error.operationMayHaveSucceeded);
  }
}

TEST_CASE("Malformed or unrecognized SFTP replies never reuse a stale server status",
          "[sftp][errors]")
{
  constexpr std::array<std::string_view, 5> details{
      "SFTP posix_rename packet too short",
      "SFTP Protocol Error: short response",
      "Failed opening remote file: malformed packet",
      "New library error description",
      "",
  };

  for (const auto detail : details)
  {
    for (const bool mutating : {false, true})
    {
      CAPTURE(detail, mutating);

      const auto error = sftp::MakeOperationError(
          "Remote operation", LIBSSH2_ERROR_SFTP_PROTOCOL,
          detail, LIBSSH2_FX_PERMISSION_DENIED, mutating);

      CHECK(error.message == (detail.empty() ? "Remote operation failed"
                                             : "Remote operation: " + std::string{detail}));
      CHECK(error.code == RemoteErrorCode::ProtocolError);
      CHECK(error.nativeCode == LIBSSH2_ERROR_SFTP_PROTOCOL);
      CHECK_FALSE(error.retryable);
      CHECK(error.operationMayHaveSucceeded == mutating);
      CHECK(error.message.find("SFTP status") == std::string::npos);
      CHECK(error.message.find("Permission denied") == std::string::npos);
    }
  }
}

TEST_CASE("SFTP status zero is not reported as the cause of a failed operation",
          "[sftp][errors]")
{
  constexpr std::array<std::string_view, 3> details{
      "SFTP Protocol Error",
      "SFTP posix_rename packet too short",
      "",
  };

  for (const auto detail : details)
  {
    for (const bool mutating : {false, true})
    {
      CAPTURE(detail, mutating);

      const auto error = sftp::MakeOperationError(
          "Remote operation", LIBSSH2_ERROR_SFTP_PROTOCOL,
          detail, LIBSSH2_FX_OK, mutating);

      CHECK(error.message == (detail.empty() ? "Remote operation failed"
                                             : "Remote operation: " + std::string{detail}));
      CHECK(error.code == RemoteErrorCode::ProtocolError);
      CHECK(error.nativeCode == LIBSSH2_ERROR_SFTP_PROTOCOL);
      CHECK_FALSE(error.retryable);
      CHECK(error.operationMayHaveSucceeded == mutating);
      CHECK(error.message.find("SFTP status") == std::string::npos);
    }
  }
}
