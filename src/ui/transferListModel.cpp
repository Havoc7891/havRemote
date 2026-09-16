// SPDX-License-Identifier: MIT

#include "ui/transferListModel.hpp"

#include "core/logSanitizer.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <limits>

namespace havremote::ui
{
  namespace
  {
    std::string SanitizedTransferCell(const std::string &cell)
    {
      auto singleLine = cell;

      for (auto &character : singleLine)
      {
        const auto byte = static_cast<unsigned char>(character);

        if (byte < 0x20U || byte == 0x7fU)
        {
          character = ' ';
        }
      }

      return SanitizeDiagnosticText(singleLine);
    }

    bool StartsSpreadsheetFormula(std::string_view cell)
    {
      // ASCII whitespace has already been flattened. Preserve Unicode space
      // characters in the report, but do not let them hide a formula prefix.
      constexpr std::string_view spaces[]{
          "\xc2\x85", "\xc2\xa0", "\xe1\x9a\x80",
          "\xe2\x80\x80", "\xe2\x80\x81", "\xe2\x80\x82", "\xe2\x80\x83",
          "\xe2\x80\x84", "\xe2\x80\x85", "\xe2\x80\x86", "\xe2\x80\x87",
          "\xe2\x80\x88", "\xe2\x80\x89", "\xe2\x80\x8a", "\xe2\x80\xa8",
          "\xe2\x80\xa9", "\xe2\x80\xaf", "\xe2\x81\x9f", "\xe3\x80\x80",
          "\xef\xbb\xbf"};

      while (!cell.empty())
      {
        if (cell.front() == ' ')
        {
          cell.remove_prefix(1);

          continue;
        }

        const auto space = std::ranges::find_if(spaces, [&](const auto candidate)
                                                { return cell.starts_with(candidate); });
        if (space == std::end(spaces))
        {
          return std::string_view{"=+-@"}.contains(cell.front());
        }

        cell.remove_prefix(space->size());
      }

      return false;
    }
  } // namespace

  std::string FormatTransferClipboardText(
      const std::span<const std::string> headers,
      const std::span<const TransferClipboardRow> rows)
  {
    if (rows.empty())
    {
      return {};
    }

    std::string text;

    const auto appendRow = [&](const std::span<const std::string> cells)
    {
      bool first = true;

      for (const auto &cell : cells)
      {
        if (!first)
        {
          text.push_back('\t');
        }

        first = false;

        text += SanitizedTransferCell(cell);
      }

      text += "\r\n";
    };

    appendRow(headers);

    for (const auto &row : rows)
    {
      appendRow(row);
    }

    return text;
  }

  std::string FormatTransferCsvText(
      const std::span<const std::string> headers,
      const std::span<const TransferClipboardRow> rows)
  {
    if (rows.empty())
    {
      return {};
    }

    std::string text{"\xef\xbb\xbf"};

    const auto appendRow = [&](const std::span<const std::string> cells)
    {
      bool first = true;

      for (const auto &cell : cells)
      {
        if (!first)
        {
          text.push_back(',');
        }

        first = false;

        const auto sanitized = SanitizedTransferCell(cell);

        text.push_back('"');

        if (StartsSpreadsheetFormula(sanitized))
        {
          text.push_back('\'');
        }

        for (const char character : sanitized)
        {
          if (character == '"')
          {
            text.push_back('"');
          }

          text.push_back(character);
        }

        text += '"';
      }

      text += "\r\n";
    };

    appendRow(headers);

    for (const auto &row : rows)
    {
      appendRow(row);
    }

    return text;
  }

  std::optional<std::uint32_t> TransferProgressPercent(
      const TransferState state,
      const TransferProgress &progress) noexcept
  {
    if (state == TransferState::Completed)
    {
      return 100U;
    }

    if (!progress.totalBytes || *progress.totalBytes == 0U)
    {
      return std::nullopt;
    }

    const auto total = *progress.totalBytes;

    if (progress.bytesTransferred >= total)
    {
      return 100U;
    }

    // Find floor(bytes * 100 / total) without overflowing bytes * 100
    const auto reachesPercent = [&](const std::uint32_t percent)
    {
      const auto threshold =
          (total / 100U) * percent +
          ((total % 100U) * percent + 99U) / 100U;

      return progress.bytesTransferred >= threshold;
    };

    std::uint32_t lower = 0U;
    std::uint32_t upper = 99U;

    while (lower < upper)
    {
      const auto middle = lower + (upper - lower + 1U) / 2U;

      if (reachesPercent(middle))
      {
        lower = middle;
      }
      else
      {
        upper = middle - 1U;
      }
    }

    return lower;
  }

