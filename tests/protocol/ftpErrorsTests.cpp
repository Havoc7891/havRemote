// SPDX-License-Identifier: MIT

#include "protocol/ftpErrors.hpp"
#include "protocol/ftpSession.hpp"

#include <catch2/catch_test_macros.hpp>

#include <curl/curl.h>

#include <array>
#include <cerrno>
#include <optional>
#include <string>
#include <string_view>

using namespace havremote;

namespace
{
  std::string CurlLabel(const CURLcode code)
  {
    return "CURLcode " + std::to_string(static_cast<int>(code)) + " (" +
           curl_easy_strerror(code) + ")";
  }
} // namespace

TEST_CASE("FTP failure details retain exact curl response buffer and socket diagnostics",
          "[ftp][errors]")
{
  const auto label = CurlLabel(CURLE_SEND_ERROR);

  CHECK(ftp::FormatFailureDetail(CURLE_SEND_ERROR, "Send failed", 200, std::nullopt) ==
        label + ", FTP response code 200, error buffer: Send failed");
  CHECK(ftp::FormatFailureDetail(CURLE_SEND_ERROR, {}, 550, std::nullopt) ==
        label + ", FTP response code 550, error buffer: <empty>");
  CHECK(ftp::FormatFailureDetail(CURLE_SEND_ERROR, {}, 0, std::nullopt) ==
        label + ", error buffer: <empty>");
  CHECK(ftp::FormatFailureDetail(CURLE_SEND_ERROR, "Send failed", -1, std::nullopt) ==
        label + ", error buffer: Send failed");
  CHECK(ftp::FormatFailureDetail(CURLE_SEND_ERROR, "Send failed", 200, 0L) ==
        label + ", FTP response code 200, error buffer: Send failed, OS socket error 0 (<not set>)");

  // System error descriptions vary by platform and locale. Verify composition
  // without assuming particular wording.
  constexpr long socketError = EACCES;

  CHECK(ftp::FormatFailureDetail(CURLE_SEND_ERROR, "Send failed", 200, socketError) ==
        label + ", FTP response code 200, error buffer: Send failed, OS socket error " +
            std::to_string(socketError) + " (" +
            ftp::SocketErrorText(static_cast<int>(socketError)) + ")");
}

TEST_CASE("FTP upload resume rejection reports unsupported restart semantics without retrying",
          "[ftp][errors][resume]")
{
  for (const auto command : {ftp::UploadResumeCommand::Rest, ftp::UploadResumeCommand::Stor})
  {
    for (const long response : {500L, 501L, 502L, 503L, 504L, 550L})
    {
      CAPTURE(command, response);

      const auto nativeCode = command == ftp::UploadResumeCommand::Rest
                                  ? CURLE_QUOTE_ERROR
                                  : CURLE_UPLOAD_FAILED;

      const auto error = ftp::MakeUploadResumeError(
          nativeCode, "Original resume rejection", response, EACCES, command);

      CHECK(error.code == RemoteErrorCode::Unsupported);
      CHECK(error.nativeCode == response);
      CHECK_FALSE(error.retryable);
      CHECK_FALSE(error.operationMayHaveSucceeded);
      CHECK(error.message.find("does not support upload resume for this file (REST + STOR), or resume is disabled") !=
            std::string::npos);
      CHECK(error.message.find(command == ftp::UploadResumeCommand::Rest
                                   ? "REST was rejected"
                                   : "STOR was rejected after REST") != std::string::npos);
      CHECK(error.message.ends_with(ftp::FormatFailureDetail(
          nativeCode, "Original resume rejection", response, EACCES)));
    }
  }
}

