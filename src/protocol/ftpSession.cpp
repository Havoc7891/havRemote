// SPDX-License-Identifier: MIT

#include "protocol/ftpErrors.hpp"
#include "protocol/ftpSession.hpp"

#include "core/logSanitizer.hpp"
#include "network/curlRuntime.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <wincrypt.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <wx/strconv.h>
#include <wx/string.h>
#endif

#include <curl/curl.h>

#include "ftpControlTrace.hpp"
#include "ftpUploadReply.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <span>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace havremote
{
  namespace
  {
    using namespace std::chrono_literals;

    constexpr long transferLowSpeedLimit = 1L;
    constexpr long transferLowSpeedTime = 30L;

    [[nodiscard]] constexpr bool IsFileTransfer(
        const ftp::OperationKind operation) noexcept
    {
      return operation == ftp::OperationKind::Retr ||
             operation == ftp::OperationKind::Stor;
    }

    struct CurlDeleter final
    {
      void operator()(CURL *handle) const noexcept { curl_easy_cleanup(handle); }
    };

    struct CurlListDeleter final
    {
      void operator()(curl_slist *list) const noexcept { curl_slist_free_all(list); }
    };

    using CurlHandle = std::unique_ptr<CURL, CurlDeleter>;
    using CurlList = std::unique_ptr<curl_slist, CurlListDeleter>;

    struct SocketEndpoint final
    {
      std::string address;
      std::uint16_t port{};
    };

    [[nodiscard]] std::optional<SocketEndpoint> ReadSocketEndpoint(
        const sockaddr *address, const std::size_t length)
    {
      if (address == nullptr)
      {
        return std::nullopt;
      }

      std::array<char, INET6_ADDRSTRLEN> text{};
      SocketEndpoint result;

      if (address->sa_family == AF_INET && length >= sizeof(sockaddr_in))
      {
        const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(address);

#if defined(_WIN32)
        if (InetNtopA(AF_INET, const_cast<in_addr *>(&ipv4->sin_addr),
                      text.data(), static_cast<DWORD>(text.size())) == nullptr)
#else
        if (inet_ntop(AF_INET, &ipv4->sin_addr, text.data(), text.size()) == nullptr)
#endif
        {
          return std::nullopt;
        }

        result.address = text.data();
        result.port = ntohs(ipv4->sin_port);

        return result;
      }

      if (address->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6))
      {
        const auto *ipv6 = reinterpret_cast<const sockaddr_in6 *>(address);

#if defined(_WIN32)
        if (InetNtopA(AF_INET6, const_cast<in6_addr *>(&ipv6->sin6_addr),
                      text.data(), static_cast<DWORD>(text.size())) == nullptr)
#else
        if (inet_ntop(AF_INET6, &ipv6->sin6_addr, text.data(), text.size()) == nullptr)
#endif
        {
          return std::nullopt;
        }

        result.address = text.data();

        if (ipv6->sin6_scope_id != 0)
        {
          result.address += "%" + std::to_string(ipv6->sin6_scope_id);
        }

        result.port = ntohs(ipv6->sin6_port);

        return result;
      }

      return std::nullopt;
    }

    [[nodiscard]] std::string FormatSocketEndpoint(const SocketEndpoint &endpoint)
    {
      if (endpoint.address.find(':') != std::string::npos)
      {
        return "[" + endpoint.address + "]:" + std::to_string(endpoint.port);
      }

      return endpoint.address + ":" + std::to_string(endpoint.port);
    }

    [[nodiscard]] int CurrentSocketError() noexcept
    {
#if defined(_WIN32)
      return WSAGetLastError();
#else
      return errno;
#endif
    }

    void RestoreSocketError(const int error) noexcept
    {
#if defined(_WIN32)
      WSASetLastError(error);
#else
      errno = error;
#endif
    }

    [[nodiscard]] std::optional<SocketEndpoint> LocalSocketEndpoint(
        const curl_socket_t socket, int &error) noexcept
    {
      sockaddr_storage address{};

#if defined(_WIN32)
      int length = sizeof(address);
#else
      socklen_t length = sizeof(address);
#endif

      if (getsockname(socket, reinterpret_cast<sockaddr *>(&address), &length) != 0)
      {
        error = CurrentSocketError();

        return std::nullopt;
      }

      error = 0;

      try
      {
        return ReadSocketEndpoint(reinterpret_cast<const sockaddr *>(&address),
                                  static_cast<std::size_t>(length));
      }
      catch (...)
      {
        return std::nullopt;
      }
    }

    [[nodiscard]] std::string UpperAsciiToken(const std::string_view value)
    {
      std::string result;

      for (const char character : value)
      {
        const auto byte = static_cast<unsigned char>(character);

        if (std::isspace(byte) != 0 || character == '\r' || character == '\n')
        {
          break;
        }

        if (result.size() == 16U)
        {
          break;
        }

        result.push_back(static_cast<char>(std::toupper(byte)));
      }

      return result;
    }

    [[nodiscard]] std::string SanitizedCurlHeader(const curl_infotype type,
                                                  const std::string_view input)
    {
      const auto first = input.find_first_not_of(" \t\r\n");

      if (first == std::string_view::npos)
      {
        return {};
      }

      const auto lineEnd = input.find_first_of("\r\n", first);
      const auto line = input.substr(
          first, lineEnd == std::string_view::npos ? input.size() - first
                                                   : lineEnd - first);

      if (type == CURLINFO_HEADER_IN)
      {
        if (line.size() >= 3U &&
            std::isdigit(static_cast<unsigned char>(line[0])) != 0 &&
            std::isdigit(static_cast<unsigned char>(line[1])) != 0 &&
            std::isdigit(static_cast<unsigned char>(line[2])) != 0)
        {
          return "FTP response " + std::string{line.substr(0, 3)};
        }

        return "FTP response <unparsed>";
      }

      const auto command = UpperAsciiToken(line);

      if (command.empty())
      {
        return "FTP command <unparsed>";
      }

      if (command == "EPRT" || command == "PORT")
      {
        return "FTP command " + SanitizeDiagnosticText(line);
      }

      if (command == "USER" || command == "PASS" || command == "ACCT" ||
          command == "ADAT")
      {
        return "FTP command " + command + " <redacted>";
      }

      return "FTP command " + command;
    }

    [[nodiscard]] constexpr std::string_view FtpOperationName(
        const ftp::OperationKind operation) noexcept
    {
      switch (operation)
      {
      case ftp::OperationKind::ControlCommand:
        return "control command";

      case ftp::OperationKind::Nlst:
        return "NLST";

      case ftp::OperationKind::Mlsd:
        return "MLSD";

      case ftp::OperationKind::List:
        return "LIST";

      case ftp::OperationKind::Retr:
        return "RETR";

      case ftp::OperationKind::Stor:
        return "STOR";

      case ftp::OperationKind::Rename:
        return "RNFR/RNTO";
      }

      return "FTP operation";
    }

    class ActiveFtpTrace final
    {
    public:
      void Begin(const DiagnosticCallback *diagnostic,
                 const ftp::OperationKind operation) noexcept
      {
        mDiagnostic = diagnostic;
        mOperation = operation;
        mPrerequisiteSeen = false;
        mControlEndpointSeen = false;
        mListenerSocket = CURL_SOCKET_BAD;
        mListenerEndpoint.reset();
        mListenerReported = false;
        mNextLine = 0;
        mLineCount = 0;
        mDumped = false;

        for (auto &line : mLines)
        {
          line.clear();
        }
      }

      void Clear() noexcept
      {
        Begin(nullptr, ftp::OperationKind::ControlCommand);
      }

      void Record(const std::string_view message) noexcept
      {
        try
        {
          auto sanitized = SanitizeDiagnosticText(message);
          if (sanitized.empty())
          {
            return;
          }

          constexpr std::size_t maximumLineLength = 1'024;

          if (sanitized.size() > maximumLineLength)
          {
            sanitized.resize(maximumLineLength - 3U);
            sanitized += "...";
          }

          mLines[mNextLine] = sanitized;
          mNextLine = (mNextLine + 1U) % mLines.size();
          mLineCount = (std::min)(mLineCount + 1U, mLines.size());
        }
        catch (...)
        {
          // No exception may cross a libcurl C callback boundary
        }
      }

      void RecordControlEndpoint(const std::string_view remoteAddress,
                                 const int remotePort,
                                 const std::string_view localAddress,
                                 const int localPort) noexcept
      {
        mPrerequisiteSeen = true;
        mControlEndpointSeen = true;

        try
        {
          Record("Established control endpoint: local " +
                 FormatEndpoint(localAddress, localPort) + ", remote " +
                 FormatEndpoint(remoteAddress, remotePort));
        }
        catch (...)
        {
        }
      }

      [[nodiscard]] bool PrerequisiteSeen() const noexcept
      {
        return mPrerequisiteSeen;
      }

      [[nodiscard]] bool ControlEndpointSeen() const noexcept
      {
        return mControlEndpointSeen;
      }

      void RecordListenerSocket(const curl_socket_t socket,
                                const curl_sockaddr *address,
                                const int socketError) noexcept
      {
        mListenerSocket = socket;

        try
        {
          const auto endpoint =
              address == nullptr
                  ? std::optional<SocketEndpoint>{}
                  : ReadSocketEndpoint(&address->addr, address->addrlen);

          std::string message = "Active listener socket(): attempted bind target ";
          message += endpoint ? FormatSocketEndpoint(*endpoint) : "<unavailable>";

          if (socket == CURL_SOCKET_BAD)
          {
            message += ". socket() failed with OS error " +
                       std::to_string(socketError) + " (" +
                       ftp::SocketErrorText(socketError) + ")";
          }
          else
          {
            message += ". socket() succeeded";
          }

          Record(message);
        }
        catch (...)
        {
        }
      }

      void ObserveSuccessfulBind() noexcept
      {
        if (mListenerSocket == CURL_SOCKET_BAD)
        {
          Record("libcurl reported a successful active-listener bind, but the "
                 "listener socket was not observable");

          return;
        }

        int error{};

        const auto endpoint = LocalSocketEndpoint(mListenerSocket, error);

        try
        {
          if (endpoint)
          {
            mListenerEndpoint = *endpoint;

            Record("libcurl bind() succeeded. Actual listener endpoint " +
                   FormatSocketEndpoint(*endpoint));
          }
          else
          {
            std::string message =
                "libcurl reported bind() success, but getsockname() could not "
                "read the listener endpoint";

            if (error != 0)
            {
              message += ": OS error " + std::to_string(error) + " (" +
                         ftp::SocketErrorText(error) + ")";
            }

            Record(message);
          }
        }
        catch (...)
        {
        }
      }

      void ObserveSuccessfulListen() noexcept
      {
        if (mListenerSocket == CURL_SOCKET_BAD)
        {
          Record("libcurl reported listen() success, but the active listener "
                 "socket was not observable");

          return;
        }

        int accepting{};

#if defined(_WIN32)
        int length = sizeof(accepting);
        const int result = getsockopt(mListenerSocket, SOL_SOCKET, SO_ACCEPTCONN,
                                      reinterpret_cast<char *>(&accepting), &length);
#else
        socklen_t length = sizeof(accepting);
        const int result = getsockopt(mListenerSocket, SOL_SOCKET, SO_ACCEPTCONN,
                                      &accepting, &length);
#endif

        const int error = result == 0 ? 0 : CurrentSocketError();

        try
        {
          if (result == 0)
          {
            Record(std::string{"libcurl listen() succeeded, SO_ACCEPTCONN="} +
                   (accepting != 0 ? "true" : "false"));

            if (accepting != 0 && !mListenerReported)
            {
              mListenerReported = true;

              std::string message = "Active FTP ";
              message += FtpOperationName(mOperation);
              message += " data listener created";

              if (mListenerEndpoint)
              {
                message += " at " + FormatSocketEndpoint(*mListenerEndpoint);
              }

              PublishLine(message);
            }
          }
          else
          {
            Record("libcurl reported listen() success, but SO_ACCEPTCONN could "
                   "not be queried: OS error " +
                   std::to_string(error) + " (" + ftp::SocketErrorText(error) + ")");
          }
        }
        catch (...)
        {
        }
      }

      void Dump() noexcept
      {
        if (mDumped)
        {
          return;
        }

        mDumped = true;

        PublishLine("Active FTP libcurl trace (bounded, credentials redacted):");

        const auto first = (mNextLine + mLines.size() - mLineCount) % mLines.size();

        for (std::size_t offset = 0; offset < mLineCount; ++offset)
        {
          PublishLine("Active FTP libcurl trace: " +
                      mLines[(first + offset) % mLines.size()]);
        }
      }

    private:
      [[nodiscard]] static std::string FormatEndpoint(
          const std::string_view address, const int port)
      {
        if (address.find(':') != std::string_view::npos)
        {
          return "[" + std::string{address} + "]:" + std::to_string(port);
        }

        return std::string{address} + ":" + std::to_string(port);
      }

      void PublishLine(const std::string_view message) noexcept
      {
        try
        {
          if (mDiagnostic != nullptr && *mDiagnostic)
          {
            (*mDiagnostic)(DiagnosticLevel::Debug, message);
          }
        }
        catch (...)
        {
          // Diagnostic sinks are application callbacks and cannot be allowed to
          // unwind through libcurl either.
        }
      }

      static constexpr std::size_t mMaximumLines = 96;
      const DiagnosticCallback *mDiagnostic{};
      ftp::OperationKind mOperation{ftp::OperationKind::ControlCommand};
      std::array<std::string, mMaximumLines> mLines{};
      std::size_t mNextLine{};
      std::size_t mLineCount{};
      curl_socket_t mListenerSocket{CURL_SOCKET_BAD};
      std::optional<SocketEndpoint> mListenerEndpoint;
      bool mPrerequisiteSeen{};
      bool mControlEndpointSeen{};
      bool mListenerReported{};
      bool mDumped{};
    };

    int ActiveFtpPrerequisite(void *clientData,
                              char *remoteAddress,
                              char *localAddress,
                              const int remotePort,
                              const int localPort) noexcept
    {
      try
      {
        auto *trace = static_cast<ActiveFtpTrace *>(clientData);
        if (trace != nullptr)
        {
          trace->RecordControlEndpoint(remoteAddress != nullptr ? remoteAddress : "<unknown>",
                                       remotePort,
                                       localAddress != nullptr ? localAddress : "<unknown>",
                                       localPort);
        }
      }
      catch (...)
      {
      }

      return CURL_PREREQFUNC_OK;
    }

    curl_socket_t ActiveFtpOpenSocket(void *clientData,
                                      const curlsocktype purpose,
                                      curl_sockaddr *address) noexcept
    {
      if (address == nullptr)
      {
        return CURL_SOCKET_BAD;
      }

      const curl_socket_t socket = ::socket(address->family, address->socktype,
                                            address->protocol);

      const int socketError = socket == CURL_SOCKET_BAD ? CurrentSocketError() : 0;

      try
      {
        auto *trace = static_cast<ActiveFtpTrace *>(clientData);

        const auto endpoint = ReadSocketEndpoint(&address->addr, address->addrlen);

        if (trace != nullptr && trace->PrerequisiteSeen() &&
            purpose == CURLSOCKTYPE_IPCXN && endpoint && endpoint->port == 0)
        {
          trace->RecordListenerSocket(socket, address, socketError);
        }
      }
      catch (...)
      {
      }

      RestoreSocketError(socketError);

      return socket;
    }

    int ActiveFtpDebug(CURL *,
                       const curl_infotype type,
                       char *data,
                       const std::size_t size,
                       void *clientData) noexcept
    {
      // Verbose mode invokes this callback for every payload chunk. Payloads
      // are irrelevant to active-listener setup and may contain file data, so
      // discard them before doing even the socket-error preservation work.
      if (type == CURLINFO_DATA_IN || type == CURLINFO_DATA_OUT ||
          type == CURLINFO_SSL_DATA_IN || type == CURLINFO_SSL_DATA_OUT)
      {
        return 0;
      }

      const int savedSocketError = CurrentSocketError();

      try
      {
        auto *trace = static_cast<ActiveFtpTrace *>(clientData);
        if (trace == nullptr || data == nullptr)
        {
          RestoreSocketError(savedSocketError);

          return 0;
        }

        if (type == CURLINFO_TEXT)
        {
          const auto sanitized = SanitizeDiagnosticText(
              std::string_view{data, size});

          trace->Record("info: " + sanitized);

          if (sanitized.find("ftp_port_bind_socket(), socket bound") !=
              std::string::npos)
          {
            trace->ObserveSuccessfulBind();
          }
          else if (sanitized.find("ftp_port_listen(), listening") !=
                   std::string::npos)
          {
            trace->ObserveSuccessfulListen();
          }
        }
        else if (type == CURLINFO_HEADER_OUT || type == CURLINFO_HEADER_IN)
        {
          trace->Record(SanitizedCurlHeader(type, std::string_view{data, size}));
        }
      }
      catch (...)
      {
      }

      RestoreSocketError(savedSocketError);

      return 0;
    }

    struct FtpDebugContext final
    {
      ActiveFtpTrace *active{};
      ftp::ControlTrace *control{};
      CURL *handle{};
      curl_off_t *lastReadyConnection{};
      const DiagnosticCallback *diagnostic{};
      ftp::UploadReplyObserver *uploadReply{};

      void ObserveReadyConnection() noexcept
      {
        // PREREQ runs after FTP authentication, not merely after TCP connects.
        // Track the control connection identity, not CURLINFO_NUM_CONNECTS:
        // the latter can include passive data connections too.
        curl_off_t id{-1};

        if (!handle || !lastReadyConnection ||
            curl_easy_getinfo(handle, CURLINFO_CONN_ID, &id) != CURLE_OK || id < 0)
        {
          return;
        }

        const auto previous = std::exchange(*lastReadyConnection, id);
        if (previous < 0 || previous == id)
        {
          return;
        }

        try
        {
          if (diagnostic && *diagnostic)
          {
            (*diagnostic)(DiagnosticLevel::Information,
                          "Reconnected to the FTP server");
          }
        }
        catch (...)
        {
          // A diagnostic sink must not alter the operation or unwind through
          // libcurl. The ID was already recorded to avoid duplicate notices.
        }
      }
    };

    int FtpSessionDebug(CURL *handle, const curl_infotype type, char *data,
                        const std::size_t size, void *clientData) noexcept
    {
      // Do not inspect or copy file contents or TLS records
      if (type == CURLINFO_DATA_IN || type == CURLINFO_DATA_OUT ||
          type == CURLINFO_SSL_DATA_IN || type == CURLINFO_SSL_DATA_OUT)
      {
        return 0;
      }

      const int savedSocketError = CurrentSocketError();

      auto *context = static_cast<FtpDebugContext *>(clientData);

      if (context && data)
      {
        if (context->uploadReply && handle == context->handle &&
            (type == CURLINFO_HEADER_IN || type == CURLINFO_HEADER_OUT))
        {
          context->uploadReply->Header(type == CURLINFO_HEADER_IN, {data, size});
        }

        if (context->active)
        {
          ActiveFtpDebug(handle, type, data, size, context->active);
        }

        if (context->control)
        {
          if (type == CURLINFO_TEXT)
          {
            context->control->Text({data, size});
          }
          else if (type == CURLINFO_HEADER_IN || type == CURLINFO_HEADER_OUT)
          {
            context->control->Header(type == CURLINFO_HEADER_IN, {data, size});
          }
        }
      }

      RestoreSocketError(savedSocketError);

      return 0;
    }

    int FtpSessionPrerequisite(void *clientData, char *remote, char *local,
                               const int remotePort, const int localPort) noexcept
    {
      const int savedSocketError = CurrentSocketError();

      auto *context = static_cast<FtpDebugContext *>(clientData);

      if (context)
      {
        context->ObserveReadyConnection();

        if (context->active)
        {
          ActiveFtpPrerequisite(context->active, remote, local, remotePort, localPort);
        }

        if (context->control)
        {
          context->control->Endpoint(remote, remotePort, local, localPort);
        }
      }

      RestoreSocketError(savedSocketError);

      return CURL_PREREQFUNC_OK;
    }

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

    // curl's upload resume option selects APPE, not REST + STOR. Keep the
    // restart command and reply guard scoped to this one upload instead.
    class UploadRestartRequest final
    {
    public:
      ~UploadRestartRequest() { ClearOptions(); }
      UploadRestartRequest() = default;
      UploadRestartRequest(const UploadRestartRequest &) = delete;
      UploadRestartRequest &operator=(const UploadRestartRequest &) = delete;

      Result<void> Configure(CURL *handle, const std::uint64_t offset,
                             FtpDebugContext *trace)
      {
        mCommands.reset(curl_slist_append(nullptr,
                                          ("REST " + std::to_string(offset)).c_str()));

        if (!mCommands)
        {
          return std::unexpected(FtpError(RemoteErrorCode::ProtocolError,
                                          "Could not allocate the FTP restart command"));
        }

        mHandle = handle;
        mTrace = trace;

        // A rejected STOR can leave a restart marker on a reusable control
        // connection. Allow reuse only once STOR has consumed that marker.
        for (const auto code : {
                 curl_easy_setopt(mHandle, CURLOPT_FORBID_REUSE, 1L),
                 curl_easy_setopt(mHandle, CURLOPT_PREQUOTE, mCommands.get()),
                 curl_easy_setopt(mHandle, CURLOPT_DEBUGFUNCTION, Debug),
                 curl_easy_setopt(mHandle, CURLOPT_DEBUGDATA, this),
                 curl_easy_setopt(mHandle, CURLOPT_VERBOSE, 1L),
                 curl_easy_setopt(mHandle, CURLOPT_HEADERFUNCTION, Header),
                 curl_easy_setopt(mHandle, CURLOPT_HEADERDATA, this)})
        {
          if (code != CURLE_OK)
          {
            return std::unexpected(FtpError(
                RemoteErrorCode::ProtocolError,
                std::string{"Could not configure FTP upload resume: "} +
                    curl_easy_strerror(code),
                static_cast<int>(code)));
          }
        }

        return {};
      }

      void ClearOptions() noexcept
      {
        if (!mHandle)
        {
          return;
        }

        curl_easy_setopt(mHandle, CURLOPT_PREQUOTE, nullptr);
        curl_easy_setopt(mHandle, CURLOPT_HEADERFUNCTION, nullptr);
        curl_easy_setopt(mHandle, CURLOPT_HEADERDATA, nullptr);
        curl_easy_setopt(mHandle, CURLOPT_DEBUGFUNCTION, FtpSessionDebug);
        curl_easy_setopt(mHandle, CURLOPT_DEBUGDATA, mTrace);
        curl_easy_setopt(mHandle, CURLOPT_VERBOSE, mTrace ? 1L : 0L);
        curl_easy_setopt(mHandle, CURLOPT_FORBID_REUSE, 0L);

        mHandle = nullptr;
      }

      [[nodiscard]] std::optional<ftp::UploadResumeCommand> RejectedCommand() const noexcept
      {
        if (mRestReply != 0 && mRestReply != 350)
        {
          return ftp::UploadResumeCommand::Rest;
        }

        if (!mStoreStarted && mStoreReply >= 400)
        {
          return ftp::UploadResumeCommand::Stor;
        }

        return std::nullopt;
      }

      [[nodiscard]] long RejectedReply() const noexcept
      {
        return mRestReply != 0 && mRestReply != 350 ? mRestReply : mStoreReply;
      }

    private:
      static int Debug(CURL *handle, const curl_infotype type, char *data,
                       const std::size_t size, void *userdata) noexcept
      {
        auto &request = *static_cast<UploadRestartRequest *>(userdata);

        if (type == CURLINFO_HEADER_OUT)
        {
          const std::string_view command{data, size};

          request.mPending.reset();

          // curl can report only the initially written command prefix here.
          // A later socket flush has no debug event. Do not continue if that
          // prefix is too short to identify the command safely.
          request.mUntrackedCommand = command.size() < 5;

          if (command.starts_with("REST "))
          {
            request.mPending = ftp::UploadResumeCommand::Rest;
            request.mRestReply = 0;
            request.mStoreReply = 0;
            request.mStoreStarted = false;
          }
          else if (command.starts_with("STOR "))
          {
            request.mPending = ftp::UploadResumeCommand::Stor;
          }
        }

        // Preserve active-listener diagnostics and the opt-in control trace
        // without changing the REST/STOR response guard or exposing payloads.
        if (request.mTrace)
        {
          return FtpSessionDebug(handle, type, data, size, request.mTrace);
        }

        return 0;
      }

      static std::size_t Header(char *data, const std::size_t size,
                                const std::size_t count, void *userdata) noexcept
      {
        auto &request = *static_cast<UploadRestartRequest *>(userdata);

        const auto bytes = size * count;

        const std::string_view line{data, bytes};

        if (request.mUntrackedCommand)
        {
          return CURL_WRITEFUNC_ERROR;
        }

        if (!request.mPending || line.size() < 4 ||
            line[0] < '0' || line[0] > '9' ||
            line[1] < '0' || line[1] > '9' ||
            line[2] < '0' || line[2] > '9')
        {
          return bytes;
        }

        const long reply = (line[0] - '0') * 100 +
                           (line[1] - '0') * 10 + line[2] - '0';

        // Match curl's final-line recognition: three digits plus a space.
        // Even an inconsistent multiline reply must not hide a non-350 line
        // that curl itself would treat as permission to proceed with STOR.
        if (line[3] != ' ')
        {
          return bytes;
        }

        if (*request.mPending == ftp::UploadResumeCommand::Rest)
        {
          request.mRestReply = reply;
          request.mPending.reset();

          // PREQUOTE alone accepts any reply below 400. Require RFC 3659's
          // 350 before curl may send STOR, which otherwise could truncate
          // the partial file on a noncompliant server's generic 200 reply.
          if (reply != 350 && reply < 400)
          {
            return CURL_WRITEFUNC_ERROR;
          }
        }
        else
        {
          request.mStoreReply = reply;

          if (reply == 125 || reply == 150)
          {
            request.mStoreStarted = true;

            curl_easy_setopt(request.mHandle, CURLOPT_FORBID_REUSE, 0L);
          }
        }

        return bytes;
      }

      CURL *mHandle{};
      FtpDebugContext *mTrace{};
      CurlList mCommands;
      std::optional<ftp::UploadResumeCommand> mPending;
      long mRestReply{};
      long mStoreReply{};
      bool mStoreStarted{};
      bool mUntrackedCommand{};
    };

    [[nodiscard]] Result<std::string> FormatFtpHost(std::string_view host)
    {
      if (!IsValidEndpointHost(host))
      {
        return std::unexpected(FtpError(
            RemoteErrorCode::InvalidArgument,
            "FTP host must be an unadorned ASCII DNS name or IPv4/IPv6 literal"));
      }

      return host.find(':') == std::string_view::npos
                 ? std::string{host}
                 : "[" + std::string{host} + "]";
    }

    [[nodiscard]] Result<bool> ActiveAddressIsAssigned(
        const std::string_view address)
    {
      if (!IsValidIpAddress(address))
      {
        return std::unexpected(FtpError(
            RemoteErrorCode::InvalidArgument,
            "The Active FTP address must be an unbracketed IPv4 or IPv6 literal"));
      }

      const std::string nullTerminated{address};

      const int family = address.find(':') == std::string_view::npos
                             ? AF_INET
                             : AF_INET6;

      in_addr expectedIpv4{};
      in6_addr expectedIpv6{};

#if defined(_WIN32)
      const auto parsed =
          family == AF_INET
              ? InetPtonA(AF_INET, nullTerminated.c_str(), &expectedIpv4)
              : InetPtonA(AF_INET6, nullTerminated.c_str(), &expectedIpv6);

      if (parsed != 1)
      {
        return std::unexpected(FtpError(
            RemoteErrorCode::InvalidArgument,
            "The Active FTP address could not be parsed as an IP literal"));
      }

      constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST |
                              GAA_FLAG_SKIP_MULTICAST |
                              GAA_FLAG_SKIP_DNS_SERVER;

      ULONG bufferSize = 15'000;

      std::vector<std::byte> buffer(bufferSize);

      for (int attempt = 0; attempt < 3; ++attempt)
      {
        auto *adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(
            buffer.data());

        const ULONG status = GetAdaptersAddresses(
            static_cast<ULONG>(family), flags, nullptr, adapters,
            &bufferSize);

        if (status == ERROR_BUFFER_OVERFLOW)
        {
          buffer.resize(bufferSize);

          continue;
        }

        if (status == ERROR_NO_DATA)
        {
          return false;
        }

        if (status != NO_ERROR)
        {
          return std::unexpected(FtpError(
              RemoteErrorCode::ProtocolError,
              "Could not enumerate local Windows interface addresses (error " +
                  std::to_string(status) + ")",
              static_cast<int>(status)));
        }

        for (auto *adapter = adapters; adapter != nullptr; adapter = adapter->Next)
        {
          if (adapter->OperStatus != IfOperStatusUp)
          {
            continue;
          }

          for (auto *unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next)
          {
            const auto *socketAddress = unicast->Address.lpSockaddr;

            if (socketAddress == nullptr || socketAddress->sa_family != family ||
                unicast->DadState == IpDadStateInvalid ||
                unicast->DadState == IpDadStateTentative ||
                unicast->DadState == IpDadStateDuplicate)
            {
              continue;
            }

            if (family == AF_INET)
            {
              const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(socketAddress);

              if (std::memcmp(&ipv4->sin_addr, &expectedIpv4, sizeof(expectedIpv4)) == 0)
              {
                return true;
              }
            }
            else
            {
              const auto *ipv6 = reinterpret_cast<const sockaddr_in6 *>(socketAddress);

              if (std::memcmp(&ipv6->sin6_addr, &expectedIpv6, sizeof(expectedIpv6)) == 0)
              {
                return true;
              }
            }
          }
        }

        return false;
      }

      return std::unexpected(FtpError(
          RemoteErrorCode::ProtocolError,
          "Local Windows interface addresses changed repeatedly during validation"));
#else
      const auto parsed =
          family == AF_INET
              ? inet_pton(AF_INET, nullTerminated.c_str(), &expectedIpv4)
              : inet_pton(AF_INET6, nullTerminated.c_str(), &expectedIpv6);

      if (parsed != 1)
      {
        return std::unexpected(FtpError(
            RemoteErrorCode::InvalidArgument,
            "The Active FTP address could not be parsed as an IP literal"));
      }

      ifaddrs *interfaces{};

      if (getifaddrs(&interfaces) != 0)
      {
        const int nativeError = errno;

        return std::unexpected(FtpError(
            RemoteErrorCode::ProtocolError,
            "Could not enumerate local interface addresses: " +
                std::string{std::strerror(nativeError)},
            nativeError));
      }

      const auto releaseInterfaces = [](ifaddrs *value)
      {
        if (value != nullptr)
        {
          freeifaddrs(value);
        }
      };

      const std::unique_ptr<ifaddrs, decltype(releaseInterfaces)> owned{interfaces, releaseInterfaces};

      for (auto *interface = interfaces; interface != nullptr; interface = interface->ifa_next)
      {
        if (interface->ifa_addr == nullptr ||
            interface->ifa_addr->sa_family != family ||
            (interface->ifa_flags & IFF_UP) == 0)
        {
          continue;
        }

        if (family == AF_INET)
        {
          const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(interface->ifa_addr);

          if (std::memcmp(&ipv4->sin_addr, &expectedIpv4, sizeof(expectedIpv4)) == 0)
          {
            return true;
          }
        }
        else
        {
          const auto *ipv6 = reinterpret_cast<const sockaddr_in6 *>(interface->ifa_addr);

          if (std::memcmp(&ipv6->sin6_addr, &expectedIpv6, sizeof(expectedIpv6)) == 0)
          {
            return true;
          }
        }
      }

      return false;
#endif
    }

    [[nodiscard]] int DecodeBase64Character(char character) noexcept
    {
      if (character >= 'A' && character <= 'Z')
      {
        return character - 'A';
      }

      if (character >= 'a' && character <= 'z')
      {
        return character - 'a' + 26;
      }

      if (character >= '0' && character <= '9')
      {
        return character - '0' + 52;
      }

      if (character == '+')
      {
        return 62;
      }

      if (character == '/')
      {
        return 63;
      }

      return -1;
    }

    [[nodiscard]] Result<std::vector<std::byte>> DecodeCertificatePem(std::string_view pem)
    {
      constexpr std::string_view beginMarker = "-----BEGIN CERTIFICATE-----";
      constexpr std::string_view endMarker = "-----END CERTIFICATE-----";

      const auto begin = pem.find(beginMarker);

      if (begin == std::string_view::npos)
      {
        return std::unexpected(FtpError(RemoteErrorCode::ParseError,
                                        "The TLS provider returned no PEM certificate"));
      }

      const auto contentsBegin = begin + beginMarker.size();
      const auto end = pem.find(endMarker, contentsBegin);

      if (end == std::string_view::npos)
      {
        return std::unexpected(FtpError(RemoteErrorCode::ParseError,
                                        "The TLS provider returned a truncated certificate"));
      }

      std::string base64;
      base64.reserve(end - contentsBegin);

      for (const char character : pem.substr(contentsBegin, end - contentsBegin))
      {
        if (std::isspace(static_cast<unsigned char>(character)) != 0)
        {
          continue;
        }

        if (character != '=' && DecodeBase64Character(character) < 0)
        {
          return std::unexpected(FtpError(RemoteErrorCode::ParseError,
                                          "The TLS certificate has invalid base64 data"));
        }

        base64.push_back(character);
      }

      if (base64.empty() || base64.size() % 4U != 0U)
      {
        return std::unexpected(FtpError(RemoteErrorCode::ParseError,
                                        "The TLS certificate has invalid base64 length"));
      }

      std::vector<std::byte> decoded;
      decoded.reserve(base64.size() / 4U * 3U);

      bool padded{};

      for (std::size_t offset = 0; offset < base64.size(); offset += 4U)
      {
        const bool thirdPadding = base64[offset + 2U] == '=';
        const bool fourthPadding = base64[offset + 3U] == '=';

        if (padded || base64[offset] == '=' || base64[offset + 1U] == '=' ||
            (thirdPadding && !fourthPadding) ||
            ((thirdPadding || fourthPadding) && offset + 4U != base64.size()))
        {
          return std::unexpected(FtpError(RemoteErrorCode::ParseError,
                                          "The TLS certificate has invalid base64 padding"));
        }

        const auto first = DecodeBase64Character(base64[offset]);
        const auto second = DecodeBase64Character(base64[offset + 1U]);
        const auto third = thirdPadding ? 0 : DecodeBase64Character(base64[offset + 2U]);
        const auto fourth = fourthPadding ? 0 : DecodeBase64Character(base64[offset + 3U]);

        if (first < 0 || second < 0 || third < 0 || fourth < 0)
        {
          return std::unexpected(FtpError(RemoteErrorCode::ParseError,
                                          "The TLS certificate has invalid base64 data"));
        }

        decoded.push_back(std::byte{static_cast<unsigned char>((first << 2) |
                                                               (second >> 4))});

        if (!thirdPadding)
        {
          decoded.push_back(std::byte{static_cast<unsigned char>(((second & 0x0f) << 4) |
                                                                 (third >> 2))});
        }

        if (!fourthPadding)
        {
          decoded.push_back(std::byte{static_cast<unsigned char>(((third & 0x03) << 6) |
                                                                 fourth)});
        }

        padded = thirdPadding || fourthPadding;
      }

      return decoded;
    }

    struct DerElement final
    {
      unsigned char tag{};
      std::size_t begin{};
      std::size_t contentBegin{};
      std::size_t end{};
    };

    [[nodiscard]] std::optional<DerElement> ReadDerElement(
        std::span<const std::byte> der, std::size_t offset) noexcept
    {
      if (offset > der.size() || der.size() - offset < 2U)
      {
        return std::nullopt;
      }

      const auto begin = offset;
      const auto tag = std::to_integer<unsigned char>(der[offset++]);
      const auto firstLength = std::to_integer<unsigned char>(der[offset++]);

      std::size_t length{};

      if ((firstLength & 0x80U) == 0U)
      {
        length = firstLength;
      }
      else
      {
        const auto octets = static_cast<std::size_t>(firstLength & 0x7fU);

        if (octets == 0U || octets > sizeof(std::size_t) ||
            offset > der.size() || der.size() - offset < octets)
        {
          return std::nullopt;
        }

        if (std::to_integer<unsigned char>(der[offset]) == 0U)
        {
          return std::nullopt;
        }

        for (std::size_t index = 0; index < octets; ++index)
        {
          if (length > ((std::numeric_limits<std::size_t>::max)() >> 8U))
          {
            return std::nullopt;
          }

          length = (length << 8U) |
                   std::to_integer<unsigned char>(der[offset + index]);
        }

        offset += octets;

        if (length < 128U)
        {
          return std::nullopt;
        }
      }

      if (offset > der.size() || length > der.size() - offset)
      {
        return std::nullopt;
      }

      return DerElement{.tag = tag,
                        .begin = begin,
                        .contentBegin = offset,
                        .end = offset + length};
    }

    [[nodiscard]] Result<std::vector<std::byte>> ExtractSubjectPublicKeyInfo(
        std::span<const std::byte> der)
    {
      const auto certificate = ReadDerElement(der, 0U);

      if (!certificate || certificate->tag != 0x30U || certificate->end != der.size())
      {
        return std::unexpected(FtpError(RemoteErrorCode::ParseError,
                                        "The TLS certificate is not valid DER"));
      }

      const auto tbs = ReadDerElement(der, certificate->contentBegin);

      if (!tbs || tbs->tag != 0x30U || tbs->end > certificate->end)
      {
        return std::unexpected(FtpError(RemoteErrorCode::ParseError,
                                        "The TLS certificate has no signed body"));
      }

      auto cursor = tbs->contentBegin;

      auto field = ReadDerElement(der, cursor);

      if (!field)
      {
        return std::unexpected(FtpError(RemoteErrorCode::ParseError,
                                        "The TLS certificate body is truncated"));
      }

      if (field->tag == 0xa0U)
      {
        cursor = field->end;
      }

      // serialNumber, signature, issuer, validity, subject
      for (int index = 0; index < 5; ++index)
      {
        field = ReadDerElement(der, cursor);

        if (!field || field->end > tbs->end)
        {
          return std::unexpected(FtpError(RemoteErrorCode::ParseError,
                                          "The TLS certificate body is truncated"));
        }

        cursor = field->end;
      }

      const auto spki = ReadDerElement(der, cursor);

      if (!spki || spki->tag != 0x30U || spki->end > tbs->end)
      {
        return std::unexpected(FtpError(RemoteErrorCode::ParseError,
                                        "The TLS certificate has no public key"));
      }

      return std::vector<std::byte>{der.begin() + static_cast<std::ptrdiff_t>(spki->begin),
                                    der.begin() + static_cast<std::ptrdiff_t>(spki->end)};
    }

    struct CertificateAssessment final
    {
      bool genuinelySelfSigned{};
      bool currentlyTimeValid{};
      bool serverAuthenticationValidExceptTrustAnchor{};
    };

    [[nodiscard]] CertificateAssessment AssessCertificate(
        std::span<const std::byte> der) noexcept
    {
#if defined(_WIN32)
      if (der.empty() || der.size() > (std::numeric_limits<DWORD>::max)())
      {
        return {};
      }

      const std::unique_ptr<const CERT_CONTEXT, decltype(&CertFreeCertificateContext)> certificate{
          CertCreateCertificateContext(
              X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
              reinterpret_cast<const BYTE *>(der.data()),
              static_cast<DWORD>(der.size())),
          CertFreeCertificateContext};

      if (!certificate || !certificate->pCertInfo)
      {
        return {};
      }

      CertificateAssessment assessment;
      assessment.currentlyTimeValid =
          CertVerifyTimeValidity(nullptr, certificate->pCertInfo) == 0;

      const bool namesMatch = CertCompareCertificateName(
                                  X509_ASN_ENCODING,
                                  &certificate->pCertInfo->Subject,
                                  &certificate->pCertInfo->Issuer) != FALSE;

      if (!namesMatch)
      {
        return assessment;
      }

      assessment.genuinelySelfSigned = CryptVerifyCertificateSignatureEx(
                                           0,
                                           X509_ASN_ENCODING,
                                           CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT,
                                           const_cast<CERT_CONTEXT *>(certificate.get()),
                                           CRYPT_VERIFY_CERT_SIGN_ISSUER_PUBKEY,
                                           &certificate->pCertInfo->SubjectPublicKeyInfo,
                                           0,
                                           nullptr) != FALSE;

      if (!assessment.genuinelySelfSigned)
      {
        return assessment;
      }

      CERT_CHAIN_PARA chainParameters{};
      chainParameters.cbSize = sizeof(chainParameters);

      LPSTR serverAuthenticationOid = const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH);
      chainParameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
      chainParameters.RequestedUsage.Usage.cUsageIdentifier = 1;
      chainParameters.RequestedUsage.Usage.rgpszUsageIdentifier =
          &serverAuthenticationOid;

      PCCERT_CHAIN_CONTEXT chain{};

      if (CertGetCertificateChain(
              nullptr,
              certificate.get(),
              nullptr,
              certificate->hCertStore,
              &chainParameters,
              CERT_CHAIN_REVOCATION_CHECK_CHAIN,
              nullptr,
              &chain) == FALSE ||
          !chain || chain->cChain == 0 || !chain->rgpChain ||
          !chain->rgpChain[0])
      {
        if (chain)
        {
          CertFreeCertificateChain(chain);
        }

        return assessment;
      }

      const DWORD errors = chain->rgpChain[0]->TrustStatus.dwErrorStatus;

      CertFreeCertificateChain(chain);

      constexpr DWORD tolerated = CERT_TRUST_IS_UNTRUSTED_ROOT |
                                  CERT_TRUST_IS_PARTIAL_CHAIN |
                                  CERT_TRUST_REVOCATION_STATUS_UNKNOWN |
                                  CERT_TRUST_IS_OFFLINE_REVOCATION |
                                  CERT_TRUST_IS_NOT_TIME_NESTED;

      assessment.serverAuthenticationValidExceptTrustAnchor = (errors & ~tolerated) == 0;

      return assessment;
#else
      if (der.empty() ||
          der.size() > static_cast<std::size_t>((std::numeric_limits<long>::max)()))
      {
        return {};
      }

      const auto *begin = reinterpret_cast<const unsigned char *>(der.data());

      const auto *cursor = begin;

      const std::unique_ptr<X509, decltype(&X509_free)> certificate{
          d2i_X509(nullptr, &cursor, static_cast<long>(der.size())), X509_free};

      if (!certificate || cursor != begin + der.size())
      {
        return {};
      }

      CertificateAssessment assessment;

      assessment.currentlyTimeValid =
          X509_cmp_current_time(X509_get0_notBefore(certificate.get())) < 0 &&
          X509_cmp_current_time(X509_get0_notAfter(certificate.get())) > 0;

      if (X509_NAME_cmp(X509_get_subject_name(certificate.get()),
                        X509_get_issuer_name(certificate.get())) != 0)
      {
        return assessment;
      }

      const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> publicKey{
          X509_get_pubkey(certificate.get()), EVP_PKEY_free};

      assessment.genuinelySelfSigned =
          publicKey && X509_verify(certificate.get(), publicKey.get()) == 1;

      if (!assessment.genuinelySelfSigned)
      {
        return assessment;
      }

      const std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> tlsContext{
          SSL_CTX_new(TLS_client_method()), SSL_CTX_free};

      const std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store{
          X509_STORE_new(), X509_STORE_free};

      const std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)> context{
          X509_STORE_CTX_new(), X509_STORE_CTX_free};

      if (!tlsContext || !store || !context ||
          X509_STORE_CTX_init(context.get(), store.get(), certificate.get(), nullptr) != 1 ||
          X509_STORE_CTX_set_purpose(context.get(), X509_PURPOSE_SSL_SERVER) != 1)
      {
        return assessment;
      }

      X509_VERIFY_PARAM_set_auth_level(
          X509_STORE_CTX_get0_param(context.get()),
          SSL_CTX_get_security_level(tlsContext.get()));

      X509_STORE_CTX_set_verify_cb(
          context.get(), [](const int valid, X509_STORE_CTX *verification) -> int
          {
            // The leaf's own signature was checked above. Continue verification
            // past its missing trust anchor, never past another certificate defect.
            return valid ||
                   (X509_STORE_CTX_get_error_depth(verification) == 0 &&
                    X509_STORE_CTX_get_error(verification) ==
                        X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT); });

      assessment.serverAuthenticationValidExceptTrustAnchor =
          X509_verify_cert(context.get()) == 1;

      return assessment;