  std::optional<std::uint64_t> TransferSpeedBytesPerSecond(
      const TransferState state,
      const TransferProgress &progress) noexcept
  {
    if (state != TransferState::Running ||
        progress.activeBytesTransferred == 0U ||
        progress.elapsed <= std::chrono::steady_clock::duration::zero())
    {
      return std::nullopt;
    }

    const auto seconds =
        std::chrono::duration<long double>{progress.elapsed}.count();
    if (!(seconds > 0.0L))
    {
      return std::nullopt;
    }

    const auto rate =
        static_cast<long double>(progress.activeBytesTransferred) / seconds;
    if (!(rate > 0.0L))
    {
      return std::nullopt;
    }

    constexpr auto maximum =
        (std::numeric_limits<std::uint64_t>::max)();
    if (rate >= static_cast<long double>(maximum))
    {
      return maximum;
    }

    // Once at least one byte has moved, keep a very slow transfer measurable
    // instead of alternating between an unavailable ETA and 0 B/s.
    return std::max<std::uint64_t>(
        1U, static_cast<std::uint64_t>(rate));
  }

  std::optional<std::chrono::seconds> TransferEstimatedTimeRemaining(
      const TransferState state,
      const TransferProgress &progress) noexcept
  {
    if (!progress.totalBytes ||
        progress.bytesTransferred >= *progress.totalBytes)
    {
      return std::nullopt;
    }

    const auto speed = TransferSpeedBytesPerSecond(state, progress);
    if (!speed || *speed == 0U)
    {
      return std::nullopt;
    }

    const auto remaining = *progress.totalBytes - progress.bytesTransferred;
    const auto seconds = remaining / *speed +
                         (remaining % *speed == 0U ? 0U : 1U);

    using SecondsRep = std::chrono::seconds::rep;

    constexpr auto maximumSeconds =
        static_cast<std::uint64_t>((std::numeric_limits<SecondsRep>::max)());

    return std::chrono::seconds{
        static_cast<SecondsRep>(std::min(seconds, maximumSeconds))};
  }

  bool IsNewlyCompletedTransfer(
      const std::span<const QueuedTransfer> previous,
      const QueuedTransfer &current) noexcept
  {
    if (current.state != TransferState::Completed)
    {
      return false;
    }

    const auto prior = std::ranges::find_if(
        previous,
        [&](const QueuedTransfer &candidate)
        {
          return candidate.job.id == current.job.id;
        });

    return prior == previous.end() ||
           prior->state != TransferState::Completed;
  }

  bool IsNewlyFailedTransfer(
      const std::span<const QueuedTransfer> previous,
      const QueuedTransfer &current) noexcept
  {
    if (current.state != TransferState::Failed ||
        (current.error &&
         (current.error->code == RemoteErrorCode::Cancelled ||
          current.error->code == RemoteErrorCode::Paused)))
    {
      return false;
    }

    const auto prior = std::ranges::find(previous, current.job.id,
                                         [](const QueuedTransfer &transfer)
                                         { return transfer.job.id; });

    return prior != previous.end() && current.attempt >= prior->attempt &&
           (prior->state != TransferState::Failed ||
            current.attempt > prior->attempt);
  }

  bool HasNewlyCompletedDownload(
      const std::span<const QueuedTransfer> previous,
      const std::span<const QueuedTransfer> current) noexcept
  {
    return std::ranges::any_of(
        current,
        [&](const QueuedTransfer &transfer)
        {
          return transfer.job.direction == TransferDirection::Download &&
                 IsNewlyCompletedTransfer(previous, transfer);
        });
  }
} // namespace havremote::ui