TEST_CASE("A confirmed final STOR storage reply explains a data-socket failure",
          "[ftp][errors][upload-reply]")
{
  for (const auto nativeCode : {CURLE_SEND_ERROR, CURLE_RECV_ERROR,
                                CURLE_PARTIAL_FILE, CURLE_UPLOAD_FAILED,
                                CURLE_REMOTE_DISK_FULL})
  {
    for (const bool mutating : {false, true})
    {
      CAPTURE(nativeCode, mutating);

      const ftp::UploadReply reply{552, "Transfer aborted: quota exceeded"};

      const auto error = ftp::MakeOperationError(
          nativeCode, "Send failure: Connection was aborted", 150, mutating,
          0L, ftp::OperationKind::Stor, &reply);

      CHECK(error.code == RemoteErrorCode::RemoteIo);
      CHECK(error.nativeCode == 552);
      CHECK_FALSE(error.retryable);
      CHECK(error.operationMayHaveSucceeded == mutating);
      CHECK(error.message.starts_with(
          "The FTP server aborted the upload because its storage allocation or quota was exceeded: "));
      CHECK(error.message.ends_with(
          ftp::FormatFailureDetail(nativeCode, "Send failure: Connection was aborted", 150, 0L) +
          ", final STOR reply: 552 Transfer aborted: quota exceeded"));
    }
  }
}

TEST_CASE("STOR reply classifications use protocol codes rather than body text guesses",
          "[ftp][errors][upload-reply]")
{
  struct Case final
  {
    long reply;
    RemoteErrorCode category;
    std::string_view description;
    bool retryable{};
  };

  constexpr std::array cases{
      Case{421, RemoteErrorCode::ConnectionLost,
           "The FTP server closed the connection", true},
      Case{425, RemoteErrorCode::ConnectionFailed,
           "The FTP server could not open the upload data connection", true},
      Case{426, RemoteErrorCode::ConnectionLost,
           "The FTP server closed the data connection and aborted the upload", true},
      Case{450, RemoteErrorCode::RemoteIo,
           "The FTP server cannot access the upload destination at present"},
      Case{451, RemoteErrorCode::RemoteIo,
           "The FTP server aborted the upload because of a local processing error"},
      Case{452, RemoteErrorCode::RemoteIo,
           "The FTP server has insufficient storage for the upload"},
      Case{500, RemoteErrorCode::Unsupported,
           "The FTP server does not support the upload command"},
      Case{502, RemoteErrorCode::Unsupported,
           "The FTP server does not support the upload command"},
      Case{504, RemoteErrorCode::Unsupported,
           "The FTP server does not support the upload command"},
      Case{530, RemoteErrorCode::AuthenticationFailed,
           "FTP authentication failed"},
      Case{532, RemoteErrorCode::PermissionDenied,
           "The FTP server requires an account before storing files"},
      Case{550, RemoteErrorCode::PermissionDenied,
           "The FTP server rejected the upload destination or denied access"},
      Case{552, RemoteErrorCode::RemoteIo,
           "The FTP server aborted the upload because its storage allocation or quota was exceeded"},
      Case{553, RemoteErrorCode::RemoteIo,
           "The FTP server rejected the upload filename"},
      Case{499, RemoteErrorCode::RemoteIo, "The FTP server rejected the upload"},
      Case{599, RemoteErrorCode::RemoteIo, "The FTP server rejected the upload"},
  };

  for (const auto &item : cases)
  {
    for (const bool capturedSeparately : {false, true})
    {
      CAPTURE(item.reply, capturedSeparately);

      // Empty bodies still classify correctly, including non-English servers
      const ftp::UploadReply reply{item.reply, {}};

      const long originalResponse = capturedSeparately ? 150 : item.reply;

      const auto error = ftp::MakeOperationError(
          CURLE_SEND_ERROR, {}, originalResponse, true, std::nullopt,
          ftp::OperationKind::Stor, capturedSeparately ? &reply : nullptr);

      CHECK(error.code == item.category);
      CHECK(error.nativeCode == item.reply);
      CHECK(error.retryable == item.retryable);
      CHECK(error.operationMayHaveSucceeded);

      auto expected = std::string{item.description} + ": " +
                      ftp::FormatFailureDetail(CURLE_SEND_ERROR, {}, originalResponse);

      if (capturedSeparately)
      {
        expected += ", final STOR reply: " + std::to_string(item.reply);
      }

      CHECK(error.message == expected);

      if (item.reply == 451)
      {
        CHECK(error.message.find("storage") == std::string::npos);
        CHECK(error.message.find("space") == std::string::npos);
        CHECK(error.message.find("quota") == std::string::npos);
      }
    }
  }
}

