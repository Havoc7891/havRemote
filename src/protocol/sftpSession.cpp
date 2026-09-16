// SPDX-License-Identifier: MIT

#include "protocol/sftpErrors.hpp"
#include "protocol/sftpHostKeyPolicy.hpp"
#include "protocol/sftpKeyFiles.hpp"
#include "protocol/sftpSession.hpp"
#include "sftpSocket.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

extern "C" WINSOCK_API_LINKAGE INT WSAAPI GetAddrInfoExCancel(LPHANDLE nameHandle);
extern "C" WINSOCK_API_LINKAGE INT WSAAPI GetAddrInfoExOverlappedResult(
    LPOVERLAPPED overlapped);
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace havremote
{
  namespace
  {

    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

#if defined(_WIN32)
    using NativeSocket = SOCKET;
    inline constexpr NativeSocket InvalidSocket = INVALID_SOCKET;
#else
    using NativeSocket = int;
    inline constexpr NativeSocket InvalidSocket = -1;
#endif

    [[nodiscard]] RemoteError SftpError(RemoteErrorCode code,
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

    [[nodiscard]] std::string DisplayAuthenticationMethods(
        const std::string_view methods)
    {
      std::string display;
      display.reserve((std::min)(methods.size(), std::size_t{256}));

      for (const unsigned char character : methods.substr(0, 256))
      {
        const bool safe =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '-' || character == '_' || character == '@' ||
            character == '.' || character == ',';

        display.push_back(safe ? static_cast<char>(character) : '?');
      }

      return display.empty() ? "none" : display;
    }

    void ClearSensitiveString(std::string &value) noexcept
    {
      if (value.empty())
      {
        return;
      }

#if defined(_WIN32)
      SecureZeroMemory(value.data(), value.size());
#else
      auto *const bytes = static_cast<volatile char *>(value.data());

      for (std::size_t index = 0; index < value.size(); ++index)
      {
        bytes[index] = '\0';
      }
#endif

      value.clear();
    }

    [[nodiscard]] std::string_view SshAgentBackendName() noexcept
    {
#if defined(_WIN32)
      // libssh2 tries its Pageant backend first and stops at the first agent
      // it can connect to. It does not expose the selected backend publicly,
      // so mirror the exact Pageant window check immediately before connect.
      return FindWindowA("Pageant", "Pageant") != nullptr
                 ? "Pageant"
                 : "Windows OpenSSH agent";
#else
      return "SSH agent";
#endif
    }

    class SensitiveStringGuard final
    {
    public:
      explicit SensitiveStringGuard(std::string &value) noexcept
          : mValue(&value)
      {
      }

      ~SensitiveStringGuard() { ClearSensitiveString(*mValue); }

      SensitiveStringGuard(const SensitiveStringGuard &) = delete;
      SensitiveStringGuard &operator=(const SensitiveStringGuard &) = delete;

    private:
      std::string *mValue;
    };

    [[nodiscard]] long Libssh2TimeoutMilliseconds(std::chrono::seconds timeout) noexcept
    {
      const auto maxSeconds = (std::numeric_limits<long>::max)() / 1000L;
      const auto seconds = (std::min)(timeout.count(), static_cast<decltype(timeout.count())>(maxSeconds));

      return static_cast<long>(seconds * 1000);
    }

    class NetworkGlobal final
    {
    public:
      NetworkGlobal()
      {
#if defined(_WIN32)
        WSADATA data{};

        mNetworkResult = WSAStartup(MAKEWORD(2, 2), &data);
#endif

        if (mNetworkResult == 0)
        {
          mSshResult = libssh2_init(0);
        }
      }

      ~NetworkGlobal()
      {
        if (mSshResult == 0)
        {
          libssh2_exit();
        }

#if defined(_WIN32)
        if (mNetworkResult == 0)
        {
          WSACleanup();
        }
#endif
      }

      [[nodiscard]] bool Ready() const noexcept
      {
        return mNetworkResult == 0 && mSshResult == 0;
      }

    private:
      int mNetworkResult{};
      int mSshResult{-1};
    };

    [[nodiscard]] NetworkGlobal &GetNetworkGlobal()
    {
      static NetworkGlobal instance;
      return instance;
    }

    [[nodiscard]] int SocketError() noexcept
    {
#if defined(_WIN32)
      return WSAGetLastError();
#else
      return errno;
#endif
    }

    [[nodiscard]] bool SocketInProgress(int error) noexcept
    {
#if defined(_WIN32)
      return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY;
#else
      return error == EINPROGRESS || error == EWOULDBLOCK || error == EALREADY;
#endif
    }

    void CloseSocket(NativeSocket socket) noexcept
    {
      if (socket == InvalidSocket)
      {
        return;
      }

#if defined(_WIN32)
      closesocket(socket);
#else
      close(socket);
#endif
    }

    [[nodiscard]] bool MakeNonblocking(NativeSocket socket) noexcept
    {
#if defined(_WIN32)
      u_long enabled = 1;

      return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
      const auto flags = fcntl(socket, F_GETFL, 0);

      return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    }

    class SocketHandle final
    {
    public:
      SocketHandle() = default;
      explicit SocketHandle(NativeSocket value) : mValue(value) {}
      ~SocketHandle() { CloseSocket(mValue); }
      SocketHandle(const SocketHandle &) = delete;
      SocketHandle &operator=(const SocketHandle &) = delete;
      SocketHandle(SocketHandle &&other) noexcept : mValue(std::exchange(other.mValue, InvalidSocket)) {}
      SocketHandle &operator=(SocketHandle &&other) noexcept
      {
        if (this != &other)
        {
          CloseSocket(mValue);

          mValue = std::exchange(other.mValue, InvalidSocket);
        }

        return *this;
      }
      [[nodiscard]] NativeSocket Get() const noexcept { return mValue; }
      void Reset() noexcept { CloseSocket(std::exchange(mValue, InvalidSocket)); }

    private:
      NativeSocket mValue{InvalidSocket};
    };

    struct ResolvedAddress final
    {
      sockaddr_storage storage{};
      int length{};
      int family{};
      int socketType{};
      int protocol{};
    };

    [[nodiscard]] Result<std::vector<ResolvedAddress>> ResolveAddresses(
        const std::string &host,
        const std::string &service,
        Clock::time_point deadline,
        std::stop_token stopToken)
    {
      if (stopToken.stop_requested())
      {
        return std::unexpected(SftpError(RemoteErrorCode::Cancelled,
                                         "SFTP name resolution was cancelled"));
      }

      std::vector<ResolvedAddress> result;

#if defined(_WIN32)
      ADDRINFOEXW hints{};
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_STREAM;
      hints.ai_protocol = IPPROTO_TCP;

      ADDRINFOEXW *rawAddresses{};

      OVERLAPPED overlapped{};
      overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

      if (!overlapped.hEvent)
      {
        return std::unexpected(SftpError(RemoteErrorCode::ConnectionFailed,
                                         "Could not create the DNS cancellation event",
                                         static_cast<int>(GetLastError()), true));
      }

      const std::wstring wideHost{host.begin(), host.end()};
      const std::wstring wideService{service.begin(), service.end()};

      HANDLE queryHandle{};

      auto resolve = GetAddrInfoExW(wideHost.c_str(), wideService.c_str(), NS_ALL,
                                    nullptr, &hints, &rawAddresses, nullptr,
                                    &overlapped, nullptr, &queryHandle);

      if (resolve == WSA_IO_PENDING)
      {
        while (true)
        {
          const auto waited = WaitForSingleObject(overlapped.hEvent, 100U);

          if (waited == WAIT_OBJECT_0)
          {
            resolve = GetAddrInfoExOverlappedResult(&overlapped);

            break;
          }

          if (waited == WAIT_FAILED)
          {
            const auto waitError = GetLastError();

            if (queryHandle)
            {
              (void)GetAddrInfoExCancel(&queryHandle);
            }

            (void)WaitForSingleObject(overlapped.hEvent, INFINITE);
            (void)GetAddrInfoExOverlappedResult(&overlapped);

            resolve = static_cast<int>(waitError);

            break;
          }

          if (stopToken.stop_requested() || Clock::now() >= deadline)
          {
            if (queryHandle)
            {
              (void)GetAddrInfoExCancel(&queryHandle);
            }

            // Winsock signals the completion event as part of cancellation.
            // Waiting is required before the OVERLAPPED storage can die.
            (void)WaitForSingleObject(overlapped.hEvent, INFINITE);
            (void)GetAddrInfoExOverlappedResult(&overlapped);

            if (rawAddresses)
            {
              FreeAddrInfoExW(rawAddresses);
            }

            (void)CloseHandle(overlapped.hEvent);

            return std::unexpected(SftpError(
                stopToken.stop_requested() ? RemoteErrorCode::Cancelled
                                           : RemoteErrorCode::TimedOut,
                stopToken.stop_requested() ? "SFTP name resolution was cancelled"
                                           : "SFTP name resolution timed out",
                0, !stopToken.stop_requested()));
          }
        }
      }

      (void)CloseHandle(overlapped.hEvent);

      const std::unique_ptr<ADDRINFOEXW, decltype(&FreeAddrInfoExW)> addresses{
          rawAddresses, FreeAddrInfoExW};

      if (resolve != 0)
      {
        return std::unexpected(SftpError(RemoteErrorCode::NameResolutionFailed,
                                         "Could not resolve the SFTP server",
                                         resolve, true));
      }

      for (auto *address = addresses.get(); address; address = address->ai_next)
      {
        if (!address->ai_addr || address->ai_addrlen == 0U ||
            address->ai_addrlen > sizeof(sockaddr_storage) ||
            address->ai_addrlen > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        {
          continue;
        }

        ResolvedAddress copy;

        std::memcpy(&copy.storage, address->ai_addr, address->ai_addrlen);
        copy.length = static_cast<int>(address->ai_addrlen);
        copy.family = address->ai_family;
        copy.socketType = address->ai_socktype;
        copy.protocol = address->ai_protocol;

        result.push_back(copy);
      }
#else
      addrinfo hints{};
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_STREAM;
      hints.ai_protocol = IPPROTO_TCP;

      addrinfo *rawAddresses{};

      const auto resolve = getaddrinfo(host.c_str(), service.c_str(), &hints, &rawAddresses);

      const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses{rawAddresses, freeaddrinfo};

      if (stopToken.stop_requested())
      {
        return std::unexpected(SftpError(RemoteErrorCode::Cancelled,
                                         "SFTP name resolution was cancelled"));
      }

      if (Clock::now() >= deadline)
      {
        return std::unexpected(SftpError(RemoteErrorCode::TimedOut,
                                         "SFTP name resolution timed out", resolve, true));
      }

      if (resolve != 0)
      {
        return std::unexpected(SftpError(RemoteErrorCode::NameResolutionFailed,
                                         "Could not resolve the SFTP server",
                                         resolve, true));
      }

      for (auto *address = addresses.get(); address; address = address->ai_next)
      {
        if (!address->ai_addr || address->ai_addrlen == 0U ||
            address->ai_addrlen > sizeof(sockaddr_storage) ||
            address->ai_addrlen > static_cast<socklen_t>((std::numeric_limits<int>::max)()))
        {
          continue;
        }

        ResolvedAddress copy;

        std::memcpy(&copy.storage, address->ai_addr, address->ai_addrlen);
        copy.length = static_cast<int>(address->ai_addrlen);
        copy.family = address->ai_family;
        copy.socketType = address->ai_socktype;
        copy.protocol = address->ai_protocol;

        result.push_back(copy);
      }
#endif
      if (result.empty())
      {
        return std::unexpected(SftpError(RemoteErrorCode::NameResolutionFailed,
                                         "The SFTP server resolved to no usable addresses"));
      }

      return result;
    }

    [[nodiscard]] Result<SocketHandle> ConnectTcp(const std::string &host,
                                                  std::uint16_t port,
                                                  std::chrono::seconds connectionTimeout,
                                                  std::stop_token stopToken)
    {
      const auto deadline = Clock::now() + connectionTimeout;

      const auto service = std::to_string(port);

      auto addresses = ResolveAddresses(host, service, deadline, stopToken);
      if (!addresses)
      {
        return std::unexpected(addresses.error());
      }

      int lastError{};

      for (const auto &address : *addresses)
      {
        if (stopToken.stop_requested())
        {
          return std::unexpected(SftpError(RemoteErrorCode::Cancelled,
                                           "The SFTP connection was cancelled"));
        }

        SocketHandle socket{::socket(address.family, address.socketType,
                                     address.protocol)};

        if (socket.Get() == InvalidSocket || !MakeNonblocking(socket.Get()))
        {
          lastError = SocketError();

          continue;
        }

#if !defined(_WIN32)
        if (!detail::PrepareSftpSocket(socket.Get()))
        {
          lastError = SocketError();

          continue;
        }
#endif

        const auto started = ::connect(
            socket.Get(), reinterpret_cast<const sockaddr *>(&address.storage), address.length);

        if (started == 0)
        {
          return socket;
        }

        lastError = SocketError();

        if (!SocketInProgress(lastError))
        {
          continue;
        }

        while (Clock::now() < deadline && !stopToken.stop_requested())
        {
          fd_set writable;
          fd_set errors;

          FD_ZERO(&writable);
          FD_ZERO(&errors);
          FD_SET(socket.Get(), &writable);
          FD_SET(socket.Get(), &errors);

          timeval timeout{.tv_sec = 0, .tv_usec = 100'000};

          const auto selected = select(static_cast<int>(socket.Get() + 1), nullptr,
                                       &writable, &errors, &timeout);

          if (selected < 0)
          {
            lastError = SocketError();

            break;
          }

          if (selected == 0)
          {
            continue;
          }

          int pending{};

#if defined(_WIN32)
          int length = sizeof(pending);
#else
          socklen_t length = sizeof(pending);
#endif

          if (getsockopt(socket.Get(), SOL_SOCKET, SO_ERROR,
                         reinterpret_cast<char *>(&pending), &length) == 0 &&
              pending == 0)
          {
            return socket;
          }

          lastError = pending != 0 ? pending : SocketError();

          break;
        }
      }

      if (stopToken.stop_requested())
      {
        return std::unexpected(SftpError(RemoteErrorCode::Cancelled,
                                         "The SFTP connection was cancelled"));
      }

      if (Clock::now() >= deadline)
      {
        return std::unexpected(SftpError(RemoteErrorCode::TimedOut,
                                         "The SFTP connection timed out", lastError, true));
      }

      return std::unexpected(SftpError(RemoteErrorCode::ConnectionFailed,
                                       "Could not connect to the SFTP server", lastError, true));
    }

    [[nodiscard]] std::string PathString(const std::filesystem::path &path)
    {
#if defined(_WIN32)
      const auto utf8 = path.u8string();

      return std::string{reinterpret_cast<const char *>(utf8.data()), utf8.size()};
#else
      return path.string();
#endif
    }

    [[nodiscard]] std::mutex &KnownHostsUpdateMutex()
    {
      static std::mutex mutex;
      return mutex;
    }

    [[nodiscard]] Result<void> AtomicReplaceFile(const std::filesystem::path &destination,
                                                 std::string_view contents)
    {
      std::filesystem::path temporary;

#if defined(_WIN32)
      HANDLE file = INVALID_HANDLE_VALUE;

      DWORD createError{};

      for (int attempt = 0; attempt < 32; ++attempt)
      {
        temporary = destination;
        temporary += L".tmp-" + std::filesystem::path{GenerateId()}.wstring();

        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);

        if (file != INVALID_HANDLE_VALUE)
        {
          break;
        }

        createError = GetLastError();

        if (createError != ERROR_FILE_EXISTS && createError != ERROR_ALREADY_EXISTS)
        {
          return std::unexpected(SftpError(
              RemoteErrorCode::LocalIo,
              "Could not create a unique temporary known_hosts file",
              static_cast<int>(createError)));
        }
      }

      if (file == INVALID_HANDLE_VALUE)
      {
        return std::unexpected(SftpError(
            RemoteErrorCode::LocalIo,
            "Could not allocate a unique temporary known_hosts filename",
            static_cast<int>(createError)));
      }

      std::size_t offset{};
      DWORD writeError{};

      while (offset < contents.size())
      {
        const auto remaining = contents.size() - offset;
        const auto chunk = static_cast<DWORD>((std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));

        DWORD written{};

        if (!WriteFile(file, contents.data() + offset, chunk, &written, nullptr) ||
            written == 0U)
        {
          writeError = GetLastError();

          break;
        }

        offset += written;
      }

      if (writeError == 0U && !FlushFileBuffers(file))
      {
        writeError = GetLastError();
      }

      if (!CloseHandle(file) && writeError == 0U)
      {
        writeError = GetLastError();
      }

      if (writeError != 0U)
      {
        (void)DeleteFileW(temporary.c_str());

        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                         "Could not durably write known_hosts",
                                         static_cast<int>(writeError)));
      }

      if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      {
        const auto replaceError = GetLastError();

        (void)DeleteFileW(temporary.c_str());

        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                         "Could not atomically replace known_hosts",
                                         static_cast<int>(replaceError)));
      }
#else
      int file = -1;
      int createError{};

      for (int attempt = 0; attempt < 32; ++attempt)
      {
        temporary = destination;
        temporary += ".tmp-" + GenerateId();

        file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);

        if (file >= 0)
        {
          break;
        }

        createError = errno;

        if (createError != EEXIST)
        {
          return std::unexpected(SftpError(
              RemoteErrorCode::LocalIo,
              "Could not create a unique temporary known_hosts file", createError));
        }
      }

      if (file < 0)
      {
        return std::unexpected(SftpError(
            RemoteErrorCode::LocalIo,
            "Could not allocate a unique temporary known_hosts filename", createError));
      }

      std::size_t offset{};
      int writeError{};

      while (offset < contents.size())
      {
        const auto written = ::write(file, contents.data() + offset, contents.size() - offset);

        if (written < 0 && errno == EINTR)
        {
          continue;
        }

        if (written <= 0)
        {
          writeError = errno != 0 ? errno : EIO;

          break;
        }

        offset += static_cast<std::size_t>(written);
      }

      if (writeError == 0 && ::fsync(file) != 0)
      {
        writeError = errno;
      }

      if (::close(file) != 0 && writeError == 0)
      {
        writeError = errno;
      }

      if (writeError != 0)
      {
        (void)::unlink(temporary.c_str());

        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                         "Could not durably write known_hosts", writeError));
      }

      if (::rename(temporary.c_str(), destination.c_str()) != 0)
      {
        const auto replaceError = errno;

        (void)::unlink(temporary.c_str());

        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                         "Could not atomically replace known_hosts",
                                         replaceError));
      }

      const auto directory = destination.parent_path().empty()
                                 ? std::filesystem::path{"."}
                                 : destination.parent_path();

