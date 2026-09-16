// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UI_TRANSFER_LIST_MODEL_HPP
#define HAVREMOTE_INCLUDE_UI_TRANSFER_LIST_MODEL_HPP

#include "core/transferQueue.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace havremote::ui
{
  using TransferClipboardRow = std::vector<std::string>;

  // Formats displayed UTF-8 cells only, not the underlying queue jobs. Every
  // cell is sanitized independently before joining it into a tab-separated
  // table with a localized header and CRLF line endings. No selected rows
  // produce no text, allowing callers to leave the clipboard untouched.
  [[nodiscard]] std::string FormatTransferClipboardText(
      std::span<const std::string> headers,
      std::span<const TransferClipboardRow> rows);

  // UTF-8 CSV with a BOM, quoted cells, CRLF records, and spreadsheet-formula
  // protection. Empty selections produce no report, just like clipboard copy.
  [[nodiscard]] std::string FormatTransferCsvText(
      std::span<const std::string> headers,
      std::span<const TransferClipboardRow> rows);

  // Returns the percentage to present in the transfer queue. Completed jobs
  // are always 100%, including valid empty-file transfers whose progress is
  // 0/0. An empty result means that no percentage can be calculated and the
  // UI should show the transferred byte count instead.
  [[nodiscard]] std::optional<std::uint32_t> TransferProgressPercent(
      TransferState state,
      const TransferProgress &progress) noexcept;

  // Throughput is based only on bytes physically transferred during the
  // current attempt. Resume offsets and conflict-policy skips therefore do not
  // inflate the displayed rate. Metrics are available only while work is
  // actively running.
  [[nodiscard]] std::optional<std::uint64_t> TransferSpeedBytesPerSecond(
      TransferState state,
      const TransferProgress &progress) noexcept;

  [[nodiscard]] std::optional<std::chrono::seconds> TransferEstimatedTimeRemaining(
      TransferState state,
      const TransferProgress &progress) noexcept;

  // Returns true only for the edge into Completed. Callers can apply their
  // own direction and purpose filters before deciding which browser to
  // refresh.
  [[nodiscard]] bool IsNewlyCompletedTransfer(
      std::span<const QueuedTransfer> previous,
      const QueuedTransfer &current) noexcept;

  // Notify once per observed transfer attempt, not when loading an existing
  // failed queue. Callers exclude external-editor jobs with recovery dialogs.
  [[nodiscard]] bool IsNewlyFailedTransfer(
      std::span<const QueuedTransfer> previous,
      const QueuedTransfer &current) noexcept;

  // Detects the completion edge used to refresh a connection tab's local
  // browser. Later queue snapshots containing the same completed download do
  // not trigger another refresh.
  [[nodiscard]] bool HasNewlyCompletedDownload(
      std::span<const QueuedTransfer> previous,
      std::span<const QueuedTransfer> current) noexcept;
} // namespace havremote::ui

#endif // HAVREMOTE_INCLUDE_UI_TRANSFER_LIST_MODEL_HPP