TEST_CASE("A final observed STOR reply takes precedence over a stale curl response",
          "[ftp][errors][upload-reply]")
{
  const ftp::UploadReply observed{452, "Insufficient storage"};

  const auto error = ftp::MakeOperationError(
      CURLE_SEND_ERROR, {}, 550, true, std::nullopt,
      ftp::OperationKind::Stor, &observed);

  CHECK(error.code == RemoteErrorCode::RemoteIo);
  CHECK(error.nativeCode == 452);
  CHECK(error.message.find("FTP response code 550") != std::string::npos);
  CHECK(error.message.ends_with(", final STOR reply: 452 Insufficient storage"));
}

TEST_CASE("An unobserved upload send failure does not imply storage exhaustion",
          "[ftp][errors][upload-reply]")
{
  const auto original = ftp::MakeOperationError(
      CURLE_SEND_ERROR, "Connection aborted", 150, true, 0L, ftp::OperationKind::Stor);

  CHECK(original.code == RemoteErrorCode::ConnectionLost);
  CHECK(original.nativeCode == CURLE_SEND_ERROR);
  CHECK(original.retryable);
  CHECK(original.operationMayHaveSucceeded);
  CHECK(original.message == "The FTP connection was interrupted: " +
                                ftp::FormatFailureDetail(CURLE_SEND_ERROR, "Connection aborted", 150, 0L));

  for (const long unusableCode : {0L, 150L, 226L, 350L, 399L, 600L, 999L})
  {
    CAPTURE(unusableCode);

    const ftp::UploadReply unusable{unusableCode, "quota exceeded"};

    const auto error = ftp::MakeOperationError(
        CURLE_SEND_ERROR, "Connection aborted", 150, true, 0L,
        ftp::OperationKind::Stor, &unusable);

    CHECK(error.code == original.code);
    CHECK(error.nativeCode == original.nativeCode);
    CHECK(error.retryable == original.retryable);
    CHECK(error.operationMayHaveSucceeded == original.operationMayHaveSucceeded);
    CHECK(error.message == original.message);
  }
}

TEST_CASE("An upload reply does not replace cancellation timeout or local failure priorities",
          "[ftp][errors][upload-reply]")
{
  const ftp::UploadReply reply{552, "Insufficient allocation"};

  for (const auto nativeCode : {CURLE_ABORTED_BY_CALLBACK, CURLE_OPERATION_TIMEDOUT,
                                CURLE_FTP_ACCEPT_TIMEOUT, CURLE_READ_ERROR,
                                CURLE_WRITE_ERROR, CURLE_FILE_COULDNT_READ_FILE,
                                CURLE_OUT_OF_MEMORY, CURLE_URL_MALFORMAT,
                                CURLE_BAD_FUNCTION_ARGUMENT, CURLE_LOGIN_DENIED,
                                CURLE_COULDNT_CONNECT, CURLE_SSL_CACERT,
                                CURLE_SSL_CERTPROBLEM, CURLE_SSL_PINNEDPUBKEYNOTMATCH})
  {
    for (const bool mutating : {false, true})
    {
      CAPTURE(nativeCode, mutating);

      const auto original = ftp::MakeOperationError(
          nativeCode, "Original failure", 150, mutating, 0L, ftp::OperationKind::Stor);

      const auto error = ftp::MakeOperationError(
          nativeCode, "Original failure", 150, mutating, 0L,
          ftp::OperationKind::Stor, &reply);

      CHECK(error.code == original.code);
      CHECK(error.nativeCode == original.nativeCode);
      CHECK(error.retryable == original.retryable);
      CHECK(error.operationMayHaveSucceeded == original.operationMayHaveSucceeded);

      if (nativeCode == CURLE_ABORTED_BY_CALLBACK)
      {
        CHECK(error.message == original.message);
      }
      else
      {
        CHECK(error.message == original.message + ", final STOR reply: 552 Insufficient allocation");
      }
    }
  }
}

