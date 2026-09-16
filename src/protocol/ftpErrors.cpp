// SPDX-License-Identifier: MIT

#include "protocol/ftpErrors.hpp"
#include "protocol/ftpSession.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <curl/curl.h>

#include <array>
#include <cstring>
#include <utility>

namespace havremote::ftp
{
  namespace
  {
    [[nodiscard]] RemoteError FtpError(RemoteErrorCode code,
                                       std::string message,
                                       int nativeCode = 0,
                                       bool retryable = false,
                                       bool uncertain = false)
    {
      return RemoteError{.code = code,
                         .message = std::move(message),
                         .nativeCode = nativeCode,
                         .retryable = retryable,
                         .operationMayHaveSucceeded = uncertain};
    }

    [[nodiscard]] bool CanReportUploadReply(const CURLcode code) noexcept
    {
      // A genuine server refusal can explain a simultaneous data-socket
      // failure. It must not replace cancellation, a timeout, a local file
      // error, or a failure establishing/authenticating the connection.
      switch (code)
      {
      case CURLE_UPLOAD_FAILED:
      case CURLE_REMOTE_DISK_FULL:
      case CURLE_PARTIAL_FILE:
      case CURLE_SEND_ERROR:
      case CURLE_RECV_ERROR:
      case CURLE_QUOTE_ERROR:
      case CURLE_FTP_COULDNT_RETR_FILE:
      case CURLE_REMOTE_ACCESS_DENIED:
        return true;

      default:
        return false;
      }
    }

    [[nodiscard]] RemoteError UploadReplyError(
        const long responseCode, const std::string &suffix, const bool mutating)
    {
      auto category = RemoteErrorCode::RemoteIo;

      std::string description;

      bool retryable{};

      switch (responseCode)
      {
      case 421:
        category = RemoteErrorCode::ConnectionLost;
        description = "The FTP server closed the connection";
        retryable = true;
        break;

      case 425:
        category = RemoteErrorCode::ConnectionFailed;
        description = "The FTP server could not open the upload data connection";
        retryable = true;
        break;

      case 426:
        category = RemoteErrorCode::ConnectionLost;
        description = "The FTP server closed the data connection and aborted the upload";
        retryable = true;
        break;

      case 450:
        description = "The FTP server cannot access the upload destination at present";
        break;

      case 451:
        description = "The FTP server aborted the upload because of a local processing error";
        break;

      case 452:
        description = "The FTP server has insufficient storage for the upload";
        break;

      case 552:
        description = "The FTP server aborted the upload because its storage allocation or quota was exceeded";
        break;

      case 500:
      case 502:
      case 504:
        category = RemoteErrorCode::Unsupported;
        description = "The FTP server does not support the upload command";
        break;

      case 530:
        category = RemoteErrorCode::AuthenticationFailed;
        description = "FTP authentication failed";
        break;

      case 532:
        category = RemoteErrorCode::PermissionDenied;
        description = "The FTP server requires an account before storing files";
        break;

      case 550:
        category = RemoteErrorCode::PermissionDenied;
        description = "The FTP server rejected the upload destination or denied access";
        break;

      case 553:
        description = "The FTP server rejected the upload filename";
        break;

      default:
        description = "The FTP server rejected the upload";
        break;
      }

      // A final negative reply does not establish how many bytes the server
      // stored before failing. Never automatically repeat a mutating upload.
      return FtpError(category, description + suffix,
                      static_cast<int>(responseCode), retryable, mutating);
    }
  } // namespace

  bool CanFallbackToList(const int nativeCode, const long responseCode) noexcept
  {
    return nativeCode == CURLE_FTP_COULDNT_RETR_FILE &&
           (responseCode == 500 || responseCode == 501 ||
            responseCode == 502 || responseCode == 504);
  }

  bool CanIgnoreOwnershipListingFailure(
      const RemoteError &error, const long responseCode) noexcept
  {
    if (error.operationMayHaveSucceeded)
    {
      return false;
    }

    switch (error.code)
    {
    case RemoteErrorCode::Unsupported:
    case RemoteErrorCode::PermissionDenied:
    case RemoteErrorCode::NotFound:
      return true;

    case RemoteErrorCode::ProtocolError:
      return CanFallbackToList(error.nativeCode, responseCode) ||
             (error.nativeCode == CURLE_FTP_COULDNT_RETR_FILE && responseCode == 550);

    default:
      return false;
    }
  }

  std::string SocketErrorText(const int error)
  {
#if defined(_WIN32)
    std::array<char, 512> buffer{};

    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(error), 0, buffer.data(),
        static_cast<DWORD>(buffer.size()), nullptr);

    if (length == 0)
    {
      return "Windows socket error " + std::to_string(error);
    }

    std::string result{buffer.data(), length};

    while (!result.empty() &&
           (result.back() == '\r' || result.back() == '\n' ||
            result.back() == ' ' || result.back() == '.'))
    {
      result.pop_back();
    }

