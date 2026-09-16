// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_SRC_PROTOCOL_FTP_UPLOAD_REPLY_HPP
#define HAVREMOTE_SRC_PROTOCOL_FTP_UPLOAD_REPLY_HPP

#include "core/logSanitizer.hpp"
#include "protocol/ftpErrors.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace havremote::ftp
{
  // Observes control-channel commands and complete replies parsed by libcurl.
  // It never reads a socket, consumes transfer data, or changes curl's result.
  class UploadReplyObserver final
  {
  public:
    void Reset(const std::string *password) noexcept
    {
      mPassword = password;
      mTracking = false;

      ClearPending();

      mReply.reset();
    }

    void Header(const bool incoming, const std::string_view input) noexcept
    {
      // HEADER_OUT can contain only the command prefix initially written to
      // the socket. This is sufficient to identify STOR. It need not end in LF.
      if (!incoming)
      {
        mTracking = input.starts_with("STOR ");

        ClearPending();

        if (mTracking)
        {
          mReply.reset();
        }

        return;
      }

      // Incoming callbacks contain complete response lines. Never promote a
      // partial line to a terminal reply.
      if (input.empty() || input.back() != '\n')
      {
        return;
      }

      try
      {
        auto line = input.substr(0, input.size() - 1);

        if (!line.empty() && line.back() == '\r')
        {
          line.remove_suffix(1);
        }

        if (!mTracking)
        {
          return;
        }

        const auto code = ReplyCode(line);
        const bool terminal = code != 0 &&
                              (line.size() == 3 || line[3] == ' ');

        if (mMultilineCode != 0)
        {
          // Only the same code followed by a space terminates an RFC 959
          // multiline reply. Other numeric lines are continuation text.
          const bool matching = code == mMultilineCode;

          AppendBody(matching ? Body(line) : line);

          if (matching && terminal && line.size() > 3)
          {
            Finish(mMultilineCode);
          }

          return;
        }

        if (code == 0)
        {
          return;
        }

        mMultilineCode = code;

        AppendBody(Body(line));

        if (terminal)
        {
          Finish(code);
        }
      }
      catch (...)
      {
        // Observation must not abort a transfer, even on allocation failure
        mTracking = false;

        ClearPending();
      }
    }

    [[nodiscard]] const UploadReply *Reply() const noexcept
    {
      return mReply ? &*mReply : nullptr;
    }

  private:
    [[nodiscard]] static long ReplyCode(const std::string_view line) noexcept
    {
      if (line.size() < 3 || line[0] < '1' || line[0] > '5' ||
          line[1] < '0' || line[1] > '9' ||
          line[2] < '0' || line[2] > '9' ||
          (line.size() > 3 && line[3] != ' ' && line[3] != '-'))
      {
        return 0;
      }

      return (line[0] - '0') * 100L + (line[1] - '0') * 10L + line[2] - '0';
    }

    [[nodiscard]] static std::string_view Body(const std::string_view line) noexcept
    {
      return line.size() > 3 ? line.substr(4) : std::string_view{};
    }

    void ClearPending() noexcept
    {
      mMultilineCode = 0;

      mPending.clear();

      mLines = 0;

      mTruncated = false;
    }

    void AppendBody(const std::string_view input)
    {
      // Preliminary (1xx) responses never supply a final upload diagnostic
      if (mMultilineCode < 200)
      {
        return;
      }

      if (mLines == mMaximumLines || mPending.size() == mMaximumTextBytes)
      {
        mTruncated = true;

        return;
      }

      ++mLines;

      if (!mPending.empty() && !input.empty())
      {
        mPending += ' ';
      }

      const auto capacity = mMaximumTextBytes - mPending.size();

      std::string safe;
      safe.reserve((std::min)(input.size(), capacity));

      std::size_t offset{};

      while (offset < input.size() && safe.size() < capacity)
      {
        // Compare against the original, untruncated line. A password beginning
        // before the text limit and ending after it must not leak a prefix.
        if (mPassword && !mPassword->empty() &&
            input.substr(offset).starts_with(*mPassword))
        {
          constexpr std::string_view hidden{"<redacted>"};

          safe.append(hidden.substr(0, capacity - safe.size()));

          offset += mPassword->size();
        }
        else
        {
          const auto byte = static_cast<unsigned char>(input[offset++]);

          safe += byte < 0x20U || byte == 0x7fU ? ' ' : static_cast<char>(byte);
        }
      }

      mTruncated |= offset < input.size();

      safe = SanitizeDiagnosticText(safe);

      if (safe.size() > capacity)
      {
        safe.resize(capacity);

        mTruncated = true;
      }

      mPending += safe;
    }

    void Finish(const long code)
    {
      if (code >= 200 && (!mReply || mReply->code < 400 || code >= 400))
      {
        auto text = mPending;

        if (mTruncated)
        {
          text += "...";
        }

        mReply = UploadReply{code, std::move(text)};
      }

      ClearPending();
    }

    // Leave room for an ellipsis within the 512-byte public reply-text bound
    static constexpr std::size_t mMaximumTextBytes = 509;
    static constexpr std::size_t mMaximumLines = 16;
    const std::string *mPassword{};
    bool mTracking{};
    long mMultilineCode{};
    std::string mPending;
    std::size_t mLines{};
    bool mTruncated{};
    std::optional<UploadReply> mReply;
  };
} // namespace havremote::ftp

#endif // HAVREMOTE_SRC_PROTOCOL_FTP_UPLOAD_REPLY_HPP