TEST_CASE("Upload reply observations never alter other FTP operations",
          "[ftp][errors][upload-reply]")
{
  const ftp::UploadReply reply{552, "Insufficient allocation"};

  for (const auto operation : {ftp::OperationKind::ControlCommand, ftp::OperationKind::Rename,
                               ftp::OperationKind::Nlst, ftp::OperationKind::Mlsd,
                               ftp::OperationKind::List, ftp::OperationKind::Retr})
  {
    for (const auto nativeCode : {CURLE_SEND_ERROR, CURLE_QUOTE_ERROR, CURLE_REMOTE_DISK_FULL})
    {
      CAPTURE(operation, nativeCode);

      const auto original = ftp::MakeOperationError(
          nativeCode, "Original failure", 550, true, 0L, operation);

      const auto error = ftp::MakeOperationError(
          nativeCode, "Original failure", 550, true, 0L, operation, &reply);

      CHECK(error.code == original.code);
      CHECK(error.nativeCode == original.nativeCode);
      CHECK(error.retryable == original.retryable);
      CHECK(error.operationMayHaveSucceeded == original.operationMayHaveSucceeded);
      CHECK(error.message == original.message);
    }
  }
}

TEST_CASE("Curl's explicit remote disk-full code remains meaningful without a final reply",
          "[ftp][errors][upload-reply]")
{
  for (const long response : {0L, 150L})
  {
    for (const bool mutating : {false, true})
    {
      CAPTURE(response, mutating);

      const auto error = ftp::MakeOperationError(
          CURLE_REMOTE_DISK_FULL, {}, response, mutating,
          std::nullopt, ftp::OperationKind::Stor);

      CHECK(error.code == RemoteErrorCode::RemoteIo);
      CHECK(error.nativeCode == CURLE_REMOTE_DISK_FULL);
      CHECK_FALSE(error.retryable);
      CHECK(error.operationMayHaveSucceeded == mutating);
      CHECK(error.message == "The FTP server has insufficient storage for the upload: " +
                                 ftp::FormatFailureDetail(CURLE_REMOTE_DISK_FULL, {}, response));
    }
  }
}

TEST_CASE("FTP upload resume requires a 350 restart reply before writing the remainder",
          "[ftp][errors][resume]")
{
  for (const long response : {0L, 150L, 200L, 250L, 331L, 399L})
  {
    CAPTURE(response);

    const auto error = ftp::MakeUploadResumeError(
        CURLE_WRITE_ERROR, "Header callback rejected reply", response, std::nullopt,
        ftp::UploadResumeCommand::Rest);

    CHECK(error.code == RemoteErrorCode::Unsupported);
    CHECK(error.nativeCode == (response > 0 ? static_cast<int>(response)
                                            : static_cast<int>(CURLE_WRITE_ERROR)));
    CHECK_FALSE(error.retryable);
    CHECK_FALSE(error.operationMayHaveSucceeded);
    CHECK(error.message.find("does not support upload resume") != std::string::npos);
    CHECK(error.message.find("REST did not return the required 350 response") != std::string::npos);
    CHECK(error.message.ends_with(ftp::FormatFailureDetail(
        CURLE_WRITE_ERROR, "Header callback rejected reply", response, std::nullopt)));
  }
}

