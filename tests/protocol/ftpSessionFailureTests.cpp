// SPDX-License-Identifier: MIT

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include "protocol/ftpSession.hpp"

#include <catch2/catch_test_macros.hpp>

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace havremote;

namespace
{
  using namespace std::chrono_literals;

  class Winsock final
  {
  public:
    Winsock()
    {
      WSADATA data{};

      if (const auto result = WSAStartup(MAKEWORD(2, 2), &data); result != 0)
      {
        throw std::runtime_error("FTP fixture WSAStartup failed: " +
                                 std::to_string(result));
      }
    }

    ~Winsock() { WSACleanup(); }
    Winsock(const Winsock &) = delete;
    Winsock &operator=(const Winsock &) = delete;
  };

  class Socket final
  {
  public:
    explicit Socket(const SOCKET value = INVALID_SOCKET) : mValue(value) {}
    ~Socket() { Reset(); }
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    Socket(Socket &&other) noexcept
        : mValue(std::exchange(other.mValue, INVALID_SOCKET)) {}
    Socket &operator=(Socket &&other) noexcept
    {
      if (this != &other)
      {
        Reset();

        mValue = std::exchange(other.mValue, INVALID_SOCKET);
      }

      return *this;
    }

    [[nodiscard]] SOCKET Get() const noexcept { return mValue; }
    void Reset() noexcept
    {
      if (mValue != INVALID_SOCKET)
      {
        closesocket(mValue);

        mValue = INVALID_SOCKET;
      }
    }

  private:
    SOCKET mValue;
  };

  [[noreturn]] void SocketFailure(const std::string_view operation)
  {
    throw std::runtime_error("FTP fixture " + std::string{operation} +
                             " failed: " + std::to_string(WSAGetLastError()));
  }

  Socket ListenOnLoopback()
  {
    Socket socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};

    if (socket.Get() == INVALID_SOCKET)
    {
      SocketFailure("socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    if (bind(socket.Get(), reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) == SOCKET_ERROR ||
        listen(socket.Get(), 4) == SOCKET_ERROR)
    {
      SocketFailure("bind/listen");
    }

    return socket;
  }

  std::uint16_t LocalPort(const Socket &socket)
  {
    sockaddr_in address{};

    int length = sizeof(address);

    if (getsockname(socket.Get(), reinterpret_cast<sockaddr *>(&address),
                    &length) == SOCKET_ERROR)
    {
      SocketFailure("getsockname");
    }

    return ntohs(address.sin_port);
  }

  void WaitForShutdownAcknowledgement(const Socket &socket,
                                      const std::stop_token stopToken)
  {
    sockaddr_in local{};
    sockaddr_in remote{};

    int localLength = sizeof(local);
    int remoteLength = sizeof(remote);

    if (getsockname(socket.Get(), reinterpret_cast<sockaddr *>(&local),
                    &localLength) == SOCKET_ERROR ||
        getpeername(socket.Get(), reinterpret_cast<sockaddr *>(&remote),
                    &remoteLength) == SOCKET_ERROR)
    {
      SocketFailure("control endpoints");
    }

    const auto deadline = std::chrono::steady_clock::now() + 2s;

    std::vector<DWORD> storage(1024);

    while (!stopToken.stop_requested() && std::chrono::steady_clock::now() < deadline)
    {
      auto *table = reinterpret_cast<MIB_TCPTABLE *>(storage.data());
      auto bytes = static_cast<ULONG>(storage.size() * sizeof(DWORD));

      const auto result = GetTcpTable(table, &bytes, FALSE);

      if (result == ERROR_INSUFFICIENT_BUFFER)
      {
        storage.resize((bytes + sizeof(DWORD) - 1) / sizeof(DWORD));

        continue;
      }

      if (result != NO_ERROR)
      {
        throw std::runtime_error("FTP fixture TCP state query failed: " +
                                 std::to_string(result));
      }

      for (DWORD index = 0; index < table->dwNumEntries; ++index)
      {
        const auto &entry = table->table[index];

        if (entry.dwLocalAddr == local.sin_addr.s_addr &&
            entry.dwRemoteAddr == remote.sin_addr.s_addr &&
            static_cast<u_short>(entry.dwLocalPort) == local.sin_port &&
            static_cast<u_short>(entry.dwRemotePort) == remote.sin_port &&
            (entry.dwState == MIB_TCP_STATE_FIN_WAIT2 ||
             entry.dwState == MIB_TCP_STATE_TIME_WAIT))
        {
          // These states confirm the peer has acknowledged the control EOF
          return;
        }
      }

      std::this_thread::yield();
    }

    throw std::runtime_error("FTP fixture did not observe acknowledgement of control EOF");
  }

