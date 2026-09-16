// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_SRC_PROTOCOL_FTP_CONTROL_TRACE_HPP
#define HAVREMOTE_SRC_PROTOCOL_FTP_CONTROL_TRACE_HPP

#include "core/logSanitizer.hpp"
#include "core/types.hpp"
#include "protocol/ftpErrors.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>

namespace havremote::ftp
{
  // Opt-in control-connection trace with per-session limits.
  // Observes callbacks without issuing commands or reading sockets.
  class ControlTrace final
  {
  public:
    void Initialize(const DiagnosticCallback *sink, const std::string *password,
                    const bool enabled) noexcept
    {
      mSink = enabled ? sink : nullptr;
      mPassword = password;
      mHandle = nullptr;
      mSession = ++mNextSession;
      mOperation = 0;
      mEvents = 0;
      mLimitReported = false;
      mLastConnection = -1;
      mFinished = {};
      mStarted = Clock::now();
    }

    [[nodiscard]] static bool Requested() noexcept
    {
      const auto *value = std::getenv("HAVREMOTE_FTP_CONTROL_TRACE");
      return value != nullptr && std::string_view{value} == "1";
    }

    [[nodiscard]] bool Enabled() const noexcept { return mSink && *mSink; }

    void Begin(CURL *handle, const std::string_view operation) noexcept
    {
      mHandle = handle;

      if (mOperation == mMaximumOperations)
      {
        Publish("operation limit reached. Further control tracing suppressed");
      }

      ++mOperation;

      if (!Enabled())
      {
        return;
      }

      try
      {
        const auto idle = mFinished == Clock::time_point{} ? 0LL : std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - mFinished).count();

        Publish("begin " + std::string{operation} + ", idle_ms=" +
                std::to_string(idle) + ", previous control connection id=" +
                std::to_string(mLastConnection) +
                ". App session marked connected (not a socket-liveness check)");
      }
      catch (...)
      {
      }
    }

    void Endpoint(const char *remote, const int remotePort,
                  const char *local, const int localPort) noexcept
    {
      if (!Enabled() || !mHandle)
      {
        return;
      }

      try
      {
        curl_off_t id{-1};

        curl_easy_getinfo(mHandle, CURLINFO_CONN_ID, &id);

        Record("control connection id=" + std::to_string(id) +
               (id < 0 ? " (identity unavailable)" : id == mLastConnection ? " (reused)"
                                                 : mLastConnection < 0     ? " (first established)"
                                                                           : " (new, prior connection replaced)") +
               ", local=" + Address(local, localPort) +
               ", remote=" + Address(remote, remotePort));
      }
      catch (...)
      {
      }
    }