#if defined(O_DIRECTORY)
      const auto directoryHandle = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
#else
      const auto directoryHandle = ::open(directory.c_str(), O_RDONLY);
#endif

      if (directoryHandle >= 0)
      {
        const auto synced = ::fsync(directoryHandle);

        const auto syncError = synced == 0 ? 0 : errno;

        (void)::close(directoryHandle);

        if (syncError != 0)
        {
          return std::unexpected(SftpError(
              RemoteErrorCode::LocalIo,
              "known_hosts was replaced, but its directory could not be flushed",
              syncError, false, true));
        }
      }
      else
      {
        return std::unexpected(SftpError(
            RemoteErrorCode::LocalIo,
            "known_hosts was replaced, but its directory could not be opened for flushing",
            errno, false, true));
      }
#endif

      return {};
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
        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
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
        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
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
        return std::unexpected(SftpError(RemoteErrorCode::AlreadyExists,
                                         "The local destination already exists",
                                         static_cast<int>(error)));
      }

      return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                       "Could not atomically finalize the download",
                                       static_cast<int>(error)));
#else
      const auto partialFile = ::open(partial.c_str(), O_RDONLY);

      if (partialFile < 0)
      {
        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
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
        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
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

        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                         "Could not atomically finalize the download",
                                         error));
      }

      if (::link(partial.c_str(), destination.c_str()) != 0)
      {
        const auto error = errno;

        return std::unexpected(SftpError(
            error == EEXIST ? RemoteErrorCode::AlreadyExists : RemoteErrorCode::LocalIo,
            error == EEXIST ? "The local destination already exists"
                            : "Could not finalize the download without overwriting",
            error));
      }

      if (::unlink(partial.c_str()) != 0)
      {
        return std::unexpected(SftpError(
            RemoteErrorCode::LocalIo,
            "The download was finalized, but its partial hard link could not be removed",
            errno, false, true));
      }

      return {};