TEST_CASE("FTP upload resume preserves authentication and connection closure rejections",
          "[ftp][errors][resume]")
{
  for (const auto command : {ftp::UploadResumeCommand::Rest, ftp::UploadResumeCommand::Stor})
  {
    for (const auto nativeCode : {CURLE_WRITE_ERROR, CURLE_QUOTE_ERROR, CURLE_UPLOAD_FAILED})
    {
      for (const long response : {421L, 530L})
      {
        CAPTURE(command, nativeCode, response);

        const auto error = ftp::MakeUploadResumeError(
            nativeCode, "Server session rejected command", response, 0L, command);

        CHECK(error.code == (response == 530 ? RemoteErrorCode::AuthenticationFailed
                                             : RemoteErrorCode::ConnectionLost));
        CHECK(error.nativeCode == response);
        CHECK(error.retryable == (response == 421));
        CHECK_FALSE(error.operationMayHaveSucceeded);
        CHECK(error.message.find("does not support upload resume") == std::string::npos);
        CHECK(error.message.ends_with(ftp::FormatFailureDetail(
            nativeCode, "Server session rejected command", response, 0L)));
      }
    }
  }
}

TEST_CASE("FTP upload resume does not misclassify temporary storage or filename rejections",
          "[ftp][errors][resume]")
{
  for (const auto command : {ftp::UploadResumeCommand::Rest, ftp::UploadResumeCommand::Stor})
  {
    for (const long response : {400L, 425L, 426L, 450L, 451L, 452L, 532L, 552L, 553L})
    {
      CAPTURE(command, response);

      const auto nativeCode = command == ftp::UploadResumeCommand::Rest
                                  ? CURLE_QUOTE_ERROR
                                  : CURLE_UPLOAD_FAILED;

      const auto error = ftp::MakeUploadResumeError(
          nativeCode, "Original server refusal", response, std::nullopt, command);

      const auto ordinaryError = ftp::MakeOperationError(
          nativeCode, "Original server refusal", response, false,
          std::nullopt, ftp::OperationKind::Stor);

      CHECK(error.code == ordinaryError.code);
      CHECK(error.code != RemoteErrorCode::Unsupported);
      CHECK(error.nativeCode == ordinaryError.nativeCode);
      CHECK(error.retryable == ordinaryError.retryable);
      CHECK_FALSE(error.operationMayHaveSucceeded);
      CHECK(error.message == ordinaryError.message);
    }
  }
}