    void Header(const bool incoming, const std::string_view input) noexcept
    {
      if (!Enabled())
      {
        return;
      }

      try
      {
        const auto first = input.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos)
        {
          return;
        }

        const auto line = input.substr(first);
        if (incoming)
        {
          // Never log server reply text: servers may echo credential material
          const bool code = line.size() >= 3 &&
                            line[0] >= '0' && line[0] <= '9' &&
                            line[1] >= '0' && line[1] <= '9' &&
                            line[2] >= '0' && line[2] <= '9';

          Record(code ? "FTP response " + std::string{line.substr(0, 3)} : "FTP response <continuation>");
        }
        else
        {
          const auto end = line.find_first_of(" \t\r\n");
          const auto token = line.substr(0, (std::min)(end, std::size_t{16}));
          const bool command = !token.empty() && std::ranges::all_of(token,
                                                                     [](const char c)
                                                                     { return c >= 'A' && c <= 'Z'; });

          // All command arguments are omitted, including paths and USER/PASS
          Record(command ? "FTP command " + std::string{token} : "FTP command <unparsed>");
        }
      }
      catch (...)
      {
      }
    }

    void Text(const std::string_view input) noexcept
    {
      if (!Enabled())
      {
        return;
      }

      // Only connection lifecycle messages, with no payload, TLS records, auth
      // negotiation details, or arbitrary server text from verbose output.
      for (const auto prefix : {"Connection ", "Reusing ", "Connected ",
                                "Trying ", "  Trying ", "Too old connection",
                                "closing connection", "shutting down connection",
                                "We got a 421", "Recv failure", "Send failure",
                                "Failed to connect", "Could not resolve",
                                "Issue another request", "connect to ",
                                "connection has input pending, not reusable",
                                "Immediate connect fail"})
      {
        if (input.starts_with(prefix))
        {
          try
          {
            Record("libcurl: " + std::string{input});
          }
          catch (...)
          {
          }

          return;
        }
      }
    }

    void Finish(const CURLcode result, const std::string_view errorBuffer) noexcept
    {
      mFinished = Clock::now();

      if (!Enabled() || !mHandle)
      {
        return;
      }

      try
      {
        long response{};
        long osError{};

        curl_off_t id{-1};

        curl_easy_getinfo(mHandle, CURLINFO_RESPONSE_CODE, &response);
        curl_easy_getinfo(mHandle, CURLINFO_OS_ERRNO, &osError);
        curl_easy_getinfo(mHandle, CURLINFO_CONN_ID, &id);

        Publish("end: " + FormatFailureDetail(result, errorBuffer, response, osError) +
                (response == 0 ? ", FTP response code 0 (none received)" : "") +
                ", control connection id=" + std::to_string(id) +
                ", backend result=" + (result == CURLE_OK ? "success" : "failure") +
                " (caller applies operation context)");

        if (id >= 0)
        {
          mLastConnection = id;
        }
      }
      catch (...)
      {
      }

      mHandle = nullptr;
    }

    void Record(const std::string_view message) noexcept
    {
      if (!Enabled())
      {
        return;
      }

      if (mEvents < mMaximumEvents)
      {
        ++mEvents;

        Publish(message);
      }
      else if (!mLimitReported)
      {
        mLimitReported = true;

        Publish("event limit reached. Further wire messages suppressed");
      }
    }

  private:
    using Clock = std::chrono::steady_clock;

    static std::string Address(const char *host, const int port)
    {
      return "[" + std::string{host ? host : "<unknown>"} + "]:" + std::to_string(port);
    }

    void Publish(const std::string_view message) noexcept
    {
      if (!Enabled() || mOperation > mMaximumOperations)
      {
        return;
      }

      try
      {
        std::string safe{message};

        if (mPassword && !mPassword->empty())
        {
          std::size_t position{};

          while ((position = safe.find(*mPassword, position)) != std::string::npos)
          {
            safe.replace(position, mPassword->size(), "<redacted>");

            position += 10;
          }
        }

        safe = SanitizeDiagnosticText(safe);

        if (safe.size() > 768)
        {
          safe = safe.substr(0, 765) + "...";
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 Clock::now() - mStarted)
                                 .count();

        (*mSink)(DiagnosticLevel::Debug,
                 "FTP control trace [session=" + std::to_string(mSession) +
                     ", operation=" + std::to_string(mOperation) + ", t=" +
                     std::to_string(elapsed) + "ms]: " + safe);
      }
      catch (...)
      {
      }
    }

    static constexpr std::size_t mMaximumEvents = 256;
    static constexpr std::size_t mMaximumOperations = 32;
    inline static std::atomic<std::uint64_t> mNextSession{};
    const DiagnosticCallback *mSink{};
    const std::string *mPassword{};
    CURL *mHandle{};
    std::uint64_t mSession{};
    std::size_t mOperation{};
    std::size_t mEvents{};
    bool mLimitReported{};
    curl_off_t mLastConnection{-1};
    Clock::time_point mStarted{};
    Clock::time_point mFinished{};
  };
}

#endif // HAVREMOTE_SRC_PROTOCOL_FTP_CONTROL_TRACE_HPP