#endif
    }

    [[nodiscard]] bool SafeSftpPath(const RemotePath &path) noexcept
    {
      if (path.Bytes().empty() || path.Bytes().find('\0') != std::string::npos)
      {
        return false;
      }

      std::string_view remaining{path.Bytes()};

      while (!remaining.empty())
      {
        const auto slash = remaining.find('/');
        const auto component = remaining.substr(0, slash);

        if (component == "." || component == "..")
        {
          return false;
        }

        if (slash == std::string_view::npos)
        {
          break;
        }

        remaining.remove_prefix(slash + 1U);
      }

      return true;
    }

    [[nodiscard]] Result<std::string> ReadLocalFile(const std::filesystem::path &path,
                                                    std::string_view description)
    {
      constexpr std::streamoff MaxKeyFileSize = 1024 * 1024;

      std::ifstream input{path, std::ios::binary};

      if (!input)
      {
        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                         "Could not open " + std::string{description}));
      }

      input.seekg(0, std::ios::end);

      const auto length = input.tellg();

      if (length < 0)
      {
        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                         "Could not determine the size of " +
                                             std::string{description}));
      }

      const auto byteCount = static_cast<std::streamoff>(length);

      if (byteCount > MaxKeyFileSize)
      {
        return std::unexpected(SftpError(
            RemoteErrorCode::InvalidArgument,
            std::string{description} + " exceeds the 1 MiB safety limit"));
      }

      std::string contents(static_cast<std::size_t>(byteCount), '\0');

      input.seekg(0, std::ios::beg);
      input.read(contents.data(), static_cast<std::streamsize>(byteCount));

      if (!input && input.gcount() != byteCount)
      {
        ClearSensitiveString(contents);

        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                         "Could not read " + std::string{description}));
      }

      return contents;
    }

    [[nodiscard]] RemoteEntryKind EntryKind(const LIBSSH2_SFTP_ATTRIBUTES &attributes) noexcept
    {
      if ((attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) == 0)
      {
        return RemoteEntryKind::Other;
      }

      if (LIBSSH2_SFTP_S_ISREG(attributes.permissions))
      {
        return RemoteEntryKind::File;
      }

      if (LIBSSH2_SFTP_S_ISDIR(attributes.permissions))
      {
        return RemoteEntryKind::Directory;
      }

      if (LIBSSH2_SFTP_S_ISLNK(attributes.permissions))
      {
        return RemoteEntryKind::Symlink;
      }

      return RemoteEntryKind::Other;
    }

    [[nodiscard]] RemoteEntry MakeEntry(const RemotePath &path,
                                        const RemotePath &name,
                                        const LIBSSH2_SFTP_ATTRIBUTES &attributes,
                                        const std::string_view longEntry = {})
    {
      RemoteEntry entry;
      entry.path = path;
      entry.name = name;
      entry.kind = EntryKind(attributes);

      if ((attributes.flags & LIBSSH2_SFTP_ATTR_SIZE) != 0)
      {
        entry.size = attributes.filesize;
      }

      if ((attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) != 0)
      {
        entry.permissions =
            static_cast<std::uint32_t>(attributes.permissions) & 07777U;
      }

      if (const auto namedOwnerGroup = sftp::ParseLongEntryOwnerGroup(longEntry))
      {
        entry.owner = namedOwnerGroup->owner;
        entry.group = namedOwnerGroup->group;
      }
      else if ((attributes.flags & LIBSSH2_SFTP_ATTR_UIDGID) != 0)
      {
        entry.owner = std::to_string(attributes.uid);
        entry.group = std::to_string(attributes.gid);
      }

      if ((attributes.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) != 0)
      {
        entry.modifiedAt = std::chrono::system_clock::time_point{
            std::chrono::seconds{attributes.mtime}};
      }

      entry.hidden = !name.Bytes().empty() && name.Bytes().front() == '.';

      return entry;
    }

    [[nodiscard]] int KnownHostKeyType(int hostKeyType) noexcept
    {
      switch (hostKeyType)
      {
      case LIBSSH2_HOSTKEY_TYPE_RSA:
        return LIBSSH2_KNOWNHOST_KEY_SSHRSA;

      case LIBSSH2_HOSTKEY_TYPE_DSS:
        return LIBSSH2_KNOWNHOST_KEY_SSHDSS;

      case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
        return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;

      case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
        return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;

      case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
        return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;

      case LIBSSH2_HOSTKEY_TYPE_ED25519:
        return LIBSSH2_KNOWNHOST_KEY_ED25519;

      default:
        return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
      }
    }

    [[nodiscard]] std::string_view HostKeyAlgorithm(int hostKeyType) noexcept
    {
      switch (hostKeyType)
      {
      case LIBSSH2_HOSTKEY_TYPE_RSA:
        return "ssh-rsa";

      case LIBSSH2_HOSTKEY_TYPE_DSS:
        return "ssh-dss";

      case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
        return "ecdsa-sha2-nistp256";

      case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
        return "ecdsa-sha2-nistp384";

      case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
        return "ecdsa-sha2-nistp521";

      case LIBSSH2_HOSTKEY_TYPE_ED25519:
        return "ssh-ed25519";

      default:
        return "unknown";
      }
    }

    class SftpSession final : public IRemoteSession
    {
    public:
      ~SftpSession() override { Disconnect(); }

      ProtocolKind Protocol() const noexcept override { return ProtocolKind::Sftp; }
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
      Result<LIBSSH2_SFTP_ATTRIBUTES> ReadAttributes(
          const RemotePath &path, std::stop_token stopToken);
      Result<void> WaitSocket(std::stop_token stopToken,
                              bool operationMayHaveSucceeded);
      template <typename Callable>
      Result<int> CallInt(Callable &&callable,
                          std::string_view operation,
                          std::stop_token stopToken,
                          bool mutating = false);
      template <typename Callable>
      Result<ssize_t> CallSize(Callable &&callable,
                               std::string_view operation,
                               std::stop_token stopToken,
                               bool mutating = false);
      template <typename Pointer, typename Callable>
      Result<Pointer *> CallPointer(Callable &&callable,
                                    std::string_view operation,
                                    std::stop_token stopToken,
                                    bool mutating = false);
      Result<void> CloseHandle(LIBSSH2_SFTP_HANDLE *handle,
                               std::stop_token stopToken,
                               bool mutating);
      Result<void> ReplaceWithPosixRename(const RemotePath &source,
                                          const RemotePath &destination,
                                          std::stop_token stopToken);
      [[nodiscard]] RemoteError LastError(std::string_view operation,
                                          int nativeCode,
                                          bool mutating = false) const;
      Result<void> VerifyHost(std::stop_token stopToken);
      Result<void> Authenticate(std::stop_token stopToken);
      Result<std::optional<std::string>> AuthenticationMethods(
          std::stop_token stopToken);
      Result<void> AuthenticatePassword(std::stop_token stopToken);
      Result<void> AuthenticatePrivateKey(std::stop_token stopToken);
      Result<void> AuthenticateAgent(std::stop_token stopToken);
      Result<void> AuthenticateKeyboardInteractive(std::stop_token stopToken);
      Result<void> AuthenticatePasswordKeyboardInteractive(
          std::stop_token stopToken);
      Result<void> ReadKnownHosts(LIBSSH2_KNOWNHOSTS *hosts) const;
      Result<void> WriteKnownHosts(LIBSSH2_KNOWNHOSTS *hosts) const;
      Result<void> RemoveImpl(const RemotePath &path,
                              bool recursive,
                              std::stop_token stopToken);
      [[nodiscard]] TransferControl ReportProgress(const ProgressCallback &callback,
                                                   std::string_view jobId,
                                                   std::uint64_t transferred,
                                                   std::optional<std::uint64_t> total,
                                                   Clock::time_point started,
                                                   std::uint64_t resumeOffset) const;
      void Log(DiagnosticLevel level, std::string_view message) const;

      static void KeyboardCallback(const char *name,
                                   int nameLength,
                                   const char *instruction,
                                   int instructionLength,
                                   int promptCount,
                                   const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts,
                                   LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses,
                                   void **abstract);

      SiteProfile mSite;
      SessionCallbacks mCallbacks;
      SocketHandle mSocket;
      LIBSSH2_SESSION *mSession{};
      LIBSSH2_SFTP *mSftp{};
      std::optional<RemoteError> mKeyboardError;
      std::stop_token mKeyboardStopToken;
      bool mKeyboardPromptReceived{};
      bool mConnected{};
    };

    RemoteError SftpSession::LastError(std::string_view operation,
                                       int nativeCode,
                                       bool mutating) const
    {
      std::string detail;

      if (mSession)
      {
        char *message{};
        int length{};

        libssh2_session_last_error(mSession, &message, &length, 0);

        if (message && length > 0)
        {
          detail.assign(message, static_cast<std::size_t>(length));
        }
      }

      std::optional<unsigned long> status;

      if (nativeCode == LIBSSH2_ERROR_SFTP_PROTOCOL && mSftp)
      {
        status = libssh2_sftp_last_error(mSftp);
      }

      return sftp::MakeOperationError(operation, nativeCode, detail, status, mutating);
    }

    Result<void> SftpSession::WaitSocket(const std::stop_token stopToken,
                                         const bool operationMayHaveSucceeded)
    {
      const auto deadline = Clock::now() + mCallbacks.commandIdleTimeout;

      while (!stopToken.stop_requested() && Clock::now() < deadline)
      {
        fd_set readable;
        fd_set writable;

        FD_ZERO(&readable);
        FD_ZERO(&writable);

        const auto directions = libssh2_session_block_directions(mSession);

        if ((directions & LIBSSH2_SESSION_BLOCK_INBOUND) != 0 || directions == 0)
        {
          FD_SET(mSocket.Get(), &readable);
        }

        if ((directions & LIBSSH2_SESSION_BLOCK_OUTBOUND) != 0 || directions == 0)
        {
          FD_SET(mSocket.Get(), &writable);
        }

        timeval timeout{.tv_sec = 0, .tv_usec = 100'000};

        const auto selected = select(static_cast<int>(mSocket.Get() + 1), &readable,
                                     &writable, nullptr, &timeout);

        if (selected > 0)
        {
          return {};
        }

        if (selected < 0)
        {
          return std::unexpected(SftpError(RemoteErrorCode::ConnectionLost,
                                           "Waiting for the SFTP socket failed",
                                           SocketError(), true,
                                           operationMayHaveSucceeded));
        }
      }

      if (stopToken.stop_requested())
      {
        return std::unexpected(SftpError(RemoteErrorCode::Cancelled,
                                         "The SFTP operation was cancelled",
                                         0, false, operationMayHaveSucceeded));
      }

      return std::unexpected(SftpError(RemoteErrorCode::TimedOut,
                                       "The SFTP server did not respond", 0, true,
                                       operationMayHaveSucceeded));
    }

    template <typename Callable>
    Result<int> SftpSession::CallInt(Callable &&callable,
                                     std::string_view operation,
                                     std::stop_token stopToken,
                                     bool mutating)
    {
      bool requestMayHaveBeenSent = false;

      while (true)
      {
        if (stopToken.stop_requested())
        {
          return std::unexpected(SftpError(RemoteErrorCode::Cancelled,
                                           std::string{operation} + " was cancelled",
                                           0, false, requestMayHaveBeenSent));
        }

        const auto result = std::forward<Callable>(callable)();

        requestMayHaveBeenSent = mutating;

        if (result == LIBSSH2_ERROR_EAGAIN)
        {
          if (auto waited = WaitSocket(stopToken, requestMayHaveBeenSent); !waited)
          {
            return std::unexpected(waited.error());
          }

          continue;
        }

        if (result < 0)
        {
          return std::unexpected(LastError(operation, result, mutating));
        }

        return result;
      }
    }

    template <typename Callable>
    Result<ssize_t> SftpSession::CallSize(Callable &&callable,
                                          std::string_view operation,
                                          std::stop_token stopToken,
                                          bool mutating)
    {
      bool requestMayHaveBeenSent = false;

      while (true)
      {
        if (stopToken.stop_requested())
        {
          return std::unexpected(SftpError(RemoteErrorCode::Cancelled,
                                           std::string{operation} + " was cancelled",
                                           0, false, requestMayHaveBeenSent));
        }

        const auto result = std::forward<Callable>(callable)();

        requestMayHaveBeenSent = mutating;

        if (result == LIBSSH2_ERROR_EAGAIN)
        {
          if (auto waited = WaitSocket(stopToken, requestMayHaveBeenSent); !waited)
          {
            return std::unexpected(waited.error());
          }

          continue;
        }

        if (result < 0)
        {
          return std::unexpected(LastError(operation, static_cast<int>(result), mutating));
        }

        return result;
      }
    }

    template <typename Pointer, typename Callable>
    Result<Pointer *> SftpSession::CallPointer(Callable &&callable,
                                               std::string_view operation,
                                               std::stop_token stopToken,
                                               bool mutating)
    {
      bool requestMayHaveBeenSent = false;

      while (true)
      {
        if (stopToken.stop_requested())
        {
          return std::unexpected(SftpError(RemoteErrorCode::Cancelled,
                                           std::string{operation} + " was cancelled",
                                           0, false, requestMayHaveBeenSent));
        }

        if (auto *result = std::forward<Callable>(callable)())
        {
          return result;
        }

        requestMayHaveBeenSent = mutating;

        const auto error = libssh2_session_last_errno(mSession);

        if (error == LIBSSH2_ERROR_EAGAIN)
        {
          if (auto waited = WaitSocket(stopToken, requestMayHaveBeenSent); !waited)
          {
            return std::unexpected(waited.error());
          }

          continue;
        }

        return std::unexpected(LastError(operation, error, mutating));
      }
    }

    Result<void> SftpSession::CloseHandle(LIBSSH2_SFTP_HANDLE *handle,
                                          const std::stop_token stopToken,
                                          const bool mutating)
    {
      if (!handle)
      {
        return {};
      }

      auto closed = CallInt([&]
                            { return libssh2_sftp_close_handle(handle); },
                            "Closing an SFTP handle", stopToken, mutating);

      if (!closed)
      {
        return std::unexpected(closed.error());
      }

      return {};
    }

    void SftpSession::Log(DiagnosticLevel level, std::string_view message) const
    {
      if (mCallbacks.diagnostic)
      {
        mCallbacks.diagnostic(level, message);
      }
    }

    Result<void> SftpSession::Connect(const SiteProfile &site,
                                      const SessionCallbacks &callbacks,
                                      std::stop_token stopToken)
    {
      if (mConnected || mSession)
      {
        return std::unexpected(SftpError(RemoteErrorCode::AlreadyConnected,
                                         "The SFTP session is already connected"));
      }

      if (site.protocol != ProtocolKind::Sftp)
      {
        return std::unexpected(SftpError(RemoteErrorCode::Unsupported,
                                         "The SFTP backend cannot open this protocol"));
      }

      if (!IsValidEndpointHost(site.host) || site.port == 0 || site.username.empty())
      {
        return std::unexpected(SftpError(RemoteErrorCode::InvalidArgument,
                                         "SFTP requires a plain DNS/IP host, port, and username"));
      }

      if (callbacks.connectionTimeout.count() <= 0 ||
          callbacks.commandIdleTimeout.count() <= 0)
      {
        return std::unexpected(SftpError(RemoteErrorCode::InvalidArgument,
                                         "SFTP timeouts must be positive whole seconds"));
      }

      if (!GetNetworkGlobal().Ready())
      {
        return std::unexpected(SftpError(RemoteErrorCode::ProtocolError,
                                         "libssh2 or the socket library could not initialize"));
      }

      mSite = site;
      mCallbacks = callbacks;

      auto socket = ConnectTcp(site.host, site.port, callbacks.connectionTimeout, stopToken);
      if (!socket)
      {
        Disconnect();

        return std::unexpected(socket.error());
      }

      mSocket = std::move(*socket);

      mSession = libssh2_session_init_ex(nullptr, nullptr, nullptr, this);
      if (!mSession)
      {
        Disconnect();

        return std::unexpected(SftpError(RemoteErrorCode::ProtocolError,
                                         "Could not create a libssh2 session"));
      }

#if !defined(_WIN32)
      libssh2_session_callback_set2(
          mSession, LIBSSH2_CALLBACK_SEND,
          reinterpret_cast<libssh2_cb_generic *>(detail::SendWithoutSigpipe));
#endif

      const char **supportedHostKeyAlgorithms{};

      const int supportedHostKeyAlgorithmCount =
          libssh2_session_supported_algs(
              mSession,
              LIBSSH2_METHOD_HOSTKEY,
              &supportedHostKeyAlgorithms);

      if (supportedHostKeyAlgorithmCount <= 0)
      {
        const auto error = LastError(
            "Reading supported SSH host-key algorithms",
            supportedHostKeyAlgorithmCount < 0
                ? supportedHostKeyAlgorithmCount
                : LIBSSH2_ERROR_METHOD_NOT_SUPPORTED);

        Disconnect();

        return std::unexpected(error);
      }

      const auto hostKeyPreference = sftp::MakeHostKeyAlgorithmPreference(
          std::span<const char *const>{
              supportedHostKeyAlgorithms,
              static_cast<std::size_t>(supportedHostKeyAlgorithmCount)});

      libssh2_free(
          mSession,
          const_cast<void *>(
              static_cast<const void *>(supportedHostKeyAlgorithms)));

      supportedHostKeyAlgorithms = nullptr;

      const int preferredHostKeys = libssh2_session_method_pref(
          mSession, LIBSSH2_METHOD_HOSTKEY, hostKeyPreference.c_str());

      if (preferredHostKeys < 0)
      {
        const auto error = LastError(
            "Selecting SSH host-key algorithms", preferredHostKeys);

        Disconnect();

        return std::unexpected(error);
      }

      libssh2_session_set_blocking(mSession, 0);
      libssh2_session_set_timeout(
          mSession, Libssh2TimeoutMilliseconds(mCallbacks.commandIdleTimeout));

      auto handshake = CallInt(
          [&]
          { return libssh2_session_handshake(mSession, mSocket.Get()); },
          "SSH handshake", stopToken);
      if (!handshake)
      {
        const auto error = handshake.error();

        Disconnect();

        return std::unexpected(error);
      }

      if (auto verified = VerifyHost(stopToken); !verified)
      {
        const auto error = verified.error();

        Disconnect();

        return std::unexpected(error);
      }

      if (auto authenticated = Authenticate(stopToken); !authenticated)
      {
        const auto error = authenticated.error();

        Disconnect();

        return std::unexpected(error);
      }

      auto sftp = CallPointer<LIBSSH2_SFTP>([&]
                                            { return libssh2_sftp_init(mSession); },
                                            "Starting the SFTP subsystem", stopToken);
      if (!sftp)
      {
        const auto error = sftp.error();

        Disconnect();

        return std::unexpected(error);
      }

      mSftp = *sftp;

      mConnected = true;

      return {};
    }

    void SftpSession::Disconnect() noexcept
    {
      mConnected = false;

      if (mSession)
      {
        libssh2_session_set_timeout(mSession, Libssh2TimeoutMilliseconds(mCallbacks.commandIdleTimeout));

        libssh2_session_set_blocking(mSession, 1);
      }

      if (mSftp)
      {
        libssh2_sftp_shutdown(mSftp);

        mSftp = nullptr;
      }

      if (mSession)
      {
        libssh2_session_disconnect(mSession, "havRemote disconnected");

        libssh2_session_free(mSession);

        mSession = nullptr;
      }

      mSocket.Reset();
      mKeyboardError.reset();

      mCallbacks = {};
      mSite = {};
    }

    Result<void> SftpSession::ReadKnownHosts(LIBSSH2_KNOWNHOSTS *hosts) const
    {
      if (mCallbacks.knownHostsFile.empty())
      {
        return {};
      }

      std::error_code error;
      if (!std::filesystem::exists(mCallbacks.knownHostsFile, error))
      {
        if (error)
        {
          return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                           "Could not inspect the known_hosts file: " +
                                               error.message(),
                                           error.value()));
        }

        return {};
      }

      std::ifstream input{mCallbacks.knownHostsFile, std::ios::binary};
      if (!input)
      {
        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                         "Could not open the known_hosts file"));
      }

      std::string line;

      while (std::getline(input, line))
      {
        line.push_back('\n');

        const auto result = libssh2_knownhost_readline(
            hosts, line.data(), line.size(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);

        if (result < 0)
        {
          return std::unexpected(SftpError(RemoteErrorCode::ParseError,
                                           "The known_hosts file contains an invalid entry",
                                           result));
        }
      }

      if (!input.eof())
      {
        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                         "Could not read the known_hosts file"));
      }

      return {};
    }

    Result<void> SftpSession::WriteKnownHosts(LIBSSH2_KNOWNHOSTS *hosts) const
    {
      if (mCallbacks.knownHostsFile.empty())
      {
        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                         "No known_hosts path is configured"));
      }

      std::error_code error;

      const auto parent = mCallbacks.knownHostsFile.parent_path();
      if (!parent.empty())
      {
        std::filesystem::create_directories(parent, error);
      }

      if (error)
      {
        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                         "Could not create the known_hosts directory: " +
                                             error.message(),
                                         error.value()));
      }

      std::string serialized;
      libssh2_knownhost *previous{};

      while (true)
      {
        libssh2_knownhost *current{};

        const auto next = libssh2_knownhost_get(hosts, &current, previous);

        if (next == 1)
        {
          break;
        }

        if (next < 0)
        {
          return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                           "Could not serialize known_hosts", next));
        }

        std::vector<char> buffer(4096);
        std::size_t length{};
        int written{};

        while (true)
        {
          written = libssh2_knownhost_writeline(hosts, current, buffer.data(),
                                                buffer.size(), &length,
                                                LIBSSH2_KNOWNHOST_FILE_OPENSSH);

          if (written != LIBSSH2_ERROR_BUFFER_TOO_SMALL)
          {
            break;
          }

          if (buffer.size() >= 1024U * 1024U)
          {
            return std::unexpected(SftpError(
                RemoteErrorCode::LocalIo,
                "A known_hosts entry exceeds the supported serialized size", written));
          }

          buffer.resize(buffer.size() * 2U);
        }

        if (written < 0 || length > buffer.size())
        {
          return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                           "Could not serialize a known_hosts entry", written));
        }

        if (length > serialized.max_size() - serialized.size())
        {
          return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                           "The known_hosts file is too large to serialize"));
        }

        serialized.append(buffer.data(), length);

        previous = current;
      }

      return AtomicReplaceFile(mCallbacks.knownHostsFile, serialized);
    }

    Result<void> SftpSession::VerifyHost(std::stop_token stopToken)
    {
      std::size_t keyLength{};

      int keyType{};

      const auto *key = libssh2_session_hostkey(mSession, &keyLength, &keyType);

      if (!key || keyLength == 0)
      {
        return std::unexpected(LastError("Reading the SSH host key",
                                         LIBSSH2_ERROR_HOSTKEY_INIT));
      }

      const auto knownType = KnownHostKeyType(keyType);
      if (knownType == LIBSSH2_KNOWNHOST_KEY_UNKNOWN)
      {
        return std::unexpected(SftpError(RemoteErrorCode::Unsupported,
                                         "The server uses an unsupported SSH host-key algorithm"));
      }

      // Protect the entire read/check/decision/write transaction. Otherwise two
      // transfer workers accepting different endpoints concurrently can each
      // rewrite a stale snapshot and silently discard the other's new key.
      std::unique_lock knownHostsLock{KnownHostsUpdateMutex()};

      const std::unique_ptr<LIBSSH2_KNOWNHOSTS, decltype(&libssh2_knownhost_free)> hosts{
          libssh2_knownhost_init(mSession), libssh2_knownhost_free};

      if (!hosts)
      {
        return std::unexpected(LastError("Initializing known_hosts",
                                         LIBSSH2_ERROR_KNOWN_HOSTS));
      }

      if (auto read = ReadKnownHosts(hosts.get()); !read)
      {
        return read;
      }

      libssh2_knownhost *existing{};

      const auto typeMask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW |
                            knownType;

      const auto checked = libssh2_knownhost_checkp(hosts.get(), mSite.host.c_str(), mSite.port,
                                                    key, keyLength, typeMask, &existing);

      if (checked == LIBSSH2_KNOWNHOST_CHECK_MATCH)
      {
        return {};
      }

      if (checked == LIBSSH2_KNOWNHOST_CHECK_FAILURE)
      {
        return std::unexpected(SftpError(RemoteErrorCode::ProtocolError,
                                         "Could not check the SSH host key",
                                         LIBSSH2_ERROR_KNOWN_HOSTS));
      }

      const bool changed = checked == LIBSSH2_KNOWNHOST_CHECK_MISMATCH;

      if (!mCallbacks.verifyTrust)
      {
        return std::unexpected(SftpError(changed ? RemoteErrorCode::HostKeyChanged
                                                 : RemoteErrorCode::TrustRejected,
                                         changed ? "The SSH host key has changed"
                                                 : "The SSH host key is not trusted"));
      }

      TrustChallenge challenge;
      challenge.kind = TrustKind::SshHostKey;
      challenge.status = changed ? TrustStatus::Changed : TrustStatus::Unknown;
      challenge.siteId = mSite.id;
      challenge.host = mSite.host;
      challenge.port = mSite.port;
      challenge.algorithm = HostKeyAlgorithm(keyType);
      challenge.sha256Fingerprint = Sha256Fingerprint(reinterpret_cast<const std::byte *>(key), keyLength);
      challenge.publicKey.resize(keyLength);

      std::memcpy(challenge.publicKey.data(), key, keyLength);

      challenge.diagnostic = changed ? "The received key differs from known_hosts"
                                     : "No matching key exists in known_hosts";

      auto decision = mCallbacks.verifyTrust(challenge, stopToken);
      if (!decision)
      {
        return std::unexpected(decision.error());
      }
      if (*decision == TrustDecision::Reject)
      {
        return std::unexpected(SftpError(changed ? RemoteErrorCode::HostKeyChanged
                                                 : RemoteErrorCode::TrustRejected,
                                         changed ? "The changed SSH host key was rejected"
                                                 : "The unknown SSH host key was rejected"));
      }

      // Changed keys are never accepted transiently or through the generic
      // permanent action: replacing one must be an explicit UI decision.
      if (changed && *decision != TrustDecision::ReplaceStored)
      {
        return std::unexpected(SftpError(RemoteErrorCode::HostKeyChanged,
                                         "A changed SSH host key must be explicitly replaced"));
      }

      if (!changed && *decision == TrustDecision::ReplaceStored)
      {
        return std::unexpected(SftpError(RemoteErrorCode::InvalidArgument,
                                         "There is no stored SSH host key to replace"));
      }

      if (*decision == TrustDecision::AcceptOnce)
      {
        return {};
      }

      if (changed && existing)
      {
        const auto removed = libssh2_knownhost_del(hosts.get(), existing);

        if (removed < 0)
        {
          return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                           "Could not remove the old SSH host key", removed));
        }
      }

      const auto storedName = mSite.port == 22
                                  ? mSite.host
                                  : "[" + mSite.host + "]:" + std::to_string(mSite.port);

      libssh2_knownhost *stored{};

      const auto added = libssh2_knownhost_addc(hosts.get(), storedName.c_str(), nullptr,
                                                key, keyLength, "havRemote", 9,
                                                typeMask, &stored);

      if (added < 0)
      {
        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                         "Could not store the SSH host key", added));
      }

      return WriteKnownHosts(hosts.get());
    }

    Result<std::optional<std::string>> SftpSession::AuthenticationMethods(
        const std::stop_token stopToken)
    {
      while (true)
      {
        if (stopToken.stop_requested())
        {
          return std::unexpected(SftpError(
              RemoteErrorCode::Cancelled,
              "Reading SSH authentication methods was cancelled"));
        }

        if (auto *const methods = libssh2_userauth_list(
                mSession, mSite.username.c_str(),
                static_cast<unsigned int>(mSite.username.size())))
        {
          // libssh2 owns this buffer and may replace it during another auth
          // operation, so never retain the returned pointer.
          return std::optional<std::string>{std::in_place, methods};
        }

        if (libssh2_userauth_authenticated(mSession) != 0)
        {
          return std::nullopt;
        }

        const auto error = libssh2_session_last_errno(mSession);

        if (error == LIBSSH2_ERROR_EAGAIN)
        {
          if (auto waited = WaitSocket(stopToken, false); !waited)
          {
            return std::unexpected(waited.error());
          }

          continue;
        }

        return std::unexpected(LastError("Reading SSH authentication methods", error));
      }
    }

    Result<void> SftpSession::Authenticate(std::stop_token stopToken)
    {
      switch (mSite.authentication.kind)
      {
      case AuthenticationKind::Password:
        return AuthenticatePassword(stopToken);

      case AuthenticationKind::PrivateKey:
        return AuthenticatePrivateKey(stopToken);

      case AuthenticationKind::Agent:
        return AuthenticateAgent(stopToken);

      case AuthenticationKind::KeyboardInteractive:
      {
        auto methods = AuthenticationMethods(stopToken);
        if (!methods)
        {
          return std::unexpected(methods.error());
        }
        if (!*methods)
        {
          return {};
        }

        if (!sftp::AuthenticationMethodOffered(**methods,
                                               "keyboard-interactive"))
        {
          return std::unexpected(SftpError(
              RemoteErrorCode::Unsupported,
              "The server does not currently offer keyboard-interactive "
              "authentication (offered: " +
                  DisplayAuthenticationMethods(**methods) +
                  "). If this server requires a password before its prompts, "
                  "select Password + keyboard-interactive."));
        }

        return AuthenticateKeyboardInteractive(stopToken);
      }

      case AuthenticationKind::PasswordKeyboardInteractive:
        return AuthenticatePasswordKeyboardInteractive(stopToken);
      }

      return std::unexpected(SftpError(RemoteErrorCode::Unsupported, "Unsupported SFTP authentication method"));
    }

    Result<void> SftpSession::AuthenticatePassword(std::stop_token stopToken)
    {
      if (!mCallbacks.requestCredential)
      {
        return std::unexpected(SftpError(RemoteErrorCode::CredentialUnavailable,
                                         "No password provider is available"));
      }

      auto password = mCallbacks.requestCredential(
          CredentialRequest{.kind = CredentialKind::Password,
                            .siteId = mSite.id,
                            .username = mSite.username,
                            .prompt = "Password for " + mSite.username + "@" + mSite.host,
                            .echo = false},
          stopToken);

      if (!password)
      {
        return std::unexpected(password.error());
      }

      SensitiveStringGuard passwordGuard{*password};

      auto result = CallInt(
          [&]
          {
            return libssh2_userauth_password_ex(
                mSession, mSite.username.c_str(),
                static_cast<unsigned int>(mSite.username.size()), password->data(),
                static_cast<unsigned int>(password->size()), nullptr);
          },
          "Password authentication", stopToken);

      if (!result)
      {
        return std::unexpected(result.error());
      }

      return {};
    }

    Result<void> SftpSession::AuthenticatePasswordKeyboardInteractive(
        const std::stop_token stopToken)
    {
      auto methods = AuthenticationMethods(stopToken);
      if (!methods)
      {
        return std::unexpected(methods.error());
      }
      if (!*methods)
      {
        return {};
      }

      if (!sftp::AuthenticationMethodOffered(**methods, "password"))
      {
        return std::unexpected(SftpError(
            RemoteErrorCode::Unsupported,
            "The server does not currently offer the password first factor "
            "required by Password + keyboard-interactive authentication "
            "(offered: " +
                DisplayAuthenticationMethods(**methods) + ")."));
      }

      auto password = AuthenticatePassword(stopToken);
      if (password)
      {
        // The server decides whether another factor is required. A complete
        // SSH authentication success cannot be followed by another userauth
        // request, so the connection is ready at this point.
        return {};
      }

      if (password.error().code != RemoteErrorCode::AuthenticationFailed)
      {
        return std::unexpected(password.error());
      }

      // RFC 4252 reports partial success in SSH_MSG_USERAUTH_FAILURE, but
      // libssh2 1.11.1 does not expose that bit. A staged server is now ready
      // for keyboard-interactive. If the password was actually rejected, it
      // will reject this request without issuing a prompt.
      auto interactive = AuthenticateKeyboardInteractive(stopToken);
      if (interactive)
      {
        return {};
      }

      if (!mKeyboardPromptReceived &&
          interactive.error().code == RemoteErrorCode::AuthenticationFailed)
      {
        return std::unexpected(SftpError(
            RemoteErrorCode::AuthenticationFailed,
            "Password + keyboard-interactive authentication failed before "
            "the server requested its second-factor prompt. The password may "
            "be incorrect, or the server may not use this authentication "
            "sequence.",
            interactive.error().nativeCode));
      }

      return std::unexpected(interactive.error());
    }

    Result<void> SftpSession::AuthenticatePrivateKey(std::stop_token stopToken)
    {
      if (mSite.authentication.privateKeyFile.empty())
      {
        return std::unexpected(SftpError(RemoteErrorCode::InvalidArgument,
                                         "Private-key authentication requires a key file"));
      }

      auto privateKey = ReadLocalFile(mSite.authentication.privateKeyFile,
                                      "the private key file");
      if (!privateKey)
      {
        return std::unexpected(privateKey.error());
      }

      SensitiveStringGuard privateKeyGuard{*privateKey};

      const bool encryptedKey =
          sftp::DetectPrivateKeyEncryption(*privateKey) ==
          sftp::PrivateKeyEncryption::Encrypted;

      std::string passphrase;

      SensitiveStringGuard passphraseGuard{passphrase};

      const auto requestPassphrase = [&](const bool forcePrompt) -> Result<void>
      {
        if (!mCallbacks.requestCredential)
        {
          return std::unexpected(SftpError(RemoteErrorCode::CredentialUnavailable,
                                           "No private-key passphrase provider is available"));
        }

        auto requested = mCallbacks.requestCredential(
            CredentialRequest{.kind = CredentialKind::PrivateKeyPassphrase,
                              .siteId = mSite.id,
                              .username = mSite.username,
                              .prompt = "Passphrase for " +
                                        PathString(mSite.authentication.privateKeyFile),
                              .echo = false,
                              .forcePrompt = forcePrompt},
            stopToken);
        if (!requested)
        {
          return std::unexpected(requested.error());
        }

        SensitiveStringGuard requestedGuard{*requested};

        ClearSensitiveString(passphrase);

        passphrase = std::move(*requested);

        return {};
      };

      if (encryptedKey ||
          !mSite.authentication.passphraseCredentialId.empty())
      {
        if (auto requested = requestPassphrase(false); !requested)
        {
          return std::unexpected(requested.error());
        }
      }

      std::string publicKey;

      if (!mSite.authentication.publicKeyFile.empty())
      {
        auto contents = ReadLocalFile(mSite.authentication.publicKeyFile, "the public key file");
        if (!contents)
        {
          return std::unexpected(contents.error());
        }

        publicKey = std::move(*contents);
      }

      const auto authenticateWithPassphrase = [&]
      {
        return CallInt(
            [&]
            {
              return libssh2_userauth_publickey_frommemory(
                  mSession, mSite.username.c_str(), mSite.username.size(),
                  publicKey.empty() ? nullptr : publicKey.data(), publicKey.size(),
                  privateKey->data(), privateKey->size(),
                  passphrase.empty() ? nullptr : passphrase.c_str());
            },
            "Private-key authentication", stopToken);
      };

      auto result = authenticateWithPassphrase();

      const auto isPassphraseOrFormatFailure = [](const RemoteError &error)
      {
        return error.nativeCode == LIBSSH2_ERROR_FILE ||
               error.nativeCode == LIBSSH2_ERROR_KEYFILE_AUTH_FAILED;
      };

      if (!result && encryptedKey &&
          isPassphraseOrFormatFailure(result.error()))
      {
        if (auto requested = requestPassphrase(true); !requested)
        {
          return std::unexpected(requested.error());
        }

        result = authenticateWithPassphrase();
      }

      if (!result)
      {
        if (encryptedKey && isPassphraseOrFormatFailure(result.error()))
        {
          return std::unexpected(SftpError(
              RemoteErrorCode::AuthenticationFailed,
              "Private-key authentication failed. Enter the correct key "
              "passphrase, or use a supported OpenSSH or PEM private-key format.",
              result.error().nativeCode));
        }

        return std::unexpected(result.error());
      }

      return {};
    }

    Result<void> SftpSession::AuthenticateAgent(std::stop_token stopToken)
    {
      const std::unique_ptr<LIBSSH2_AGENT, decltype(&libssh2_agent_free)> agent{
          libssh2_agent_init(mSession), libssh2_agent_free};
      if (!agent)
      {
        return std::unexpected(LastError("Initializing the SSH agent",
                                         LIBSSH2_ERROR_AGENT_PROTOCOL));
      }

      const auto backendName = SshAgentBackendName();
      if (auto connected = CallInt([&]
                                   { return libssh2_agent_connect(agent.get()); },
                                   "Connecting to the SSH agent", stopToken);
          !connected)
      {
        return std::unexpected(connected.error());
      }

      if (auto listed = CallInt([&]
                                { return libssh2_agent_list_identities(agent.get()); },
                                "Listing SSH agent identities", stopToken);
          !listed)
      {
        libssh2_agent_disconnect(agent.get());

        return std::unexpected(listed.error());
      }

      libssh2_agent_publickey *previous{};

      RemoteError lastAuthentication = SftpError(RemoteErrorCode::AuthenticationFailed,
                                                 "No SSH agent identity was accepted");

      while (!stopToken.stop_requested())
      {
        libssh2_agent_publickey *identity{};

        auto next = CallInt(
            [&]
            { return libssh2_agent_get_identity(agent.get(), &identity, previous); },
            "Reading an SSH agent identity", stopToken);
        if (!next)
        {
          libssh2_agent_disconnect(agent.get());

          return std::unexpected(next.error());
        }

        if (*next == 1)
        {
          break;
        }

        auto authenticated = CallInt(
            [&]
            { return libssh2_agent_userauth(agent.get(), mSite.username.c_str(), identity); },
            "SSH agent authentication", stopToken);
        if (authenticated)
        {
          std::string message{"SSH agent authentication succeeded via "};
          message.append(backendName);

          if (identity->blob && identity->blob_len != 0U)
          {
            message.append(" with key ");
            message.append(Sha256Fingerprint(
                reinterpret_cast<const std::byte *>(identity->blob),
                identity->blob_len));
          }

          Log(DiagnosticLevel::Information, message);

          libssh2_agent_disconnect(agent.get());

          return {};
        }

        lastAuthentication = authenticated.error();

        previous = identity;
      }

      libssh2_agent_disconnect(agent.get());

      if (stopToken.stop_requested())
      {
        return std::unexpected(SftpError(RemoteErrorCode::Cancelled,
                                         "SSH agent authentication was cancelled"));
      }

      return std::unexpected(lastAuthentication);
    }

    void SftpSession::KeyboardCallback(
        const char *, int, const char *instruction, int instructionLength, int promptCount,
        const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts,
        LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses, void **abstract)
    {
      if (!abstract || !*abstract)
      {
        return;
      }

      auto &self = *static_cast<SftpSession *>(*abstract);

      if (promptCount > 0)
      {
        self.mKeyboardPromptReceived = true;
      }

      if (instruction && instructionLength > 0)
      {
        self.Log(DiagnosticLevel::Information,
                 std::string_view{instruction, static_cast<std::size_t>(instructionLength)});
      }

      if (!self.mCallbacks.requestCredential)
      {
        self.mKeyboardError = SftpError(RemoteErrorCode::CredentialUnavailable,
                                        "No keyboard-interactive provider is available");
        return;
      }

      for (int index = 0; index < promptCount; ++index)
      {
        const auto prompt = std::string{
            reinterpret_cast<const char *>(prompts[index].text), prompts[index].length};

        auto answer = self.mCallbacks.requestCredential(
            CredentialRequest{.kind = CredentialKind::KeyboardInteractive,
                              .siteId = self.mSite.id,
                              .username = self.mSite.username,
                              .prompt = prompt,
                              .echo = prompts[index].echo != 0},
            self.mKeyboardStopToken);
        if (!answer)
        {
          self.mKeyboardError = answer.error();

          return;
        }

        auto *allocated = static_cast<char *>(std::malloc(answer->size() + 1U));
        if (!allocated)
        {
          self.mKeyboardError = SftpError(RemoteErrorCode::ProtocolError,
                                          "Could not allocate an authentication response");

          return;
        }

        std::memcpy(allocated, answer->data(), answer->size());
        allocated[answer->size()] = '\0';

        responses[index].text = allocated;
        responses[index].length = static_cast<unsigned int>(answer->size());

        std::fill(answer->begin(), answer->end(), '\0');
      }
    }

    Result<void> SftpSession::AuthenticateKeyboardInteractive(std::stop_token stopToken)
    {
      mKeyboardError.reset();
      mKeyboardPromptReceived = false;
      mKeyboardStopToken = stopToken;

      auto result = CallInt(
          [&]
          {
            return libssh2_userauth_keyboard_interactive_ex(
                mSession, mSite.username.c_str(),
                static_cast<unsigned int>(mSite.username.size()), KeyboardCallback);
          },
          "Keyboard-interactive authentication", stopToken);

      mKeyboardStopToken = {};

      if (mKeyboardError)
      {
        return std::unexpected(std::move(*mKeyboardError));
      }

      if (!result)
      {
        if (!mKeyboardPromptReceived &&
            result.error().code == RemoteErrorCode::AuthenticationFailed)
        {
          return std::unexpected(SftpError(
              RemoteErrorCode::AuthenticationFailed,
              "The server rejected keyboard-interactive authentication "
              "without requesting any credential input.",
              result.error().nativeCode));
        }

        return std::unexpected(result.error());
      }

      return {};
    }

    Result<std::vector<RemoteEntry>> SftpSession::List(const RemotePath &path,
                                                       std::stop_token stopToken)
    {
      if (!mConnected || !mSftp)
      {
        return std::unexpected(SftpError(RemoteErrorCode::NotConnected,
                                         "The SFTP session is not connected"));
      }

      if (!SafeSftpPath(path))
      {
        return std::unexpected(SftpError(RemoteErrorCode::InvalidArgument,
                                         "Invalid remote directory path"));
      }

      auto opened = CallPointer<LIBSSH2_SFTP_HANDLE>(
          [&]
          {
            return libssh2_sftp_open_ex(mSftp, path.Bytes().data(),
                                        static_cast<unsigned int>(path.Bytes().size()),
                                        0, 0, LIBSSH2_SFTP_OPENDIR);
          },
          "Opening the remote directory", stopToken);

      if (!opened)
      {
        return std::unexpected(opened.error());
      }

      auto *handle = *opened;

      std::unique_ptr<LIBSSH2_SFTP_HANDLE, decltype(&libssh2_sftp_close_handle)> guard{
          handle, libssh2_sftp_close_handle};

      std::vector<RemoteEntry> entries;
      std::array<char, 65536> nameBuffer{};

      // libssh2 accepts SFTP packets up to 256 KiB. The long-name field can be
      // much larger than the filename itself, and libssh2 rejects the whole
      // READDIR entry when the supplied destination is too small. Keep this on
      // the heap and leave one byte for libssh2's terminating NUL.
      std::vector<char> longEntryBuffer((256U * 1024U) + 1U, '\0');

      while (true)
      {
        LIBSSH2_SFTP_ATTRIBUTES attributes{};

        longEntryBuffer.front() = '\0';

        auto read = CallInt(
            [&]
            {
              return libssh2_sftp_readdir_ex(handle, nameBuffer.data(), nameBuffer.size(),
                                             longEntryBuffer.data(), longEntryBuffer.size(),
                                             &attributes);
            },
            "Reading the remote directory", stopToken);
        if (!read)
        {
          return std::unexpected(read.error());
        }
        if (*read == 0)
        {
          break;
        }

        std::string name{nameBuffer.data(), static_cast<std::size_t>(*read)};
        if (!IsValidRemoteChildName(name))
        {
          continue;
        }

        RemotePath remoteName{std::move(name)};

        entries.push_back(MakeEntry(path.Joined(remoteName), remoteName, attributes,
                                    std::string_view{longEntryBuffer.data()}));
      }

      if (auto closed = CloseHandle(handle, stopToken, false); !closed)
      {
        return std::unexpected(closed.error());
      }

      guard.release();

      return entries;
    }

    Result<RemoteEntry> SftpSession::Stat(const RemotePath &path,
                                          std::stop_token stopToken)
    {
      auto attributes = ReadAttributes(path, stopToken);

      if (!attributes)
      {
        return std::unexpected(attributes.error());
      }

      return MakeEntry(path, path.IsRoot() ? RemotePath{"/"} : path.Filename(), *attributes);
    }

    Result<LIBSSH2_SFTP_ATTRIBUTES> SftpSession::ReadAttributes(
        const RemotePath &path, std::stop_token stopToken)
    {
      if (!mConnected || !mSftp)
      {
        return std::unexpected(SftpError(RemoteErrorCode::NotConnected,
                                         "The SFTP session is not connected"));
      }

      if (!SafeSftpPath(path))
      {
        return std::unexpected(SftpError(RemoteErrorCode::InvalidArgument,
                                         "Invalid remote path"));
      }

      LIBSSH2_SFTP_ATTRIBUTES attributes{};

      auto result = CallInt(
          [&]
          {
            return libssh2_sftp_stat_ex(mSftp, path.Bytes().data(),
                                        static_cast<unsigned int>(path.Bytes().size()),
                                        LIBSSH2_SFTP_LSTAT, &attributes);
          },
          "Reading remote file information", stopToken);

      if (!result)
      {
        return std::unexpected(result.error());
      }

      return attributes;
    }

    Result<void> SftpSession::SetPermissions(const RemotePath &path,
                                             const std::uint32_t permissions,
                                             std::stop_token stopToken)
    {
      if (!mConnected || !mSftp)
      {
        return std::unexpected(SftpError(RemoteErrorCode::NotConnected,
                                         "The SFTP session is not connected"));
      }

      if ((permissions & ~07777U) != 0U)
      {
        return std::unexpected(SftpError(RemoteErrorCode::InvalidArgument,
                                         "Remote permissions must be an octal mode from 0000 to 7777"));
      }

      if (!SafeSftpPath(path) || path.IsRoot())
      {
        return std::unexpected(SftpError(RemoteErrorCode::InvalidArgument,
                                         "Invalid remote permissions path"));
      }

      auto info = Stat(path, stopToken);
      if (!info)
      {
        return std::unexpected(info.error());
      }
      if (info->kind != RemoteEntryKind::File &&
          info->kind != RemoteEntryKind::Directory)
      {
        return std::unexpected(SftpError(
            RemoteErrorCode::Unsupported,
            "Changing permissions on remote links or special files is not supported"));
      }

      LIBSSH2_SFTP_ATTRIBUTES attributes{};
      attributes.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
      attributes.permissions = static_cast<unsigned long>(permissions);

      auto changed = CallInt(
          [&]
          {
            return libssh2_sftp_stat_ex(
                mSftp, path.Bytes().data(),
                static_cast<unsigned int>(path.Bytes().size()),
                LIBSSH2_SFTP_SETSTAT, &attributes);
          },
          "Changing remote permissions", stopToken, true);
      if (!changed)
      {
        return std::unexpected(changed.error());
      }

      return {};
    }

    Result<void> SftpSession::Mkdir(const RemotePath &path,
                                    bool recursive,
                                    std::stop_token stopToken)
    {
      if (!mConnected || !mSftp)
      {
        return std::unexpected(SftpError(RemoteErrorCode::NotConnected,
                                         "The SFTP session is not connected"));
      }

      if (!SafeSftpPath(path))
      {
        return std::unexpected(SftpError(RemoteErrorCode::InvalidArgument,
                                         "Invalid remote directory path"));
      }

      if (path.IsRoot())
      {
        return {};
      }

      auto makeOne = [&](std::string_view directory) -> Result<void>
      {
        auto made = CallInt(
            [&]
            {
              return libssh2_sftp_mkdir_ex(mSftp, directory.data(),
                                           static_cast<unsigned int>(directory.size()), 0755);
            },
            "Creating a remote directory", stopToken, true);
        if (made)
        {
          return {};
        }
        if (made.error().code != RemoteErrorCode::AlreadyExists &&
            made.error().code != RemoteErrorCode::RemoteIo)
        {
          return std::unexpected(made.error());
        }

        auto existing = Stat(RemotePath{std::string{directory}}, stopToken);
        if (existing && existing->kind == RemoteEntryKind::Directory)
        {
          return {};
        }

        return std::unexpected(made.error());
      };

      if (!recursive)
      {
        return makeOne(path.Bytes());
      }

      std::string current = path.IsAbsolute() ? "/" : "";
      std::string_view remaining{path.Bytes()};
      if (!remaining.empty() && remaining.front() == '/')
      {
        remaining.remove_prefix(1);
      }

      while (!remaining.empty())
      {
        const auto slash = remaining.find('/');
        const auto component = remaining.substr(0, slash);

        if (!IsValidRemoteChildName(component))
        {
          return std::unexpected(SftpError(RemoteErrorCode::InvalidArgument,
                                           "Invalid remote directory component"));
        }

        if (current.size() > 1 && current.back() != '/')
        {
          current.push_back('/');
        }

        current.append(component);

        if (auto made = makeOne(current); !made)
        {
          return made;
        }

        if (slash == std::string_view::npos)
        {
          break;
        }

        remaining.remove_prefix(slash + 1U);
      }

      return {};
    }

    Result<void> SftpSession::CreateRemoteFile(const RemotePath &path,
                                               const bool overwrite,
                                               std::stop_token stopToken)
    {
      if (!mConnected || !mSftp)
      {
        return std::unexpected(SftpError(RemoteErrorCode::NotConnected,
                                         "The SFTP session is not connected"));
      }

      if (!SafeSftpPath(path) || path.IsRoot() || path.Bytes().back() == '/')
      {
        return std::unexpected(SftpError(RemoteErrorCode::InvalidArgument,
                                         "Invalid remote file path"));
      }

      constexpr unsigned long flags =
          LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL;

      const auto openExclusive = [&](const RemotePath &target)
      {
        return CallPointer<LIBSSH2_SFTP_HANDLE>(
            [&]
            {
              return libssh2_sftp_open_ex(
                  mSftp, target.Bytes().data(),
                  static_cast<unsigned int>(target.Bytes().size()), flags, 0644,
                  LIBSSH2_SFTP_OPENFILE);
            },
            "Creating a remote file", stopToken, true);
      };

      const auto closeCreatedFile = [&](LIBSSH2_SFTP_HANDLE *handle) -> Result<void>
      {
        std::unique_ptr<LIBSSH2_SFTP_HANDLE, decltype(&libssh2_sftp_close_handle)> guard{
            handle, libssh2_sftp_close_handle};

        if (auto closed = CloseHandle(handle, stopToken, true); !closed)
        {
          auto error = closed.error();

          // Obtaining the handle means the exclusive open already created a
          // file, even if its final handle state cannot be confirmed.
          error.operationMayHaveSucceeded = true;

          return std::unexpected(std::move(error));
        }

        guard.release();

        return {};
      };

      // Exclusive creation at the requested path guarantees that the default
      // action cannot truncate a file or follow a destination symlink.
      if (!overwrite)
      {
        auto existing = Stat(path, stopToken);
        if (existing)
        {
          return std::unexpected(SftpError(RemoteErrorCode::AlreadyExists,
                                           "The remote destination already exists"));
        }
        if (existing.error().code != RemoteErrorCode::NotFound)
        {
          return std::unexpected(existing.error());
        }

        auto opened = openExclusive(path);
        if (!opened)
        {
          return std::unexpected(opened.error());
        }

        return closeCreatedFile(*opened);
      }

      // Overwrite through a unique sibling and rename it into place. Opening the
      // destination with TRUNC would follow a symlink on many SFTP servers.
      std::optional<RemotePath> temporaryPath;
      LIBSSH2_SFTP_HANDLE *temporaryHandle{};

      for (unsigned int attempt = 0; attempt < 32; ++attempt)
      {
        const auto suffix = ".havremote." + GenerateId() + ".tmp";

        RemotePath candidate{path.Bytes() + suffix,
                             path.DisplayUtf8() + suffix};

        auto opened = openExclusive(candidate);
        if (opened)
        {
          temporaryPath = std::move(candidate);

          temporaryHandle = *opened;

          break;
        }

        if (opened.error().code != RemoteErrorCode::AlreadyExists)
        {
          return std::unexpected(opened.error());
        }
      }

      if (!temporaryPath)
      {
        return std::unexpected(SftpError(
            RemoteErrorCode::Conflict,
            "Could not create a unique temporary remote file"));
      }

      if (auto closed = closeCreatedFile(temporaryHandle); !closed)
      {
        return closed;
      }

      const auto cleanupTemporary = [this, &temporaryPath]
      {
        auto removed = CallInt(
            [&]
            {
              return libssh2_sftp_unlink_ex(
                  mSftp, temporaryPath->Bytes().data(),
                  static_cast<unsigned int>(temporaryPath->Bytes().size()));
            },
            "Removing a temporary remote file", std::stop_token{}, true);

        // A missing source can mean an uncertain rename actually completed
        if (!removed && removed.error().code != RemoteErrorCode::NotFound)
        {
          Log(DiagnosticLevel::Warning,
              "Could not remove a temporary remote file after create-file finalization failed");
        }
      };

      if (stopToken.stop_requested())
      {
        cleanupTemporary();

        return std::unexpected(SftpError(RemoteErrorCode::Cancelled,
                                         "The create-file operation was cancelled"));
      }

      auto finalized = Rename(*temporaryPath, path, true, stopToken);
      if (!finalized)
      {
        cleanupTemporary();
      }

      return finalized;
    }

    Result<void> SftpSession::Rename(const RemotePath &source,
                                     const RemotePath &destination,
                                     bool overwrite,
                                     std::stop_token stopToken)
    {
      if (!mConnected || !mSftp)
      {
        return std::unexpected(SftpError(RemoteErrorCode::NotConnected,
                                         "The SFTP session is not connected"));
      }

      if (!SafeSftpPath(source) || !SafeSftpPath(destination) || source.IsRoot() ||
          destination.IsRoot())
      {
        return std::unexpected(SftpError(RemoteErrorCode::InvalidArgument,
                                         "Invalid remote rename path"));
      }

      if (!overwrite)
      {
        auto existing = Stat(destination, stopToken);
        if (existing)
        {
          return std::unexpected(SftpError(RemoteErrorCode::AlreadyExists,
                                           "The remote destination already exists"));
        }
        if (existing.error().code != RemoteErrorCode::NotFound)
        {
          return std::unexpected(existing.error());
        }
      }

      if (overwrite)
      {
        auto replaced = ReplaceWithPosixRename(source, destination, stopToken);
        if (replaced)
        {
          return {};
        }
        if (replaced.error().code != RemoteErrorCode::Unsupported ||
            replaced.error().operationMayHaveSucceeded)
        {
          return std::unexpected(replaced.error());
        }

        // libssh2 negotiates SFTP v3 where the standard rename packet cannot
        // carry overwrite flags. Some servers nevertheless implement overwrite
        // semantics for that packet and libssh2 explicitly recommends it as
        // the fallback when posix-rename@openssh.com is unavailable.
        Log(DiagnosticLevel::Debug,
            "The SFTP server does not advertise POSIX rename. Trying its native rename semantics");
      }

      long flags = LIBSSH2_SFTP_RENAME_ATOMIC | LIBSSH2_SFTP_RENAME_NATIVE;

      if (overwrite)
      {
        flags |= LIBSSH2_SFTP_RENAME_OVERWRITE;
      }

      auto renamed = CallInt(
          [&]
          {
            return libssh2_sftp_rename_ex(
                mSftp, source.Bytes().data(), static_cast<unsigned int>(source.Bytes().size()),
                destination.Bytes().data(),
                static_cast<unsigned int>(destination.Bytes().size()), flags);
          },
          "Renaming a remote path", stopToken, true);
      if (!renamed)
      {
        return std::unexpected(renamed.error());
      }

      return {};
    }

    Result<void> SftpSession::ReplaceWithPosixRename(
        const RemotePath &source,
        const RemotePath &destination,
        std::stop_token stopToken)
    {
      bool requestMayHaveBeenSent = false;

      while (true)
      {
        if (stopToken.stop_requested())
        {
          return std::unexpected(SftpError(
              RemoteErrorCode::Cancelled,
              "Replacing a remote path was cancelled", 0, false,
              requestMayHaveBeenSent));
        }

        const auto result = libssh2_sftp_posix_rename_ex(
            mSftp, source.Bytes().data(), source.Bytes().size(),
            destination.Bytes().data(), destination.Bytes().size());

        // Unlike the rest of libssh2's SFTP API, an unavailable POSIX rename
        // extension is reported as the positive SFTP status value 8. It must
        // not be mistaken for a successful operation.
        if (result == LIBSSH2_FX_OP_UNSUPPORTED && !requestMayHaveBeenSent)
        {
          return std::unexpected(SftpError(
              RemoteErrorCode::Unsupported,
              "The SFTP server does not support POSIX rename",
              LIBSSH2_FX_OP_UNSUPPORTED));
        }

        requestMayHaveBeenSent = true;

        if (result == LIBSSH2_ERROR_EAGAIN)
        {
          if (auto waited = WaitSocket(stopToken, true); !waited)
          {
            return std::unexpected(waited.error());
          }

          continue;
        }

        if (result < 0)
        {
          return std::unexpected(LastError("Replacing a remote path", result, true));
        }

        if (result != 0)
        {
          return std::unexpected(SftpError(
              RemoteErrorCode::ProtocolError,
              "Replacing a remote path returned an unexpected SFTP status",
              result, false, true));
        }

        return {};
      }
    }

    Result<void> SftpSession::Remove(const RemotePath &path,
                                     bool recursive,
                                     std::stop_token stopToken)
    {
      if (!mConnected || !mSftp)
      {
        return std::unexpected(SftpError(RemoteErrorCode::NotConnected,
                                         "The SFTP session is not connected"));
      }

      if (!SafeSftpPath(path) || path.IsRoot())
      {
        return std::unexpected(SftpError(RemoteErrorCode::InvalidArgument,
                                         "Refusing to remove an invalid or root path"));
      }

      return RemoveImpl(path, recursive, stopToken);
    }

    Result<void> SftpSession::RemoveImpl(const RemotePath &path,
                                         bool recursive,
                                         std::stop_token stopToken)
    {
      auto info = Stat(path, stopToken);
      if (!info)
      {
        return std::unexpected(info.error());
      }
      if (info->kind == RemoteEntryKind::Directory)
      {
        if (recursive)
        {
          auto children = List(path, stopToken);
          if (!children)
          {
            return std::unexpected(children.error());
          }

          for (const auto &child : *children)
          {
            // list/lstat identifies links, so this never traverses a symlink
            if (auto removed = RemoveImpl(child.path, true, stopToken); !removed)
            {
              return removed;
            }
          }
        }

        auto removed = CallInt(
            [&]
            {
              return libssh2_sftp_rmdir_ex(mSftp, path.Bytes().data(),
                                           static_cast<unsigned int>(path.Bytes().size()));
            },
            "Removing a remote directory", stopToken, true);
        if (!removed)
        {
          return std::unexpected(removed.error());
        }

        return {};
      }

      auto removed = CallInt(
          [&]
          {
            return libssh2_sftp_unlink_ex(mSftp, path.Bytes().data(),
                                          static_cast<unsigned int>(path.Bytes().size()));
          },
          "Removing a remote file", stopToken, true);
      if (!removed)
      {
        return std::unexpected(removed.error());
      }

      return {};
    }

    TransferControl SftpSession::ReportProgress(const ProgressCallback &callback,
                                                std::string_view jobId,
                                                std::uint64_t transferred,
                                                std::optional<std::uint64_t> total,
                                                Clock::time_point started,
                                                const std::uint64_t resumeOffset) const
    {
      if (!callback)
      {
        return TransferControl::Continue;
      }

      return callback(TransferProgress{.jobId = std::string{jobId},
                                       .bytesTransferred = transferred,
                                       .totalBytes = total,
                                       .activeBytesTransferred =
                                           transferred >= resumeOffset
                                               ? transferred - resumeOffset
                                               : 0U,
                                       .elapsed = Clock::now() - started});
    }

    Result<void> SftpSession::Upload(const std::filesystem::path &localPath,
                                     const RemotePath &remotePath,
                                     const TransferOptions &options,
                                     const ProgressCallback &progress,
                                     std::stop_token stopToken)
    {
      if (!mConnected || !mSftp)
      {
        return std::unexpected(SftpError(RemoteErrorCode::NotConnected,
                                         "The SFTP session is not connected"));
      }

      if (!SafeSftpPath(remotePath) || remotePath.IsRoot())
      {
        return std::unexpected(SftpError(RemoteErrorCode::InvalidArgument,
                                         "Invalid remote upload path"));
      }

      std::error_code filesystemError;

      const auto total = std::filesystem::file_size(localPath, filesystemError);

      if (filesystemError)
      {
        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                         "Could not read the local file: " +
                                             filesystemError.message(),
                                         filesystemError.value()));
      }

      if (options.resumeOffset > total)
      {
        return std::unexpected(SftpError(RemoteErrorCode::InvalidArgument,
                                         "The upload resume offset exceeds the file size"));
      }

      RemotePath target = remotePath;

      if (options.useTemporaryName)
      {
        target = options.temporaryRemotePath.value_or(
            RemotePath{remotePath.Bytes() + ".havremote.part",
                       remotePath.DisplayUtf8() + ".havremote.part"});

        if (!SafeSftpPath(target) || target.IsRoot())
        {
          return std::unexpected(SftpError(RemoteErrorCode::InvalidArgument,
                                           "Invalid remote temporary upload path"));
        }

        if (target.Bytes() == remotePath.Bytes())
        {
          return std::unexpected(SftpError(
              RemoteErrorCode::InvalidArgument,
              "The remote temporary upload path must differ from its destination"));
        }
      }

      if (options.resumeOffset > 0)
      {
        auto partial = Stat(target, stopToken);

        if (!partial || partial->kind != RemoteEntryKind::File ||
            partial->size != options.resumeOffset)
        {
          return std::unexpected(SftpError(RemoteErrorCode::Conflict,
                                           "The remote partial file no longer matches its resume metadata"));
        }
      }

      unsigned long flags = LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT;

      if (options.resumeOffset == 0)
      {
        flags |= LIBSSH2_FXF_TRUNC;
      }

      auto opened = CallPointer<LIBSSH2_SFTP_HANDLE>(
          [&]
          {
            return libssh2_sftp_open_ex(mSftp, target.Bytes().data(),
                                        static_cast<unsigned int>(target.Bytes().size()),
                                        flags, 0644, LIBSSH2_SFTP_OPENFILE);
          },
          "Opening the remote upload file", stopToken, true);
      if (!opened)
      {
        return std::unexpected(opened.error());
      }

      auto *handle = *opened;

      std::unique_ptr<LIBSSH2_SFTP_HANDLE, decltype(&libssh2_sftp_close_handle)> guard{
          handle, libssh2_sftp_close_handle};

      auto stopTransfer = [&](RemoteError error) -> Result<void>
      {
        if (!stopToken.stop_requested())
        {
          if (auto closed = CloseHandle(handle, stopToken, true); closed)
          {
            guard.release();
          }
          else if (closed.error().operationMayHaveSucceeded)
          {
            return std::unexpected(closed.error());
          }
        }

        return std::unexpected(std::move(error));
      };

      if (options.resumeOffset > 0)
      {
        libssh2_sftp_seek64(handle, options.resumeOffset);
      }

      std::ifstream input{localPath, std::ios::binary};
      if (!input)
      {
        return std::unexpected(SftpError(RemoteErrorCode::LocalIo, "Could not open the local upload file"));
      }

      input.seekg(static_cast<std::streamoff>(options.resumeOffset));

      if (!input)
      {
        return std::unexpected(SftpError(RemoteErrorCode::LocalIo, "Could not seek the local upload file"));
      }

      const auto started = Clock::now();

      std::uint64_t transferred = options.resumeOffset;

      if (const auto control = ReportProgress(
              progress, options.jobId, transferred, total, started,
              options.resumeOffset);
          control != TransferControl::Continue)
      {
        return stopTransfer(SftpError(control == TransferControl::Pause
                                          ? RemoteErrorCode::Paused
                                          : RemoteErrorCode::Cancelled,
                                      "The upload was stopped before it started", 0, true));
      }

      std::array<char, 64 * 1024> buffer{};

      while (input && transferred < total)
      {
        if (stopToken.stop_requested())
        {
          return std::unexpected(SftpError(RemoteErrorCode::Cancelled,
                                           "The upload was cancelled"));
        }

        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));

        const auto count = static_cast<std::size_t>(input.gcount());

        if (count == 0)
        {
          break;
        }

        std::size_t offset{};

        while (offset < count)
        {
          auto written = CallSize(
              [&]
              { return libssh2_sftp_write(handle, buffer.data() + offset, count - offset); },
              "Writing the remote upload file", stopToken, true);

          if (!written)
          {
            return std::unexpected(written.error());
          }

          if (*written == 0)
          {
            return std::unexpected(SftpError(RemoteErrorCode::RemoteIo,
                                             "The SFTP server stopped accepting upload data"));
          }

          offset += static_cast<std::size_t>(*written);
          transferred += static_cast<std::uint64_t>(*written);

          const auto control = ReportProgress(
              progress, options.jobId, transferred, total, started,
              options.resumeOffset);

          if (control == TransferControl::Pause)
          {
            return stopTransfer(SftpError(RemoteErrorCode::Paused,
                                          "The upload was paused", 0, true));
          }

          if (control == TransferControl::Cancel)
          {
            return stopTransfer(SftpError(RemoteErrorCode::Cancelled,
                                          "The upload was cancelled"));
          }
        }
      }

      if (!input.eof() && input.fail())
      {
        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                         "Could not read the local upload file"));
      }

      if (transferred != total)
      {
        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                         "The local upload file changed while being read"));
      }

      auto synced = CallInt([&]
                            { return libssh2_sftp_fsync(handle); },
                            "Flushing the remote upload file", stopToken, true);

      if (!synced && synced.error().code != RemoteErrorCode::Unsupported)
      {
        Log(DiagnosticLevel::Warning, synced.error().message);

        if (synced.error().operationMayHaveSucceeded)
        {
          return std::unexpected(synced.error());
        }
      }

      if (auto closed = CloseHandle(handle, stopToken, true); !closed)
      {
        return std::unexpected(closed.error());
      }

      guard.release();

      if (!options.useTemporaryName)
      {
        return {};
      }

      if (stopToken.stop_requested())
      {
        return std::unexpected(SftpError(RemoteErrorCode::Cancelled,
                                         "The upload was cancelled before finalization"));
      }

      if (const auto control = ReportProgress(
              progress, options.jobId, transferred, total, started,
              options.resumeOffset);
          control != TransferControl::Continue)
      {
        return std::unexpected(SftpError(
            control == TransferControl::Pause ? RemoteErrorCode::Paused
                                              : RemoteErrorCode::Cancelled,
            control == TransferControl::Pause
                ? "The upload was paused before finalization"
                : "The upload was cancelled before finalization"));
      }

      bool overwrite = options.overwrite;

      if (options.beforeFinalize)
      {
        auto decision = options.beforeFinalize(overwrite);
        if (!decision)
        {
          return std::unexpected(decision.error());
        }

        overwrite = *decision;
      }

      return Rename(target, remotePath, overwrite, stopToken);
    }

    Result<void> SftpSession::Download(const RemotePath &remotePath,
                                       const std::filesystem::path &localPath,
                                       const TransferOptions &options,
                                       const ProgressCallback &progress,
                                       std::stop_token stopToken)
    {
      if (!mConnected || !mSftp)
      {
        return std::unexpected(SftpError(RemoteErrorCode::NotConnected,
                                         "The SFTP session is not connected"));
      }

      if (!SafeSftpPath(remotePath) || remotePath.IsRoot())
      {
        return std::unexpected(SftpError(RemoteErrorCode::InvalidArgument,
                                         "Invalid remote download path"));
      }

      auto attributes = ReadAttributes(remotePath, stopToken);
      if (!attributes)
      {
        return std::unexpected(attributes.error());
      }

      const auto kind = EntryKind(*attributes);

      const std::optional<std::uint64_t> total =
          (attributes->flags & LIBSSH2_SFTP_ATTR_SIZE) != 0
              ? std::optional<std::uint64_t>{attributes->filesize}
              : std::nullopt;

      if (kind == RemoteEntryKind::Directory)
      {
        return std::unexpected(SftpError(RemoteErrorCode::IsDirectory,
                                         "The remote download source is a directory"));
      }

      if (kind == RemoteEntryKind::Symlink)
      {
        return std::unexpected(SftpError(RemoteErrorCode::Unsupported,
                                         "Downloading through a symbolic link is disabled"));
      }

      if (options.resumeOffset > 0 && !total)
      {
        return std::unexpected(SftpError(RemoteErrorCode::Conflict,
                                         "Cannot resume a download without a known remote file size"));
      }

      if (total && options.resumeOffset > *total)
      {
        return std::unexpected(SftpError(RemoteErrorCode::Conflict,
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
          return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                           "Could not create the download directory: " +
                                               filesystemError.message(),
                                           filesystemError.value()));
        }
      }

      if (options.resumeOffset > 0)
      {
        const auto partialSize = std::filesystem::file_size(partPath, filesystemError);

        if (filesystemError || partialSize != options.resumeOffset)
        {
          return std::unexpected(SftpError(RemoteErrorCode::Conflict,
                                           "The local partial file no longer matches its resume metadata"));
        }
      }

      auto opened = CallPointer<LIBSSH2_SFTP_HANDLE>(
          [&]
          {
            return libssh2_sftp_open_ex(mSftp, remotePath.Bytes().data(),
                                        static_cast<unsigned int>(remotePath.Bytes().size()),
                                        LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
          },
          "Opening the remote download file", stopToken);
      if (!opened)
      {
        return std::unexpected(opened.error());
      }

      auto *handle = *opened;

      std::unique_ptr<LIBSSH2_SFTP_HANDLE, decltype(&libssh2_sftp_close_handle)> guard{
          handle, libssh2_sftp_close_handle};

      auto stopTransfer = [&](RemoteError error) -> Result<void>
      {
        if (!stopToken.stop_requested())
        {
          if (auto closed = CloseHandle(handle, stopToken, false); closed)
          {
            guard.release();
          }
        }

        return std::unexpected(std::move(error));
      };

      if (options.resumeOffset > 0)
      {
        libssh2_sftp_seek64(handle, options.resumeOffset);
      }

      const auto mode = std::ios::binary |
                        (options.resumeOffset > 0 ? std::ios::app : std::ios::trunc);

      std::ofstream output{partPath, mode};

      if (!output)
      {
        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                         "Could not open the partial download file"));
      }

      const auto started = Clock::now();

      std::uint64_t transferred = options.resumeOffset;

      if (const auto control = ReportProgress(
              progress, options.jobId, transferred, total, started,
              options.resumeOffset);
          control != TransferControl::Continue)
      {
        return stopTransfer(SftpError(control == TransferControl::Pause
                                          ? RemoteErrorCode::Paused
                                          : RemoteErrorCode::Cancelled,
                                      "The download was stopped before it started"));
      }

      std::array<char, 64 * 1024> buffer{};

      // Bound the bytes requested by havRemote and avoid an extra read at the
      // known end. libssh2 may pipeline speculative wire READs beyond this
      // buffer and handles their FX_EOF replies internally.
      while (!total || transferred < *total)
      {
        const auto remaining = total ? *total - transferred : buffer.size();
        const auto readLength = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), remaining));

        auto read = CallSize([&]
                             { return libssh2_sftp_read(handle, buffer.data(), readLength); },
                             "Reading the remote download file", stopToken);
        if (!read)
        {
          return stopTransfer(read.error());
        }

        // libssh2 consumes FX_EOF as a successful zero-byte read and discards
        // outstanding speculative replies. Never reclassify it as a failure.
        if (*read == 0)
        {
          break;
        }

        if (static_cast<std::uint64_t>(*read) > readLength)
        {
          return stopTransfer(SftpError(RemoteErrorCode::ProtocolError,
                                        "The SFTP read exceeded the requested length"));
        }

        output.write(buffer.data(), static_cast<std::streamsize>(*read));
        if (!output)
        {
          return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                           "Could not write the partial download file"));
        }

        transferred += static_cast<std::uint64_t>(*read);

        const auto control = ReportProgress(
            progress, options.jobId, transferred, total, started,
            options.resumeOffset);
        if (control == TransferControl::Pause)
        {
          return stopTransfer(SftpError(RemoteErrorCode::Paused,
                                        "The download was paused", 0, true));
        }
        if (control == TransferControl::Cancel)
        {
          return stopTransfer(SftpError(RemoteErrorCode::Cancelled,
                                        "The download was cancelled"));
        }
      }

      if (total && transferred != *total)
      {
        return stopTransfer(SftpError(RemoteErrorCode::RemoteIo,
                                      "The remote file changed during the download", 0, true));
      }

      // Recheck the opened file at the size bound to detect growth and retain
      // the partial download instead of publishing a truncated prefix.
      if (total)
      {
        LIBSSH2_SFTP_ATTRIBUTES finalAttributes{};

        auto verified = CallInt(
            [&]
            { return libssh2_sftp_fstat(handle, &finalAttributes); },
            "Verifying the remote download file", stopToken);
        if (!verified)
        {
          return stopTransfer(verified.error());
        }

        if ((finalAttributes.flags & LIBSSH2_SFTP_ATTR_SIZE) == 0 ||
            finalAttributes.filesize != transferred ||
            ((attributes->flags & finalAttributes.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) != 0 &&
             attributes->mtime != finalAttributes.mtime))
        {
          return stopTransfer(SftpError(RemoteErrorCode::RemoteIo,
                                        "The remote file changed during the download", 0, true));
        }
      }

      output.flush();

      if (!output)
      {
        return std::unexpected(SftpError(RemoteErrorCode::LocalIo,
                                         "Could not flush the partial download file"));
      }

      output.close();

      if (auto closed = CloseHandle(handle, stopToken, false); !closed)
      {
        return std::unexpected(closed.error());
      }

      guard.release();

      if (!options.useTemporaryName || partPath == localPath)
      {
        return {};
      }

      if (stopToken.stop_requested())
      {
        return std::unexpected(SftpError(RemoteErrorCode::Cancelled,
                                         "The download was cancelled before finalization"));
      }

      if (const auto control = ReportProgress(
              progress, options.jobId, transferred, total, started,
              options.resumeOffset);
          control != TransferControl::Continue)
      {
        return std::unexpected(SftpError(
            control == TransferControl::Pause ? RemoteErrorCode::Paused
                                              : RemoteErrorCode::Cancelled,
            control == TransferControl::Pause
                ? "The download was paused before finalization"
                : "The download was cancelled before finalization"));
      }

      bool overwrite = options.overwrite;

      if (options.beforeFinalize)
      {
        auto decision = options.beforeFinalize(overwrite);
        if (!decision)
        {
          return std::unexpected(decision.error());
        }

        overwrite = *decision;
      }

      if (stopToken.stop_requested())
      {
        return std::unexpected(SftpError(
            RemoteErrorCode::Cancelled,
            "The download was cancelled before finalization"));
      }

      return FinalizeLocalDownload(partPath, localPath, overwrite);
    }
  } // namespace

  namespace sftp
  {
    bool AuthenticationMethodOffered(const std::string_view methods,
                                     const std::string_view method) noexcept
    {
      if (method.empty())
      {
        return false;
      }

      std::size_t begin{};

      while (begin <= methods.size())
      {
        const auto end = methods.find(',', begin);
        const auto token = methods.substr(
            begin, end == std::string_view::npos ? methods.size() - begin
                                                 : end - begin);

        if (token == method)
        {
          return true;
        }

        if (end == std::string_view::npos)
        {
          break;
        }

        begin = end + 1U;
      }

      return false;
    }

    namespace
    {
      [[nodiscard]] bool AsciiSpace(const unsigned char character) noexcept
      {
        return character == ' ' || character == '\t' || character == '\r' ||
               character == '\n' || character == '\f' || character == '\v';
      }

      [[nodiscard]] std::optional<std::string_view> NextLongEntryField(
          std::string_view &remaining) noexcept
      {
        while (!remaining.empty() &&
               AsciiSpace(static_cast<unsigned char>(remaining.front())))
        {
          remaining.remove_prefix(1U);
        }

        if (remaining.empty())
        {
          return std::nullopt;
        }

        const auto end = std::ranges::find_if(remaining, [](const char character)
                                              { return AsciiSpace(static_cast<unsigned char>(character)); });
        const auto length = static_cast<std::size_t>(end - remaining.begin());
        const auto field = remaining.substr(0U, length);

        remaining.remove_prefix(length);

        return field;
      }

      [[nodiscard]] bool ValidLongEntryMode(std::string_view mode) noexcept
      {
        if (mode.size() == 11U)
        {
          if (mode.back() != '+' && mode.back() != '.' && mode.back() != '@')
          {
            return false;
          }

          mode.remove_suffix(1U);
        }

        if (mode.size() != 10U ||
            std::string_view{"-dlcbps"}.find(mode[0]) == std::string_view::npos)
        {
          return false;
        }

        const auto oneOf = [](const char value, const std::string_view allowed)
        {
          return allowed.find(value) != std::string_view::npos;
        };

        return oneOf(mode[1], "r-") && oneOf(mode[2], "w-") &&
               oneOf(mode[3], "xsS-") && oneOf(mode[4], "r-") &&
               oneOf(mode[5], "w-") && oneOf(mode[6], "xsS-") &&
               oneOf(mode[7], "r-") && oneOf(mode[8], "w-") &&
               oneOf(mode[9], "xtT-");
      }

      [[nodiscard]] bool ValidUtf8(std::string_view value) noexcept
      {
        for (std::size_t offset = 0; offset < value.size();)
        {
          const auto first = static_cast<unsigned char>(value[offset]);

          if (first <= 0x7fU)
          {
            if (first < 0x20U || first == 0x7fU)
            {
              return false;
            }

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
              secondMinimum = 0xa0U;
            }

            if (first == 0xedU)
            {
              secondMaximum = 0x9fU;
            }
          }
          else if (first >= 0xf0U && first <= 0xf4U)
          {
            length = 4U;

            if (first == 0xf0U)
            {
              secondMinimum = 0x90U;
            }

            if (first == 0xf4U)
            {
              secondMaximum = 0x8fU;
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

        return !value.empty() && value.size() <= 1024U && value != "?";
      }
    } // namespace

    std::optional<OwnerGroup> ParseLongEntryOwnerGroup(
        std::string_view longEntry)
    {
      auto mode = NextLongEntryField(longEntry);
      auto links = NextLongEntryField(longEntry);
      auto owner = NextLongEntryField(longEntry);
      auto group = NextLongEntryField(longEntry);
      auto sizeOrDevice = NextLongEntryField(longEntry);

      if (!mode || !links || !owner || !group || !sizeOrDevice ||
          !ValidLongEntryMode(*mode) || !ValidUtf8(*owner) ||
          !ValidUtf8(*group))
      {
        return std::nullopt;
      }

      std::uint64_t linkCount{};

      const auto [end, error] = std::from_chars(
          links->data(), links->data() + links->size(), linkCount);

      if (error != std::errc{} || end != links->data() + links->size())
      {
        return std::nullopt;
      }

      return OwnerGroup{.owner = std::string{*owner},
                        .group = std::string{*group}};
    }
  } // namespace sftp

  RemoteSessionPtr MakeSftpSession() { return std::make_unique<SftpSession>(); }
} // namespace havremote
