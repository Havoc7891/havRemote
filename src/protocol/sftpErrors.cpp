// SPDX-License-Identifier: MIT

#include "protocol/sftpErrors.hpp"

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <string>

namespace havremote::sftp
{
  namespace
  {
    [[nodiscard]] bool ReportsStatusReply(const std::string_view detail)
    {
      // libssh2 also returns SFTP_PROTOCOL for malformed packets, sometimes
      // leaving an earlier request's last SFTP status in place. These exact
      // messages are emitted after it has parsed a non-OK STATUS reply. If a
      // future libssh2 version changes them, retain its own diagnostic instead
      // of treating a potentially stale status as the cause of this failure.
      return detail == "SFTP Protocol Error" ||
             detail == "Failed opening remote file" ||
             detail == "SFTP READ error" ||
             detail == "FXP write failed" ||
             detail == "fsync failed" ||
             detail == "posix_rename failed" ||
             detail == "Operation Not Supported" ||
             detail == "File already exists and SSH_FXP_RENAME_OVERWRITE not specified";
    }

    [[nodiscard]] std::string_view StatusDescription(const unsigned long status)
    {
      switch (status)
      {
      case LIBSSH2_FX_EOF:
        return "End of file";

      case LIBSSH2_FX_NO_SUCH_FILE:
        return "No such file";

      case LIBSSH2_FX_PERMISSION_DENIED:
        return "Permission denied";

      case LIBSSH2_FX_FAILURE:
        return "Failure";

      case LIBSSH2_FX_BAD_MESSAGE:
        return "Invalid SFTP message";

      case LIBSSH2_FX_NO_CONNECTION:
        return "No connection";

      case LIBSSH2_FX_CONNECTION_LOST:
        return "Connection lost";

      case LIBSSH2_FX_OP_UNSUPPORTED:
        return "Operation unsupported";

      case LIBSSH2_FX_INVALID_HANDLE:
        return "Invalid handle";

      case LIBSSH2_FX_NO_SUCH_PATH:
        return "No such path";

      case LIBSSH2_FX_FILE_ALREADY_EXISTS:
        return "File already exists";

      case LIBSSH2_FX_WRITE_PROTECT:
        return "Write protected";

      case LIBSSH2_FX_NO_MEDIA:
        return "No media available";

      case LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM:
        return "No space on filesystem";

      case LIBSSH2_FX_QUOTA_EXCEEDED:
        return "Quota exceeded";

      case LIBSSH2_FX_UNKNOWN_PRINCIPAL:
        return "Unknown principal";

      case LIBSSH2_FX_LOCK_CONFLICT:
        return "Lock conflict";

      case LIBSSH2_FX_DIR_NOT_EMPTY:
        return "Directory not empty";

      case LIBSSH2_FX_NOT_A_DIRECTORY:
        return "Not a directory";

      case LIBSSH2_FX_INVALID_FILENAME:
        return "Invalid filename";

      case LIBSSH2_FX_LINK_LOOP:
        return "Symbolic link loop";

      default:
        return "Unknown server error";
      }
    }
  } // namespace

  RemoteError MakeOperationError(
      const std::string_view operation,
      const int nativeCode,
      const std::string_view libraryDetail,
      const std::optional<unsigned long> status,
      const bool mutating)
  {
    RemoteError error;
    error.nativeCode = nativeCode;
    error.message = std::string{operation};

    if (nativeCode == LIBSSH2_ERROR_SFTP_PROTOCOL && status &&
        *status != LIBSSH2_FX_OK && ReportsStatusReply(libraryDetail))
    {
      error.message += ": ";
      error.message += StatusDescription(*status);
      error.message += " (SFTP status " + std::to_string(*status) + ")";
      error.nativeCode = static_cast<int>(*status);

      switch (*status)
      {
      case LIBSSH2_FX_NO_SUCH_FILE:
      case LIBSSH2_FX_NO_SUCH_PATH:
        error.code = RemoteErrorCode::NotFound;
        break;

      case LIBSSH2_FX_PERMISSION_DENIED:
      case LIBSSH2_FX_WRITE_PROTECT:
        error.code = RemoteErrorCode::PermissionDenied;
        break;

      case LIBSSH2_FX_FILE_ALREADY_EXISTS:
        error.code = RemoteErrorCode::AlreadyExists;
        break;

      case LIBSSH2_FX_DIR_NOT_EMPTY:
        error.code = RemoteErrorCode::DirectoryNotEmpty;
        break;

      case LIBSSH2_FX_NOT_A_DIRECTORY:
        error.code = RemoteErrorCode::NotDirectory;
        break;

      case LIBSSH2_FX_CONNECTION_LOST:
      case LIBSSH2_FX_NO_CONNECTION:
        error.code = RemoteErrorCode::ConnectionLost;
        error.retryable = true;
        error.operationMayHaveSucceeded = mutating;
        break;

      case LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM:
      case LIBSSH2_FX_QUOTA_EXCEEDED:
        error.code = RemoteErrorCode::RemoteIo;
        break;

      case LIBSSH2_FX_OP_UNSUPPORTED:
        error.code = RemoteErrorCode::Unsupported;
        break;

      default:
        error.code = RemoteErrorCode::RemoteIo;
        error.operationMayHaveSucceeded = mutating;
        break;
      }

      return error;
    }

    // Authentication/transport errors must not be labelled with a stale SFTP
    // status left over from an earlier request on the session.
    if (libraryDetail.empty())
    {
      error.message += " failed";
    }
    else
    {
      error.message += ": ";
      error.message += libraryDetail;
    }

    if (nativeCode == LIBSSH2_ERROR_AUTHENTICATION_FAILED ||
        nativeCode == LIBSSH2_ERROR_KEYFILE_AUTH_FAILED ||
        nativeCode == LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED)
    {
      error.code = RemoteErrorCode::AuthenticationFailed;
    }
    else if (nativeCode == LIBSSH2_ERROR_TIMEOUT || nativeCode == LIBSSH2_ERROR_SOCKET_TIMEOUT)
    {
      error.code = RemoteErrorCode::TimedOut;
      error.retryable = true;
      error.operationMayHaveSucceeded = mutating;
    }
    else if (nativeCode == LIBSSH2_ERROR_SOCKET_DISCONNECT ||
             nativeCode == LIBSSH2_ERROR_SOCKET_RECV || nativeCode == LIBSSH2_ERROR_SOCKET_SEND ||
             nativeCode == LIBSSH2_ERROR_BAD_SOCKET)
    {
      error.code = RemoteErrorCode::ConnectionLost;
      error.retryable = true;
      error.operationMayHaveSucceeded = mutating;
    }
    else if (nativeCode == LIBSSH2_ERROR_METHOD_NOT_SUPPORTED ||
             nativeCode == LIBSSH2_ERROR_ALGO_UNSUPPORTED)
    {
      error.code = RemoteErrorCode::Unsupported;
    }
    else
    {
      error.code = RemoteErrorCode::ProtocolError;
      error.operationMayHaveSucceeded = mutating;
    }

    return error;
  }
} // namespace havremote::sftp