#endif
    }

    [[nodiscard]] Result<ftp::TlsPublicKeyIdentity> InspectCertificate(
        std::string_view pem)
    {
      auto der = DecodeCertificatePem(pem);

      if (!der)
      {
        return std::unexpected(der.error());
      }

      const auto assessment = AssessCertificate(*der);

      auto publicKey = ExtractSubjectPublicKeyInfo(*der);

      if (!publicKey)
      {
        return std::unexpected(publicKey.error());
      }

      auto fingerprint = Sha256Fingerprint(publicKey->data(), publicKey->size());

      if (!fingerprint.starts_with("SHA256:"))
      {
        return std::unexpected(FtpError(RemoteErrorCode::ProtocolError,
                                        "Could not fingerprint the TLS public key"));
      }

      std::string pin = "sha256//" + fingerprint.substr(7U);

      while (pin.size() % 4U != 0U)
      {
        pin.push_back('=');
      }

      return ftp::TlsPublicKeyIdentity{.curlPin = std::move(pin),
                                       .sha256Fingerprint = std::move(fingerprint),
                                       .subjectPublicKeyInfo = std::move(*publicKey),
                                       .genuinelySelfSigned = assessment.genuinelySelfSigned,
                                       .currentlyTimeValid = assessment.currentlyTimeValid,
                                       .serverAuthenticationValidExceptTrustAnchor =
                                           assessment.serverAuthenticationValidExceptTrustAnchor};
    }

    [[nodiscard]] std::string_view Trim(std::string_view value) noexcept
    {
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
      {
        value.remove_prefix(1);
      }

      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
      {
        value.remove_suffix(1);
      }

      return value;
    }

    [[nodiscard]] std::string Lower(std::string_view value)
    {
      std::string result{value};

      std::ranges::transform(result, result.begin(), [](unsigned char character)
                             { return static_cast<char>(std::tolower(character)); });

      return result;
    }

    template <typename Integer>
    [[nodiscard]] std::optional<Integer> ParseInteger(std::string_view value, int base = 10)
    {
      Integer result{};

      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result, base);

      if (error != std::errc{} || end != value.data() + value.size())
      {
        return std::nullopt;
      }

      return result;
    }

    [[nodiscard]] bool ValidUtf8(std::string_view value) noexcept
    {
      for (std::size_t offset = 0; offset < value.size();)
      {
        const auto first = static_cast<unsigned char>(value[offset]);

        if (first <= 0x7fU)
        {
          ++offset;

          continue;
        }

        std::size_t length{};

        unsigned char secondMinimum = 0x80U;
        unsigned char secondMaximum = 0xbfU;

        if (first >= 0xc2U && first <= 0xdfU)
        {
          length = 2U;
        }
        else if (first >= 0xe0U && first <= 0xefU)
        {
          length = 3U;

          if (first == 0xe0U)
          {
            secondMinimum = 0xa0U; // No overlong form
          }

          if (first == 0xedU)
          {
            secondMaximum = 0x9fU; // No UTF-16 surrogate
          }
        }
        else if (first >= 0xf0U && first <= 0xf4U)
        {
          length = 4U;

          if (first == 0xf0U)
          {
            secondMinimum = 0x90U; // No overlong form
          }

          if (first == 0xf4U)
          {
            secondMaximum = 0x8fU; // At most U+10FFFF
          }
        }
        else
        {
          return false;
        }

        if (length > value.size() - offset)
        {
          return false;
        }

        const auto second = static_cast<unsigned char>(value[offset + 1U]);
        if (second < secondMinimum || second > secondMaximum)
        {
          return false;
        }

        for (std::size_t index = 2U; index < length; ++index)
        {
          const auto continuation = static_cast<unsigned char>(value[offset + index]);

          if (continuation < 0x80U || continuation > 0xbfU)
          {
            return false;
          }
        }

        offset += length;
      }

      return true;
    }

    [[nodiscard]] std::optional<std::uint32_t> CodePageAlias(
        std::string_view loweredName)
    {
      struct Alias final
      {
        std::string_view name;
        std::uint32_t codePage;
      };

      static constexpr std::array aliases{
          Alias{"utf-8", 65001U},
          Alias{"utf8", 65001U},
          Alias{"unicode-1-1-utf-8", 65001U},
          Alias{"us-ascii", 20127U},
          Alias{"ascii", 20127U},
          Alias{"ansi_x3.4-1968", 20127U},
          Alias{"latin1", 28591U},
          Alias{"latin-1", 28591U},
          Alias{"iso-ir-100", 28591U},
          Alias{"shift_jis", 932U},
          Alias{"shift-jis", 932U},
          Alias{"sjis", 932U},
          Alias{"windows-31j", 932U},
          Alias{"ms_kanji", 932U},
          Alias{"gb2312", 936U},
          Alias{"gbk", 936U},
          Alias{"x-gbk", 936U},
          Alias{"big5", 950U},
          Alias{"csbig5", 950U},
          Alias{"euc-kr", 949U},
          Alias{"ks_c_5601-1987", 949U},
          Alias{"ks-c-5601", 949U},
          Alias{"gb18030", 54936U},
          Alias{"koi8-r", 20866U},
          Alias{"koi8-u", 21866U},
      };

      if (const auto found = std::ranges::find(aliases, loweredName, &Alias::name);
          found != aliases.end())
      {
        return found->codePage;
      }

      if (loweredName.starts_with("iso-8859-"))
      {
        const auto part = ParseInteger<unsigned>(loweredName.substr(9U));

        if (part && *part >= 1U && *part <= 9U)
        {
          return 28590U + *part;
        }

        if (part && *part == 13U)
        {
          return 28603U;
        }

        if (part && *part == 15U)
        {
          return 28605U;
        }

        return std::nullopt;
      }

      auto numeric = loweredName;
      if (numeric.starts_with("windows-"))
      {
        numeric.remove_prefix(8U);
      }
      else if (numeric.starts_with("cp"))
      {
        numeric.remove_prefix(2U);
      }
      else if (numeric.starts_with("ibm"))
      {
        numeric.remove_prefix(3U);
      }
      else
      {
        return std::nullopt;
      }

      return ParseInteger<std::uint32_t>(numeric);
    }

    [[nodiscard]] std::string CanonicalCodePageName(std::uint32_t codePage)
    {
      if (codePage == 65001U)
      {
        return "UTF-8";
      }

      if (codePage == 20127U)
      {
        return "US-ASCII";
      }

      if (codePage >= 1250U && codePage <= 1258U)
      {
        return "windows-" + std::to_string(codePage);
      }

      if (codePage >= 28591U && codePage <= 28599U)
      {
        return "ISO-8859-" + std::to_string(codePage - 28590U);
      }

      if (codePage == 28603U)
      {
        return "ISO-8859-13";
      }

      if (codePage == 28605U)
      {
        return "ISO-8859-15";
      }

      if (codePage == 932U)
      {
        return "Shift_JIS";
      }

      if (codePage == 936U)
      {
        return "GBK";
      }

      if (codePage == 949U)
      {
        return "EUC-KR";
      }

      if (codePage == 950U)
      {
        return "Big5";
      }

      if (codePage == 54936U)
      {
        return "GB18030";
      }

      if (codePage == 20866U)
      {
        return "KOI8-R";
      }

      if (codePage == 21866U)
      {
        return "KOI8-U";
      }

      return "CP" + std::to_string(codePage);
    }

    [[nodiscard]] bool StructurallyUsableFtpCodePage(std::uint32_t codePage) noexcept
    {
      // FTP commands and path separators are ASCII octets, so wide and
      // stateful encodings cannot be made safe merely by percent-encoding URLs.
      if (codePage == 0U || codePage == 42U || codePage == 65000U ||
          codePage == 1200U || codePage == 1201U ||
          codePage == 12000U || codePage == 12001U ||
          (codePage >= 50220U && codePage <= 50229U) ||
          (codePage >= 57002U && codePage <= 57011U))
      {
        return false;
      }

      return codePage <= 65535U;
    }