TEST_CASE("FTP curl failures retain classifications and destructive-operation safety flags",
          "[ftp][errors]")
{
  struct Case final
  {
    CURLcode curlCode;
    RemoteErrorCode code;
    std::string_view description;
    bool retryable{};
    bool uncertainWhenMutating{};
  };

  constexpr std::array cases{
      Case{CURLE_LOGIN_DENIED, RemoteErrorCode::AuthenticationFailed,
           "FTP authentication failed"},
      Case{CURLE_COULDNT_RESOLVE_HOST, RemoteErrorCode::NameResolutionFailed,
           "Could not resolve the FTP server", true},
      Case{CURLE_COULDNT_CONNECT, RemoteErrorCode::ConnectionFailed,
           "Could not connect to the FTP server", true},
      Case{CURLE_FTP_PORT_FAILED, RemoteErrorCode::ConnectionFailed,
           "Could not negotiate the active FTP address and port", true},
      Case{CURLE_FTP_ACCEPT_FAILED, RemoteErrorCode::ConnectionFailed,
           "The FTP server could not establish the active data connection", true, true},
      Case{CURLE_FTP_ACCEPT_TIMEOUT, RemoteErrorCode::TimedOut,
           "Timed out waiting for the FTP server to establish the active data connection", true, true},
      Case{CURLE_OPERATION_TIMEDOUT, RemoteErrorCode::TimedOut,
           "The FTP operation timed out", true, true},
      Case{CURLE_PARTIAL_FILE, RemoteErrorCode::ConnectionLost,
           "The FTP connection was interrupted", true, true},
      Case{CURLE_RECV_ERROR, RemoteErrorCode::ConnectionLost,
           "The FTP connection was interrupted", true, true},
      Case{CURLE_SEND_ERROR, RemoteErrorCode::ConnectionLost,
           "The FTP connection was interrupted", true, true},
      Case{CURLE_SSL_CACERT, RemoteErrorCode::CertificateInvalid,
           "The FTPS certificate is not trusted"},
      Case{CURLE_PEER_FAILED_VERIFICATION, RemoteErrorCode::CertificateInvalid,
           "The FTPS certificate is not trusted"},
      Case{CURLE_SSL_CERTPROBLEM, RemoteErrorCode::CertificateInvalid,
           "The FTPS certificate is not trusted"},
      Case{CURLE_SSL_PINNEDPUBKEYNOTMATCH, RemoteErrorCode::HostKeyChanged,
           "The FTPS endpoint public key no longer matches its saved PIN"},
      Case{CURLE_REMOTE_FILE_NOT_FOUND, RemoteErrorCode::NotFound,
           "The remote path was not found"},
      Case{CURLE_REMOTE_ACCESS_DENIED, RemoteErrorCode::PermissionDenied,
           "The FTP server denied the operation"},
  };

  for (const auto &item : cases)
  {
    for (const bool mutating : {false, true})
    {
      CAPTURE(item.curlCode, mutating);

      const auto error = ftp::MakeOperationError(
          item.curlCode, "Original curl error", 200, mutating, 0L,
          ftp::OperationKind::ControlCommand);

      CHECK(error.code == item.code);
      CHECK(error.nativeCode == static_cast<int>(item.curlCode));
      CHECK(error.retryable == item.retryable);
      CHECK(error.operationMayHaveSucceeded == (mutating && item.uncertainWhenMutating));
      CHECK(error.message == std::string{item.description} + ": " +
                                 ftp::FormatFailureDetail(item.curlCode, "Original curl error", 200, 0L));
    }
  }
}

TEST_CASE("FTP quote failures distinguish rename paths from active data transport",
          "[ftp][errors]")
{
  for (const auto operation : {ftp::OperationKind::ControlCommand, ftp::OperationKind::Rename})
  {
    for (const long response : {421L, 500L, 502L, 504L, 550L, 400L, 450L, 553L, 399L, 0L})
    {
      for (const bool mutating : {false, true})
      {
        CAPTURE(operation, response, mutating);

        const auto error = ftp::MakeOperationError(
            CURLE_QUOTE_ERROR, "QUOT command failed", response, mutating,
            std::nullopt, operation);

        auto expectedCode = RemoteErrorCode::ProtocolError;
        auto expectedNative = static_cast<int>(response);

        bool retryable{};
        bool uncertain{};

        std::string description;

        if (response == 421)
        {
          expectedCode = RemoteErrorCode::ConnectionLost;
          retryable = true;
          uncertain = mutating;
          description = "The FTP server closed the connection";
        }
        else if (response == 500 || response == 502 || response == 504)
        {
          expectedCode = RemoteErrorCode::Unsupported;
          description = "The FTP server does not support the requested command";
        }
        else if (response == 550)
        {
          expectedCode = RemoteErrorCode::PermissionDenied;
          description = operation == ftp::OperationKind::Rename
                            ? "The FTP server rejected the rename source or destination path (RNFR/RNTO)"
                            : "The FTP server rejected the file operation";
        }
        else if (response >= 400)
        {
          description = operation == ftp::OperationKind::Rename
                            ? "The FTP server rejected the rename request (RNFR/RNTO)"
                            : "The FTP server rejected the requested command";
        }
        else
        {
          expectedNative = static_cast<int>(CURLE_QUOTE_ERROR);
          uncertain = mutating;
          description = "The libcurl FTP operation failed";
        }

        CHECK(error.code == expectedCode);
        CHECK(error.nativeCode == expectedNative);
        CHECK(error.retryable == retryable);
        CHECK(error.operationMayHaveSucceeded == uncertain);
        CHECK(error.message == description + ": " +
                                   ftp::FormatFailureDetail(CURLE_QUOTE_ERROR, "QUOT command failed",
                                                            response, std::nullopt));
        CHECK(error.message.find("active") == std::string::npos);
        CHECK(error.message.find("data connection") == std::string::npos);
      }
    }
  }
}

