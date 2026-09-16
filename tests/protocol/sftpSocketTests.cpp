// SPDX-License-Identifier: MIT

#include "protocol/sftpSocket.hpp"

#include <catch2/catch_test_macros.hpp>

#if !defined(_WIN32)

#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <array>

using namespace havremote;

namespace
{
  class SocketPair final
  {
  public:
    SocketPair() { REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, mSockets.data()) == 0); }
    SocketPair(const SocketPair &) = delete;
    SocketPair &operator=(const SocketPair &) = delete;
    ~SocketPair()
    {
      for (const auto socket : mSockets)
      {
        if (socket >= 0)
        {
          ::close(socket);
        }
      }
    }
    [[nodiscard]] int Writer() const { return mSockets[0]; }
    [[nodiscard]] int Reader() const { return mSockets[1]; }
    void CloseReader()
    {
      ::close(mSockets[1]);
      mSockets[1] = -1;
    }

  private:
    std::array<int, 2> mSockets{-1, -1};
  };
}

TEST_CASE("SFTP writes to a closed peer return an error without SIGPIPE",
          "[protocol][sftp][sockets]")
{
  SocketPair pair;
  pair.CloseReader();

  const auto child = fork();

  REQUIRE(child >= 0);

  if (child == 0)
  {
    struct sigaction action{};
    action.sa_handler = SIG_DFL;

    sigemptyset(&action.sa_mask);

    if (sigaction(SIGPIPE, &action, nullptr) != 0)
    {
      _exit(2);
    }

    const char byte = 'x';
    const auto sent = detail::SendWithoutSigpipe(pair.Writer(), &byte, 1, 0, nullptr);

    _exit(sent == -EPIPE || sent == -ECONNRESET ? 0 : 3);
  }

  int status{};

  REQUIRE(waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("SFTP send callback preserves bytes and retryable backpressure",
          "[protocol][sftp][sockets]")
{
  SocketPair pair;

  REQUIRE(detail::PrepareSftpSocket(pair.Writer()));

  const int oldFlags = fcntl(pair.Writer(), F_GETFL, 0);

  REQUIRE(oldFlags >= 0);
  REQUIRE(fcntl(pair.Writer(), F_SETFL, oldFlags | O_NONBLOCK) == 0);

  constexpr std::array<char, 3> message{'a', '\0', 'b'};

  REQUIRE(detail::SendWithoutSigpipe(pair.Writer(), message.data(), message.size(),
                                     0, nullptr) == static_cast<ssize_t>(message.size()));

  std::array<char, 3> received{};

  REQUIRE(recv(pair.Reader(), received.data(), received.size(), 0) ==
          static_cast<ssize_t>(received.size()));
  CHECK(received == message);

  const std::array<char, 65536> block{};

  bool retryable{};

  for (unsigned attempt = 0; attempt < 1024; ++attempt)
  {
    const auto sent = detail::SendWithoutSigpipe(pair.Writer(), block.data(), block.size(),
                                                 0, nullptr);

    if (sent == -EAGAIN)
    {
      retryable = true;

      break;
    }

    REQUIRE(sent >= 0);
  }
  CHECK(retryable);
}

#endif // !_WIN32