#if defined(_WIN32)
    [[nodiscard]] DWORD WideConversionFlags(std::uint32_t codePage) noexcept
    {
      return codePage == 65001U || codePage == 54936U
                 ? WC_ERR_INVALID_CHARS
                 : WC_NO_BEST_FIT_CHARS;
    }

    [[nodiscard]] bool InstalledAsciiCompatibleCodePage(
        std::uint32_t codePage) noexcept
    {
      if (!StructurallyUsableFtpCodePage(codePage) ||
          IsValidCodePage(static_cast<UINT>(codePage)) == FALSE)
      {
        return false;
      }

      CPINFOEXW information{};

      if (GetCPInfoExW(static_cast<UINT>(codePage), 0, &information) == FALSE)
      {
        return false;
      }

      constexpr wchar_t ascii[] =
          L"\r\n !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ"
          L"[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";

      constexpr std::string_view expected =
          "\r\n !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ"
          "[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";

      std::array<char, expected.size()> converted{};

      BOOL usedDefault{};

      const BOOL *noDefaultCharacter =
          codePage == 65001U || codePage == 54936U ? nullptr : &usedDefault;

      const auto count = WideCharToMultiByte(
          static_cast<UINT>(codePage),
          WideConversionFlags(codePage),
          ascii,
          static_cast<int>(expected.size()),
          converted.data(),
          static_cast<int>(converted.size()),
          nullptr,
          const_cast<BOOL *>(noDefaultCharacter));

      return count == static_cast<int>(expected.size()) &&
             usedDefault == FALSE &&
             std::string_view{converted.data(), converted.size()} == expected;
    }
#else
    [[nodiscard]] std::string ConverterCodePageName(const std::uint32_t codePage)
    {
      // These Windows code pages include mappings beyond the similarly named
      // IANA character sets. Ask the platform converter for the exact code page.
      if (codePage == 932U || codePage == 936U || codePage == 949U || codePage == 950U)
      {
        return "CP" + std::to_string(codePage);
      }

      return CanonicalCodePageName(codePage);
    }

    [[nodiscard]] std::optional<std::string> ConvertWithWx(
        const wxMBConv &source, const wxMBConv &destination, const std::string_view text)
    {
      if (text.empty())
      {
        return std::string{};
      }

      std::size_t wideLength{};

      const auto wide = source.cMB2WC(text.data(), text.size(), &wideLength);
      if (!wide)
      {
        return std::nullopt;
      }

      std::size_t byteLength{};

      const auto bytes = destination.cWC2MB(wide.data(), wideLength, &byteLength);
      if (!bytes)
      {
        return std::nullopt;
      }

      return std::string{bytes.data(), byteLength};
    }

    [[nodiscard]] bool InstalledAsciiCompatibleCodePage(const std::uint32_t codePage)
    {
      const auto name = ConverterCodePageName(codePage);

      const wxCSConv converter{wxString::FromUTF8(name.data(), name.size())};
      if (!converter.IsOk())
      {
        return false;
      }

      constexpr std::string_view ascii =
          "\r\n !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ"
          "[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";

      const wxMBConvStrictUTF8 utf8;

      return ConvertWithWx(utf8, converter, ascii) == ascii &&
             ConvertWithWx(converter, utf8, ascii) == ascii;
    }
#endif

    [[nodiscard]] Result<ftp::TextEncoding> ResolveFtpTextEncoding(
        std::string_view name)
    {
      const auto trimmedName = Trim(name);
      if (name != trimmedName || name.empty() ||
          !std::ranges::all_of(name, [](unsigned char character)
                               { return character >= 0x21U && character <= 0x7eU; }))
      {
        return std::unexpected(FtpError(
            RemoteErrorCode::InvalidArgument,
            "FTP encoding must be a non-empty ASCII code-page name"));
      }

      const auto codePage = CodePageAlias(Lower(name));
      if (!codePage || !StructurallyUsableFtpCodePage(*codePage))
      {
        return std::unexpected(FtpError(
            RemoteErrorCode::InvalidArgument,
            "Unsupported FTP encoding '" + std::string{name} + "'"));
      }

      if (!InstalledAsciiCompatibleCodePage(*codePage))
      {
        return std::unexpected(FtpError(
            RemoteErrorCode::InvalidArgument,
            "FTP encoding '" + std::string{name} +
                "' is unavailable or is not ASCII-compatible on this system"));
      }

      return ftp::TextEncoding{.windowsCodePage = *codePage,
                               .canonicalName = CanonicalCodePageName(*codePage),
                               .utf8 = *codePage == 65001U};
    }

    [[nodiscard]] Result<std::string> EncodeFtpServerText(
        std::string_view utf8, const ftp::TextEncoding &encoding)
    {
      if (utf8.find('\0') != std::string_view::npos || !ValidUtf8(utf8))
      {
        return std::unexpected(FtpError(
            RemoteErrorCode::InvalidArgument,
            "The FTP path is not valid NUL-free UTF-8"));
      }

      // wxCSConv intentionally treats ASCII as Latin-1. Enforce the selected
      // seven-bit wire encoding before using any platform converter.
      if (encoding.windowsCodePage == 20127U &&
          std::ranges::any_of(utf8, [](const unsigned char value)
                              { return value > 0x7fU; }))
      {
        return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                        "The FTP path contains text that cannot be represented exactly in US-ASCII"));
      }

      if (encoding.utf8)
      {
        return std::string{utf8};
      }

#if defined(_WIN32)
      if (utf8.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
      {
        return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                        "The FTP path is too long to transcode"));
      }

      if (utf8.empty())
      {
        return std::string{};
      }

      const auto wideLength = MultiByteToWideChar(
          CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
          nullptr, 0);

      if (wideLength <= 0)
      {
        return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                        "The FTP path is not valid UTF-8",
                                        static_cast<int>(GetLastError())));
      }

      std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');

      if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                              static_cast<int>(utf8.size()), wide.data(), wideLength) !=
          wideLength)
      {
        return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                        "Could not decode the UTF-8 FTP path",
                                        static_cast<int>(GetLastError())));
      }

      BOOL usedDefault{};
      BOOL *usedDefaultPointer =
          encoding.windowsCodePage == 54936U ? nullptr : &usedDefault;

      const auto flags = WideConversionFlags(encoding.windowsCodePage);
      const auto encodedLength = WideCharToMultiByte(
          static_cast<UINT>(encoding.windowsCodePage), flags, wide.data(), wideLength,
          nullptr, 0, nullptr, usedDefaultPointer);

      if (encodedLength <= 0 || usedDefault != FALSE)
      {
        return std::unexpected(FtpError(
            RemoteErrorCode::InvalidArgument,
            "The FTP path contains text that cannot be represented exactly in " +
                encoding.canonicalName,
            static_cast<int>(GetLastError())));
      }

      std::string encoded(static_cast<std::size_t>(encodedLength), '\0');
      usedDefault = FALSE;

      if (WideCharToMultiByte(
              static_cast<UINT>(encoding.windowsCodePage), flags, wide.data(), wideLength,
              encoded.data(), encodedLength, nullptr, usedDefaultPointer) != encodedLength ||
          usedDefault != FALSE)
      {
        return std::unexpected(FtpError(
            RemoteErrorCode::InvalidArgument,
            "The FTP path could not be represented exactly in " +
                encoding.canonicalName,
            static_cast<int>(GetLastError())));
      }

      return encoded;
#else
      const auto name = ConverterCodePageName(encoding.windowsCodePage);

      const wxCSConv converter{wxString::FromUTF8(name.data(), name.size())};

      const wxMBConvStrictUTF8 unicode;

      if (converter.IsOk())
      {
        auto encoded = ConvertWithWx(unicode, converter, utf8);

        if (encoded && ConvertWithWx(converter, unicode, *encoded) == utf8)
        {
          return std::move(*encoded);
        }
      }

      return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                      "The FTP path contains text that cannot be represented exactly in " +
                                          encoding.canonicalName));
#endif
    }

    [[nodiscard]] Result<std::string> DecodeFtpServerText(
        std::string_view bytes, const ftp::TextEncoding &encoding)
    {
      if (bytes.find('\0') != std::string_view::npos)
      {
        return std::unexpected(FtpError(RemoteErrorCode::ParseError,
                                        "The FTP server returned a NUL in a path name"));
      }

      if (encoding.windowsCodePage == 20127U &&
          std::ranges::any_of(bytes, [](const unsigned char value)
                              { return value > 0x7fU; }))
      {
        return std::unexpected(FtpError(RemoteErrorCode::ParseError,
                                        "The FTP server returned an invalid US-ASCII path name"));
      }

      if (encoding.utf8)
      {
        if (!ValidUtf8(bytes))
        {
          return std::unexpected(FtpError(
              RemoteErrorCode::ParseError,
              "The FTP server returned an invalid UTF-8 path name"));
        }

        return std::string{bytes};
      }

#if defined(_WIN32)
      if (bytes.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
      {
        return std::unexpected(FtpError(RemoteErrorCode::ParseError,
                                        "The FTP path name is too long to transcode"));
      }

      if (bytes.empty())
      {
        return std::string{};
      }

      const auto wideLength = MultiByteToWideChar(
          static_cast<UINT>(encoding.windowsCodePage), MB_ERR_INVALID_CHARS,
          bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);

      if (wideLength <= 0)
      {
        return std::unexpected(FtpError(
            RemoteErrorCode::ParseError,
            "The FTP server returned an invalid " + encoding.canonicalName +
                " path name",
            static_cast<int>(GetLastError())));
      }

      std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');

      if (MultiByteToWideChar(
              static_cast<UINT>(encoding.windowsCodePage), MB_ERR_INVALID_CHARS,
              bytes.data(), static_cast<int>(bytes.size()), wide.data(), wideLength) !=
          wideLength)
      {
        return std::unexpected(FtpError(RemoteErrorCode::ParseError,
                                        "Could not decode the FTP server path name",
                                        static_cast<int>(GetLastError())));
      }

      const auto utf8Length = WideCharToMultiByte(
          CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wideLength,
          nullptr, 0, nullptr, nullptr);

      if (utf8Length <= 0)
      {
        return std::unexpected(FtpError(RemoteErrorCode::ParseError,
                                        "The FTP server path is not valid Unicode",
                                        static_cast<int>(GetLastError())));
      }

      std::string result(static_cast<std::size_t>(utf8Length), '\0');

      if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wideLength,
                              result.data(), utf8Length, nullptr, nullptr) != utf8Length)
      {
        return std::unexpected(FtpError(RemoteErrorCode::ParseError,
                                        "Could not create the display UTF-8 path name",
                                        static_cast<int>(GetLastError())));
      }

      return result;