TEST_CASE("FTP callback cancellation retains its terse message and mutation uncertainty",
          "[ftp][errors]")
{
  for (const bool mutating : {false, true})
  {
    CAPTURE(mutating);

    const auto error = ftp::MakeOperationError(
        CURLE_ABORTED_BY_CALLBACK, "Callback aborted", 550, mutating,
        0L, ftp::OperationKind::Stor);

    CHECK(error.message == "The FTP operation was cancelled");
    CHECK(error.code == RemoteErrorCode::Cancelled);
    CHECK(error.nativeCode == static_cast<int>(CURLE_ABORTED_BY_CALLBACK));
    CHECK_FALSE(error.retryable);
    CHECK(error.operationMayHaveSucceeded == mutating);
    CHECK(error.message.find("CURLcode") == std::string::npos);
    CHECK(error.message.find("550") == std::string::npos);
  }
}

TEST_CASE("Unmapped curl failures preserve protocol fallback diagnostics and mutation uncertainty",
          "[ftp][errors]")
{
  for (const auto code : {CURLE_URL_MALFORMAT, CURLE_WRITE_ERROR, CURLE_BAD_FUNCTION_ARGUMENT})
  {
    for (const bool mutating : {false, true})
    {
      CAPTURE(code, mutating);

      const auto error = ftp::MakeOperationError(
          code, "Original curl detail", 0, mutating,
          std::nullopt, ftp::OperationKind::ControlCommand);

      CHECK(error.code == RemoteErrorCode::ProtocolError);
      CHECK(error.nativeCode == static_cast<int>(code));
      CHECK_FALSE(error.retryable);
      CHECK(error.operationMayHaveSucceeded == mutating);
      CHECK(error.message == "The libcurl FTP operation failed: " +
                                 ftp::FormatFailureDetail(code, "Original curl detail", 0, std::nullopt));
    }
  }
}

TEST_CASE("FTP listing fallback is limited to an unsupported MLSD command",
          "[ftp][errors]")
{
  for (const long response : {500L, 501L, 502L, 504L})
  {
    CAPTURE(response);
    CHECK(ftp::CanFallbackToList(CURLE_FTP_COULDNT_RETR_FILE, response));
    CHECK_FALSE(ftp::CanFallbackToList(CURLE_QUOTE_ERROR, response));
    CHECK_FALSE(ftp::CanFallbackToList(CURLE_LOGIN_DENIED, response));
  }

  for (const long response : {0L, 200L, 421L, 425L, 426L, 450L, 530L, 550L})
  {
    CAPTURE(response);
    CHECK_FALSE(ftp::CanFallbackToList(CURLE_FTP_COULDNT_RETR_FILE, response));
  }

  for (const auto code : {CURLE_COULDNT_CONNECT, CURLE_COULDNT_RESOLVE_HOST,
                          CURLE_OPERATION_TIMEDOUT, CURLE_SEND_ERROR,
                          CURLE_RECV_ERROR, CURLE_SSL_CACERT,
                          CURLE_SSL_PINNEDPUBKEYNOTMATCH, CURLE_ABORTED_BY_CALLBACK})
  {
    CAPTURE(code);
    CHECK_FALSE(ftp::CanFallbackToList(code, 500));
  }
}

