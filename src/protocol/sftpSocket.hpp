// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_SRC_PROTOCOL_SFTP_SOCKET_HPP
#define HAVREMOTE_SRC_PROTOCOL_SFTP_SOCKET_HPP

#if !defined(_WIN32)

#include <libssh2.h>
#include <sys/socket.h>
#include <cerrno>

namespace havremote::detail
{
  inline bool PrepareSftpSocket(const libssh2_socket_t socket) noexcept
  {
#if defined(SO_NOSIGPIPE)
    const int enabled = 1;

    return setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE,
                      &enabled, sizeof(enabled)) == 0;
#else
    (void)socket;

    return true;
#endif
  }

  inline LIBSSH2_SEND_FUNC(SendWithoutSigpipe)
  {
    (void)abstract;

    // libssh2 also passes its privately owned SSH-agent socket here. Protect
    // both sockets without changing the process-wide signal disposition.
    if (!PrepareSftpSocket(socket))
    {
      const int error = errno;

      return error == EINTR || error == EWOULDBLOCK ? -EAGAIN : -error;
    }

#if defined(MSG_NOSIGNAL)
    flags |= MSG_NOSIGNAL;
#endif
    const auto sent = ::send(socket, buffer, length, flags);

    if (sent >= 0)
    {
      return sent;
    }

    const int error = errno;

    // Preserve the retry/error convention required by libssh2's send callback
    return error == EINTR || error == EWOULDBLOCK ? -EAGAIN : -error;
  }
}

#endif // !_WIN32

#endif // HAVREMOTE_SRC_PROTOCOL_SFTP_SOCKET_HPP