#else
      const auto name = ConverterCodePageName(encoding.windowsCodePage);

      const wxCSConv converter{wxString::FromUTF8(name.data(), name.size())};

      const wxMBConvStrictUTF8 unicode;

      if (converter.IsOk())
      {
        auto decoded = ConvertWithWx(converter, unicode, bytes);

        if (decoded && ValidUtf8(*decoded))
        {
          return std::move(*decoded);
        }
      }

      return std::unexpected(FtpError(RemoteErrorCode::ParseError,
                                      "The FTP server returned an invalid " + encoding.canonicalName +
                                          " path name"));
#endif
    }

    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> ParseMlsdTime(
        std::string_view value)
    {
      if (value.size() < 14)
      {
        return std::nullopt;
      }

      const auto year = ParseInteger<int>(value.substr(0, 4));
      const auto month = ParseInteger<unsigned>(value.substr(4, 2));
      const auto day = ParseInteger<unsigned>(value.substr(6, 2));
      const auto hour = ParseInteger<int>(value.substr(8, 2));
      const auto minute = ParseInteger<int>(value.substr(10, 2));
      const auto second = ParseInteger<int>(value.substr(12, 2));

      if (!year || !month || !day || !hour || !minute || !second)
      {
        return std::nullopt;
      }

      const std::chrono::year_month_day date{std::chrono::year{*year},
                                             std::chrono::month{*month},
                                             std::chrono::day{*day}};

      if (!date.ok() || *hour > 23 || *minute > 59 || *second > 60)
      {
        return std::nullopt;
      }

      return std::chrono::sys_days{date} + std::chrono::hours{*hour} +
             std::chrono::minutes{*minute} + std::chrono::seconds{std::min(*second, 59)};
    }

    [[nodiscard]] std::optional<unsigned> MonthNumber(std::string_view month)
    {
      static constexpr std::array<std::string_view, 12> names{
          "jan",
          "feb",
          "mar",
          "apr",
          "may",
          "jun",
          "jul",
          "aug",
          "sep",
          "oct",
          "nov",
          "dec",
      };

      const auto lowered = Lower(month);
      const auto found = std::ranges::find(names, lowered);

      if (found == names.end())
      {
        return std::nullopt;
      }

      return static_cast<unsigned>(std::distance(names.begin(), found) + 1);
    }

    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> ParseListTime(
        std::string_view monthText,
        std::string_view dayText,
        std::string_view yearOrTime)
    {
      const auto month = MonthNumber(monthText);
      const auto day = ParseInteger<unsigned>(dayText);
      if (!month || !day)
      {
        return std::nullopt;
      }

      int year{};
      int hour{};
      int minute{};

      if (const auto colon = yearOrTime.find(':'); colon != std::string_view::npos)
      {
        const auto parsedHour = ParseInteger<int>(yearOrTime.substr(0, colon));
        const auto parsedMinute = ParseInteger<int>(yearOrTime.substr(colon + 1));

        if (!parsedHour || !parsedMinute)
        {
          return std::nullopt;
        }

        hour = *parsedHour;
        minute = *parsedMinute;

        const auto now = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
        const std::chrono::year_month_day today{now};

        year = static_cast<int>(today.year());

        std::chrono::year_month_day candidate{std::chrono::year{year},
                                              std::chrono::month{*month},
                                              std::chrono::day{*day}};

        if (candidate.ok() && std::chrono::sys_days{candidate} > now + 1h * 24 * 31)
        {
          --year;
        }
      }
      else
      {
        const auto parsedYear = ParseInteger<int>(yearOrTime);
        if (!parsedYear)
        {
          return std::nullopt;
        }

        year = *parsedYear;
      }

      const std::chrono::year_month_day date{std::chrono::year{year},
                                             std::chrono::month{*month},
                                             std::chrono::day{*day}};

      if (!date.ok() || hour > 23 || minute > 59)
      {
        return std::nullopt;
      }

      return std::chrono::sys_days{date} + std::chrono::hours{hour} +
             std::chrono::minutes{minute};
    }

    [[nodiscard]] bool SafeFtpCommandPath(std::string_view value) noexcept
    {
      if (value.empty() || value.find('\r') != std::string_view::npos ||
          value.find('\n') != std::string_view::npos || value.find('\0') != std::string_view::npos)
      {
        return false;
      }

      while (!value.empty())
      {
        const auto slash = value.find('/');
        const auto component = value.substr(0, slash);

        if (component == "." || component == "..")
        {
          return false;
        }

        if (slash == std::string_view::npos)
        {
          break;
        }

        value.remove_prefix(slash + 1U);
      }

      return true;
    }

    [[nodiscard]] Result<RemotePath> EncodeFtpRemotePath(
        const RemotePath &path, const ftp::TextEncoding &encoding)
    {
      if (!SafeFtpCommandPath(path.DisplayUtf8()))
      {
        return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                        "Invalid or unsafe display FTP path"));
      }

      auto encodeWhole = [&]() -> Result<RemotePath>
      {
        auto encoded = EncodeFtpServerText(path.DisplayUtf8(), encoding);
        if (!encoded)
        {
          return std::unexpected(encoded.error());
        }

        if (!SafeFtpCommandPath(*encoded))
        {
          return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                          "The encoded FTP path is unsafe"));
        }

        return RemotePath{std::move(*encoded), path.DisplayUtf8()};
      };

      // A path returned by a listing already carries exact server bytes. Keep
      // every component whose decoded value still matches the display value.
      // Components appended later by the UI are transcoded independently, so a
      // non-canonical but valid legacy spelling in an ancestor is not lost.
      if (path.Bytes() == path.DisplayUtf8())
      {
        return encodeWhole();
      }

      std::string encodedPath;
      encodedPath.reserve(path.Bytes().size());

      std::size_t rawOffset{};
      std::size_t displayOffset{};

      while (true)
      {
        const auto rawSlash = path.Bytes().find('/', rawOffset);
        const auto displaySlash = path.DisplayUtf8().find('/', displayOffset);

        if ((rawSlash == std::string::npos) !=
            (displaySlash == std::string::npos))
        {
          return encodeWhole();
        }

        const auto rawEnd = rawSlash == std::string::npos
                                ? path.Bytes().size()
                                : rawSlash;
        const auto displayEnd = displaySlash == std::string::npos
                                    ? path.DisplayUtf8().size()
                                    : displaySlash;
        const auto rawComponent = std::string_view{path.Bytes()}.substr(
            rawOffset, rawEnd - rawOffset);
        const auto displayComponent = std::string_view{path.DisplayUtf8()}.substr(
            displayOffset, displayEnd - displayOffset);

        auto decoded = DecodeFtpServerText(rawComponent, encoding);

        if (decoded && *decoded == displayComponent)
        {
          encodedPath.append(rawComponent);
        }
        else
        {
          auto encoded = EncodeFtpServerText(displayComponent, encoding);
          if (!encoded)
          {
            return std::unexpected(encoded.error());
          }

          encodedPath.append(*encoded);
        }

        if (rawSlash == std::string::npos)
        {
          break;
        }

        encodedPath.push_back('/');

        rawOffset = rawSlash + 1U;
        displayOffset = displaySlash + 1U;
      }

      if (!SafeFtpCommandPath(encodedPath))
      {
        return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                        "The encoded FTP path is unsafe"));
      }

      return RemotePath{std::move(encodedPath), path.DisplayUtf8()};
    }

    [[nodiscard]] std::vector<std::string_view> Lines(std::string_view input)
    {
      std::vector<std::string_view> result;

      while (!input.empty())
      {
        const auto end = input.find('\n');

        auto line = input.substr(0, end);

        if (!line.empty() && line.back() == '\r')
        {
          line.remove_suffix(1);
        }

        if (!line.empty())
        {
          result.push_back(line);
        }

        if (end == std::string_view::npos)
        {
          break;
        }

        input.remove_prefix(end + 1U);
      }

      return result;
    }

    struct TextSink final
    {
      std::string data;
      std::stop_token stopToken;
    };

    std::size_t AppendText(char *contents, std::size_t size, std::size_t count, void *userdata)
    {
      auto &sink = *static_cast<TextSink *>(userdata);

      if (sink.stopToken.stop_requested())
      {
        return 0;
      }

      const auto bytes = size * count;

      sink.data.append(contents, bytes);

      return bytes;
    }

    std::size_t DiscardText(char *, std::size_t size, std::size_t count, void *)
    {
      return size * count;
    }

    struct TransferContext final
    {
      std::stop_token stopToken;
      ProgressCallback callback;
      TransferProgress progress;
      std::uint64_t baseOffset{};
      bool upload{};
      TransferControl control{TransferControl::Continue};
      std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
    };

    int OnFtpTransferProgress(void *userdata,
                              curl_off_t downloadTotal,
                              curl_off_t downloadNow,
                              curl_off_t uploadTotal,
                              curl_off_t uploadNow)
    {
      auto &context = *static_cast<TransferContext *>(userdata);

      if (context.stopToken.stop_requested())
      {
        context.control = TransferControl::Cancel;

        return 1;
      }

      const auto now = context.upload ? uploadNow : downloadNow;
      const auto total = context.upload ? uploadTotal : downloadTotal;

      context.progress.bytesTransferred =
          context.baseOffset + static_cast<std::uint64_t>(std::max<curl_off_t>(0, now));
      context.progress.activeBytesTransferred =
          context.progress.bytesTransferred - context.baseOffset;

      if (total >= 0)
      {
        context.progress.totalBytes =
            context.baseOffset + static_cast<std::uint64_t>(total);
      }

      context.progress.elapsed = std::chrono::steady_clock::now() - context.started;

      if (context.callback)
      {
        context.control = context.callback(context.progress);
      }

      return context.control == TransferControl::Continue ? 0 : 1;
    }

    struct UploadSource final
    {
      std::ifstream stream;
      TransferContext *transfer{};
    };

    std::size_t ReadUpload(char *buffer, std::size_t size, std::size_t count, void *userdata)
    {
      auto &source = *static_cast<UploadSource *>(userdata);

      if (source.transfer->stopToken.stop_requested() ||
          source.transfer->control != TransferControl::Continue)
      {
        return CURL_READFUNC_ABORT;
      }

      const auto capacity = size * count;

      source.stream.read(buffer, static_cast<std::streamsize>(capacity));

      return static_cast<std::size_t>(source.stream.gcount());
    }

    std::size_t ReadEmptyUpload(char *, std::size_t, std::size_t, void *) noexcept
    {
      return 0;
    }

    struct DownloadSink final
    {
      std::ofstream stream;
      TransferContext *transfer{};
    };

    std::size_t WriteDownload(char *buffer, std::size_t size, std::size_t count, void *userdata)
    {
      auto &sink = *static_cast<DownloadSink *>(userdata);

      if (sink.transfer->stopToken.stop_requested() ||
          sink.transfer->control != TransferControl::Continue)
      {
        return 0;
      }

      const auto bytes = size * count;

      sink.stream.write(buffer, static_cast<std::streamsize>(bytes));

      return sink.stream ? bytes : 0;
    }

    [[nodiscard]] Result<void> FinalizeLocalDownload(
        const std::filesystem::path &partial,
        const std::filesystem::path &destination,
        bool overwrite)
    {
#if defined(_WIN32)
      const auto partialFile = CreateFileW(partial.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                           nullptr);

      if (partialFile == INVALID_HANDLE_VALUE)
      {
        return std::unexpected(FtpError(RemoteErrorCode::LocalIo,
                                        "Could not open the partial download for flushing",
                                        static_cast<int>(GetLastError())));
      }

      DWORD flushError{};

      if (!FlushFileBuffers(partialFile))
      {
        flushError = GetLastError();
      }

      if (!CloseHandle(partialFile) && flushError == 0U)
      {
        flushError = GetLastError();
      }

      if (flushError != 0U)
      {
        return std::unexpected(FtpError(RemoteErrorCode::LocalIo,
                                        "Could not durably flush the partial download",
                                        static_cast<int>(flushError)));
      }

      const DWORD flags = MOVEFILE_WRITE_THROUGH |
                          (overwrite ? MOVEFILE_REPLACE_EXISTING : 0U);

      if (MoveFileExW(partial.c_str(), destination.c_str(), flags))
      {
        return {};
      }

      const auto error = GetLastError();

      if (!overwrite && (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS))
      {
        return std::unexpected(FtpError(RemoteErrorCode::AlreadyExists,
                                        "The local destination already exists",
                                        static_cast<int>(error)));
      }

      return std::unexpected(FtpError(RemoteErrorCode::LocalIo,
                                      "Could not atomically finalize the download",
                                      static_cast<int>(error)));
#else
      const auto partialFile = ::open(partial.c_str(), O_RDONLY);

      if (partialFile < 0)
      {
        return std::unexpected(FtpError(RemoteErrorCode::LocalIo,
                                        "Could not open the partial download for flushing",
                                        errno));
      }

      int flushError{};

      if (::fsync(partialFile) != 0)
      {
        flushError = errno;
      }

      if (::close(partialFile) != 0 && flushError == 0)
      {
        flushError = errno;
      }

      if (flushError != 0)
      {
        return std::unexpected(FtpError(RemoteErrorCode::LocalIo,
                                        "Could not durably flush the partial download",
                                        flushError));
      }

      if (overwrite)
      {
        if (::rename(partial.c_str(), destination.c_str()) == 0)
        {
          return {};
        }

        const auto error = errno;

        return std::unexpected(FtpError(RemoteErrorCode::LocalIo,
                                        "Could not atomically finalize the download",
                                        error));
      }

      if (::link(partial.c_str(), destination.c_str()) != 0)
      {
        const auto error = errno;

        return std::unexpected(FtpError(
            error == EEXIST ? RemoteErrorCode::AlreadyExists : RemoteErrorCode::LocalIo,
            error == EEXIST ? "The local destination already exists"
                            : "Could not finalize the download without overwriting",
            error));
      }

      if (::unlink(partial.c_str()) != 0)
      {
        return std::unexpected(FtpError(
            RemoteErrorCode::LocalIo,
            "The download was finalized, but its partial hard link could not be removed",
            errno, false, true));
      }

      return {};
#endif
    }

    [[nodiscard]] RemoteError TransferAbortError(const TransferContext &context)
    {
      if (context.control == TransferControl::Pause)
      {
        return FtpError(RemoteErrorCode::Paused, "The transfer was paused", 0, true);
      }

      return FtpError(RemoteErrorCode::Cancelled, "The transfer was cancelled");
    }

    struct TlsProbeOutcome final
    {
      CURLcode code{CURLE_FAILED_INIT};
      long responseCode{};
      std::string detail;
      std::optional<ftp::TlsPublicKeyIdentity> identity;
    };

    [[nodiscard]] bool TlsProbeReachedFtp(CURLcode code) noexcept
    {
      return code == CURLE_OK || code == CURLE_LOGIN_DENIED ||
             code == CURLE_REMOTE_ACCESS_DENIED || code == CURLE_REMOTE_FILE_NOT_FOUND ||
             code == CURLE_FTP_COULDNT_RETR_FILE;
    }

    [[nodiscard]] bool CertificateVerificationFailed(CURLcode code) noexcept
    {
      return code == CURLE_SSL_CACERT || code == CURLE_PEER_FAILED_VERIFICATION ||
             code == CURLE_SSL_CERTPROBLEM;
    }

    class FtpSession final : public IRemoteSession
    {
    public:
      ~FtpSession() override { Disconnect(); }

      ProtocolKind Protocol() const noexcept override { return mSite.protocol; }
      bool Connected() const noexcept override { return mConnected; }

      Result<void> Connect(const SiteProfile &site,
                           const SessionCallbacks &callbacks,
                           std::stop_token stopToken) override;
      void Disconnect() noexcept override;
      Result<std::vector<RemoteEntry>> List(const RemotePath &path,
                                            std::stop_token stopToken) override;
      Result<RemoteEntry> Stat(const RemotePath &path, std::stop_token stopToken) override;
      Result<void> Mkdir(const RemotePath &path,
                         bool recursive,
                         std::stop_token stopToken) override;
      Result<void> CreateRemoteFile(const RemotePath &path,
                                    bool overwrite,
                                    std::stop_token stopToken) override;
      Result<void> Rename(const RemotePath &source,
                          const RemotePath &destination,
                          bool overwrite,
                          std::stop_token stopToken) override;
      Result<void> Remove(const RemotePath &path,
                          bool recursive,
                          std::stop_token stopToken) override;
      Result<void> SetPermissions(const RemotePath &path,
                                  std::uint32_t permissions,
                                  std::stop_token stopToken) override;
      Result<void> Upload(const std::filesystem::path &localPath,
                          const RemotePath &remotePath,
                          const TransferOptions &options,
                          const ProgressCallback &progress,
                          std::stop_token stopToken) override;
      Result<void> Download(const RemotePath &remotePath,
                            const std::filesystem::path &localPath,
                            const TransferOptions &options,
                            const ProgressCallback &progress,
                            std::stop_token stopToken) override;

    private:
      [[nodiscard]] Result<RemotePath> ServerPath(const RemotePath &path) const;
      Result<void> Configure(const RemotePath &path,
                             ftp::OperationKind operation,
                             bool directory = false);
      Result<void> ConfigureHandle(CURL *handle,
                                   const RemotePath &path,
                                   bool directory,
                                   std::string_view username,
                                   std::string_view password,
                                   bool verifyPeer,
                                   std::string_view publicKeyPin,
                                   char *errorBuffer,
                                   bool includeSessionCommands,
                                   ftp::OperationKind operation);
      Result<TlsProbeOutcome> ProbeTls(bool verifyPeer,
                                       std::string_view publicKeyPin,
                                       bool collectIdentity,
                                       std::stop_token stopToken);
      Result<void> PrepareTlsTrust(std::stop_token stopToken);
      Result<TrustDecision> RequestTlsTrust(
          const ftp::TlsPublicKeyIdentity &identity,
          TrustStatus status,
          std::string diagnostic,
          std::stop_token stopToken) const;
      [[nodiscard]] RemoteError ReportCurlFailure(
          CURLcode code, long responseCode, bool mutating,
          std::optional<ftp::UploadResumeCommand> resumeCommand = std::nullopt);
      Result<void> Perform(std::stop_token stopToken, bool mutating = false,
                           long *responseCode = nullptr);
      CURLcode PerformTraced();
      Result<void> Command(const std::vector<std::string> &commands,
                           std::stop_token stopToken,
                           bool uncertain = true,
                           ftp::OperationKind operation =
                               ftp::OperationKind::ControlCommand);
      Result<void> RemoveImpl(const RemotePath &path,
                              bool recursive,
                              std::stop_token stopToken);
      [[nodiscard]] std::string MakeUrl(CURL *handle,
                                        const RemotePath &path,
                                        bool directory) const;
      [[nodiscard]] std::string EscapePath(CURL *handle, std::string_view path) const;
      void Log(DiagnosticLevel level, std::string_view message) const;

      SiteProfile mSite;
      SessionCallbacks mCallbacks;
      CurlHandle mEasy;
      CurlList mSessionCommands;
      std::string mPassword;
      std::string mUrlHost;
      std::string mActivePortSpecification;
      std::string mActiveTlsPin;
      ftp::TextEncoding mEncoding;
      std::array<char, CURL_ERROR_SIZE> mErrorBuffer{};
      ActiveFtpTrace mActiveTrace;
      ftp::ControlTrace mControlTrace;
      ftp::UploadReplyObserver mUploadReply;
      FtpDebugContext mDebugContext;
      curl_off_t mLastReadyConnection{-1};
      ftp::OperationKind mConfiguredOperation{
          ftp::OperationKind::ControlCommand};
      bool mConnected{};
    };

    Result<void> FtpSession::Connect(const SiteProfile &site,
                                     const SessionCallbacks &callbacks,
                                     std::stop_token stopToken)
    {
      if (mConnected)
      {
        return std::unexpected(FtpError(RemoteErrorCode::AlreadyConnected,
                                        "The FTP session is already connected"));
      }

      if (site.protocol != ProtocolKind::Ftp && site.protocol != ProtocolKind::FtpsExplicit &&
          site.protocol != ProtocolKind::FtpsImplicit)
      {
        return std::unexpected(FtpError(RemoteErrorCode::Unsupported,
                                        "The FTP backend cannot open this protocol"));
      }

      if (site.port == 0)
      {
        return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                        "FTP host and port are required"));
      }

      auto formattedHost = ftp::FormatHostForUrl(site.host);
      if (!formattedHost)
      {
        return std::unexpected(formattedHost.error());
      }

      auto encoding = ftp::ResolveTextEncoding(site.ftpEncoding);
      if (!encoding)
      {
        return std::unexpected(encoding.error());
      }

      if (site.authentication.kind != AuthenticationKind::Password)
      {
        return std::unexpected(FtpError(RemoteErrorCode::Unsupported,
                                        "FTP currently supports password authentication only"));
      }

      if (site.ftpDataConnectionMode != FtpDataConnectionMode::Passive &&
          site.ftpDataConnectionMode != FtpDataConnectionMode::Active)
      {
        return std::unexpected(FtpError(
            RemoteErrorCode::InvalidArgument,
            "The FTP data connection mode is invalid"));
      }

      if (network::CurlRuntimeResult() != CURLE_OK)
      {
        return std::unexpected(FtpError(RemoteErrorCode::ProtocolError,
                                        "libcurl initialization failed"));
      }

      auto activePortSpecification = ftp::MakeActiveModePortSpecification(
          site.ftpActiveAddress);
      if (!activePortSpecification)
      {
        return std::unexpected(activePortSpecification.error());
      }

      if (site.ftpDataConnectionMode == FtpDataConnectionMode::Active &&
          !site.ftpActiveAddress.empty())
      {
        auto assigned = ActiveAddressIsAssigned(site.ftpActiveAddress);
        if (!assigned)
        {
          return std::unexpected(assigned.error());
        }
        if (!*assigned)
        {
          return std::unexpected(FtpError(
              RemoteErrorCode::InvalidArgument,
              "The Active FTP address is not assigned to an active local "
              "network interface. An explicit CURLOPT_FTPPORT address is "
              "used for both the listener bind and EPRT/PORT."));
        }
      }

      if (!callbacks.requestCredential)
      {
        return std::unexpected(FtpError(RemoteErrorCode::CredentialUnavailable,
                                        "No credential provider is available"));
      }

      if (callbacks.connectionTimeout.count() <= 0 ||
          callbacks.commandIdleTimeout.count() <= 0 ||
          callbacks.connectionTimeout.count() > (std::numeric_limits<long>::max)() ||
          callbacks.commandIdleTimeout.count() > (std::numeric_limits<long>::max)() ||
          (site.ftpDataConnectionMode == FtpDataConnectionMode::Active &&
           callbacks.connectionTimeout.count() >
               (std::numeric_limits<long>::max)() / 1000L))
      {
        return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                        "FTP timeouts must be positive whole seconds"));
      }

      mSite = site;

      if (mSite.username.empty())
      {
        mSite.username = "anonymous";
      }

      mCallbacks = callbacks;

      mControlTrace.Initialize(&mCallbacks.diagnostic, &mPassword,
                               ftp::ControlTrace::Requested());

      mUrlHost = std::move(*formattedHost);

      mActivePortSpecification = std::move(*activePortSpecification);

      mEncoding = std::move(*encoding);

      mEasy.reset(curl_easy_init());

      if (!mEasy)
      {
        Disconnect();

        return std::unexpected(FtpError(RemoteErrorCode::ProtocolError,
                                        "Could not create a libcurl session"));
      }

      mConnected = true;

      curl_slist *sessionCommands = nullptr;

      const auto appendSessionCommand = [&sessionCommands](const char *command)
      {
        auto *appended = curl_slist_append(sessionCommands, command);
        if (!appended)
        {
          curl_slist_free_all(sessionCommands);

          sessionCommands = nullptr;

          return false;
        }

        sessionCommands = appended;

        return true;
      };

      if (mEncoding.utf8 && !appendSessionCommand("*OPTS UTF8 ON"))
      {
        Disconnect();

        return std::unexpected(FtpError(RemoteErrorCode::ProtocolError,
                                        "Could not allocate the UTF-8 FTP command"));
      }

      if (!appendSessionCommand(
              "*OPTS MLST type;size;modify;unix.mode;unix.uid;unix.gid;"))
      {
        Disconnect();

        return std::unexpected(FtpError(
            RemoteErrorCode::ProtocolError,
            "Could not allocate the FTP metadata command"));
      }

      mSessionCommands.reset(sessionCommands);

      if (mSite.protocol == ProtocolKind::Ftp)
      {
        Log(DiagnosticLevel::Warning,
            "Plain FTP is insecure: credentials and file contents are not encrypted");
      }
      else if (auto trust = PrepareTlsTrust(stopToken); !trust)
      {
        const auto error = trust.error();

        Disconnect();

        return std::unexpected(error);
      }

      // The FTPS certificate (including an accepted endpoint PIN) is settled
      // before the application asks for, stores, or sends the real credential.
      auto password = mCallbacks.requestCredential(
          CredentialRequest{.kind = CredentialKind::Password,
                            .siteId = mSite.id,
                            .username = mSite.username,
                            .prompt = "Password for " + mSite.username + "@" + mSite.host,
                            .echo = false},
          stopToken);

      if (!password)
      {
        const auto error = password.error();

        Disconnect();

        return std::unexpected(error);
      }

      mPassword = std::move(*password);

      TextSink sink{.data = {}, .stopToken = stopToken};

      if (auto configured = Configure(RemotePath::Root(),
                                      ftp::OperationKind::Nlst,
                                      true);
          !configured)
      {
        Disconnect();

        return configured;
      }

      curl_easy_setopt(mEasy.get(), CURLOPT_DIRLISTONLY, 1L);
      curl_easy_setopt(mEasy.get(), CURLOPT_WRITEFUNCTION, AppendText);
      curl_easy_setopt(mEasy.get(), CURLOPT_WRITEDATA, &sink);

      auto result = Perform(stopToken);

      if (!result)
      {
        Disconnect();
      }

      return result;
    }

    void FtpSession::Disconnect() noexcept
    {
      mControlTrace.Record("application disconnect() requested");

      mConnected = false;

      mEasy.reset();

      mSessionCommands.reset();

      std::fill(mPassword.begin(), mPassword.end(), '\0');
      mPassword.clear();

      std::fill(mActiveTlsPin.begin(), mActiveTlsPin.end(), '\0');
      mActiveTlsPin.clear();

      mUrlHost.clear();

      mActivePortSpecification.clear();

      mEncoding = {};

      mActiveTrace.Clear();

      mControlTrace.Initialize(nullptr, nullptr, false);

      mUploadReply.Reset(nullptr);

      mDebugContext = {};

      mLastReadyConnection = -1;

      mConfiguredOperation = ftp::OperationKind::ControlCommand;

      mCallbacks = {};

      mSite = {};
    }

    void FtpSession::Log(DiagnosticLevel level, std::string_view message) const
    {
      if (mCallbacks.diagnostic)
      {
        mCallbacks.diagnostic(level, message);
      }
    }

    Result<RemotePath> FtpSession::ServerPath(const RemotePath &path) const
    {
      return EncodeFtpRemotePath(path, mEncoding);
    }

    std::string FtpSession::EscapePath(CURL *handle, std::string_view path) const
    {
      std::string escaped;
      escaped.reserve(path.size() + 8U);

      std::size_t offset = 0;

      while (offset < path.size())
      {
        if (path[offset] == '/')
        {
          escaped.push_back('/');

          ++offset;

          continue;
        }

        const auto slash = path.find('/', offset);

        const auto length = slash == std::string_view::npos ? path.size() - offset : slash - offset;

        char *encoded = curl_easy_escape(handle, path.data() + offset,
                                         static_cast<int>(length));

        if (encoded)
        {
          escaped += encoded;

          curl_free(encoded);
        }

        offset += length;
      }

      return escaped;
    }

    std::string FtpSession::MakeUrl(CURL *handle,
                                    const RemotePath &path,
                                    bool directory) const
    {
      const auto scheme = mSite.protocol == ProtocolKind::FtpsImplicit ? "ftps" : "ftp";

      std::string result = std::string{scheme} + "://" + mUrlHost + ":" +
                           std::to_string(mSite.port);

      auto remote = path.Bytes();

      if (remote.empty() || remote.front() != '/')
      {
        remote.insert(remote.begin(), '/');
      }

      result += EscapePath(handle, remote);

      if (directory && result.back() != '/')
      {
        result.push_back('/');
      }

      return result;
    }

    Result<void> FtpSession::ConfigureHandle(CURL *handle,
                                             const RemotePath &path,
                                             bool directory,
                                             std::string_view username,
                                             std::string_view password,
                                             bool verifyPeer,
                                             std::string_view publicKeyPin,
                                             char *errorBuffer,
                                             bool includeSessionCommands,
                                             const ftp::OperationKind operation)
    {
      if (!handle)
      {
        return std::unexpected(FtpError(RemoteErrorCode::ProtocolError,
                                        "Could not create a libcurl session"));
      }

      auto encodedPath = ServerPath(path);
      if (!encodedPath)
      {
        return std::unexpected(encodedPath.error());
      }

      curl_easy_reset(handle);

      const bool usesDataConnection = ftp::UsesDataConnection(operation);
      const bool instrumentActiveMode =
          mSite.ftpDataConnectionMode == FtpDataConnectionMode::Active &&
          usesDataConnection && includeSessionCommands && handle == mEasy.get();

      if (handle == mEasy.get())
      {
        mConfiguredOperation = operation;

        mUploadReply.Reset(&mPassword);

        if (instrumentActiveMode)
        {
          mActiveTrace.Begin(&mCallbacks.diagnostic, operation);
        }
        else
        {
          mActiveTrace.Clear();
        }

        mDebugContext = {instrumentActiveMode ? &mActiveTrace : nullptr,
                         mControlTrace.Enabled() ? &mControlTrace : nullptr,
                         handle, &mLastReadyConnection, &mCallbacks.diagnostic,
                         operation == ftp::OperationKind::Stor ? &mUploadReply : nullptr};
      }

      if (errorBuffer != nullptr)
      {
        errorBuffer[0] = '\0';
      }

      const auto requireOption = [](CURLcode result,
                                    std::string_view option) -> Result<void>
      {
        if (result == CURLE_OK)
        {
          return {};
        }

        return std::unexpected(FtpError(
            RemoteErrorCode::ProtocolError,
            "Could not apply required libcurl option " + std::string{option} +
                ": CURLcode " + std::to_string(static_cast<int>(result)) +
                " (" + curl_easy_strerror(result) + ")",
            static_cast<int>(result)));
      };

      if (auto applied = requireOption(
              curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errorBuffer),
              "CURLOPT_ERRORBUFFER");
          !applied)
      {
        return applied;
      }

      const auto url = MakeUrl(handle, *encodedPath, directory);
      if (auto applied = requireOption(curl_easy_setopt(handle, CURLOPT_URL, url.c_str()),
                                       "CURLOPT_URL");
          !applied)
      {
        return applied;
      }

      if (auto applied = requireOption(
              curl_easy_setopt(handle, CURLOPT_USERNAME, username.data()),
              "CURLOPT_USERNAME");
          !applied)
      {
        return applied;
      }

      if (auto applied = requireOption(
              curl_easy_setopt(handle, CURLOPT_PASSWORD, password.data()),
              "CURLOPT_PASSWORD");
          !applied)
      {
        return applied;
      }

      curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT,
                       static_cast<long>(mCallbacks.connectionTimeout.count()));
      curl_easy_setopt(handle, CURLOPT_FTP_RESPONSE_TIMEOUT,
                       static_cast<long>(mCallbacks.commandIdleTimeout.count()));

      // File transfers have their own sustained low-throughput deadline,
      // independent of connection/authentication and command-response timeouts.
      const long lowSpeedTime = IsFileTransfer(operation)
                                    ? transferLowSpeedTime
                                    : static_cast<long>(mCallbacks.commandIdleTimeout.count());

      if (auto applied = requireOption(
              curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, transferLowSpeedLimit),
              "CURLOPT_LOW_SPEED_LIMIT");
          !applied)
      {
        return applied;
      }

      if (auto applied = requireOption(
              curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, lowSpeedTime),
              "CURLOPT_LOW_SPEED_TIME");
          !applied)
      {
        return applied;
      }

      curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);

      if (mSite.ftpDataConnectionMode == FtpDataConnectionMode::Active &&
          usesDataConnection)
      {
        // Recheck immediately before every CURLOPT_FTPPORT application. The
        // interface may have disappeared since Connect() (for example when a
        // VPN disconnects), and curl must never receive a now-nonlocal
        // explicit bind address.
        if (!mSite.ftpActiveAddress.empty())
        {
          auto assigned = ActiveAddressIsAssigned(mSite.ftpActiveAddress);
          if (!assigned)
          {
            return std::unexpected(assigned.error());
          }
          if (!*assigned)
          {
            return std::unexpected(FtpError(
                RemoteErrorCode::InvalidArgument,
                "The Active FTP address is no longer assigned to an active "
                "local network interface. It was not passed to "
                "CURLOPT_FTPPORT."));
          }
        }

        // "-" uses the control socket's local endpoint and an ephemeral port.
        // An explicit local address is used for both binding and EPRT/PORT.
        // libcurl has no separate public bind/advertise address options.
        if (instrumentActiveMode)
        {
          try
          {
            const std::string setup =
                mActivePortSpecification == "-"
                    ? "CURLOPT_FTPPORT=\"-\" (automatic mode, curl uses the "
                      "established control socket's local address)"
                    : "CURLOPT_FTPPORT=\"" + mActivePortSpecification +
                          "\" (configured ftpActiveAddress, curl uses it for "
                          "both the local listener bind and EPRT/PORT)";

            mActiveTrace.Record(setup);
          }
          catch (...)
          {
          }

          const auto applyTraceOption = [&requireOption](
                                            const CURLcode result,
                                            const std::string_view option)
              -> Result<void>
          {
            return requireOption(result, option);
          };

          if (auto applied = applyTraceOption(
                  curl_easy_setopt(handle, CURLOPT_VERBOSE, 1L),
                  "CURLOPT_VERBOSE");
              !applied)
          {
            return applied;
          }

          if (auto applied = applyTraceOption(
                  curl_easy_setopt(handle, CURLOPT_DEBUGFUNCTION,
                                   ActiveFtpDebug),
                  "CURLOPT_DEBUGFUNCTION");
              !applied)
          {
            return applied;
          }

          if (auto applied = applyTraceOption(
                  curl_easy_setopt(handle, CURLOPT_DEBUGDATA, &mActiveTrace),
                  "CURLOPT_DEBUGDATA");
              !applied)
          {
            return applied;
          }

          if (auto applied = applyTraceOption(
                  curl_easy_setopt(handle, CURLOPT_OPENSOCKETFUNCTION,
                                   ActiveFtpOpenSocket),
                  "CURLOPT_OPENSOCKETFUNCTION");
              !applied)
          {
            return applied;
          }

          if (auto applied = applyTraceOption(
                  curl_easy_setopt(handle, CURLOPT_OPENSOCKETDATA,
                                   &mActiveTrace),
                  "CURLOPT_OPENSOCKETDATA");
              !applied)
          {
            return applied;
          }
        }

        if (auto applied = requireOption(
                curl_easy_setopt(handle, CURLOPT_FTPPORT,
                                 mActivePortSpecification.c_str()),
                "CURLOPT_FTPPORT");
            !applied)
        {
          return applied;
        }

        if (auto applied = requireOption(
                curl_easy_setopt(handle, CURLOPT_FTP_USE_EPRT, 1L),
                "CURLOPT_FTP_USE_EPRT");
            !applied)
        {
          return applied;
        }

        const auto acceptTimeoutMilliseconds = static_cast<long>(mCallbacks.connectionTimeout.count()) * 1000L;

        if (auto applied = requireOption(
                curl_easy_setopt(handle, CURLOPT_ACCEPTTIMEOUT_MS,
                                 acceptTimeoutMilliseconds),
                "CURLOPT_ACCEPTTIMEOUT_MS");
            !applied)
        {
          return applied;
        }
      }
      else if (usesDataConnection)
      {
        if (auto applied = requireOption(
                curl_easy_setopt(handle, CURLOPT_FTP_USE_EPSV, 1L),
                "CURLOPT_FTP_USE_EPSV");
            !applied)
        {
          return applied;
        }
      }

      if (handle == mEasy.get())
      {
        // Observe every ready main control connection without enabling verbose
        // tracing. Isolated FTPS certificate probes never enter this path.
        for (const auto code : {
                 curl_easy_setopt(handle, CURLOPT_PREREQFUNCTION, FtpSessionPrerequisite),
                 curl_easy_setopt(handle, CURLOPT_PREREQDATA, &mDebugContext)})
        {
          if (auto applied = requireOption(code, "FTP connection observer"); !applied)
          {
            return applied;
          }
        }
      }

      if (handle == mEasy.get() && (mControlTrace.Enabled() || mDebugContext.uploadReply))
      {
        // This only observes libcurl's existing callbacks. In particular, do
        // not poll/read the idle socket here or inject FTP keepalive commands.
        // STOR also needs bounded final-reply capture without the opt-in trace.
        // curl decrypts FTPS control replies before delivering these events.
        for (const auto code : {
                 curl_easy_setopt(handle, CURLOPT_VERBOSE, 1L),
                 curl_easy_setopt(handle, CURLOPT_DEBUGFUNCTION, FtpSessionDebug),
                 curl_easy_setopt(handle, CURLOPT_DEBUGDATA, &mDebugContext)})
        {
          if (auto applied = requireOption(code, "FTP control reply/trace callback"); !applied)
          {
            return applied;
          }
        }
      }

      curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);

      if (mSite.protocol == ProtocolKind::FtpsExplicit ||
          mSite.protocol == ProtocolKind::FtpsImplicit)
      {
        if (auto applied = requireOption(
                curl_easy_setopt(handle, CURLOPT_USE_SSL, CURLUSESSL_ALL),
                "CURLOPT_USE_SSL");
            !applied)
        {
          return applied;
        }

        if (auto applied = requireOption(
                curl_easy_setopt(handle, CURLOPT_FTPSSLAUTH, CURLFTPAUTH_TLS),
                "CURLOPT_FTPSSLAUTH");
            !applied)
        {
          return applied;
        }

        if (auto applied = requireOption(
                curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L),
                "CURLOPT_SSL_VERIFYHOST");
            !applied)
        {
          return applied;
        }

        if (!publicKeyPin.empty())
        {
          // Install the PIN before disabling chain verification. If copying
          // or accepting the PIN option fails, this handle is never
          // performed and therefore cannot fail open with real credentials.
          if (auto applied = requireOption(
                  curl_easy_setopt(handle, CURLOPT_PINNEDPUBLICKEY,
                                   publicKeyPin.data()),
                  "CURLOPT_PINNEDPUBLICKEY");
              !applied)
          {
            return applied;
          }
        }

        if (auto applied = requireOption(
                curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER,
                                 verifyPeer ? 1L : 0L),
                "CURLOPT_SSL_VERIFYPEER");
            !applied)
        {
          return applied;
        }
      }
      else
      {
        if (auto applied = requireOption(
                curl_easy_setopt(handle, CURLOPT_USE_SSL, CURLUSESSL_NONE),
                "CURLOPT_USE_SSL");
            !applied)
        {
          return applied;
        }
      }

      if (includeSessionCommands && mSessionCommands)
      {
        if (auto applied = requireOption(
                curl_easy_setopt(handle, CURLOPT_QUOTE, mSessionCommands.get()),
                "CURLOPT_QUOTE");
            !applied)
        {
          return applied;
        }
      }

      return {};
    }

    Result<void> FtpSession::Configure(
        const RemotePath &path,
        const ftp::OperationKind operation,
        const bool directory)
    {
      if (!mConnected || !mEasy)
      {
        return std::unexpected(FtpError(RemoteErrorCode::NotConnected,
                                        "The FTP session is not connected"));
      }

      mErrorBuffer.fill('\0');

      return ConfigureHandle(mEasy.get(), path, directory, mSite.username, mPassword,
                             mActiveTlsPin.empty(), mActiveTlsPin, mErrorBuffer.data(),
                             true, operation);
    }

    Result<TlsProbeOutcome> FtpSession::ProbeTls(bool verifyPeer,
                                                 std::string_view publicKeyPin,
                                                 bool collectIdentity,
                                                 std::stop_token stopToken)
    {
      CurlHandle probe{curl_easy_init()};

      if (!probe)
      {
        return std::unexpected(FtpError(RemoteErrorCode::ProtocolError,
                                        "Could not create an FTPS trust probe"));
      }

      std::array<char, CURL_ERROR_SIZE> error{};

      // Neither the real username nor its password is sent by this isolated
      // operation. A matching PIN is rechecked by the real connection before it
      // can transmit USER/PASS, closing the probe-to-login rotation window.
      constexpr std::string_view probeUsername = "havremote-tls-probe";
      constexpr std::string_view probePassword = "not-a-user-credential";

      if (auto configured = ConfigureHandle(probe.get(), RemotePath::Root(), true,
                                            probeUsername, probePassword, verifyPeer,
                                            publicKeyPin, error.data(), false,
                                            ftp::OperationKind::Nlst);
          !configured)
      {
        return std::unexpected(configured.error());
      }

      curl_easy_setopt(probe.get(), CURLOPT_DIRLISTONLY, 1L);
      curl_easy_setopt(probe.get(), CURLOPT_WRITEFUNCTION, DiscardText);
      curl_easy_setopt(probe.get(), CURLOPT_CERTINFO, collectIdentity ? 1L : 0L);
      curl_easy_setopt(probe.get(), CURLOPT_FRESH_CONNECT, 1L);
      curl_easy_setopt(probe.get(), CURLOPT_FORBID_REUSE, 1L);
      curl_easy_setopt(probe.get(), CURLOPT_SSL_SESSIONID_CACHE, 0L);

      TransferContext cancellation{.stopToken = stopToken,
                                   .callback = {},
                                   .progress = {},
                                   .baseOffset = 0,
                                   .upload = false,
                                   .control = TransferControl::Continue,
                                   .started = std::chrono::steady_clock::now()};

      curl_easy_setopt(probe.get(), CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(probe.get(), CURLOPT_XFERINFOFUNCTION, OnFtpTransferProgress);
      curl_easy_setopt(probe.get(), CURLOPT_XFERINFODATA, &cancellation);

      TlsProbeOutcome outcome;
      outcome.code = curl_easy_perform(probe.get());

      curl_easy_getinfo(probe.get(), CURLINFO_RESPONSE_CODE, &outcome.responseCode);

      outcome.detail = error.data();

      if (stopToken.stop_requested())
      {
        return std::unexpected(FtpError(RemoteErrorCode::Cancelled,
                                        "The FTPS trust check was cancelled"));
      }

      if (collectIdentity && TlsProbeReachedFtp(outcome.code))
      {
        curl_certinfo *certificates{};

        if (curl_easy_getinfo(probe.get(), CURLINFO_CERTINFO, &certificates) != CURLE_OK ||
            !certificates || certificates->num_of_certs < 1 || !certificates->certinfo ||
            !certificates->certinfo[0])
        {
          return std::unexpected(FtpError(
              RemoteErrorCode::CertificateInvalid,
              "The TLS provider did not expose the FTPS leaf certificate"));
        }

        std::string_view pem;

        for (auto *field = certificates->certinfo[0]; field; field = field->next)
        {
          if (field->data && std::string_view{field->data}.starts_with("Cert:"))
          {
            pem = std::string_view{field->data}.substr(5U);

            break;
          }
        }

        if (pem.empty())
        {
          return std::unexpected(FtpError(
              RemoteErrorCode::CertificateInvalid,
              "The TLS provider omitted the FTPS leaf certificate bytes"));
        }

        auto identity = ftp::InspectCertificatePem(pem);
        if (!identity)
        {
          return std::unexpected(identity.error());
        }

        outcome.identity = std::move(*identity);
      }

      return outcome;
    }

    Result<TrustDecision> FtpSession::RequestTlsTrust(
        const ftp::TlsPublicKeyIdentity &identity,
        TrustStatus status,
        std::string diagnostic,
        std::stop_token stopToken) const
    {
      if (!mCallbacks.verifyTrust)
      {
        return std::unexpected(FtpError(
            status == TrustStatus::Changed ? RemoteErrorCode::HostKeyChanged
                                           : RemoteErrorCode::CertificateInvalid,
            status == TrustStatus::Changed
                ? "The FTPS endpoint public key has changed"
                : "The FTPS certificate is not trusted and no trust handler is available"));
      }

      TrustChallenge challenge;
      challenge.kind = TrustKind::TlsCertificate;
      challenge.status = status;
      challenge.siteId = mSite.id;
      challenge.host = mSite.host;
      challenge.port = mSite.port;
      challenge.algorithm = "X.509 SubjectPublicKeyInfo";
      challenge.sha256Fingerprint = identity.sha256Fingerprint;
      challenge.publicKey = identity.subjectPublicKeyInfo;
      challenge.diagnostic = std::move(diagnostic);

      auto decision = mCallbacks.verifyTrust(challenge, stopToken);
      if (!decision)
      {
        return std::unexpected(decision.error());
      }
      if (!ftp::IsTlsTrustDecisionAllowed(status, *decision))
      {
        if (*decision == TrustDecision::Reject)
        {
          return std::unexpected(FtpError(
              status == TrustStatus::Changed ? RemoteErrorCode::HostKeyChanged
                                             : RemoteErrorCode::TrustRejected,
              status == TrustStatus::Changed
                  ? "The changed FTPS endpoint public key was rejected"
                  : "The untrusted FTPS certificate was rejected"));
        }

        return std::unexpected(FtpError(
            status == TrustStatus::Changed ? RemoteErrorCode::HostKeyChanged
                                           : RemoteErrorCode::InvalidArgument,
            status == TrustStatus::Changed
                ? "A changed FTPS endpoint PIN must be explicitly replaced"
                : "There is no stored FTPS endpoint PIN to replace"));
      }

      return decision;
    }

    Result<void> FtpSession::PrepareTlsTrust(std::stop_token stopToken)
    {
      mActiveTlsPin.clear();

      auto mapProbeFailure = [](const TlsProbeOutcome &outcome) -> Result<void>
      {
        return std::unexpected(ftp::MakeOperationError(
            outcome.code, outcome.detail, outcome.responseCode, false,
            std::nullopt, ftp::OperationKind::ControlCommand));
      };

      auto obtainIdentity = [&]() -> Result<ftp::TlsPublicKeyIdentity>
      {
        auto probe = ProbeTls(false, {}, true, stopToken);
        if (!probe)
        {
          return std::unexpected(probe.error());
        }

        if (!TlsProbeReachedFtp(probe->code))
        {
          if (CertificateVerificationFailed(probe->code))
          {
            return std::unexpected(FtpError(
                RemoteErrorCode::CertificateInvalid,
                "The FTPS certificate hostname does not match the requested endpoint",
                static_cast<int>(probe->code)));
          }

          return std::unexpected(ftp::MakeOperationError(
              probe->code, probe->detail, probe->responseCode, false,
              std::nullopt, ftp::OperationKind::ControlCommand));
        }

        if (!probe->identity)
        {
          return std::unexpected(FtpError(RemoteErrorCode::CertificateInvalid,
                                          "Could not inspect the FTPS leaf certificate"));
        }

        return std::move(*probe->identity);
      };

      auto verifyChosenPin = [&](const std::string &pin) -> Result<void>
      {
        auto check = ProbeTls(false, pin, false, stopToken);
        if (!check)
        {
          return std::unexpected(check.error());
        }

        if (!TlsProbeReachedFtp(check->code))
        {
          return mapProbeFailure(*check);
        }

        return {};
      };

      // Always perform normal TLS verification, even when an endpoint PIN
      // is already stored. A PIN is allowed to override only the missing trust
      // anchor of a currently valid, cryptographically self-signed leaf. It must
      // never hide expiration, revocation, hostname, or other chain failures.
      auto verified = ProbeTls(true, {}, false, stopToken);
      if (!verified)
      {
        return std::unexpected(verified.error());
      }

      const bool normallyTrusted = TlsProbeReachedFtp(verified->code);

      std::optional<ftp::TlsVerificationFailure> verificationFailure;

      if (!normallyTrusted)
      {
        if (!CertificateVerificationFailed(verified->code))
        {
          return mapProbeFailure(*verified);
        }

        verificationFailure = ftp::ClassifyTlsVerificationFailure(verified->detail);

        if (*verificationFailure != ftp::TlsVerificationFailure::UntrustedIssuer)
        {
          return std::unexpected(FtpError(
              RemoteErrorCode::CertificateInvalid,
              "The FTPS certificate failed validation and is not eligible for a "
              "self-signed endpoint exception: " +
                  verified->detail,
              static_cast<int>(verified->code)));
        }
      }

      auto requireEligibleException = [&](const ftp::TlsPublicKeyIdentity &identity)
          -> Result<void>
      {
        if (normallyTrusted ||
            (verificationFailure &&
             ftp::IsTlsPinExceptionEligible(identity, *verificationFailure)))
        {
          return {};
        }

        return std::unexpected(FtpError(
            RemoteErrorCode::CertificateInvalid,
            "The FTPS certificate is not a currently valid, cryptographically "
            "self-signed certificate. Endpoint PIN exceptions are not permitted",
            static_cast<int>(verified->code)));
      };

      if (!mCallbacks.tlsPinnedPublicKey.empty())
      {
        auto pinned = ProbeTls(false, mCallbacks.tlsPinnedPublicKey,
                               !normallyTrusted, stopToken);
        if (!pinned)
        {
          return std::unexpected(pinned.error());
        }

        if (TlsProbeReachedFtp(pinned->code))
        {
          if (!normallyTrusted)
          {
            if (!pinned->identity)
            {
              return std::unexpected(FtpError(
                  RemoteErrorCode::CertificateInvalid,
                  "Could not inspect the pinned FTPS leaf certificate"));
            }

            if (auto eligible = requireEligibleException(*pinned->identity); !eligible)
            {
              return eligible;
            }
          }

          mActiveTlsPin = mCallbacks.tlsPinnedPublicKey;

          return {};
        }

        if (pinned->code != CURLE_SSL_PINNEDPUBKEYNOTMATCH)
        {
          return mapProbeFailure(*pinned);
        }

        auto identity = obtainIdentity();
        if (!identity)
        {
          return std::unexpected(identity.error());
        }

        if (auto eligible = requireEligibleException(*identity); !eligible)
        {
          return eligible;
        }

        auto decision = RequestTlsTrust(
            *identity, TrustStatus::Changed,
            "The received FTPS public key differs from the saved endpoint PIN. "
            "Certificate hostname verification succeeded.",
            stopToken);

        if (!decision)
        {
          return std::unexpected(decision.error());
        }

        if (auto checked = verifyChosenPin(identity->curlPin); !checked)
        {
          return checked;
        }

        mActiveTlsPin = identity->curlPin;

        return {};
      }

      if (normallyTrusted)
      {
        return {};
      }

      auto identity = obtainIdentity();
      if (!identity)
      {
        return std::unexpected(identity.error());
      }

      if (auto eligible = requireEligibleException(*identity); !eligible)
      {
        return eligible;
      }

      auto decision = RequestTlsTrust(
          *identity, TrustStatus::Invalid,
          "The TLS provider rejected this currently valid, cryptographically self-signed "
          "certificate because its issuer is not trusted. Its hostname was verified "
          "independently before this endpoint-scoped PIN was offered.",
          stopToken);

      if (!decision)
      {
        return std::unexpected(decision.error());
      }

      if (auto checked = verifyChosenPin(identity->curlPin); !checked)
      {
        return checked;
      }

      mActiveTlsPin = identity->curlPin;

      return {};
    }

    RemoteError FtpSession::ReportCurlFailure(
        const CURLcode code, const long responseCode, const bool mutating,
        const std::optional<ftp::UploadResumeCommand> resumeCommand)
    {
      const auto operation = mConfiguredOperation;

      long osError{};

      const CURLcode osErrorResult =
          curl_easy_getinfo(mEasy.get(), CURLINFO_OS_ERRNO, &osError);

      const std::optional<long> reportedOsError =
          osErrorResult == CURLE_OK ? std::optional<long>{osError} : std::nullopt;

      const auto detail = ftp::FormatFailureDetail(
          code, mErrorBuffer.data(), responseCode, reportedOsError);

      const auto *uploadReply = operation == ftp::OperationKind::Stor
                                    ? mUploadReply.Reply()
                                    : nullptr;

      if (operation == ftp::OperationKind::Stor)
      {
        // Never read a raw control socket here: curl owns its buffers and TLS
        // state. On a premature data failure curl may skip the final reply and
        // close that connection, so "unavailable" is not evidence of quota.
        Log(DiagnosticLevel::Debug,
            uploadReply
                ? "FTP STOR final control reply: " + std::to_string(uploadReply->code) +
                      " " + uploadReply->text
                : "FTP STOR final control reply: unavailable (libcurl did not deliver "
                  "a complete final STOR reply, last FTP response code " +
                      std::to_string(responseCode) + ")");
      }

      if (mSite.ftpDataConnectionMode == FtpDataConnectionMode::Active &&
          ftp::UsesDataConnection(operation))
      {
        if (!mActiveTrace.ControlEndpointSeen())
        {
          char *remoteAddress{};
          char *localAddress{};
          long remotePort{-1};
          long localPort{-1};

          const bool endpointAvailable =
              curl_easy_getinfo(mEasy.get(), CURLINFO_PRIMARY_IP,
                                &remoteAddress) == CURLE_OK &&
              curl_easy_getinfo(mEasy.get(), CURLINFO_PRIMARY_PORT,
                                &remotePort) == CURLE_OK &&
              curl_easy_getinfo(mEasy.get(), CURLINFO_LOCAL_IP,
                                &localAddress) == CURLE_OK &&
              curl_easy_getinfo(mEasy.get(), CURLINFO_LOCAL_PORT,
                                &localPort) == CURLE_OK &&
              remoteAddress != nullptr && localAddress != nullptr;

          if (endpointAvailable)
          {
            mActiveTrace.RecordControlEndpoint(
                remoteAddress, static_cast<int>(remotePort), localAddress,
                static_cast<int>(localPort));
          }
          else
          {
            mActiveTrace.Record(
                "The established control endpoint was unavailable through "
                "CURLOPT_PREREQFUNCTION and CURLINFO_LOCAL_IP/LOCAL_PORT");
          }
        }

        mActiveTrace.Record("curl_easy_perform() failed: " + detail);

        mActiveTrace.Dump();
      }

      if (operation == ftp::OperationKind::Rename &&
          code == CURLE_QUOTE_ERROR && responseCode >= 400)
      {
        Log(DiagnosticLevel::Debug,
            "FTP rename/path failure (RNFR/RNTO): " + detail);
      }
      else
      {
        // The caller reports the returned error with the action's context.
        // Keep raw library diagnostics without duplicating that error entry.
        Log(DiagnosticLevel::Debug, "libcurl FTP failure: " + detail);
      }

      if (resumeCommand)
      {
        return ftp::MakeUploadResumeError(code, mErrorBuffer.data(), responseCode,
                                          reportedOsError, *resumeCommand);
      }

      return ftp::MakeOperationError(code, mErrorBuffer.data(), responseCode, mutating,
                                     reportedOsError, operation, uploadReply);
    }

    CURLcode FtpSession::PerformTraced()
    {
      const int entrySocketError = CurrentSocketError();

      if (IsFileTransfer(mConfiguredOperation))
      {
        Log(DiagnosticLevel::Debug,
            "FTP " + std::string{FtpOperationName(mConfiguredOperation)} +
                " low-speed policy: CURLOPT_LOW_SPEED_LIMIT=" +
                std::to_string(transferLowSpeedLimit) +
                " byte/sec, CURLOPT_LOW_SPEED_TIME=" +
                std::to_string(transferLowSpeedTime) + " seconds");
      }

      mControlTrace.Begin(mEasy.get(), FtpOperationName(mConfiguredOperation));

      RestoreSocketError(entrySocketError);

      const auto result = curl_easy_perform(mEasy.get());

      const int savedSocketError = CurrentSocketError();

      mControlTrace.Finish(result, mErrorBuffer.data());

      RestoreSocketError(savedSocketError);

      return result;
    }

    Result<void> FtpSession::Perform(std::stop_token stopToken,
                                     const bool mutating,
                                     long *const responseCode)
    {
      TransferContext cancellation{.stopToken = stopToken,
                                   .callback = {},
                                   .progress = {},
                                   .baseOffset = 0,
                                   .upload = false,
                                   .control = TransferControl::Continue,
                                   .started = std::chrono::steady_clock::now()};

      curl_easy_setopt(mEasy.get(), CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(mEasy.get(), CURLOPT_XFERINFOFUNCTION, OnFtpTransferProgress);
      curl_easy_setopt(mEasy.get(), CURLOPT_XFERINFODATA, &cancellation);

      const auto result = PerformTraced();

      long response{};

      curl_easy_getinfo(mEasy.get(), CURLINFO_RESPONSE_CODE, &response);

      if (responseCode != nullptr)
      {
        *responseCode = response;
      }

      if (result == CURLE_OK)
      {
        return {};
      }

      if (stopToken.stop_requested())
      {
        return std::unexpected(FtpError(RemoteErrorCode::Cancelled,
                                        "The FTP operation was cancelled",
                                        0, false, mutating));
      }

      return std::unexpected(ReportCurlFailure(result, response, mutating));
    }

    Result<std::vector<RemoteEntry>> FtpSession::List(const RemotePath &path,
                                                      std::stop_token stopToken)
    {
      auto encodedDirectory = ServerPath(path);

      if (!encodedDirectory)
      {
        return std::unexpected(encodedDirectory.error());
      }

      if (auto configured = Configure(path, ftp::OperationKind::Mlsd, true);
          !configured)
      {
        return std::unexpected(configured.error());
      }

      TextSink sink{.data = {}, .stopToken = stopToken};

      curl_easy_setopt(mEasy.get(), CURLOPT_CUSTOMREQUEST, "MLSD");
      curl_easy_setopt(mEasy.get(), CURLOPT_WRITEFUNCTION, AppendText);
      curl_easy_setopt(mEasy.get(), CURLOPT_WRITEDATA, &sink);

      long response{};

      auto result = Perform(stopToken, false, &response);

      if (result)
      {
        auto parsed = ftp::ParseMlsdListing(sink.data, *encodedDirectory, mEncoding);

        if (parsed)
        {
          const bool ownershipMissing = std::ranges::any_of(
              *parsed, [](const RemoteEntry &entry)
              { return !entry.owner || !entry.group; });

          if (!ownershipMissing)
          {
            return parsed;
          }

          // Owner/group are not standard MLSD facts. Unix FTP servers often
          // expose them only through LIST, so use a second listing as a
          // best-effort metadata supplement. MLSD remains authoritative for
          // every field other than a missing owner or group.
          sink.data.clear();

          if (auto configured = Configure(path, ftp::OperationKind::List, true);
              !configured)
          {
            return std::unexpected(configured.error());
          }

          curl_easy_setopt(mEasy.get(), CURLOPT_CUSTOMREQUEST, "LIST");
          curl_easy_setopt(mEasy.get(), CURLOPT_WRITEFUNCTION, AppendText);
          curl_easy_setopt(mEasy.get(), CURLOPT_WRITEDATA, &sink);

          if (auto listed = Perform(stopToken, false, &response);
              !listed)
          {
            if (!ftp::CanIgnoreOwnershipListingFailure(listed.error(), response))
            {
              return std::unexpected(listed.error());
            }

            return parsed;
          }

          auto listEntries = ftp::ParseListListing(sink.data, *encodedDirectory, mEncoding);

          if (listEntries)
          {
            ftp::SupplementMlsdOwnerGroup(*parsed, *listEntries);
          }

          return parsed;
        }

        Log(DiagnosticLevel::Warning, "The server returned an invalid MLSD listing. Trying LIST");
      }
      else if (!ftp::CanFallbackToList(result.error().nativeCode, response))
      {
        return std::unexpected(result.error());
      }

      sink.data.clear();

      if (auto configured = Configure(path, ftp::OperationKind::List, true);
          !configured)
      {
        return std::unexpected(configured.error());
      }

      curl_easy_setopt(mEasy.get(), CURLOPT_CUSTOMREQUEST, "LIST");
      curl_easy_setopt(mEasy.get(), CURLOPT_WRITEFUNCTION, AppendText);
      curl_easy_setopt(mEasy.get(), CURLOPT_WRITEDATA, &sink);

      if (auto fallback = Perform(stopToken);
          !fallback)
      {
        return std::unexpected(fallback.error());
      }

      return ftp::ParseListListing(sink.data, *encodedDirectory, mEncoding);
    }

    Result<RemoteEntry> FtpSession::Stat(const RemotePath &path, std::stop_token stopToken)
    {
      if (path.IsRoot())
      {
        RemoteEntry root;
        root.path = path;
        root.name = RemotePath{"/"};
        root.kind = RemoteEntryKind::Directory;

        return root;
      }

      auto encodedPath = ServerPath(path);
      if (!encodedPath)
      {
        return std::unexpected(encodedPath.error());
      }

      auto parent = List(encodedPath->Parent(), stopToken);
      if (!parent)
      {
        return std::unexpected(parent.error());
      }

      const auto name = encodedPath->Filename().Bytes();

      const auto found = std::ranges::find(*parent, name, [](const RemoteEntry &entry)
                                           { return entry.name.Bytes(); });

      if (found == parent->end())
      {
        return std::unexpected(FtpError(RemoteErrorCode::NotFound,
                                        "The remote path was not found"));
      }

      return *found;
    }

    Result<void> FtpSession::Command(const std::vector<std::string> &commands,
                                     std::stop_token stopToken,
                                     const bool uncertain,
                                     const ftp::OperationKind operation)
    {
      for (const auto &commandText : commands)
      {
        if (commandText.find('\r') != std::string::npos ||
            commandText.find('\n') != std::string::npos ||
            commandText.find('\0') != std::string::npos)
        {
          return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                          "Unsafe characters in an FTP command"));
        }
      }

      if (auto configured = Configure(RemotePath::Root(), operation, true);
          !configured)
      {
        return configured;
      }

      curl_slist *raw = nullptr;

      for (const auto &commandText : commands)
      {
        auto *appended = curl_slist_append(raw, commandText.c_str());

        if (!appended)
        {
          curl_slist_free_all(raw);

          return std::unexpected(FtpError(
              RemoteErrorCode::ProtocolError,
              "Could not allocate an FTP command"));
        }

        raw = appended;
      }

      CurlList list{raw};

      // CURLOPT_QUOTE runs before libcurl restores the connection's initial
      // login directory. Path-bearing commands must run afterwards so their
      // login-relative operands address the same objects as ordinary FTP URL
      // transfers. PREQUOTE also runs for NOBODY requests and keeps RNFR/RNTO
      // together on the same control connection.
      if (const auto result = curl_easy_setopt(mEasy.get(), CURLOPT_PREQUOTE, list.get());
          result != CURLE_OK)
      {
        return std::unexpected(FtpError(
            RemoteErrorCode::ProtocolError,
            std::string{"Could not configure the FTP command: "} +
                curl_easy_strerror(result),
            static_cast<int>(result)));
      }

      if (const auto result = curl_easy_setopt(mEasy.get(), CURLOPT_NOBODY, 1L);
          result != CURLE_OK)
      {
        curl_easy_setopt(mEasy.get(), CURLOPT_PREQUOTE, nullptr);

        return std::unexpected(FtpError(
            RemoteErrorCode::ProtocolError,
            std::string{"Could not configure the FTP command request: "} +
                curl_easy_strerror(result),
            static_cast<int>(result)));
      }

      curl_easy_setopt(mEasy.get(), CURLOPT_WRITEFUNCTION, DiscardText);

      auto result = Perform(stopToken, uncertain);

      curl_easy_setopt(mEasy.get(), CURLOPT_PREQUOTE, nullptr);

      return result;
    }

    Result<void> FtpSession::Mkdir(const RemotePath &path,
                                   bool recursive,
                                   std::stop_token stopToken)
    {
      auto encodedPath = ServerPath(path);
      if (!encodedPath)
      {
        return std::unexpected(encodedPath.error());
      }
      if (encodedPath->IsRoot())
      {
        return path.IsRoot()
                   ? Result<void>{}
                   : std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                              "Invalid remote directory path"));
      }

      if (!recursive)
      {
        auto commandPath = ftp::MakeLoginRelativeCommandPath(encodedPath->Bytes());

        if (!commandPath)
        {
          return std::unexpected(commandPath.error());
        }

        return Command({"MKD " + *commandPath}, stopToken);
      }

      std::string current;

      if (encodedPath->IsAbsolute())
      {
        current = "/";
      }

      std::string_view remaining{encodedPath->Bytes()};
      if (!remaining.empty() && remaining.front() == '/')
      {
        remaining.remove_prefix(1);
      }

      while (!remaining.empty())
      {
        const auto slash = remaining.find('/');
        const auto segment = remaining.substr(0, slash);

        if (!IsValidRemoteChildName(segment))
        {
          return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                          "Invalid remote directory component"));
        }

        if (current.size() > 1 && current.back() != '/')
        {
          current.push_back('/');
        }

        current.append(segment);

        auto commandPath = ftp::MakeLoginRelativeCommandPath(current);
        if (!commandPath)
        {
          return std::unexpected(commandPath.error());
        }

        if (auto made = Command({"MKD " + *commandPath}, stopToken); !made)
        {
          // Some servers use 550 for an already existing directory. Only that
          // definite path rejection warrants a follow-up existence check.
          if (made.error().operationMayHaveSucceeded ||
              (made.error().code != RemoteErrorCode::AlreadyExists &&
               !(made.error().code == RemoteErrorCode::PermissionDenied &&
                 made.error().nativeCode == 550)))
          {
            return made;
          }

          auto display = ftp::DecodeServerText(current, mEncoding);
          if (!display)
          {
            return std::unexpected(display.error());
          }

          auto existing = Stat(RemotePath{current, std::move(*display)}, stopToken);
          if (!existing && existing.error().code != RemoteErrorCode::NotFound)
          {
            return std::unexpected(existing.error());
          }
          if (!existing || existing->kind != RemoteEntryKind::Directory)
          {
            return made;
          }
        }

        if (slash == std::string_view::npos)
        {
          break;
        }

        remaining.remove_prefix(slash + 1U);
      }

      return {};
    }

    Result<void> FtpSession::CreateRemoteFile(const RemotePath &path,
                                              const bool overwrite,
                                              std::stop_token stopToken)
    {
      auto encodedPath = ServerPath(path);
      if (!encodedPath)
      {
        return std::unexpected(encodedPath.error());
      }
      if (encodedPath->IsRoot() || encodedPath->Bytes().back() == '/')
      {
        return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                        "Invalid remote file path"));
      }

      if (stopToken.stop_requested())
      {
        return std::unexpected(FtpError(RemoteErrorCode::Cancelled,
                                        "The create-file operation was cancelled"));
      }

      if (!overwrite)
      {
        auto existing = Stat(path, stopToken);
        if (existing)
        {
          return std::unexpected(FtpError(RemoteErrorCode::AlreadyExists,
                                          "The remote destination already exists"));
        }
        if (existing.error().code != RemoteErrorCode::NotFound)
        {
          return std::unexpected(existing.error());
        }
      }

      std::optional<RemotePath> temporaryPath;

      for (unsigned int attempt = 0; attempt < 32; ++attempt)
      {
        const auto suffix = ".havremote." + GenerateId() + ".part";

        RemotePath candidate{path.Bytes() + suffix,
                             path.DisplayUtf8() + suffix};

        auto candidateInfo = Stat(candidate, stopToken);
        if (!candidateInfo &&
            candidateInfo.error().code == RemoteErrorCode::NotFound)
        {
          temporaryPath = std::move(candidate);

          break;
        }

        if (!candidateInfo &&
            candidateInfo.error().code != RemoteErrorCode::NotFound)
        {
          return std::unexpected(candidateInfo.error());
        }
      }

      if (!temporaryPath)
      {
        return std::unexpected(FtpError(
            RemoteErrorCode::Conflict,
            "Could not find an unused temporary remote filename"));
      }

      if (auto configured = Configure(*temporaryPath,
                                      ftp::OperationKind::Stor);
          !configured)
      {
        return configured;
      }

      curl_easy_setopt(mEasy.get(), CURLOPT_UPLOAD, 1L);
      curl_easy_setopt(mEasy.get(), CURLOPT_READFUNCTION, ReadEmptyUpload);
      curl_easy_setopt(mEasy.get(), CURLOPT_READDATA, nullptr);
      curl_easy_setopt(mEasy.get(), CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(0));

      if (auto created = Perform(stopToken, true);
          !created)
      {
        return created;
      }

      const auto cleanupTemporary = [this, &temporaryPath]
      {
        auto encodedTemporary = ServerPath(*temporaryPath);
        if (!encodedTemporary)
        {
          return;
        }

        auto commandPath = ftp::MakeLoginRelativeCommandPath(encodedTemporary->Bytes());
        if (!commandPath)
        {
          return;
        }

        if (auto removed = Command({"DELE " + *commandPath},
                                   std::stop_token{});
            !removed)
        {
          Log(DiagnosticLevel::Warning,
              "Could not remove a temporary remote file after create-file finalization failed");
        }
      };

      if (stopToken.stop_requested())
      {
        cleanupTemporary();

        return std::unexpected(FtpError(RemoteErrorCode::Cancelled,
                                        "The create-file operation was cancelled"));
      }

      // Baseline FTP has no exclusive-create command for a caller-selected
      // pathname. Upload to a unique sibling first so an existing destination
      // is never passed to STOR. Finalization uses the requested conflict policy.
      // The non-overwrite rename path checks the destination again immediately
      // before RNTO.
      auto finalized = Rename(*temporaryPath, path, overwrite, stopToken);
      if (!finalized)
      {
        cleanupTemporary();
      }

      return finalized;
    }

    Result<void> FtpSession::Rename(const RemotePath &source,
                                    const RemotePath &destination,
                                    bool overwrite,
                                    std::stop_token stopToken)
    {
      auto encodedSource = ServerPath(source);
      if (!encodedSource)
      {
        return std::unexpected(encodedSource.error());
      }

      auto encodedDestination = ServerPath(destination);
      if (!encodedDestination)
      {
        return std::unexpected(encodedDestination.error());
      }

      if (encodedSource->IsRoot() || encodedDestination->IsRoot())
      {
        return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                        "Invalid remote rename path"));
      }

      if (!overwrite)
      {
        auto destinationInfo = Stat(*encodedDestination, stopToken);
        if (destinationInfo)
        {
          return std::unexpected(FtpError(RemoteErrorCode::AlreadyExists,
                                          "The destination already exists"));
        }
        if (destinationInfo.error().code != RemoteErrorCode::NotFound)
        {
          return std::unexpected(destinationInfo.error());
        }
      }

      auto sourceCommandPath = ftp::MakeLoginRelativeCommandPath(encodedSource->Bytes());
      if (!sourceCommandPath)
      {
        return std::unexpected(sourceCommandPath.error());
      }

      auto destinationCommandPath = ftp::MakeLoginRelativeCommandPath(encodedDestination->Bytes());
      if (!destinationCommandPath)
      {
        return std::unexpected(destinationCommandPath.error());
      }

      return Command({"RNFR " + *sourceCommandPath,
                      "RNTO " + *destinationCommandPath},
                     stopToken,
                     true,
                     ftp::OperationKind::Rename);
    }

    Result<void> FtpSession::Remove(const RemotePath &path,
                                    bool recursive,
                                    std::stop_token stopToken)
    {
      auto encodedPath = ServerPath(path);
      if (!encodedPath)
      {
        return std::unexpected(encodedPath.error());
      }
      if (encodedPath->IsRoot())
      {
        return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                        "Refusing to remove an invalid or root path"));
      }
      return RemoveImpl(*encodedPath, recursive, stopToken);
    }

    Result<void> FtpSession::SetPermissions(const RemotePath &path,
                                            const std::uint32_t permissions,
                                            std::stop_token stopToken)
    {
      if ((permissions & ~07777U) != 0U)
      {
        return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                        "Remote permissions must be an octal mode from 0000 to 7777"));
      }
      auto encodedPath = ServerPath(path);
      if (!encodedPath)
      {
        return std::unexpected(encodedPath.error());
      }
      if (encodedPath->IsRoot())
      {
        return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                        "Refusing to change permissions on the remote root"));
      }

      auto info = Stat(path, stopToken);
      if (!info)
      {
        return std::unexpected(info.error());
      }
      if (info->kind != RemoteEntryKind::File &&
          info->kind != RemoteEntryKind::Directory)
      {
        return std::unexpected(FtpError(
            RemoteErrorCode::Unsupported,
            "Changing permissions on remote links or special files is not supported"));
      }

      std::array<char, 4> digits{};
      const auto [end, error] =
          std::to_chars(digits.data(), digits.data() + digits.size(), permissions, 8);

      if (error != std::errc{})
      {
        return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                        "Could not format the remote permissions"));
      }

      std::string mode(4U - static_cast<std::size_t>(end - digits.data()), '0');
      mode.append(digits.data(), end);

      auto commandPath = ftp::MakeLoginRelativeCommandPath(encodedPath->Bytes());
      if (!commandPath)
      {
        return std::unexpected(commandPath.error());
      }

      return Command({"SITE CHMOD " + mode + " " + *commandPath},
                     stopToken,
                     true);
    }

    Result<void> FtpSession::RemoveImpl(const RemotePath &path,
                                        bool recursive,
                                        std::stop_token stopToken)
    {
      if (stopToken.stop_requested())
      {
        return std::unexpected(FtpError(RemoteErrorCode::Cancelled,
                                        "The remove operation was cancelled"));
      }

      auto info = Stat(path, stopToken);
      if (!info)
      {
        return std::unexpected(info.error());
      }
      if (info->kind != RemoteEntryKind::Directory)
      {
        auto encodedPath = ServerPath(path);
        if (!encodedPath)
        {
          return std::unexpected(encodedPath.error());
        }

        auto commandPath = ftp::MakeLoginRelativeCommandPath(encodedPath->Bytes());
        if (!commandPath)
        {
          return std::unexpected(commandPath.error());
        }

        return Command({"DELE " + *commandPath}, stopToken);
      }

      if (!recursive)
      {
        auto encodedPath = ServerPath(path);
        if (!encodedPath)
        {
          return std::unexpected(encodedPath.error());
        }

        auto commandPath = ftp::MakeLoginRelativeCommandPath(encodedPath->Bytes());
        if (!commandPath)
        {
          return std::unexpected(commandPath.error());
        }

        return Command({"RMD " + *commandPath}, stopToken);
      }

      auto entries = List(path, stopToken);
      if (!entries)
      {
        return std::unexpected(entries.error());
      }

      for (const auto &entry : *entries)
      {
        if (auto removed = RemoveImpl(entry.path, true, stopToken); !removed)
        {
          return removed;
        }
      }

      auto encodedPath = ServerPath(path);
      if (!encodedPath)
      {
        return std::unexpected(encodedPath.error());
      }

      auto commandPath = ftp::MakeLoginRelativeCommandPath(encodedPath->Bytes());
      if (!commandPath)
      {
        return std::unexpected(commandPath.error());
      }

      return Command({"RMD " + *commandPath}, stopToken);
    }

    Result<void> FtpSession::Upload(const std::filesystem::path &localPath,
                                    const RemotePath &remotePath,
                                    const TransferOptions &options,
                                    const ProgressCallback &progress,
                                    std::stop_token stopToken)
    {
      std::error_code filesystemError;

      const auto size = std::filesystem::file_size(localPath, filesystemError);

      if (filesystemError)
      {
        return std::unexpected(FtpError(RemoteErrorCode::LocalIo,
                                        "Could not read the local file: " + filesystemError.message(),
                                        filesystemError.value()));
      }

      if (options.resumeOffset > size)
      {
        return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                        "The upload resume offset exceeds the local file size"));
      }

      if (size > static_cast<std::uintmax_t>((std::numeric_limits<curl_off_t>::max)()) ||
          options.resumeOffset >
              static_cast<std::uintmax_t>((std::numeric_limits<std::streamoff>::max)()))
      {
        return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                        "The upload size or resume offset is too large"));
      }

      RemotePath target = remotePath;

      if (options.useTemporaryName)
      {
        target = options.temporaryRemotePath.value_or(
            RemotePath{remotePath.Bytes() + ".havremote.part",
                       remotePath.DisplayUtf8() + ".havremote.part"});

        auto encodedTarget = ServerPath(target);
        if (!encodedTarget)
        {
          return std::unexpected(encodedTarget.error());
        }

        auto encodedDestination = ServerPath(remotePath);
        if (!encodedDestination)
        {
          return std::unexpected(encodedDestination.error());
        }

        if (encodedTarget->IsRoot())
        {
          return std::unexpected(FtpError(RemoteErrorCode::InvalidArgument,
                                          "Invalid remote temporary upload path"));
        }

        if (encodedTarget->Bytes() == encodedDestination->Bytes())
        {
          return std::unexpected(FtpError(
              RemoteErrorCode::InvalidArgument,
              "The remote temporary upload path must differ from its destination"));
        }
      }

      if (options.resumeOffset > 0)
      {
        auto partial = Stat(target, stopToken);
        if (!partial)
        {
          return std::unexpected(partial.error());
        }
        if (partial->kind != RemoteEntryKind::File ||
            partial->size > size || partial->size != options.resumeOffset)
        {
          return std::unexpected(FtpError(
              RemoteErrorCode::Conflict,
              "The remote partial file no longer matches its resume metadata"));
        }
      }

      if (auto configured = Configure(target, ftp::OperationKind::Stor);
          !configured)
      {
        return configured;
      }

      TransferContext context{.stopToken = stopToken,
                              .callback = progress,
                              .progress = TransferProgress{.jobId = options.jobId,
                                                           .bytesTransferred = 0,
                                                           .totalBytes = std::nullopt,
                                                           .elapsed = {}},
                              .baseOffset = options.resumeOffset,
                              .upload = true};

      UploadSource source{.stream = std::ifstream{localPath, std::ios::binary}, .transfer = &context};

      if (!source.stream)
      {
        return std::unexpected(FtpError(RemoteErrorCode::LocalIo,
                                        "Could not open the local file for upload"));
      }

      source.stream.seekg(static_cast<std::streamoff>(options.resumeOffset));

      if (!source.stream)
      {
        return std::unexpected(FtpError(RemoteErrorCode::LocalIo,
                                        "Could not seek to the upload resume offset"));
      }

      curl_easy_setopt(mEasy.get(), CURLOPT_UPLOAD, 1L);
      curl_easy_setopt(mEasy.get(), CURLOPT_READFUNCTION, ReadUpload);
      curl_easy_setopt(mEasy.get(), CURLOPT_READDATA, &source);
      curl_easy_setopt(mEasy.get(), CURLOPT_INFILESIZE_LARGE,
                       static_cast<curl_off_t>(size - options.resumeOffset));

      // The stream is already positioned and its remaining length is known.
      // A positive curl resume option would select APPE and subtract the
      // offset from this remaining length a second time.
      curl_easy_setopt(mEasy.get(), CURLOPT_RESUME_FROM_LARGE, curl_off_t{0});
      curl_easy_setopt(mEasy.get(), CURLOPT_APPEND, 0L);

      UploadRestartRequest restart;

      if (options.resumeOffset > 0)
      {
        if (auto configured = restart.Configure(
                mEasy.get(), options.resumeOffset,
                mDebugContext.active || mDebugContext.control || mDebugContext.uploadReply
                    ? &mDebugContext
                    : nullptr);
            !configured)
        {
          return configured;
        }
      }

      curl_easy_setopt(mEasy.get(), CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(mEasy.get(), CURLOPT_XFERINFOFUNCTION, OnFtpTransferProgress);
      curl_easy_setopt(mEasy.get(), CURLOPT_XFERINFODATA, &context);

      const auto curlResult = PerformTraced();

      // Detach stack-owned callbacks and commands before finalization or any
      // other operation reconfigures this shared easy handle.
      restart.ClearOptions();

      if (curlResult != CURLE_OK)
      {
        if (context.control != TransferControl::Continue || stopToken.stop_requested())
        {
          return std::unexpected(TransferAbortError(context));
        }

        long response{};

        curl_easy_getinfo(mEasy.get(), CURLINFO_RESPONSE_CODE, &response);

        if (const auto rejected = restart.RejectedCommand())
        {
          return std::unexpected(ReportCurlFailure(
              curlResult, restart.RejectedReply(), false, rejected));
        }

        return std::unexpected(ReportCurlFailure(curlResult, response, true));
      }

      if (!options.useTemporaryName)
      {
        return {};
      }

      if (stopToken.stop_requested())
      {
        return std::unexpected(FtpError(RemoteErrorCode::Cancelled,
                                        "The upload was cancelled before finalization"));
      }

      if (progress)
      {
        context.control = progress(TransferProgress{
            .jobId = options.jobId,
            .bytesTransferred = static_cast<std::uint64_t>(size),
            .totalBytes = static_cast<std::uint64_t>(size),
            .activeBytesTransferred =
                static_cast<std::uint64_t>(size) - options.resumeOffset,
            .elapsed = std::chrono::steady_clock::now() - context.started});

        if (context.control != TransferControl::Continue)
        {
          return std::unexpected(TransferAbortError(context));
        }
      }

      auto overwrite = options.overwrite;

      if (options.beforeFinalize)
      {
        auto finalization = options.beforeFinalize(overwrite);
        if (!finalization)
        {
          return std::unexpected(finalization.error());
        }

        overwrite = *finalization;
      }

      // RNTO is the only safe overwrite attempt available in baseline FTP. Do
      // not delete the existing destination first: servers that cannot replace
      // it atomically must reject RNTO and leave the good file intact.
      return Rename(target, remotePath, overwrite, stopToken);
    }

    Result<void> FtpSession::Download(const RemotePath &remotePath,
                                      const std::filesystem::path &localPath,
                                      const TransferOptions &options,
                                      const ProgressCallback &progress,
                                      std::stop_token stopToken)
    {
      auto remoteInfo = Stat(remotePath, stopToken);
      if (!remoteInfo)
      {
        return std::unexpected(remoteInfo.error());
      }
      if (remoteInfo->kind == RemoteEntryKind::Directory)
      {
        return std::unexpected(FtpError(RemoteErrorCode::IsDirectory,
                                        "The remote download source is a directory"));
      }
      if (remoteInfo->kind == RemoteEntryKind::Symlink)
      {
        return std::unexpected(FtpError(RemoteErrorCode::Unsupported,
                                        "Downloading through a symbolic link is disabled"));
      }

      if (options.resumeOffset > remoteInfo->size && remoteInfo->size != 0)
      {
        return std::unexpected(FtpError(RemoteErrorCode::Conflict,
                                        "The download resume offset exceeds the remote file size"));
      }

      auto partPath = localPath;

      if (options.useTemporaryName)
      {
        partPath += ".havremote.part";
      }

      std::error_code filesystemError;

      if (!partPath.parent_path().empty())
      {
        std::filesystem::create_directories(partPath.parent_path(), filesystemError);

        if (filesystemError)
        {
          return std::unexpected(FtpError(RemoteErrorCode::LocalIo,
                                          "Could not create the local directory: " +
                                              filesystemError.message(),
                                          filesystemError.value()));
        }
      }

      if (options.resumeOffset > 0)
      {
        const auto existing = std::filesystem::file_size(partPath, filesystemError);

        if (filesystemError || existing != options.resumeOffset)
        {
          return std::unexpected(FtpError(RemoteErrorCode::Conflict,
                                          "The partial download no longer matches its resume metadata"));
        }
      }

      if (auto configured = Configure(remotePath, ftp::OperationKind::Retr);
          !configured)
      {
        return configured;
      }

      TransferContext context{.stopToken = stopToken,
                              .callback = progress,
                              .progress = TransferProgress{.jobId = options.jobId,
                                                           .bytesTransferred = 0,
                                                           .totalBytes = std::nullopt,
                                                           .elapsed = {}},
                              .baseOffset = options.resumeOffset,
                              .upload = false};

      const auto mode = std::ios::binary |
                        (options.resumeOffset > 0 ? std::ios::app : std::ios::trunc);

      DownloadSink sink{.stream = std::ofstream{partPath, mode}, .transfer = &context};

      if (!sink.stream)
      {
        return std::unexpected(FtpError(RemoteErrorCode::LocalIo,
                                        "Could not open the partial download file"));
      }

      curl_easy_setopt(mEasy.get(), CURLOPT_WRITEFUNCTION, WriteDownload);
      curl_easy_setopt(mEasy.get(), CURLOPT_WRITEDATA, &sink);

      if (options.resumeOffset > 0)
      {
        curl_easy_setopt(mEasy.get(), CURLOPT_RESUME_FROM_LARGE,
                         static_cast<curl_off_t>(options.resumeOffset));
      }

      curl_easy_setopt(mEasy.get(), CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(mEasy.get(), CURLOPT_XFERINFOFUNCTION, OnFtpTransferProgress);
      curl_easy_setopt(mEasy.get(), CURLOPT_XFERINFODATA, &context);

      const auto curlResult = PerformTraced();

      sink.stream.close();

      if (curlResult != CURLE_OK)
      {
        if (context.control != TransferControl::Continue || stopToken.stop_requested())
        {
          return std::unexpected(TransferAbortError(context));
        }

        long response{};

        curl_easy_getinfo(mEasy.get(), CURLINFO_RESPONSE_CODE, &response);

        return std::unexpected(ReportCurlFailure(curlResult, response, false));
      }

      if (!options.useTemporaryName || partPath == localPath)
      {
        return {};
      }

      if (stopToken.stop_requested())
      {
        return std::unexpected(FtpError(RemoteErrorCode::Cancelled,
                                        "The download was cancelled before finalization"));
      }

      if (progress)
      {
        context.control = progress(TransferProgress{
            .jobId = options.jobId,
            .bytesTransferred = remoteInfo->size,
            .totalBytes = remoteInfo->size,
            .activeBytesTransferred =
                remoteInfo->size >= options.resumeOffset
                    ? remoteInfo->size - options.resumeOffset
                    : 0U,
            .elapsed = std::chrono::steady_clock::now() - context.started});

        if (context.control != TransferControl::Continue)
        {
          return std::unexpected(TransferAbortError(context));
        }
      }

      auto overwrite = options.overwrite;

      if (options.beforeFinalize)
      {
        auto finalization = options.beforeFinalize(overwrite);
        if (!finalization)
        {
          return std::unexpected(finalization.error());
        }

        overwrite = *finalization;
      }

      if (stopToken.stop_requested())
      {
        return std::unexpected(FtpError(
            RemoteErrorCode::Cancelled,
            "The download was cancelled before finalization"));
      }

      return FinalizeLocalDownload(partPath, localPath, overwrite);
    }
  } // namespace

  namespace ftp
  {
    Result<std::string> FormatHostForUrl(std::string_view host)
    {
      return FormatFtpHost(host);
    }

    Result<std::string> MakeActiveModePortSpecification(
        const std::string_view localAddress)
    {
      if (localAddress.empty())
      {
        return std::string{"-"};
      }

      if (!IsValidIpAddress(localAddress))
      {
        return std::unexpected(FtpError(
            RemoteErrorCode::InvalidArgument,
            "The Active FTP address must be an unbracketed IPv4 or IPv6 literal"));
      }

      return std::string{localAddress};
    }

    Result<bool> IsAssignedLocalAddress(const std::string_view address)
    {
      return ActiveAddressIsAssigned(address);
    }

    Result<std::string> MakeLoginRelativeCommandPath(
        std::string_view encodedPath)
    {
      if (encodedPath.empty() ||
          encodedPath.find_first_not_of('/') == std::string_view::npos ||
          encodedPath.find('\0') != std::string_view::npos ||
          encodedPath.find('\r') != std::string_view::npos ||
          encodedPath.find('\n') != std::string_view::npos)
      {
        return std::unexpected(FtpError(
            RemoteErrorCode::InvalidArgument,
            "The FTP command path is empty or contains unsafe characters"));
      }

      if (encodedPath.front() == '/')
      {
        encodedPath.remove_prefix(1);
      }

      if (encodedPath.empty())
      {
        return std::unexpected(FtpError(
            RemoteErrorCode::InvalidArgument,
            "The FTP login root cannot be used as a file-operation path"));
      }

      return std::string{encodedPath};
    }

    bool UsesDataConnection(const OperationKind operation) noexcept
    {
      switch (operation)
      {
      case OperationKind::Nlst:
      case OperationKind::Mlsd:
      case OperationKind::List:
      case OperationKind::Retr:
      case OperationKind::Stor:
        return true;

      case OperationKind::ControlCommand:
      case OperationKind::Rename:
        return false;
      }

      return false;
    }

    Result<TextEncoding> ResolveTextEncoding(std::string_view name)
    {
      return ResolveFtpTextEncoding(name);
    }

    Result<std::string> EncodeServerText(std::string_view utf8,
                                         const TextEncoding &encoding)
    {
      return EncodeFtpServerText(utf8, encoding);
    }

    Result<std::string> DecodeServerText(std::string_view bytes,
                                         const TextEncoding &encoding)
    {
      return DecodeFtpServerText(bytes, encoding);
    }

    TlsVerificationFailure ClassifyTlsVerificationFailure(std::string_view diagnostic)
    {
      const auto description = Lower(diagnostic);

      if (description.find("revoked") != std::string::npos)
      {
        return TlsVerificationFailure::Revoked;
      }

      if (description.find("not time valid") != std::string::npos ||
          description.find("expired") != std::string::npos ||
          description.find("not yet valid") != std::string::npos)
      {
        return TlsVerificationFailure::ExpiredOrNotYetValid;
      }

      if (description.find("hostname") != std::string::npos ||
          description.find("does not match") != std::string::npos ||
          description.find("cn_no_match") != std::string::npos)
      {
        return TlsVerificationFailure::HostnameMismatch;
      }

      if (description.find("sec_e_untrusted_root") != std::string::npos ||
          description.find("certificate chain is incomplete") != std::string::npos ||
          description.find("based on an untrusted root") != std::string::npos ||
          description.find("self-signed certificate") != std::string::npos ||
          description.find("self signed certificate") != std::string::npos ||
          description.find("unable to get local issuer certificate") != std::string::npos ||
          description.find("unable to verify the first certificate") != std::string::npos)
      {
        return TlsVerificationFailure::UntrustedIssuer;
      }

      return TlsVerificationFailure::Other;
    }

    Result<TlsPublicKeyIdentity> InspectCertificatePem(std::string_view certificatePem)
    {
      return InspectCertificate(certificatePem);
    }

    bool IsTlsPinExceptionEligible(const TlsPublicKeyIdentity &identity,
                                   TlsVerificationFailure failure) noexcept
    {
      return failure == TlsVerificationFailure::UntrustedIssuer &&
             identity.genuinelySelfSigned && identity.currentlyTimeValid &&
             identity.serverAuthenticationValidExceptTrustAnchor;
    }

    bool IsTlsTrustDecisionAllowed(TrustStatus status,
                                   TrustDecision decision) noexcept
    {
      if (decision == TrustDecision::Reject)
      {
        return false;
      }

      if (status == TrustStatus::Changed)
      {
        return decision == TrustDecision::ReplaceStored;
      }

      return decision == TrustDecision::AcceptOnce ||
             decision == TrustDecision::AcceptPermanently;
    }

    Result<std::vector<RemoteEntry>> ParseMlsdListing(std::string_view listing,
                                                      const RemotePath &directory)
    {
      return ParseMlsdListing(listing, directory, TextEncoding{});
    }

    Result<std::vector<RemoteEntry>> ParseMlsdListing(
        std::string_view listing,
        const RemotePath &directory,
        const TextEncoding &encoding)
    {
      std::vector<RemoteEntry> entries;

      for (const auto line : Lines(listing))
      {
        const auto separator = line.find(' ');

        if (separator == std::string_view::npos)
        {
          return std::unexpected(FtpError(RemoteErrorCode::ParseError,
                                          "Malformed MLSD line"));
        }

        const auto nameText = line.substr(separator + 1U);

        if (!IsValidRemoteChildName(nameText))
        {
          continue;
        }

        RemoteEntry entry;
        entry.name = RemotePath{std::string{nameText}};
        entry.path = directory.Joined(entry.name);
        entry.hidden = nameText.front() == '.';

        std::optional<std::string> ownerName;
        std::optional<std::string> owner;
        std::optional<std::string> ownerId;
        std::optional<std::string> groupName;
        std::optional<std::string> group;
        std::optional<std::string> groupId;

        auto facts = line.substr(0, separator);

        while (!facts.empty())
        {
          const auto semicolon = facts.find(';');

          auto fact = facts.substr(0, semicolon);

          if (!fact.empty())
          {
            const auto equals = fact.find('=');
            if (equals != std::string_view::npos)
            {
              const auto key = Lower(fact.substr(0, equals));
              const auto value = fact.substr(equals + 1U);

              if (key == "type")
              {
                const auto type = Lower(value);

                if (type == "cdir" || type == "pdir")
                {
                  entry.kind = RemoteEntryKind::Other;
                  entry.name = RemotePath{"."};

                  break;
                }

                if (type == "file")
                {
                  entry.kind = RemoteEntryKind::File;
                }
                else if (type == "dir")
                {
                  entry.kind = RemoteEntryKind::Directory;
                }
                else if (type.find("slink") != std::string::npos)
                {
                  entry.kind = RemoteEntryKind::Symlink;
                }
              }
              else if (key == "size")
              {
                if (const auto size = ParseInteger<std::uint64_t>(value))
                {
                  entry.size = *size;
                }
              }
              else if (key == "modify")
              {
                entry.modifiedAt = ParseMlsdTime(value);
              }
              else if (key == "unix.mode")
              {
                auto modeText = value;

                if (modeText.starts_with("0o") || modeText.starts_with("0O"))
                {
                  modeText.remove_prefix(2U);
                }

                if (const auto mode = ParseInteger<std::uint32_t>(modeText, 8))
                {
                  entry.permissions = *mode & 07777U;
                }
              }
              else if (key == "unix.ownername" && !value.empty())
              {
                ownerName = value;
              }
              else if (key == "unix.owner" && !value.empty())
              {
                owner = value;
              }
              else if (key == "unix.uid" && !value.empty())
              {
                ownerId = value;
              }
              else if (key == "unix.groupname" && !value.empty())
              {
                groupName = value;
              }
              else if (key == "unix.group" && !value.empty())
              {
                group = value;
              }
              else if (key == "unix.gid" && !value.empty())
              {
                groupId = value;
              }
            }
          }

          if (semicolon == std::string_view::npos)
          {
            break;
          }

          facts.remove_prefix(semicolon + 1U);
        }

        if (entry.name.Bytes() == ".")
        {
          continue;
        }

        if (ownerName)
        {
          entry.owner = std::move(ownerName);
        }
        else if (owner)
        {
          entry.owner = std::move(owner);
        }
        else if (ownerId)
        {
          entry.owner = std::move(ownerId);
        }

        if (groupName)
        {
          entry.group = std::move(groupName);
        }
        else if (group)
        {
          entry.group = std::move(group);
        }
        else if (groupId)
        {
          entry.group = std::move(groupId);
        }

        entries.push_back(std::move(entry));
      }

      for (auto &entry : entries)
      {
        const auto rawName = entry.name.Bytes();

        auto displayName = DecodeServerText(rawName, encoding);
        if (!displayName)
        {
          return std::unexpected(displayName.error());
        }

        if (!IsValidRemoteChildName(*displayName))
        {
          return std::unexpected(FtpError(
              RemoteErrorCode::ParseError,
              "The decoded MLSD name is not a safe remote child name"));
        }

        entry.name = RemotePath{rawName, std::move(*displayName)};
        entry.path = directory.Joined(entry.name);
        entry.hidden = rawName.front() == '.';

        if (entry.owner)
        {
          auto displayOwner = DecodeServerText(*entry.owner, encoding);
          if (displayOwner)
          {
            entry.owner = std::move(*displayOwner);
          }
          else
          {
            entry.owner.reset();
          }
        }

        if (entry.group)
        {
          auto displayGroup = DecodeServerText(*entry.group, encoding);
          if (displayGroup)
          {
            entry.group = std::move(*displayGroup);
          }
          else
          {
            entry.group.reset();
          }
        }
      }

      return entries;
    }

    Result<std::vector<RemoteEntry>> ParseListListing(std::string_view listing,
                                                      const RemotePath &directory)
    {
      return ParseListListing(listing, directory, TextEncoding{});
    }

    Result<std::vector<RemoteEntry>> ParseListListing(
        std::string_view listing,
        const RemotePath &directory,
        const TextEncoding &encoding)
    {
      static const std::regex unixPattern{
          R"(^([bcdlps-][rwxStTs-]{9})\s+\d+\s+(\S+)\s+(\S+)\s+(\d+)\s+([A-Za-z]{3})\s+(\d{1,2})\s+([0-9:]{4,5})\s+(.*)$)"};

      static const std::regex dosPattern{
          R"(^(\d{2})-(\d{2})-(\d{2,4})\s+(\d{1,2}):(\d{2})(AM|PM)\s+(<DIR>|\d+)\s+(.*)$)",
          std::regex::icase};

      std::vector<RemoteEntry> entries;

      for (const auto rawLine : Lines(listing))
      {
        const std::string line{rawLine};
        std::smatch match;
        RemoteEntry entry;
        std::string name;

        if (std::regex_match(line, match, unixPattern))
        {
          const auto mode = match[1].str();
          if (mode.front() == 'd')
          {
            entry.kind = RemoteEntryKind::Directory;
          }
          else if (mode.front() == 'l')
          {
            entry.kind = RemoteEntryKind::Symlink;
          }
          else if (mode.front() == '-')
          {
            entry.kind = RemoteEntryKind::File;
          }
          else
          {
            entry.kind = RemoteEntryKind::Other;
          }

          entry.owner = match[2].str();
          entry.group = match[3].str();

          if (const auto size = ParseInteger<std::uint64_t>(match[4].str()))
          {
            entry.size = *size;
          }

          entry.modifiedAt = ParseListTime(match[5].str(), match[6].str(), match[7].str());

          name = match[8].str();

          if (entry.kind == RemoteEntryKind::Symlink)
          {
            if (const auto arrow = name.find(" -> "); arrow != std::string::npos)
            {
              name.resize(arrow);
            }
          }

          std::uint32_t permissions{};

          if (mode[1] == 'r')
          {
            permissions |= 0400U;
          }

          if (mode[2] == 'w')
          {
            permissions |= 0200U;
          }

          if (mode[3] == 'x' || mode[3] == 's')
          {
            permissions |= 0100U;
          }

          if (mode[3] == 's' || mode[3] == 'S')
          {
            permissions |= 04000U;
          }

          if (mode[4] == 'r')
          {
            permissions |= 0040U;
          }

          if (mode[5] == 'w')
          {
            permissions |= 0020U;
          }

          if (mode[6] == 'x' || mode[6] == 's')
          {
            permissions |= 0010U;
          }

          if (mode[6] == 's' || mode[6] == 'S')
          {
            permissions |= 02000U;
          }

          if (mode[7] == 'r')
          {
            permissions |= 0004U;
          }

          if (mode[8] == 'w')
          {
            permissions |= 0002U;
          }

          if (mode[9] == 'x' || mode[9] == 't')
          {
            permissions |= 0001U;
          }

          if (mode[9] == 't' || mode[9] == 'T')
          {
            permissions |= 01000U;
          }

          entry.permissions = permissions;
        }
        else if (std::regex_match(line, match, dosPattern))
        {
          entry.kind = Lower(match[7].str()) == "<dir>" ? RemoteEntryKind::Directory
                                                        : RemoteEntryKind::File;

          if (entry.kind == RemoteEntryKind::File)
          {
            if (const auto size = ParseInteger<std::uint64_t>(match[7].str()))
            {
              entry.size = *size;
            }
          }

          name = match[8].str();

          int year = ParseInteger<int>(match[3].str()).value_or(0);

          if (year < 100)
          {
            year += year < 70 ? 2000 : 1900;
          }

          auto hour = ParseInteger<int>(match[4].str()).value_or(0);

          const auto minute = ParseInteger<int>(match[5].str()).value_or(0);

          const bool pm = Lower(match[6].str()) == "pm";

          if (hour == 12)
          {
            hour = 0;
          }

          if (pm)
          {
            hour += 12;
          }

          const auto month = ParseInteger<unsigned>(match[1].str()).value_or(0);

          const auto day = ParseInteger<unsigned>(match[2].str()).value_or(0);

          const std::chrono::year_month_day date{std::chrono::year{year},
                                                 std::chrono::month{month},
                                                 std::chrono::day{day}};

          if (date.ok() && hour <= 23 && minute <= 59)
          {
            entry.modifiedAt = std::chrono::sys_days{date} + std::chrono::hours{hour} +
                               std::chrono::minutes{minute};
          }
        }
        else
        {
          continue; // LIST is server-specific. Ignore banners and unknown records.
        }

        if (!IsValidRemoteChildName(name))
        {
          continue;
        }

        entry.name = RemotePath{std::move(name)};
        entry.path = directory.Joined(entry.name);
        entry.hidden = entry.name.Bytes().front() == '.';

        entries.push_back(std::move(entry));
      }

      if (entries.empty() && !Trim(listing).empty())
      {
        return std::unexpected(FtpError(RemoteErrorCode::ParseError,
                                        "The server returned an unsupported LIST format"));
      }

      for (auto &entry : entries)
      {
        const auto rawName = entry.name.Bytes();

        auto displayName = DecodeServerText(rawName, encoding);
        if (!displayName)
        {
          return std::unexpected(displayName.error());
        }

        if (!IsValidRemoteChildName(*displayName))
        {
          return std::unexpected(FtpError(
              RemoteErrorCode::ParseError,
              "The decoded LIST name is not a safe remote child name"));
        }

        entry.name = RemotePath{rawName, std::move(*displayName)};
        entry.path = directory.Joined(entry.name);
        entry.hidden = rawName.front() == '.';

        if (entry.owner)
        {
          auto displayOwner = DecodeServerText(*entry.owner, encoding);
          if (displayOwner)
          {
            entry.owner = std::move(*displayOwner);
          }
          else
          {
            entry.owner.reset();
          }
        }

        if (entry.group)
        {
          auto displayGroup = DecodeServerText(*entry.group, encoding);
          if (displayGroup)
          {
            entry.group = std::move(*displayGroup);
          }
          else
          {
            entry.group.reset();
          }
        }
      }

      return entries;
    }

    void SupplementMlsdOwnerGroup(
        std::vector<RemoteEntry> &mlsdEntries,
        const std::vector<RemoteEntry> &listEntries)
    {
      struct Candidate final
      {
        const RemoteEntry *entry{};
        bool unique{true};
      };

      std::unordered_map<std::string_view, Candidate> listByRawName;
      listByRawName.reserve(listEntries.size());

      for (const auto &entry : listEntries)
      {
        const auto [candidate, inserted] = listByRawName.emplace(
            entry.name.Bytes(), Candidate{.entry = &entry});

        if (!inserted)
        {
          candidate->second.unique = false;
        }
      }

      std::unordered_map<std::string_view, std::size_t> mlsdNameCounts;
      mlsdNameCounts.reserve(mlsdEntries.size());

      for (const auto &entry : mlsdEntries)
      {
        ++mlsdNameCounts[entry.name.Bytes()];
      }

      for (auto &entry : mlsdEntries)
      {
        if (entry.owner && entry.group)
        {
          continue;
        }

        const auto count = mlsdNameCounts.find(entry.name.Bytes());
        if (count == mlsdNameCounts.end() || count->second != 1U)
        {
          continue;
        }

        const auto candidate = listByRawName.find(entry.name.Bytes());
        if (candidate == listByRawName.end() || !candidate->second.unique ||
            candidate->second.entry->kind != entry.kind)
        {
          continue;
        }

        if (!entry.owner && candidate->second.entry->owner)
        {
          entry.owner = candidate->second.entry->owner;
        }

        if (!entry.group && candidate->second.entry->group)
        {
          entry.group = candidate->second.entry->group;
        }
      }
    }
  } // namespace ftp

  RemoteSessionPtr MakeFtpSession() { return std::make_unique<FtpSession>(); }
} // namespace havremote