    return result;
#else
    return std::strerror(error);
#endif
  }

  std::string FormatFailureDetail(
      const int nativeCode,
      const std::string_view errorBuffer,
      const long responseCode,
      const std::optional<long> osError)
  {
    const auto code = static_cast<CURLcode>(nativeCode);

    std::string result = "CURLcode " +
                         std::to_string(static_cast<int>(code)) + " (" +
                         curl_easy_strerror(code) + ")";

    if (responseCode > 0)
    {
      result += ", FTP response code " + std::to_string(responseCode);
    }

    result += ", error buffer: ";
    result += errorBuffer.empty() ? "<empty>" : std::string{errorBuffer};

    if (osError)
    {
      result += ", OS socket error " + std::to_string(*osError);

      if (*osError != 0)
      {
        result += " (" + SocketErrorText(static_cast<int>(*osError)) + ")";
      }
      else
      {
        result += " (<not set>)";
      }
    }

    return result;
  }

  RemoteError MakeOperationError(
      const int nativeCode,
      const std::string_view detail,
      const long responseCode,
      const bool mutating,
      const std::optional<long> osError,
      const OperationKind operation,
      const UploadReply *uploadReply)
  {
    const auto code = static_cast<CURLcode>(nativeCode);

    auto suffix = ": " + FormatFailureDetail(code, detail, responseCode, osError);

    const bool observedUploadFailure = operation == OperationKind::Stor &&
                                       uploadReply != nullptr &&
                                       uploadReply->code >= 400 &&
                                       uploadReply->code < 600;

    if (observedUploadFailure)
    {
      suffix += ", final STOR reply: " + std::to_string(uploadReply->code);

      if (!uploadReply->text.empty())
      {
        suffix += " " + uploadReply->text;
      }
    }

    if (operation == OperationKind::Stor && CanReportUploadReply(code))
    {
      const auto uploadResponse = observedUploadFailure ? uploadReply->code : responseCode;

      if (uploadResponse >= 400 && uploadResponse < 600)
      {
        return UploadReplyError(uploadResponse, suffix, mutating);
      }

      // Unlike a generic send failure, curl's explicit disk-full code is
      // meaningful even when no final FTP reply was captured.
      if (code == CURLE_REMOTE_DISK_FULL)
      {
        return FtpError(RemoteErrorCode::RemoteIo,
                        "The FTP server has insufficient storage for the upload" + suffix,
                        nativeCode, false, mutating);
      }
    }

    // A server can reject authentication or close the session after login as
    // well. curl reports these command replies differently from USER/PASS
    // failures, but neither permits a compatibility fallback or more work.
    const bool commandRejected = code == CURLE_QUOTE_ERROR ||
                                 code == CURLE_FTP_COULDNT_RETR_FILE ||
                                 code == CURLE_REMOTE_ACCESS_DENIED;

    if (commandRejected && responseCode == 530)
    {
      return FtpError(RemoteErrorCode::AuthenticationFailed,
                      "FTP authentication failed" + suffix,
                      static_cast<int>(responseCode));
    }

    if (commandRejected && responseCode == 421)
    {
      return FtpError(RemoteErrorCode::ConnectionLost,
                      "The FTP server closed the connection" + suffix,
                      static_cast<int>(responseCode), true, mutating);
    }

    switch (code)
    {
    case CURLE_LOGIN_DENIED:
      return FtpError(RemoteErrorCode::AuthenticationFailed,
                      "FTP authentication failed" + suffix,
                      static_cast<int>(code));

    case CURLE_COULDNT_RESOLVE_HOST:
      return FtpError(RemoteErrorCode::NameResolutionFailed,
                      "Could not resolve the FTP server" + suffix,
                      static_cast<int>(code), true);

    case CURLE_COULDNT_CONNECT:
      return FtpError(RemoteErrorCode::ConnectionFailed,
                      "Could not connect to the FTP server" + suffix,
                      static_cast<int>(code), true);

    case CURLE_FTP_PORT_FAILED:
      return FtpError(
          RemoteErrorCode::ConnectionFailed,
          "Could not negotiate the active FTP address and port" + suffix,
          static_cast<int>(code), true);

    case CURLE_FTP_ACCEPT_FAILED:
      return FtpError(
          RemoteErrorCode::ConnectionFailed,
          "The FTP server could not establish the active data connection" +
              suffix,
          static_cast<int>(code), true, mutating);

    case CURLE_FTP_ACCEPT_TIMEOUT:
      return FtpError(
          RemoteErrorCode::TimedOut,
          "Timed out waiting for the FTP server to establish the active data connection" +
              suffix,
          static_cast<int>(code), true, mutating);

    case CURLE_OPERATION_TIMEDOUT:
      return FtpError(RemoteErrorCode::TimedOut,
                      "The FTP operation timed out" + suffix,
                      static_cast<int>(code), true, mutating);

    case CURLE_PARTIAL_FILE:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
      return FtpError(RemoteErrorCode::ConnectionLost,
                      "The FTP connection was interrupted" + suffix,
                      static_cast<int>(code), true, mutating);

    case CURLE_SSL_CACERT:
#if CURLE_PEER_FAILED_VERIFICATION != CURLE_SSL_CACERT
    case CURLE_PEER_FAILED_VERIFICATION:
#endif
    case CURLE_SSL_CERTPROBLEM:
      return FtpError(RemoteErrorCode::CertificateInvalid,
                      "The FTPS certificate is not trusted" + suffix,
                      static_cast<int>(code));

    case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
      return FtpError(RemoteErrorCode::HostKeyChanged,
                      "The FTPS endpoint public key no longer matches its saved PIN" + suffix,
                      static_cast<int>(code));

    case CURLE_REMOTE_FILE_NOT_FOUND:
      return FtpError(RemoteErrorCode::NotFound,
                      "The remote path was not found" + suffix,
                      static_cast<int>(code));

    case CURLE_REMOTE_ACCESS_DENIED:
      return FtpError(RemoteErrorCode::PermissionDenied,
                      "The FTP server denied the operation" + suffix,
                      static_cast<int>(code));

    case CURLE_ABORTED_BY_CALLBACK:
      return FtpError(RemoteErrorCode::Cancelled, "The FTP operation was cancelled",
                      static_cast<int>(code), false, mutating);

    case CURLE_QUOTE_ERROR:
      if (responseCode == 500 || responseCode == 502 || responseCode == 504)
      {
        return FtpError(RemoteErrorCode::Unsupported,
                        "The FTP server does not support the requested command" + suffix,
                        static_cast<int>(responseCode));
      }

      if (responseCode == 550)
      {
        if (operation == OperationKind::Rename)
        {
          return FtpError(
              RemoteErrorCode::PermissionDenied,
              "The FTP server rejected the rename source or destination "
              "path (RNFR/RNTO)" +
                  suffix,
              static_cast<int>(responseCode));
        }

        return FtpError(RemoteErrorCode::PermissionDenied,
                        "The FTP server rejected the file operation" + suffix,
                        static_cast<int>(responseCode));
      }

      if (responseCode >= 400)
      {
        if (operation == OperationKind::Rename)
        {
          return FtpError(RemoteErrorCode::ProtocolError,
                          "The FTP server rejected the rename request "
                          "(RNFR/RNTO)" +
                              suffix,
                          static_cast<int>(responseCode));
        }

        return FtpError(RemoteErrorCode::ProtocolError,
                        "The FTP server rejected the requested command" + suffix,
                        static_cast<int>(responseCode));
      }

      [[fallthrough]];
    default:
      return FtpError(RemoteErrorCode::ProtocolError,
                      "The libcurl FTP operation failed" + suffix,
                      static_cast<int>(code), false, mutating);
    }
  }

  RemoteError MakeUploadResumeError(
      const int nativeCode,
      const std::string_view errorBuffer,
      const long responseCode,
      const std::optional<long> osError,
      const UploadResumeCommand command)
  {
    const auto suffix = ": " +
                        FormatFailureDetail(nativeCode, errorBuffer, responseCode, osError);

    // A header callback can reject REST before curl classifies its reply, and
    // rejected STOR uses a different curl error from an ordinary quote. The
    // server's authentication/closure replies still identify these failures.
    if (responseCode == 530)
    {
      return FtpError(RemoteErrorCode::AuthenticationFailed,
                      "FTP authentication failed" + suffix,
                      static_cast<int>(responseCode));
    }

    if (responseCode == 421)
    {
      return FtpError(RemoteErrorCode::ConnectionLost,
                      "The FTP server closed the connection" + suffix,
                      static_cast<int>(responseCode), true);
    }

    const bool invalidRestReply = command == UploadResumeCommand::Rest &&
                                  responseCode < 400 && responseCode != 350;

    if (invalidRestReply || responseCode == 500 || responseCode == 501 ||
        responseCode == 502 || responseCode == 503 || responseCode == 504 ||
        responseCode == 550)
    {
      std::string message =
          "The FTP server does not support upload resume for this file "
          "(REST + STOR), or resume is disabled. ";

      if (invalidRestReply)
      {
        message += "REST did not return the required 350 response";
      }
      else if (command == UploadResumeCommand::Rest)
      {
        message += "REST was rejected";
      }
      else
      {
        message += "STOR was rejected after REST";
      }

      return FtpError(RemoteErrorCode::Unsupported, message + suffix,
                      responseCode > 0 ? static_cast<int>(responseCode) : nativeCode);
    }

    // Temporary, storage and filename rejections do not prove REST + STOR is
    // unsupported. No data was transferred, so the rejection is definite.
    return MakeOperationError(nativeCode, errorBuffer, responseCode, false,
                              osError, OperationKind::Stor);
  }
} // namespace havremote::ftp