  bool Readable(const Socket &socket,
                const std::stop_token stopToken,
                const std::chrono::steady_clock::time_point deadline)
  {
    while (!stopToken.stop_requested())
    {
      if (std::chrono::steady_clock::now() >= deadline)
      {
        throw std::runtime_error("FTP fixture timed out waiting for a socket");
      }

      fd_set sockets;

      FD_ZERO(&sockets);
      FD_SET(socket.Get(), &sockets);

      timeval interval{0, 25'000};

      const auto result = select(0, &sockets, nullptr, nullptr, &interval);
      if (result == SOCKET_ERROR)
      {
        SocketFailure("select");
      }

      if (result != 0)
      {
        return true;
      }
    }

    return false;
  }

  Socket AcceptClient(const Socket &listener, const std::stop_token stopToken)
  {
    if (!Readable(listener, stopToken, std::chrono::steady_clock::now() + 5s))
    {
      return Socket{};
    }

    Socket socket{accept(listener.Get(), nullptr, nullptr)};
    if (socket.Get() == INVALID_SOCKET)
    {
      SocketFailure("accept");
    }

    const DWORD sendTimeout = 2'000;

    if (setsockopt(socket.Get(), SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char *>(&sendTimeout),
                   sizeof(sendTimeout)) == SOCKET_ERROR)
    {
      SocketFailure("send timeout");
    }

    return socket;
  }

  void SendText(const Socket &socket, std::string_view text)
  {
    while (!text.empty())
    {
      const auto sent = send(socket.Get(), text.data(),
                             static_cast<int>(text.size()), 0);
      if (sent == SOCKET_ERROR || sent == 0)
      {
        SocketFailure("send");
      }

      text.remove_prefix(static_cast<std::size_t>(sent));
    }
  }

  std::optional<std::string> ReadCommand(const Socket &socket,
                                         std::string &pending,
                                         const std::stop_token stopToken)
  {
    const auto deadline = std::chrono::steady_clock::now() + 5s;

    while (!stopToken.stop_requested())
    {
      if (const auto newline = pending.find('\n'); newline != std::string::npos)
      {
        auto command = pending.substr(0, newline);

        pending.erase(0, newline + 1);

        if (!command.empty() && command.back() == '\r')
        {
          command.pop_back();
        }

        return command;
      }

      if (!Readable(socket, stopToken, deadline))
      {
        return std::nullopt;
      }

      std::array<char, 1024> buffer{};

      const auto received = recv(socket.Get(), buffer.data(),
                                 static_cast<int>(buffer.size()), 0);

      if (received == SOCKET_ERROR)
      {
        const auto error = WSAGetLastError();
        if (error == WSAECONNRESET || error == WSAECONNABORTED)
        {
          return std::nullopt;
        }

        SocketFailure("recv");
      }

      if (received == 0)
      {
        return std::nullopt;
      }

      pending.append(buffer.data(), static_cast<std::size_t>(received));
      if (pending.size() > 4096)
      {
        throw std::runtime_error("FTP fixture received an oversized command");
      }
    }

    return std::nullopt;
  }

  enum class Scenario
  {
    ReconnectAfterNlst,
    CloseDuringNextCommand,
    RejectLoginAfterNlst,
    RejectLoginAfterMlsd,
    MlsdUnsupported,
    RejectLoginAfterDeniedMkdir,
  };

  struct Transcript final
  {
    std::vector<std::string> commands;
    std::string failure;
  };

  // This deliberately small server is only a protocol regression fixture. Both
  // listeners bind to loopback on ephemeral ports. It never reaches a configured
  // site, reads credentials, or accesses the filesystem.
  class FtpFixture final
  {
  public:
    explicit FtpFixture(const Scenario scenario)
        : mListener(ListenOnLoopback()),
          mPort(LocalPort(mListener)),
          mScenario(scenario),
          mWorker([this](const std::stop_token stopToken)
                  {
                    try
                    {
                      Run(stopToken);
                    }
                    catch (const std::exception &exception)
                    {
                      std::scoped_lock lock{mMutex};
                      mTranscript.failure = exception.what();
                      mControlClosed.notify_all();
                    } }) {}

    ~FtpFixture() { Stop(); }
    FtpFixture(const FtpFixture &) = delete;
    FtpFixture &operator=(const FtpFixture &) = delete;

    [[nodiscard]] SiteProfile Site() const
    {
      SiteProfile result;
      result.id = "loopback-ftp-fixture";
      result.name = "Loopback FTP fixture";
      result.protocol = ProtocolKind::Ftp;
      result.host = "127.0.0.1";
      result.port = mPort;
      result.username = "fixture-user";
      result.authentication.kind = AuthenticationKind::Password;
      result.ftpDataConnectionMode = FtpDataConnectionMode::Passive;

      return result;
    }

    void Stop()
    {
      mWorker.request_stop();

      if (mWorker.joinable())
      {
        mWorker.join();
      }
    }

    [[nodiscard]] Transcript GetTranscript() const
    {
      std::scoped_lock lock{mMutex};
      return mTranscript;
    }

    void WaitForControlClose()
    {
      std::unique_lock lock{mMutex};

      if (!mControlClosed.wait_for(lock, 3s, [this]
                                   { return mControlCloseAcknowledged || !mTranscript.failure.empty(); }))
      {
        throw std::runtime_error("FTP fixture timed out waiting for control EOF");
      }

      if (!mTranscript.failure.empty())
      {
        throw std::runtime_error(mTranscript.failure);
      }
    }

  private:
    void CloseControl(const Socket &control, const std::stop_token stopToken)
    {
      if (shutdown(control.Get(), SD_SEND) == SOCKET_ERROR)
      {
        SocketFailure("control shutdown");
      }

      WaitForShutdownAcknowledgement(control, stopToken);
      {
        std::scoped_lock lock{mMutex};
        mControlCloseAcknowledged = true;
      }

      mControlClosed.notify_all();
    }

    void Run(const std::stop_token stopToken)
    {
      bool rejectLogin{};
      bool closedAfterNlst{};

      while (!stopToken.stop_requested())
      {
        auto control = AcceptClient(mListener, stopToken);

        if (control.Get() == INVALID_SOCKET)
        {
          return;
        }

        SendText(control, "220 havRemote isolated test fixture\r\n");

        std::string pending;
        Socket dataListener;
        bool listedNames{};

        while (const auto command = ReadCommand(control, pending, stopToken))
        {
          const auto verb = command->substr(0, command->find(' '));
          {
            std::scoped_lock lock{mMutex};
            mTranscript.commands.push_back(verb);
          }

          if (verb == "USER")
          {
            SendText(control, "331 Password required\r\n");
          }
          else if (verb == "PASS")
          {
            SendText(control, rejectLogin ? "530 Login rejected by fixture\r\n"
                                          : "230 Login successful\r\n");
          }
          else if (verb == "PWD")
          {
            SendText(control, "257 \"/\" is the current directory\r\n");
          }
          else if (verb == "OPTS" || verb == "TYPE" || verb == "NOOP")
          {
            if (verb == "OPTS" && listedNames &&
                mScenario == Scenario::CloseDuringNextCommand)
            {
              if (shutdown(control.Get(), SD_SEND) == SOCKET_ERROR)
              {
                SocketFailure("control shutdown during command");
              }

              break;
            }

            SendText(control, "200 Command accepted\r\n");
          }
          else if (verb == "CWD")
          {
            SendText(control, "250 Directory changed\r\n");
          }
          else if (verb == "SYST")
          {
            SendText(control, "215 UNIX Type: L8\r\n");
          }
          else if (verb == "FEAT")
          {
            SendText(control, "211 End\r\n");
          }
          else if (verb == "MKD" && mScenario == Scenario::RejectLoginAfterDeniedMkdir)
          {
            SendText(control, "550 Directory creation denied\r\n");

            rejectLogin = true;

            shutdown(control.Get(), SD_SEND);

            break;
          }
          else if (verb == "EPSV" || verb == "PASV")
          {
            dataListener = ListenOnLoopback();

            const auto port = LocalPort(dataListener);

            SendText(control,
                     verb == "EPSV"
                         ? "229 Entering Extended Passive Mode (|||" +
                               std::to_string(port) + "|)\r\n"
                         : "227 Entering Passive Mode (127,0,0,1," +
                               std::to_string(port / 256) + "," +
                               std::to_string(port % 256) + ")\r\n");
          }
          else if (verb == "NLST" || verb == "MLSD" || verb == "LIST")
          {
            if (dataListener.Get() == INVALID_SOCKET)
            {
              throw std::runtime_error("FTP fixture listing has no passive listener");
            }

            auto data = AcceptClient(dataListener, stopToken);
            if (data.Get() == INVALID_SOCKET)
            {
              return;
            }

            dataListener.Reset();

            if (verb == "MLSD" && mScenario == Scenario::MlsdUnsupported)
            {
              SendText(control, "500 MLSD is not supported\r\n");

              continue;
            }

            SendText(control, "150 Opening data connection\r\n");
            SendText(data,
                     verb == "NLST"   ? "file.txt\r\n"
                     : verb == "MLSD" ? "type=file;size=4;modify=20260906120000; file.txt\r\n"
                                      : "-rw-r--r-- 1 owner group 4 Jan 02 2026 file.txt\r\n");

            if (verb == "MLSD" && mScenario == Scenario::RejectLoginAfterMlsd)
            {
              rejectLogin = true;

              SendText(control, "226 Transfer complete\r\n");

              CloseControl(control, stopToken);

              shutdown(data.Get(), SD_SEND);

              break;
            }

            shutdown(data.Get(), SD_SEND);

            data.Reset();

            SendText(control, "226 Transfer complete\r\n");

            listedNames = listedNames || verb == "NLST";

            if (verb == "NLST" && mScenario == Scenario::ReconnectAfterNlst &&
                !closedAfterNlst)
            {
              closedAfterNlst = true;

              CloseControl(control, stopToken);

              break;
            }

            if (verb == "NLST" && mScenario == Scenario::RejectLoginAfterNlst)
            {
              rejectLogin = true;

              CloseControl(control, stopToken);

              break;
            }
          }
          else if (verb == "QUIT")
          {
            SendText(control, "221 Goodbye\r\n");

            break;
          }
          else
          {
            throw std::runtime_error("FTP fixture received unexpected command: " + verb);
          }
        }
      }
    }

    Winsock mWinsock;
    Socket mListener;
    std::uint16_t mPort;
    Scenario mScenario;
    mutable std::mutex mMutex;
    Transcript mTranscript;
    std::condition_variable mControlClosed;
    bool mControlCloseAcknowledged{};
    std::jthread mWorker;
  };

  constexpr std::string_view uploadName = "payload.bin";
  constexpr std::string_view partialUploadName = "payload.bin.havremote.part";

  class TemporaryUploadFile final
  {
  public:
    explicit TemporaryUploadFile(const std::string_view bytes)
        : mDirectory(std::filesystem::temp_directory_path() /
                     ("havremote-ftp-upload-" + GenerateId())),
          mPath(mDirectory / "source.bin")
    {
      if (!std::filesystem::create_directory(mDirectory))
      {
        throw std::runtime_error("Could not create FTP upload fixture directory");
      }

      std::ofstream output{mPath, std::ios::binary};
      output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      output.close();

      if (!output)
      {
        throw std::runtime_error("Could not write FTP upload fixture source");
      }
    }

    ~TemporaryUploadFile()
    {
      std::error_code ignored;

      auto part = mPath;
      part += ".havremote.part";

      std::filesystem::remove(part, ignored);
      std::filesystem::remove(mPath, ignored);
      std::filesystem::remove(mDirectory, ignored);
    }

    TemporaryUploadFile(const TemporaryUploadFile &) = delete;
    TemporaryUploadFile &operator=(const TemporaryUploadFile &) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const noexcept { return mPath; }

  private:
    std::filesystem::path mDirectory;
    std::filesystem::path mPath;
  };

  struct UploadReplies final
  {
    int rest{350};
    int stor{150};
    int completion{226};
    bool rejectFirstStorOnly{};
    bool multilineRest{};
    bool stallUpload{};
    bool resetUploadAfterPrefix{};
    std::optional<int> resetReply{};
    bool resetReplyAfterData{};
  };

  struct UploadTranscript final
  {
    // Preserve arguments as well as verbs: REST offsets and STOR/RNFR/RNTO
    // paths are part of the behavior under test.
    std::vector<std::string> commands;
    std::string failure;
    std::optional<std::string> partial;
    std::optional<std::string> final;
    std::string receivedBytes;
    std::vector<std::string> completionRepliesSent;
    bool transferCompleted{};
    bool renamedBeforeCompletion{};
  };

  struct DownloadBehavior final
  {
    std::size_t prefixSize{};
    std::chrono::milliseconds pause{};
    bool stallDownload{};
  };

  // Only local source/destination files are on disk. The loopback server stores
  // remote partial/final files in memory and implements byte-positioned STOR
  // itself, so duplicate seeking/appending cannot hide behind a mock transport.
  class UploadFtpFixture final
  {
  public:
    explicit UploadFtpFixture(std::optional<std::string> partial = std::nullopt,
                              const UploadReplies replies = {},
                              const DownloadBehavior downloadBehavior = {})
        : mListener(ListenOnLoopback()),
          mPort(LocalPort(mListener)),
          mReplies(replies),
          mDownloadBehavior(downloadBehavior),
          mTranscript{.commands = {},
                      .failure = {},
                      .partial = std::move(partial),
                      .final = std::nullopt,
                      .receivedBytes = {},
                      .completionRepliesSent = {},
                      .transferCompleted = false,
                      .renamedBeforeCompletion = false},
          mWorker([this](const std::stop_token stopToken)
                  {
                    try
                    {
                      Run(stopToken);
                    }
                    catch (const std::exception &exception)
                    {
                      std::scoped_lock lock{mMutex};

                      mTranscript.failure = exception.what();
                    } }) {}

    ~UploadFtpFixture() { Stop(); }
    UploadFtpFixture(const UploadFtpFixture &) = delete;
    UploadFtpFixture &operator=(const UploadFtpFixture &) = delete;

    [[nodiscard]] SiteProfile Site() const
    {
      SiteProfile result;
      result.id = "loopback-ftp-upload-fixture";
      result.name = "Loopback FTP upload fixture";
      result.protocol = ProtocolKind::Ftp;
      result.host = "127.0.0.1";
      result.port = mPort;
      result.username = "fixture-user";
      result.authentication.kind = AuthenticationKind::Password;
      result.ftpDataConnectionMode = FtpDataConnectionMode::Passive;

      return result;
    }

    void Stop()
    {
      mWorker.request_stop();

      if (mWorker.joinable())
      {
        mWorker.join();
      }
    }

    [[nodiscard]] UploadTranscript GetTranscript() const
    {
      std::scoped_lock lock{mMutex};
      return mTranscript;
    }

  private:
    static std::string_view Filename(std::string_view argument)
    {
      if (argument.starts_with("./"))
      {
        argument.remove_prefix(2);
      }

      if (argument.starts_with('/'))
      {
        argument.remove_prefix(1);
      }

      return argument;
    }

    std::string Listing(const std::string_view verb) const
    {
      const auto state = GetTranscript();

      std::string result;

      const auto add = [&result, verb](const std::string_view name,
                                       const std::optional<std::string> &bytes)
      {
        if (!bytes)
        {
          return;
        }

        if (verb == "MLSD")
        {
          result += "type=file;size=" + std::to_string(bytes->size()) +
                    ";unix.owner=fixture;unix.group=fixture; " + std::string{name} + "\r\n";
        }
        else if (verb == "LIST")
        {
          result += "-rw-r--r-- 1 fixture fixture " + std::to_string(bytes->size()) +
                    " Jan 02 2026 " + std::string{name} + "\r\n";
        }
        else
        {
          result += std::string{name} + "\r\n";
        }
      };

      add(partialUploadName, state.partial);
      add(uploadName, state.final);

      return result;
    }

    static std::string ReceiveUpload(const Socket &data,
                                     const std::stop_token stopToken)
    {
      std::string bytes;

      while (Readable(data, stopToken, std::chrono::steady_clock::now() + 5s))
      {
        std::array<char, 4096> buffer{};

        const auto received = recv(data.Get(), buffer.data(),
                                   static_cast<int>(buffer.size()), 0);

        if (received == SOCKET_ERROR)
        {
          SocketFailure("upload recv");
        }

        if (received == 0)
        {
          break;
        }

        bytes.append(buffer.data(), static_cast<std::size_t>(received));
      }

      return bytes;
    }

    void Run(const std::stop_token stopToken)
    {
      bool storRejected{};

      while (!stopToken.stop_requested())
      {
        auto control = AcceptClient(mListener, stopToken);

        if (control.Get() == INVALID_SOCKET)
        {
          return;
        }

        SendText(control, "220 havRemote isolated upload fixture\r\n");

        std::string pending;

        Socket dataListener;

        std::optional<std::size_t> restart;

        bool renamePending{};

        while (const auto command = ReadCommand(control, pending, stopToken))
        {
          const auto separator = command->find(' ');
          const auto verb = command->substr(0, separator);
          const auto argument = separator == std::string::npos
                                    ? std::string_view{}
                                    : std::string_view{*command}.substr(separator + 1);
          {
            std::scoped_lock lock{mMutex};

            mTranscript.commands.push_back(*command);
          }

          if (verb == "USER")
          {
            SendText(control, "331 Password required\r\n");
          }
          else if (verb == "PASS")
          {
            SendText(control, "230 Login successful\r\n");
          }
          else if (verb == "PWD")
          {
            SendText(control, "257 \"/\" is the current directory\r\n");
          }
          else if (verb == "OPTS" || verb == "TYPE" || verb == "NOOP")
          {
            SendText(control, "200 Command accepted\r\n");
          }
          else if (verb == "CWD")
          {
            SendText(control, "250 Directory changed\r\n");
          }
          else if (verb == "SYST")
          {
            SendText(control, "215 UNIX Type: L8\r\n");
          }
          else if (verb == "FEAT")
          {
            SendText(control, "211 End\r\n");
          }
          else if (verb == "EPSV" || verb == "PASV")
          {
            dataListener = ListenOnLoopback();

            const auto port = LocalPort(dataListener);

            SendText(control,
                     verb == "EPSV"
                         ? "229 Entering Extended Passive Mode (|||" +
                               std::to_string(port) + "|)\r\n"
                         : "227 Entering Passive Mode (127,0,0,1," +
                               std::to_string(port / 256) + "," +
                               std::to_string(port % 256) + ")\r\n");
          }
          else if (verb == "NLST" || verb == "MLSD" || verb == "LIST")
          {
            if (dataListener.Get() == INVALID_SOCKET)
            {
              throw std::runtime_error("FTP upload fixture listing has no data listener");
            }

            auto data = AcceptClient(dataListener, stopToken);
            if (data.Get() == INVALID_SOCKET)
            {
              return;
            }

            dataListener.Reset();

            SendText(control, "150 Opening listing data connection\r\n");
            SendText(data, Listing(verb));

            shutdown(data.Get(), SD_SEND);

            data.Reset();

            SendText(control, "226 Listing complete\r\n");
          }
          else if (verb == "SIZE")
          {
            const auto state = GetTranscript();
            const auto &bytes = Filename(argument) == partialUploadName
                                    ? state.partial
                                    : state.final;

            SendText(control, bytes ? "213 " + std::to_string(bytes->size()) + "\r\n"
                                    : "550 File does not exist\r\n");
          }
          else if (verb == "REST")
          {
            if (mReplies.rest == 350)
            {
              restart = static_cast<std::size_t>(std::stoull(std::string{argument}));
            }

            if (mReplies.multilineRest)
            {
              SendText(control, "350-REST fixture multiline prefix\r\n");
            }

            SendText(control, std::to_string(mReplies.rest) + " REST fixture reply\r\n");
          }
          else if (verb == "RETR")
          {
            const auto state = GetTranscript();

            if (Filename(argument) != partialUploadName || !state.partial)
            {
              throw std::runtime_error("FTP download fixture RETR used a missing filename");
            }

            if (dataListener.Get() == INVALID_SOCKET)
            {
              throw std::runtime_error("FTP download fixture RETR has no data listener");
            }

            auto data = AcceptClient(dataListener, stopToken);
            if (data.Get() == INVALID_SOCKET)
            {
              return;
            }

            dataListener.Reset();

            SendText(control, "150 Opening download data connection\r\n");

            const auto prefixSize = (std::min)(mDownloadBehavior.prefixSize,
                                               state.partial->size());

            const auto content = std::string_view{*state.partial};

            SendText(data, content.substr(0, prefixSize));

            if (mDownloadBehavior.stallDownload)
            {
              // Never send the remainder or a completion reply. Observe only
              // the client's eventual close, with a hard fixture deadline so
              // a broken low-speed policy cannot hang the hidden test.
              if (!Readable(data, stopToken, std::chrono::steady_clock::now() + 45s))
              {
                return;
              }

              char ignored{};

              const auto received = recv(data.Get(), &ignored, 1, 0);
              if (received > 0)
              {
                throw std::runtime_error("FTP download fixture received unexpected client data");
              }
              if (received == SOCKET_ERROR)
              {
                const auto error = WSAGetLastError();

                if (error != WSAECONNRESET && error != WSAECONNABORTED)
                {
                  SocketFailure("stalled download recv");
                }
              }

              return;
            }

            if (mDownloadBehavior.pause > 0ms)
            {
              std::mutex pauseMutex;
              std::condition_variable_any pauseCv;

              std::unique_lock lock{pauseMutex};

              (void)pauseCv.wait_for(lock, stopToken, mDownloadBehavior.pause,
                                     []
                                     { return false; });

              if (stopToken.stop_requested())
              {
                return;
              }
            }

            SendText(data, content.substr(prefixSize));

            shutdown(data.Get(), SD_SEND);

            data.Reset();

            SendText(control, "226 Download complete\r\n");
          }
          else if (verb == "STOR")
          {
            if (Filename(argument) != partialUploadName)
            {
              throw std::runtime_error("FTP upload fixture STOR used the wrong filename");
            }

            if (dataListener.Get() == INVALID_SOCKET)
            {
              throw std::runtime_error("FTP upload fixture STOR has no data listener");
            }

            auto data = AcceptClient(dataListener, stopToken);
            if (data.Get() == INVALID_SOCKET)
            {
              return;
            }

            dataListener.Reset();

            if (mReplies.stor != 150 &&
                (!mReplies.rejectFirstStorOnly || !storRejected))
            {
              storRejected = true;

              SendText(control, std::to_string(mReplies.stor) + " STOR fixture reply\r\n");

              continue;
            }

            if (mReplies.stallUpload || mReplies.resetUploadAfterPrefix)
            {
              const int receiveBufferSize = 1024;

              if (setsockopt(data.Get(), SOL_SOCKET, SO_RCVBUF,
                             reinterpret_cast<const char *>(&receiveBufferSize),
                             sizeof(receiveBufferSize)) == SOCKET_ERROR)
              {
                SocketFailure("stalled upload receive buffer");
              }
            }

            SendText(control, "150 Opening upload data connection\r\n");

            if (mReplies.stallUpload || mReplies.resetUploadAfterPrefix)
            {
              if (!Readable(data, stopToken, std::chrono::steady_clock::now() + 5s))
              {
                return;
              }

              char prefix{};

              if (recv(data.Get(), &prefix, 1, 0) != 1)
              {
                SocketFailure("stalled upload prefix recv");
              }

              {
                std::scoped_lock lock{mMutex};

                if (restart && (!mTranscript.partial || *restart > mTranscript.partial->size()))
                {
                  throw std::runtime_error("FTP stalled upload fixture received an invalid restart");
                }

                if (!restart)
                {
                  mTranscript.partial = std::string{};
                }

                mTranscript.partial->resize(restart.value_or(0));
                mTranscript.partial->push_back(prefix);
                mTranscript.receivedBytes.push_back(prefix);
              }

              if (mReplies.resetUploadAfterPrefix)
              {
                const auto sendCompletion = [&]
                {
                  if (!mReplies.resetReply)
                  {
                    return;
                  }

                  const auto reply = std::to_string(*mReplies.resetReply) +
                                     " Fixture upload storage allocation exceeded";

                  SendText(control, reply + "\r\n");

                  std::scoped_lock lock{mMutex};

                  mTranscript.completionRepliesSent.push_back(reply);
                };

                if (!mReplies.resetReplyAfterData)
                {
                  sendCompletion();
                }

                const linger abortiveClose{1, 0};

                if (setsockopt(data.Get(), SOL_SOCKET, SO_LINGER,
                               reinterpret_cast<const char *>(&abortiveClose),
                               sizeof(abortiveClose)) == SOCKET_ERROR)
                {
                  SocketFailure("upload reset linger");
                }

                data.Reset();

                if (mReplies.resetReplyAfterData)
                {
                  try
                  {
                    sendCompletion();
                  }
                  catch (const std::exception &)
                  {
                    // A data reset can make curl close the control channel
                    // before this later server reply is sent. Record no sent
                    // reply in that case, rather than claiming it arrived.
                    const auto error = WSAGetLastError();

                    if (error != WSAECONNRESET && error != WSAECONNABORTED &&
                        error != WSAESHUTDOWN && error != WSAENOTCONN)
                    {
                      throw;
                    }
                  }
                }

                restart.reset();

                continue;
              }

              // Leaving queued bytes unread closes the TCP receive window.
              // Destruction requests stop as soon as curl returns. Otherwise,
              // terminate the fixture after a bounded regression deadline.
              std::mutex pauseMutex;
              std::condition_variable_any pauseCv;

              std::unique_lock lock{pauseMutex};

              (void)pauseCv.wait_for(lock, stopToken, 45s, []
                                     { return false; });

              if (!stopToken.stop_requested())
              {
                throw std::runtime_error("FTP stalled upload exceeded its 45-second fixture deadline");
              }

              return;
            }

            const auto bytes = ReceiveUpload(data, stopToken);

            data.Reset();
            {
              std::scoped_lock lock{mMutex};

              if (restart && (!mTranscript.partial || *restart > mTranscript.partial->size()))
              {
                throw std::runtime_error("FTP upload fixture received an invalid restart position");
              }

              if (!restart)
              {
                mTranscript.partial = std::string{};
              }

              mTranscript.partial->resize(restart.value_or(0));
              mTranscript.partial->append(bytes);
              mTranscript.receivedBytes += bytes;
              mTranscript.transferCompleted = mReplies.completion == 226;
            }

            restart.reset();

            const auto reply = std::to_string(mReplies.completion) + " Upload fixture completion";

            SendText(control, reply + "\r\n");
            {
              std::scoped_lock lock{mMutex};

              mTranscript.completionRepliesSent.push_back(reply);
            }
          }
          else if (verb == "APPE")
          {
            dataListener.Reset();

            SendText(control, "500 APPE is forbidden by the upload fixture\r\n");
          }
          else if (verb == "RNFR")
          {
            if (Filename(argument) != partialUploadName)
            {
              throw std::runtime_error("FTP upload fixture RNFR used the wrong filename");
            }

            {
              std::scoped_lock lock{mMutex};

              mTranscript.renamedBeforeCompletion = !mTranscript.transferCompleted;
            }

            renamePending = true;

            SendText(control, "350 Ready for destination\r\n");
          }
          else if (verb == "RNTO")
          {
            if (!renamePending || Filename(argument) != uploadName)
            {
              throw std::runtime_error("FTP upload fixture received an invalid RNTO");
            }

            {
              std::scoped_lock lock{mMutex};

              mTranscript.final = std::move(mTranscript.partial);
              mTranscript.partial.reset();
            }

            renamePending = false;

            SendText(control, "250 Rename complete\r\n");
          }
          else if (verb == "QUIT")
          {
            SendText(control, "221 Goodbye\r\n");

            break;
          }
          else
          {
            throw std::runtime_error("FTP upload fixture received unexpected command: " + *command);
          }
        }
      }
    }

    Winsock mWinsock;
    Socket mListener;
    std::uint16_t mPort;
    UploadReplies mReplies;
    DownloadBehavior mDownloadBehavior;
    mutable std::mutex mMutex;
    UploadTranscript mTranscript;
    std::jthread mWorker;
  };

  std::ptrdiff_t CommandCount(const UploadTranscript &transcript,
                              const std::string_view verb)
  {
    return std::ranges::count_if(transcript.commands, [verb](const std::string &command)
                                 { return command == verb ||
                                          command.starts_with(std::string{verb} + " "); });
  }

  void CheckNoUploadFinalization(const UploadTranscript &transcript)
  {
    INFO(transcript.failure);
    CHECK(transcript.failure.empty());
    CHECK(CommandCount(transcript, "APPE") == 0);
    CHECK(CommandCount(transcript, "RNFR") == 0);
    CHECK(CommandCount(transcript, "RNTO") == 0);
    CHECK_FALSE(transcript.final);
  }

  struct Diagnostic final
  {
    DiagnosticLevel level;
    std::string text;
  };

  SessionCallbacks Callbacks(std::vector<Diagnostic> &diagnostics)
  {
    SessionCallbacks result;
    result.connectionTimeout = 2s;
    result.commandIdleTimeout = 2s;
    result.requestCredential = [](const CredentialRequest &,
                                  std::stop_token) -> Result<std::string>
    { return std::string{"fixture-password"}; };
    result.diagnostic = [&diagnostics](const DiagnosticLevel level,
                                       const std::string_view text)
    { diagnostics.push_back(Diagnostic{level, std::string{text}}); };

    return result;
  }

  std::ptrdiff_t ReconnectionCount(const std::vector<Diagnostic> &diagnostics)
  {
    return std::ranges::count_if(diagnostics, [](const Diagnostic &entry)
                                 { return entry.text.find("Reconnected to the FTP server") !=
                                          std::string::npos; });
  }

  std::ptrdiff_t LowSpeedPolicyCount(const std::vector<Diagnostic> &diagnostics)
  {
    return std::ranges::count_if(diagnostics, [](const Diagnostic &entry)
                                 { return entry.text.find("low-speed policy:") !=
                                          std::string::npos; });
  }

  void CheckTransferLowSpeedPolicy(const std::vector<Diagnostic> &diagnostics,
                                   const std::string_view verb,
                                   const std::ptrdiff_t expectedCount = 1)
  {
    const auto expected = "FTP " + std::string{verb} +
                          " low-speed policy: CURLOPT_LOW_SPEED_LIMIT=1 byte/sec, CURLOPT_LOW_SPEED_TIME=30 seconds";

    CHECK(LowSpeedPolicyCount(diagnostics) == expectedCount);
    CHECK(std::ranges::count_if(diagnostics, [&expected](const Diagnostic &entry)
                                { return entry.level == DiagnosticLevel::Debug &&
                                         entry.text == expected; }) == expectedCount);
  }

  std::string ReadFileBytes(const std::filesystem::path &path)
  {
    std::ifstream stream{path, std::ios::binary};

    REQUIRE(stream.is_open());

    const std::string bytes{std::istreambuf_iterator<char>{stream},
                            std::istreambuf_iterator<char>{}};

    REQUIRE_FALSE(stream.bad());

    return bytes;
  }

  void CheckFailureDiagnostics(const std::vector<Diagnostic> &diagnostics)
  {
    CHECK(std::ranges::any_of(diagnostics, [](const Diagnostic &entry)
                              { return entry.level == DiagnosticLevel::Debug &&
                                       entry.text.find("CURLcode") != std::string::npos; }));

    for (const auto &entry : diagnostics)
    {
      INFO(entry.text);
      CHECK(entry.level != DiagnosticLevel::Error);
      CHECK(entry.text.find("fixture-password") == std::string::npos);
    }
  }
} // namespace

TEST_CASE("FTP reports a successful replacement control connection once without extra probes",
          "[ftp][loopback][reconnection]")
{
  FtpFixture server{Scenario::ReconnectAfterNlst};

  std::vector<Diagnostic> diagnostics;

  const auto session = MakeFtpSession();
  const auto connected = session->Connect(server.Site(), Callbacks(diagnostics), {});

  INFO((connected ? "Connected" : connected.error().message));
  REQUIRE(connected);
  CHECK(ReconnectionCount(diagnostics) == 0);

  server.WaitForControlClose();

  const auto listed = session->List(RemotePath::Root(), {});

  INFO((listed ? "Listed after reconnect" : listed.error().message));
  REQUIRE(listed);
  REQUIRE(listed->size() == 1U);
  CHECK(listed->front().name.Bytes() == "file.txt");
  CHECK(listed->front().owner == std::optional<std::string>{"owner"});
  CHECK(listed->front().group == std::optional<std::string>{"group"});
  CHECK(ReconnectionCount(diagnostics) == 1);

  const auto reconnected = std::ranges::find_if(diagnostics, [](const Diagnostic &entry)
                                                { return entry.text.find("Reconnected to the FTP server") !=
                                                         std::string::npos; });

  REQUIRE(reconnected != diagnostics.end());
  CHECK(reconnected->level == DiagnosticLevel::Information);

  const auto listedAgain = session->List(RemotePath::Root(), {});

  INFO((listedAgain ? "Listed on reused connection" : listedAgain.error().message));
  REQUIRE(listedAgain);
  CHECK(ReconnectionCount(diagnostics) == 1);

  session->Disconnect();
  server.Stop();

  const auto transcript = server.GetTranscript();

  INFO(transcript.failure);
  CHECK(transcript.failure.empty());
  CHECK(std::ranges::count(transcript.commands, "USER") == 2);
  CHECK(std::ranges::count(transcript.commands, "PASS") == 2);
  CHECK(std::ranges::count(transcript.commands, "NLST") == 1);
  CHECK(std::ranges::count(transcript.commands, "MLSD") == 2);
  CHECK(std::ranges::count(transcript.commands, "LIST") == 2);
  CHECK(std::ranges::count(transcript.commands, "NOOP") == 0);
  CHECK(ReconnectionCount(diagnostics) == 1);
  CHECK(LowSpeedPolicyCount(diagnostics) == 0);

  for (const auto &entry : diagnostics)
  {
    INFO(entry.text);
    CHECK(entry.text.find("fixture-password") == std::string::npos);
  }
}

TEST_CASE("FTP reports connection loss during a reused command without replaying it",
          "[ftp][loopback][reconnection]")
{
  FtpFixture server{Scenario::CloseDuringNextCommand};

  std::vector<Diagnostic> diagnostics;

  const auto session = MakeFtpSession();
  const auto connected = session->Connect(server.Site(), Callbacks(diagnostics), {});

  INFO((connected ? "Connected" : connected.error().message));
  REQUIRE(connected);

  const auto listed = session->List(RemotePath::Root(), {});

  INFO((listed ? "Unexpected listing success" : listed.error().message));
  REQUIRE_FALSE(listed);
  CHECK(listed.error().code == RemoteErrorCode::ConnectionLost);
  CHECK(listed.error().nativeCode == CURLE_RECV_ERROR);
  CHECK(listed.error().retryable);
  CHECK_FALSE(listed.error().operationMayHaveSucceeded);
  CHECK(ReconnectionCount(diagnostics) == 0);

  session->Disconnect();
  server.Stop();

  const auto transcript = server.GetTranscript();

  INFO(transcript.failure);
  CHECK(transcript.failure.empty());
  CHECK(std::ranges::count(transcript.commands, "USER") == 1);
  CHECK(std::ranges::count(transcript.commands, "PASS") == 1);
  CHECK(std::ranges::count(transcript.commands, "OPTS") == 3);
  CHECK(std::ranges::count(transcript.commands, "NLST") == 1);
  CHECK(std::ranges::count(transcript.commands, "MLSD") == 0);
  CHECK(std::ranges::count(transcript.commands, "LIST") == 0);
  CHECK(std::ranges::count(transcript.commands, "NOOP") == 0);

  CheckFailureDiagnostics(diagnostics);
}

TEST_CASE("FTP explicit disconnect and connect starts fresh without a reconnection notification",
          "[ftp][loopback][reconnection]")
{
  FtpFixture server{Scenario::MlsdUnsupported};

  std::vector<Diagnostic> diagnostics;

  const auto session = MakeFtpSession();

  for (unsigned int attempt = 0; attempt < 2U; ++attempt)
  {
    CAPTURE(attempt);

    const auto connected = session->Connect(server.Site(), Callbacks(diagnostics), {});

    INFO((connected ? "Connected" : connected.error().message));
    REQUIRE(connected);
    CHECK(ReconnectionCount(diagnostics) == 0);

    const auto listed = session->List(RemotePath::Root(), {});

    INFO((listed ? "Listed" : listed.error().message));
    REQUIRE(listed);
    CHECK(ReconnectionCount(diagnostics) == 0);

    session->Disconnect();
  }

  server.Stop();

  const auto transcript = server.GetTranscript();

  INFO(transcript.failure);
  CHECK(transcript.failure.empty());
  CHECK(std::ranges::count(transcript.commands, "USER") == 2);
  CHECK(std::ranges::count(transcript.commands, "PASS") == 2);
  CHECK(std::ranges::count(transcript.commands, "NLST") == 2);
  CHECK(std::ranges::count(transcript.commands, "MLSD") == 2);
  CHECK(std::ranges::count(transcript.commands, "LIST") == 2);
  CHECK(std::ranges::count(transcript.commands, "NOOP") == 0);
  CHECK(ReconnectionCount(diagnostics) == 0);
}

TEST_CASE("FTP rejects a fresh login without retrying the failure through LIST",
          "[ftp][loopback]")
{
  FtpFixture server{Scenario::RejectLoginAfterNlst};

  std::vector<Diagnostic> diagnostics;

  const auto session = MakeFtpSession();
  const auto connected = session->Connect(server.Site(), Callbacks(diagnostics), {});

  INFO((connected ? "Connected" : connected.error().message));
  REQUIRE(connected);

  server.WaitForControlClose();

  const auto listed = session->List(RemotePath::Root(), {});

  INFO((listed ? "Unexpected listing success" : listed.error().message));
  REQUIRE_FALSE(listed);
  CHECK(listed.error().code == RemoteErrorCode::AuthenticationFailed);

  session->Disconnect();
  server.Stop();

  const auto transcript = server.GetTranscript();

  INFO(transcript.failure);
  CHECK(transcript.failure.empty());
  CHECK(std::ranges::count(transcript.commands, "USER") == 2);
  CHECK(std::ranges::count(transcript.commands, "PASS") == 2);
  CHECK(std::ranges::count(transcript.commands, "NLST") == 1);
  CHECK(std::ranges::count(transcript.commands, "MLSD") == 0);
  CHECK(std::ranges::count(transcript.commands, "LIST") == 0);
  CHECK(ReconnectionCount(diagnostics) == 0);

  CheckFailureDiagnostics(diagnostics);
}

TEST_CASE("FTP ownership supplementation propagates fresh login rejection",
          "[ftp][loopback]")
{
  FtpFixture server{Scenario::RejectLoginAfterMlsd};

  std::vector<Diagnostic> diagnostics;

  const auto session = MakeFtpSession();
  const auto connected = session->Connect(server.Site(), Callbacks(diagnostics), {});

  INFO((connected ? "Connected" : connected.error().message));
  REQUIRE(connected);

  const auto listed = session->List(RemotePath::Root(), {});

  INFO((listed ? "Unexpected listing success" : listed.error().message));
  REQUIRE_FALSE(listed);
  CHECK(listed.error().code == RemoteErrorCode::AuthenticationFailed);

  session->Disconnect();
  server.Stop();

  const auto transcript = server.GetTranscript();

  INFO(transcript.failure);
  CHECK(transcript.failure.empty());
  CHECK(std::ranges::count(transcript.commands, "USER") == 2);
  CHECK(std::ranges::count(transcript.commands, "PASS") == 2);
  CHECK(std::ranges::count(transcript.commands, "NLST") == 1);
  CHECK(std::ranges::count(transcript.commands, "MLSD") == 1);
  CHECK(std::ranges::count(transcript.commands, "LIST") == 0);
  CHECK(ReconnectionCount(diagnostics) == 0);

  CheckFailureDiagnostics(diagnostics);
}

TEST_CASE("FTP still falls back to LIST when MLSD is unsupported",
          "[ftp][loopback]")
{
  FtpFixture server{Scenario::MlsdUnsupported};

  std::vector<Diagnostic> diagnostics;

  const auto session = MakeFtpSession();
  const auto connected = session->Connect(server.Site(), Callbacks(diagnostics), {});

  INFO((connected ? "Connected" : connected.error().message));
  REQUIRE(connected);

  const auto listed = session->List(RemotePath::Root(), {});

  INFO((listed ? "Listed" : listed.error().message));
  REQUIRE(listed);
  REQUIRE(listed->size() == 1);
  CHECK(listed->front().name.Bytes() == "file.txt");
  CHECK(listed->front().owner == std::optional<std::string>{"owner"});
  CHECK(listed->front().group == std::optional<std::string>{"group"});

  session->Disconnect();
  server.Stop();

  const auto transcript = server.GetTranscript();

  INFO(transcript.failure);
  CHECK(transcript.failure.empty());
  CHECK(std::ranges::count(transcript.commands, "NLST") == 1);
  CHECK(std::ranges::count(transcript.commands, "MLSD") == 1);
  CHECK(std::ranges::count(transcript.commands, "LIST") == 1);
  CHECK(LowSpeedPolicyCount(diagnostics) == 0);

  CheckFailureDiagnostics(diagnostics);
}

TEST_CASE("Recursive FTP mkdir does not probe paths after fresh login rejection",
          "[ftp][loopback]")
{
  FtpFixture server{Scenario::RejectLoginAfterNlst};

  std::vector<Diagnostic> diagnostics;

  const auto session = MakeFtpSession();
  const auto connected = session->Connect(server.Site(), Callbacks(diagnostics), {});

  INFO((connected ? "Connected" : connected.error().message));
  REQUIRE(connected);

  server.WaitForControlClose();

  const auto made = session->Mkdir(RemotePath{"/first/second"}, true, {});

  INFO((made ? "Unexpected mkdir success" : made.error().message));
  REQUIRE_FALSE(made);
  CHECK(made.error().code == RemoteErrorCode::AuthenticationFailed);

  session->Disconnect();
  server.Stop();

  const auto transcript = server.GetTranscript();

  INFO(transcript.failure);
  CHECK(transcript.failure.empty());
  CHECK(std::ranges::count(transcript.commands, "USER") == 2);
  CHECK(std::ranges::count(transcript.commands, "PASS") == 2);
  CHECK(std::ranges::count(transcript.commands, "NLST") == 1);
  CHECK(std::ranges::count(transcript.commands, "MKD") == 0);
  CHECK(std::ranges::count(transcript.commands, "MLSD") == 0);
  CHECK(std::ranges::count(transcript.commands, "LIST") == 0);
  CHECK(ReconnectionCount(diagnostics) == 0);

  CheckFailureDiagnostics(diagnostics);
}

TEST_CASE("Recursive FTP mkdir propagates authentication failure from its existence check",
          "[ftp][loopback]")
{
  FtpFixture server{Scenario::RejectLoginAfterDeniedMkdir};

  std::vector<Diagnostic> diagnostics;

  const auto session = MakeFtpSession();
  const auto connected = session->Connect(server.Site(), Callbacks(diagnostics), {});

  INFO((connected ? "Connected" : connected.error().message));
  REQUIRE(connected);

  const auto made = session->Mkdir(RemotePath{"/nested"}, true, {});

  INFO((made ? "Unexpected mkdir success" : made.error().message));
  REQUIRE_FALSE(made);
  CHECK(made.error().code == RemoteErrorCode::AuthenticationFailed);

  session->Disconnect();
  server.Stop();

  const auto transcript = server.GetTranscript();

  INFO(transcript.failure);
  CHECK(transcript.failure.empty());
  CHECK(std::ranges::count(transcript.commands, "USER") == 2);
  CHECK(std::ranges::count(transcript.commands, "PASS") == 2);
  CHECK(std::ranges::count(transcript.commands, "NLST") == 1);
  CHECK(std::ranges::count(transcript.commands, "MKD") == 1);
  CHECK(std::ranges::count(transcript.commands, "MLSD") == 0);
  CHECK(std::ranges::count(transcript.commands, "LIST") == 0);
  CHECK(ReconnectionCount(diagnostics) == 0);

  CheckFailureDiagnostics(diagnostics);
}

TEST_CASE("FTP uploads preserve exact bytes and finalize only after successful STOR",
          "[ftp][loopback][upload-resume]")
{
  const std::string sourceBytes = std::string{"0123456789"} + '\0' + "binary\r\nsuffix";

  std::uint64_t offset{7};

  UploadReplies replies;

  std::optional<std::string> initialPartial = sourceBytes.substr(0, offset);

  SECTION("resuming seeks the local source exactly once") {}
  SECTION("a multiline REST reply arms restart only at its final 350 response")
  {
    replies.multilineRest = true;
  }
  SECTION("a complete partial file resumes at EOF without losing content")
  {
    offset = sourceBytes.size();
    initialPartial = sourceBytes;
  }
  SECTION("a fresh upload sends STOR without REST")
  {
    offset = 0;
    initialPartial.reset();
  }

  TemporaryUploadFile source{sourceBytes};
  UploadFtpFixture server{initialPartial, replies};

  std::vector<Diagnostic> diagnostics;
  std::vector<TransferProgress> progress;

  const auto session = MakeFtpSession();
  const auto connected = session->Connect(server.Site(), Callbacks(diagnostics), {});

  INFO((connected ? "Connected" : connected.error().message));
  REQUIRE(connected);
  CHECK(LowSpeedPolicyCount(diagnostics) == 0);

  bool finalized{};

  TransferOptions options;
  options.resumeOffset = offset;
  options.beforeFinalize = [&server, &sourceBytes, &finalized](const bool overwrite) -> Result<bool>
  {
    const auto state = server.GetTranscript();

    CHECK(state.transferCompleted);
    CHECK(state.partial == std::optional<std::string>{sourceBytes});
    CHECK_FALSE(state.final);

    finalized = true;

    return overwrite;
  };

  const auto uploaded = session->Upload(source.Path(), RemotePath{"/payload.bin"}, options,
                                        [&progress](const TransferProgress &value)
                                        {
                                          progress.push_back(value);
                                          return TransferControl::Continue;
                                        },
                                        {});

  INFO((uploaded ? "Uploaded" : uploaded.error().message));
  REQUIRE(uploaded);
  CHECK(finalized);

  session->Disconnect();
  server.Stop();

  const auto transcript = server.GetTranscript();

  INFO(transcript.failure);
  CHECK(transcript.failure.empty());
  CHECK(transcript.final == std::optional<std::string>{sourceBytes});
  CHECK_FALSE(transcript.partial);
  CHECK(transcript.receivedBytes == sourceBytes.substr(offset));
  CHECK_FALSE(transcript.renamedBeforeCompletion);
  CHECK(CommandCount(transcript, "STOR") == 1);
  CHECK(CommandCount(transcript, "APPE") == 0);
  CHECK(CommandCount(transcript, "REST") == (offset > 0 ? 1 : 0));
  CHECK(CommandCount(transcript, "RNFR") == 1);
  CHECK(CommandCount(transcript, "RNTO") == 1);
  const auto stor = std::ranges::find(transcript.commands,
                                      "STOR " + std::string{partialUploadName});
  REQUIRE(stor != transcript.commands.end());

  if (offset > 0)
  {
    REQUIRE(stor != transcript.commands.begin());
    CHECK(*(stor - 1) == "REST " + std::to_string(offset));
  }

  REQUIRE_FALSE(progress.empty());
  CHECK(progress.back().bytesTransferred == sourceBytes.size());
  CHECK(progress.back().activeBytesTransferred == sourceBytes.size() - offset);
  CHECK(progress.back().totalBytes == std::optional<std::uint64_t>{sourceBytes.size()});

  CheckTransferLowSpeedPolicy(diagnostics, "STOR");
}

TEST_CASE("FTP rejects unsafe remote upload resume sizes before sending upload commands",
          "[ftp][loopback][upload-resume]")
{
  const std::string sourceBytes{"0123456789"};

  std::string partial = sourceBytes.substr(0, 6);

  TransferOptions options;
  options.resumeOffset = 4;

  auto expected = RemoteErrorCode::Conflict;

  SECTION("the remote partial differs from the saved checkpoint") {}
  SECTION("the remote partial is larger than the source despite a smaller checkpoint")
  {
    partial = sourceBytes + "extra";
  }
  SECTION("the resume offset itself exceeds the local source size")
  {
    partial = sourceBytes + "extra";

    options.resumeOffset = partial.size();

    expected = RemoteErrorCode::InvalidArgument;
  }

  TemporaryUploadFile source{sourceBytes};
  UploadFtpFixture server{partial};

  std::vector<Diagnostic> diagnostics;

  const auto session = MakeFtpSession();

  REQUIRE(session->Connect(server.Site(), Callbacks(diagnostics), {}));

  const auto uploaded = session->Upload(source.Path(), RemotePath{"/payload.bin"}, options, {}, {});

  INFO((uploaded ? "Unexpected upload success" : uploaded.error().message));
  REQUIRE_FALSE(uploaded);
  CHECK(uploaded.error().code == expected);

  session->Disconnect();
  server.Stop();

  const auto transcript = server.GetTranscript();

  CheckNoUploadFinalization(transcript);

  CHECK(transcript.partial == std::optional<std::string>{partial});
  CHECK(CommandCount(transcript, "REST") == 0);
  CHECK(CommandCount(transcript, "STOR") == 0);
  CHECK(transcript.receivedBytes.empty());

  CHECK(LowSpeedPolicyCount(diagnostics) == 0);

  if (expected == RemoteErrorCode::Conflict)
  {
    CHECK(CommandCount(transcript, "MLSD") > 0);
  }
}

TEST_CASE("FTP reports unsupported upload restart when REST is rejected or not armed",
          "[ftp][loopback][upload-resume]")
{
  UploadReplies replies;

  SECTION("REST is unrecognized") { replies.rest = 500; }
  SECTION("REST is disabled by server policy") { replies.rest = 550; }
  SECTION("a generic success response does not arm a restart") { replies.rest = 200; }
  SECTION("a misleading multiline 350 prefix cannot arm a final 200 response")
  {
    replies.multilineRest = true;
    replies.rest = 200;
  }

  const std::string sourceBytes{"0123456789abcdef"};
  const auto partial = sourceBytes.substr(0, 4);

  TemporaryUploadFile source{sourceBytes};
  UploadFtpFixture server{partial, replies};

  std::vector<Diagnostic> diagnostics;

  const auto session = MakeFtpSession();

  REQUIRE(session->Connect(server.Site(), Callbacks(diagnostics), {}));

  TransferOptions options;
  options.resumeOffset = partial.size();

  const auto uploaded = session->Upload(source.Path(), RemotePath{"/payload.bin"}, options, {}, {});

  INFO((uploaded ? "Unexpected upload success" : uploaded.error().message));
  REQUIRE_FALSE(uploaded);
  CHECK(uploaded.error().code == RemoteErrorCode::Unsupported);
  CHECK(uploaded.error().message.find("does not support upload resume") != std::string::npos);
  CHECK(uploaded.error().nativeCode != 0);
  session->Disconnect();

  server.Stop();

  const auto transcript = server.GetTranscript();

  CheckNoUploadFinalization(transcript);

  CHECK(CommandCount(transcript, "REST") == 1);
  CHECK(CommandCount(transcript, "STOR") == 0);
  CHECK(transcript.receivedBytes.empty());
  CHECK(transcript.partial == std::optional<std::string>{partial});

  CheckTransferLowSpeedPolicy(diagnostics, "STOR");

  CheckFailureDiagnostics(diagnostics);
}

TEST_CASE("FTP handles a rejected resumed STOR without append fallback or rename",
          "[ftp][loopback][upload-resume]")
{
  UploadReplies replies;

  SECTION("STOR is unrecognized after REST") { replies.stor = 500; }
  SECTION("server disallows restarted STOR") { replies.stor = 550; }

  const std::string sourceBytes{"0123456789abcdef"};
  const auto partial = sourceBytes.substr(0, 4);

  TemporaryUploadFile source{sourceBytes};
  UploadFtpFixture server{partial, replies};

  std::vector<Diagnostic> diagnostics;

  const auto session = MakeFtpSession();

  REQUIRE(session->Connect(server.Site(), Callbacks(diagnostics), {}));

  TransferOptions options;
  options.resumeOffset = partial.size();

  const auto uploaded = session->Upload(source.Path(), RemotePath{"/payload.bin"}, options, {}, {});

  INFO((uploaded ? "Unexpected upload success" : uploaded.error().message));
  REQUIRE_FALSE(uploaded);
  CHECK(uploaded.error().code == RemoteErrorCode::Unsupported);
  CHECK(uploaded.error().message.find("does not support upload resume") != std::string::npos);
  CHECK(uploaded.error().nativeCode != 0);

  session->Disconnect();
  server.Stop();

  const auto transcript = server.GetTranscript();

  CheckNoUploadFinalization(transcript);

  CHECK(CommandCount(transcript, "REST") == 1);
  CHECK(CommandCount(transcript, "STOR") == 1);
  CHECK(transcript.receivedBytes.empty());
  CHECK(transcript.partial == std::optional<std::string>{partial});

  CheckTransferLowSpeedPolicy(diagnostics, "STOR");

  CheckFailureDiagnostics(diagnostics);
}

TEST_CASE("FTP preserves authentication and storage failures during resumed uploads",
          "[ftp][loopback][upload-resume]")
{
  UploadReplies replies;

  auto expected = RemoteErrorCode::AuthenticationFailed;

  SECTION("REST requires authentication") { replies.rest = 530; }
  SECTION("STOR requires authentication") { replies.stor = 530; }
  SECTION("STOR exhausts server storage")
  {
    replies.stor = 552;
    expected = RemoteErrorCode::RemoteIo;
  }

  const std::string sourceBytes{"0123456789abcdef"};
  const auto partial = sourceBytes.substr(0, 4);

  TemporaryUploadFile source{sourceBytes};
  UploadFtpFixture server{partial, replies};

  std::vector<Diagnostic> diagnostics;

  const auto session = MakeFtpSession();

  REQUIRE(session->Connect(server.Site(), Callbacks(diagnostics), {}));

  TransferOptions options;
  options.resumeOffset = partial.size();

  const auto uploaded = session->Upload(source.Path(), RemotePath{"/payload.bin"}, options, {}, {});

  INFO((uploaded ? "Unexpected upload success" : uploaded.error().message));
  REQUIRE_FALSE(uploaded);
  CHECK(uploaded.error().code == expected);
  CHECK(uploaded.error().message.find("does not support upload resume") == std::string::npos);
  CHECK(uploaded.error().nativeCode != 0);
  session->Disconnect();

  server.Stop();

  const auto transcript = server.GetTranscript();

  CheckNoUploadFinalization(transcript);

  CHECK(transcript.receivedBytes.empty());
  CHECK(transcript.partial == std::optional<std::string>{partial});

  CheckTransferLowSpeedPolicy(diagnostics, "STOR");

  CheckFailureDiagnostics(diagnostics);
}

TEST_CASE("FTP never finalizes an upload without a successful transfer completion reply",
          "[ftp][loopback][upload-resume]")
{
  const std::string sourceBytes{"0123456789abcdef"};
  const auto partial = sourceBytes.substr(0, 4);

  TemporaryUploadFile source{sourceBytes};
  UploadFtpFixture server{partial, UploadReplies{.completion = 426}};

  std::vector<Diagnostic> diagnostics;

  const auto session = MakeFtpSession();

  REQUIRE(session->Connect(server.Site(), Callbacks(diagnostics), {}));

  TransferOptions options;
  options.resumeOffset = partial.size();

  bool finalized{};

  options.beforeFinalize = [&finalized](const bool overwrite) -> Result<bool>
  {
    finalized = true;
    return overwrite;
  };

  const auto uploaded = session->Upload(source.Path(), RemotePath{"/payload.bin"}, options, {}, {});

  INFO((uploaded ? "Unexpected upload success" : uploaded.error().message));
  REQUIRE_FALSE(uploaded);
  CHECK(uploaded.error().code != RemoteErrorCode::Unsupported);
  CHECK_FALSE(finalized);

  session->Disconnect();
  server.Stop();

  const auto transcript = server.GetTranscript();

  CheckNoUploadFinalization(transcript);

  CHECK(CommandCount(transcript, "STOR") == 1);
  CHECK(transcript.receivedBytes == sourceBytes.substr(partial.size()));
  CHECK(transcript.partial == std::optional<std::string>{sourceBytes});
  CHECK_FALSE(transcript.transferCompleted);

  CheckTransferLowSpeedPolicy(diagnostics, "STOR");

  CheckFailureDiagnostics(diagnostics);
}

TEST_CASE("FTP discards a rejected restart before a subsequent fresh upload",
          "[ftp][loopback][upload-resume]")
{
  const std::string sourceBytes{"0123456789abcdef"};
  const auto partial = sourceBytes.substr(0, 4);

  TemporaryUploadFile source{sourceBytes};
  UploadFtpFixture server{partial, UploadReplies{.stor = 550, .rejectFirstStorOnly = true}};

  std::vector<Diagnostic> diagnostics;

  const auto session = MakeFtpSession();

  REQUIRE(session->Connect(server.Site(), Callbacks(diagnostics), {}));

  TransferOptions options;
  options.resumeOffset = partial.size();

  const auto resumed = session->Upload(source.Path(), RemotePath{"/payload.bin"}, options, {}, {});

  REQUIRE_FALSE(resumed);
  CHECK(resumed.error().code == RemoteErrorCode::Unsupported);
  CheckNoUploadFinalization(server.GetTranscript());

  options.resumeOffset = 0;

  const auto fresh = session->Upload(source.Path(), RemotePath{"/payload.bin"}, options, {}, {});

  INFO((fresh ? "Uploaded" : fresh.error().message));
  REQUIRE(fresh);

  session->Disconnect();
  server.Stop();

  const auto transcript = server.GetTranscript();

  INFO(transcript.failure);
  CHECK(transcript.failure.empty());
  CHECK(transcript.final == std::optional<std::string>{sourceBytes});
  CHECK(transcript.receivedBytes == sourceBytes);
  CHECK_FALSE(transcript.renamedBeforeCompletion);
  CHECK(CommandCount(transcript, "USER") >= 2);
  CHECK(CommandCount(transcript, "REST") == 1);
  CHECK(CommandCount(transcript, "STOR") == 2);
  CHECK(CommandCount(transcript, "APPE") == 0);
  CHECK(CommandCount(transcript, "RNFR") == 1);
  CHECK(CommandCount(transcript, "RNTO") == 1);

  CheckTransferLowSpeedPolicy(diagnostics, "STOR", 2);
}

TEST_CASE("FTP preserves observed final upload storage and processing failures",
          "[ftp][loopback][upload-resume][upload-completion]")
{
  int completion = 552;

  bool resumed = true;

  SECTION("552 reports exceeded allocation for a resumed upload") {}
  SECTION("552 reports exceeded allocation for a fresh upload") { resumed = false; }
  SECTION("452 reports insufficient storage for a resumed upload") { completion = 452; }
  SECTION("452 reports insufficient storage for a fresh upload")
  {
    completion = 452;
    resumed = false;
  }
  SECTION("451 reports resumed upload processing failure without guessing quota") { completion = 451; }
  SECTION("451 reports fresh upload processing failure without guessing quota")
  {
    completion = 451;
    resumed = false;
  }

  const std::string sourceBytes{"saved checkpoint plus upload remainder"};
  const auto partial = sourceBytes.substr(0, 4);

  const auto offset = resumed ? partial.size() : 0U;

  TemporaryUploadFile source{sourceBytes};
  UploadFtpFixture server{resumed ? std::optional<std::string>{partial} : std::nullopt,
                          UploadReplies{.completion = completion}};

  std::vector<Diagnostic> diagnostics;

  const auto session = MakeFtpSession();

  REQUIRE(session->Connect(server.Site(), Callbacks(diagnostics), {}));

  TransferOptions options;
  options.resumeOffset = offset;
  options.temporaryRemotePath = RemotePath{"/payload.bin.havremote.part"};

  bool finalized{};

  options.beforeFinalize = [&finalized](const bool overwrite) -> Result<bool>
  {
    finalized = true;
    return overwrite;
  };

  const auto uploaded = session->Upload(source.Path(), RemotePath{"/payload.bin"},
                                        options, {}, {});

  session->Disconnect();
  server.Stop();

  INFO((uploaded ? "Unexpected upload completion success" : uploaded.error().message));
  REQUIRE_FALSE(uploaded);
  CHECK(uploaded.error().code == RemoteErrorCode::RemoteIo);
  CHECK(uploaded.error().message.find(std::to_string(completion)) != std::string::npos);
  CHECK_FALSE(finalized);
  CHECK(options.resumeOffset == offset);
  CHECK(options.temporaryRemotePath ==
        std::optional<RemotePath>{RemotePath{"/payload.bin.havremote.part"}});
  CHECK(ReadFileBytes(source.Path()) == sourceBytes);

  if (completion == 451)
  {
    CHECK(uploaded.error().message.find("quota") == std::string::npos);
    CHECK(uploaded.error().message.find("storage") == std::string::npos);
    CHECK(uploaded.error().message.find("allocation") == std::string::npos);
  }
  else
  {
    CHECK((uploaded.error().message.find("storage") != std::string::npos ||
           uploaded.error().message.find("allocation") != std::string::npos));
  }

  const auto reply = std::to_string(completion) + " Upload fixture completion";

  CHECK(std::ranges::count_if(diagnostics, [&reply](const Diagnostic &entry)
                              { return entry.text == "FTP STOR final control reply: " + reply; }) == 1);

  const auto transcript = server.GetTranscript();

  CheckNoUploadFinalization(transcript);

  CHECK(transcript.completionRepliesSent == std::vector<std::string>{reply});
  CHECK(transcript.partial == std::optional<std::string>{sourceBytes});
  CHECK(transcript.receivedBytes == sourceBytes.substr(offset));
  CHECK_FALSE(transcript.transferCompleted);
  CHECK(CommandCount(transcript, "USER") == 1);
  CHECK(CommandCount(transcript, "PASS") == 1);
  CHECK(CommandCount(transcript, "REST") == (resumed ? 1 : 0));
  CHECK(CommandCount(transcript, "STOR") == 1);
  CHECK(CommandCount(transcript, "NOOP") == 0);

  CheckTransferLowSpeedPolicy(diagnostics, "STOR");

  CheckFailureDiagnostics(diagnostics);
}

TEST_CASE("FTP data resets preserve upload partials without inventing an unobserved server reply",
          "[ftp][loopback][upload-resume][upload-completion]")
{
  UploadReplies replies{.resetUploadAfterPrefix = true};

  SECTION("data reset with no final control reply") {}
  SECTION("server sends 552 before resetting the data connection")
  {
    replies.resetReply = 552;
  }
  SECTION("server attempts 552 after resetting the data connection")
  {
    replies.resetReply = 552;
    replies.resetReplyAfterData = true;
  }

  // Keep the client in STOR when the peer resets: socket buffering must not
  // turn this into an already-completed upload awaiting its final reply.
  const std::string sourceBytes(16U * 1024U * 1024U, 'x');
  const auto partial = sourceBytes.substr(0, 4);

  TemporaryUploadFile source{sourceBytes};

  const auto sourceModified = std::filesystem::last_write_time(source.Path());

  UploadFtpFixture server{partial, replies};

  std::vector<Diagnostic> diagnostics;

  const auto session = MakeFtpSession();

  REQUIRE(session->Connect(server.Site(), Callbacks(diagnostics), {}));

  TransferOptions options;
  options.resumeOffset = partial.size();
  options.temporaryRemotePath = RemotePath{"/payload.bin.havremote.part"};

  bool finalized{};

  options.beforeFinalize = [&finalized](const bool overwrite) -> Result<bool>
  {
    finalized = true;
    return overwrite;
  };

  const auto uploaded = session->Upload(source.Path(), RemotePath{"/payload.bin"},
                                        options, {}, {});

  session->Disconnect();
  server.Stop();

  INFO((uploaded ? "Unexpected upload success after data reset" : uploaded.error().message));
  REQUIRE_FALSE(uploaded);

  const std::string observedReply{
      "FTP STOR final control reply: 552 Fixture upload storage allocation exceeded"};

  const bool observed552 = std::ranges::any_of(diagnostics, [&observedReply](const Diagnostic &entry)
                                               { return entry.text == observedReply; });

  if (observed552)
  {
    // Only a reply actually delivered by libcurl may justify server-specific
    // classification. The fixture merely sending it is not sufficient.
    REQUIRE(replies.resetReply == std::optional<int>{552});
    CHECK(uploaded.error().code == RemoteErrorCode::RemoteIo);
    CHECK(uploaded.error().message.find("552") != std::string::npos);
  }
  else
  {
    CHECK(uploaded.error().code == RemoteErrorCode::ConnectionLost);
    CHECK(uploaded.error().nativeCode == static_cast<int>(CURLE_SEND_ERROR));
    CHECK(uploaded.error().message.find("CURLcode 55") != std::string::npos);
    CHECK(uploaded.error().message.find("FTP response code 150") != std::string::npos);
    CHECK(uploaded.error().message.find("quota") == std::string::npos);
    CHECK(uploaded.error().message.find("storage") == std::string::npos);
    CHECK(uploaded.error().message.find("552") == std::string::npos);
    CHECK(std::ranges::count_if(diagnostics, [](const Diagnostic &entry)
                                { return entry.text ==
                                         "FTP STOR final control reply: unavailable (libcurl did not deliver a complete final STOR reply, last FTP response code 150)"; }) == 1);
  }
  CHECK_FALSE(finalized);
  CHECK(options.resumeOffset == partial.size());
  CHECK(options.temporaryRemotePath ==
        std::optional<RemotePath>{RemotePath{"/payload.bin.havremote.part"}});
  CHECK(std::filesystem::file_size(source.Path()) == sourceBytes.size());
  CHECK(std::filesystem::last_write_time(source.Path()) == sourceModified);

  const auto transcript = server.GetTranscript();

  CheckNoUploadFinalization(transcript);

  CHECK(transcript.partial == std::optional<std::string>{partial + "x"});
  CHECK(transcript.receivedBytes == "x");
  CHECK_FALSE(transcript.transferCompleted);
  CHECK(CommandCount(transcript, "USER") == 1);
  CHECK(CommandCount(transcript, "PASS") == 1);
  CHECK(CommandCount(transcript, "REST") == 1);
  CHECK(CommandCount(transcript, "STOR") == 1);
  CHECK(CommandCount(transcript, "NOOP") == 0);

  if (!replies.resetReply)
  {
    CHECK(transcript.completionRepliesSent.empty());
    CHECK_FALSE(observed552);
  }
  else if (!replies.resetReplyAfterData || observed552)
  {
    CHECK(transcript.completionRepliesSent == std::vector<std::string>{
                                                  "552 Fixture upload storage allocation exceeded"});
  }

  // For the post-reset variant, curl may close the control socket before
  // the fixture can send its reply. Do not claim a reply that was not sent.
  CHECK(transcript.completionRepliesSent.size() <= 1U);

  CheckTransferLowSpeedPolicy(diagnostics, "STOR");

  CheckFailureDiagnostics(diagnostics);
}

TEST_CASE("FTP downloads tolerate a short stall independently of the command idle timeout",
          "[ftp][loopback][low-speed]")
{
  const std::string sourceBytes = std::string{"a"} + '\0' + "binary download\r\nremainder";

  TemporaryUploadFile destination{"original local contents"};
  UploadFtpFixture server{sourceBytes, {}, DownloadBehavior{.prefixSize = 1U, .pause = 4s}};

  std::vector<Diagnostic> diagnostics;

  const auto session = MakeFtpSession();

  // Callbacks() deliberately configures a two-second command idle timeout.
  // File transfers must retain their independent thirty-second threshold.
  REQUIRE(session->Connect(server.Site(), Callbacks(diagnostics), {}));
  CHECK(LowSpeedPolicyCount(diagnostics) == 0);

  TransferOptions options;
  options.overwrite = true;

  const auto started = std::chrono::steady_clock::now();
  const auto downloaded = session->Download(RemotePath{"/payload.bin.havremote.part"},
                                            destination.Path(), options, {}, {});
  const auto elapsed = std::chrono::steady_clock::now() - started;

  INFO((downloaded ? "Downloaded after short stall" : downloaded.error().message));
  REQUIRE(downloaded);
  CHECK(elapsed >= 4s);
  CHECK(ReadFileBytes(destination.Path()) == sourceBytes);

  auto part = destination.Path();
  part += ".havremote.part";

  CHECK_FALSE(std::filesystem::exists(part));

  session->Disconnect();
  server.Stop();

  const auto transcript = server.GetTranscript();

  INFO(transcript.failure);
  CHECK(transcript.failure.empty());
  CHECK(CommandCount(transcript, "USER") == 1);
  CHECK(CommandCount(transcript, "RETR") == 1);
  CHECK(CommandCount(transcript, "REST") == 0);
  CHECK(CommandCount(transcript, "NOOP") == 0);

  CheckTransferLowSpeedPolicy(diagnostics, "RETR");
}

TEST_CASE("FTP stalled downloads time out at thirty seconds and retain their partial file",
          "[.slow][ftp][loopback][low-speed]")
{
  const std::string sourceBytes{"a stalled download never sends its remaining bytes"};
  const std::string original{"preserve the existing destination"};

  TemporaryUploadFile destination{original};
  UploadFtpFixture server{sourceBytes, {}, DownloadBehavior{.prefixSize = 1U, .stallDownload = true}};

  std::vector<Diagnostic> diagnostics;

  const auto session = MakeFtpSession();

  auto sessionCallbacks = Callbacks(diagnostics);
  sessionCallbacks.commandIdleTimeout = 90s;

  REQUIRE(session->Connect(server.Site(), sessionCallbacks, {}));
  CHECK(LowSpeedPolicyCount(diagnostics) == 0);

  TransferOptions options;
  options.overwrite = true;

  bool finalized{};

  options.beforeFinalize = [&finalized](const bool overwrite) -> Result<bool>
  {
    finalized = true;
    return overwrite;
  };

  const auto started = std::chrono::steady_clock::now();
  const auto downloaded = session->Download(RemotePath{"/payload.bin.havremote.part"},
                                            destination.Path(), options, {}, {});
  const auto elapsed = std::chrono::steady_clock::now() - started;

  session->Disconnect();
  server.Stop();

  INFO((downloaded ? "Unexpected stalled download success" : downloaded.error().message));
  INFO("Elapsed seconds: " << std::chrono::duration<double>(elapsed).count());
  REQUIRE_FALSE(downloaded);
  CHECK(downloaded.error().code == RemoteErrorCode::TimedOut);
  CHECK(downloaded.error().nativeCode == static_cast<int>(CURLE_OPERATION_TIMEDOUT));
  CHECK(downloaded.error().message.find("CURLcode 28") != std::string::npos);
  CHECK(downloaded.error().message.find(
            "Less than 1 bytes/sec transferred the last 30 seconds") != std::string::npos);
  CHECK(elapsed >= 28s);
  CHECK(elapsed < 45s);
  CHECK_FALSE(finalized);
  CHECK_FALSE(downloaded.error().operationMayHaveSucceeded);
  CHECK(ReadFileBytes(destination.Path()) == original);

  auto part = destination.Path();
  part += ".havremote.part";

  CHECK(ReadFileBytes(part) == sourceBytes.substr(0, 1));

  const auto transcript = server.GetTranscript();

  INFO(transcript.failure);
  CHECK(transcript.failure.empty());
  CHECK(CommandCount(transcript, "USER") == 1);
  CHECK(CommandCount(transcript, "PASS") == 1);
  CHECK(CommandCount(transcript, "RETR") == 1);
  CHECK(CommandCount(transcript, "REST") == 0);
  CHECK(CommandCount(transcript, "NOOP") == 0);

  CheckTransferLowSpeedPolicy(diagnostics, "RETR");

  CheckFailureDiagnostics(diagnostics);
}

TEST_CASE("FTP stalled uploads time out at thirty seconds without retrying or finalizing",
          "[.slow][ftp][loopback][low-speed]")
{
  // A small advertised receive window and a source much larger than socket
  // buffers make this a stalled transfer rather than an already-sent upload
  // merely waiting for its final control reply. Client progress may include
  // buffered bytes beyond the single byte actually consumed by the server.
  const std::string sourceBytes(16U * 1024U * 1024U, 'x');

  TemporaryUploadFile source{sourceBytes};
  UploadFtpFixture server{std::nullopt, UploadReplies{.stallUpload = true}};

  std::vector<Diagnostic> diagnostics;

  const auto session = MakeFtpSession();

  auto sessionCallbacks = Callbacks(diagnostics);
  sessionCallbacks.commandIdleTimeout = 90s;

  REQUIRE(session->Connect(server.Site(), sessionCallbacks, {}));
  CHECK(LowSpeedPolicyCount(diagnostics) == 0);

  TransferOptions options;

  bool finalized{};

  options.beforeFinalize = [&finalized](const bool overwrite) -> Result<bool>
  {
    finalized = true;
    return overwrite;
  };

  const auto started = std::chrono::steady_clock::now();
  const auto uploaded = session->Upload(source.Path(), RemotePath{"/payload.bin"},
                                        options, {}, {});
  const auto elapsed = std::chrono::steady_clock::now() - started;

  session->Disconnect();
  server.Stop();

  INFO((uploaded ? "Unexpected stalled upload success" : uploaded.error().message));
  INFO("Elapsed seconds: " << std::chrono::duration<double>(elapsed).count());
  REQUIRE_FALSE(uploaded);
  CHECK(uploaded.error().code == RemoteErrorCode::TimedOut);
  CHECK(uploaded.error().nativeCode == static_cast<int>(CURLE_OPERATION_TIMEDOUT));
  CHECK(uploaded.error().message.find("CURLcode 28") != std::string::npos);
  CHECK(uploaded.error().message.find(
            "Less than 1 bytes/sec transferred the last 30 seconds") != std::string::npos);
  CHECK(elapsed >= 28s);
  CHECK(elapsed < 45s);
  CHECK_FALSE(finalized);
  CHECK(std::filesystem::file_size(source.Path()) == sourceBytes.size());

  const auto transcript = server.GetTranscript();

  CheckNoUploadFinalization(transcript);

  CHECK(transcript.partial == std::optional<std::string>{sourceBytes.substr(0, 1)});
  CHECK(transcript.receivedBytes == sourceBytes.substr(0, 1));
  CHECK(CommandCount(transcript, "USER") == 1);
  CHECK(CommandCount(transcript, "PASS") == 1);
  CHECK(CommandCount(transcript, "STOR") == 1);
  CHECK(CommandCount(transcript, "REST") == 0);
  CHECK(CommandCount(transcript, "NOOP") == 0);

  CheckTransferLowSpeedPolicy(diagnostics, "STOR");

  CheckFailureDiagnostics(diagnostics);
}