TEST_CASE("FTP authentication rejection during a command invalidates the session",
          "[ftp][errors]")
{
  for (const auto code : {CURLE_LOGIN_DENIED, CURLE_QUOTE_ERROR,
                          CURLE_FTP_COULDNT_RETR_FILE, CURLE_REMOTE_ACCESS_DENIED})
  {
    for (const bool mutating : {false, true})
    {
      CAPTURE(code, mutating);

      const auto error = ftp::MakeOperationError(
          code, "Login required", 530, mutating, std::nullopt,
          ftp::OperationKind::ControlCommand);

      CHECK(error.code == RemoteErrorCode::AuthenticationFailed);
      CHECK(error.nativeCode == (code == CURLE_LOGIN_DENIED ? static_cast<int>(code) : 530));
      CHECK_FALSE(error.retryable);
      CHECK_FALSE(error.operationMayHaveSucceeded);
      CHECK(error.message.find("FTP authentication failed") != std::string::npos);
      CHECK(error.message.find("FTP response code 530") != std::string::npos);
    }
  }
}

TEST_CASE("FTP session closure during a listing is not a listing compatibility failure",
          "[ftp][errors]")
{
  for (const auto code : {CURLE_FTP_COULDNT_RETR_FILE, CURLE_REMOTE_ACCESS_DENIED,
                          CURLE_QUOTE_ERROR})
  {
    const auto error = ftp::MakeOperationError(
        code, "Service closing control connection", 421,
        false, std::nullopt, ftp::OperationKind::Mlsd);

    CHECK(error.code == RemoteErrorCode::ConnectionLost);
    CHECK(error.nativeCode == 421);
    CHECK(error.retryable);
    CHECK_FALSE(error.operationMayHaveSucceeded);
    CHECK_FALSE(ftp::CanFallbackToList(error.nativeCode, 421));
    CHECK_FALSE(ftp::CanIgnoreOwnershipListingFailure(error, 421));
  }
}

TEST_CASE("Optional FTP ownership metadata never hides a session failure",
          "[ftp][errors]")
{
  for (const auto code : {RemoteErrorCode::AuthenticationFailed,
                          RemoteErrorCode::CredentialUnavailable, RemoteErrorCode::TrustRejected,
                          RemoteErrorCode::CertificateInvalid, RemoteErrorCode::HostKeyChanged,
                          RemoteErrorCode::NameResolutionFailed, RemoteErrorCode::ConnectionFailed,
                          RemoteErrorCode::ConnectionLost, RemoteErrorCode::NotConnected,
                          RemoteErrorCode::TimedOut, RemoteErrorCode::Cancelled,
                          RemoteErrorCode::Paused, RemoteErrorCode::LocalIo})
  {
    CAPTURE(code);

    // Even a stale response/native value must not override the error category
    const RemoteError error{.code = code, .message = "Failure", .nativeCode = CURLE_FTP_COULDNT_RETR_FILE};

    CHECK_FALSE(ftp::CanIgnoreOwnershipListingFailure(error, 500));
  }

  for (const long response : {500L, 501L, 502L, 504L, 550L})
  {
    const auto error = ftp::MakeOperationError(
        CURLE_FTP_COULDNT_RETR_FILE, "Listing command denied", response,
        false, std::nullopt, ftp::OperationKind::List);

    CHECK(ftp::CanIgnoreOwnershipListingFailure(error, response));
  }

  for (const auto code : {RemoteErrorCode::Unsupported, RemoteErrorCode::PermissionDenied,
                          RemoteErrorCode::NotFound})
  {
    RemoteError error{.code = code, .message = "Optional listing unavailable"};

    CHECK(ftp::CanIgnoreOwnershipListingFailure(error, 550));

    error.operationMayHaveSucceeded = true;

    CHECK_FALSE(ftp::CanIgnoreOwnershipListingFailure(error, 550));
  }

  const auto malformed = ftp::MakeOperationError(
      CURLE_WEIRD_SERVER_REPLY, "Malformed reply", 0,
      false, std::nullopt, ftp::OperationKind::List);

  CHECK_FALSE(ftp::CanIgnoreOwnershipListingFailure(malformed, 0));
}
