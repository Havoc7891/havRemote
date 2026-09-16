// SPDX-License-Identifier: MIT

#include "ui/transferListModel.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace havremote;
using namespace havremote::ui;
using namespace std::chrono_literals;

namespace
{
  QueuedTransfer Transfer(std::string id,
                          const TransferDirection direction,
                          const TransferState state)
  {
    return QueuedTransfer{
        .job = TransferJob{.id = std::move(id),
                           .siteId = "site",
                           .siteEndpoint = std::nullopt,
                           .direction = direction,
                           .localPath = "item",
                           .remotePath = RemotePath{"/item"},
                           .recursive = false,
                           .conflictPolicy = ConflictPolicy::Ask,
                           .expectedRemoteRevision = std::nullopt},
        .state = state,
        .progress = {},
        .error = std::nullopt,
        .attempt = 0};
  }
} // namespace

TEST_CASE("transfer clipboard text includes headers and displayed cells as a TSV table")
{
  const std::vector<std::string> headers{"Direction", "Item", "Progress", "Details"};
  const std::vector<TransferClipboardRow> rows{
      {"Upload", "report.txt", "100%", "Completed"}};

  CHECK(FormatTransferClipboardText(headers, rows) ==
        "Direction\tItem\tProgress\tDetails\r\n"
        "Upload\treport.txt\t100%\tCompleted\r\n");
}

TEST_CASE("transfer clipboard text preserves Unicode row order and empty columns")
{
  const std::vector<std::string> headers{"Verbindung", "Element", "Fortschritt", "Details"};
  const std::vector<TransferClipboardRow> rows{
      {"Büro", "Grüße.txt", "", "Übertragung fehlgeschlagen"},
      {"東京", "資料🔌.txt", "100%", ""},
      {"", "last.txt", "0 B", "Queued"}};

  CHECK(FormatTransferClipboardText(headers, rows) ==
        "Verbindung\tElement\tFortschritt\tDetails\r\n"
        "Büro\tGrüße.txt\t\tÜbertragung fehlgeschlagen\r\n"
        "東京\t資料🔌.txt\t100%\t\r\n"
        "\tlast.txt\t0 B\tQueued\r\n");
}

TEST_CASE("transfer clipboard text is empty when no rows are selected")
{
  const std::vector<std::string> headers{"Item", "State"};

  CHECK(FormatTransferClipboardText(headers, {}).empty());
  CHECK(FormatTransferClipboardText({}, {}).empty());

  const std::vector<std::string> emptyHeaders{"", ""};
  const std::vector<TransferClipboardRow> emptyCells{{"", ""}};

  CHECK(FormatTransferClipboardText(emptyHeaders, emptyCells) == "\t\r\n\t\r\n");
}

TEST_CASE("transfer clipboard cells cannot inject tab separators or line breaks")
{
  const std::vector<std::string> headers{"Item\tName", "Error\r\nDetails"};
  const std::vector<TransferClipboardRow> rows{
      {"file\tname\r\n.txt", std::string{"before\0after", 12}}};

  CHECK(FormatTransferClipboardText(headers, rows) ==
        "Item Name\tError  Details\r\n"
        "file name  .txt\tbefore after\r\n");

  std::string controls;

  for (unsigned int byte = 0; byte < 0x20U; ++byte)
  {
    controls.push_back(static_cast<char>(byte));
  }

  controls.push_back('\x7f');

  const std::vector<std::string> controlHeader{controls};
  const std::vector<TransferClipboardRow> controlRows{{controls}};
  const std::string expected(controls.size(), ' ');

  CHECK(FormatTransferClipboardText(controlHeader, controlRows) ==
        expected + "\r\n" + expected + "\r\n");
}

TEST_CASE("transfer clipboard redacts every cell independently without changing its source")
{
  const std::vector<std::string> headers{
      "password=header-secret", "Details", "Address", "Command", "State"};

  const std::vector<TransferClipboardRow> rows{
      {"file.txt", "password: upload-secret", "ftp://alice:url-secret@example.test/path",
       "PASS command-secret", "Failed"},
      {"other.txt", "token\t= token-secret", "sftp://example.test/path",
       "passphrase\x7f= phrase-secret", "Still visible"}};

  const auto originalHeaders = headers;
  const auto originalRows = rows;

  CHECK(FormatTransferClipboardText(headers, rows) ==
        "password=<redacted>\tDetails\tAddress\tCommand\tState\r\n"
        "file.txt\tpassword: <redacted>\tftp://<credentials>@example.test/path\t"
        "PASS <redacted>\tFailed\r\n"
        "other.txt\ttoken = <redacted>\tsftp://example.test/path\t"
        "passphrase = <redacted>\tStill visible\r\n");
  CHECK(headers == originalHeaders);
  CHECK(rows == originalRows);
}

TEST_CASE("transfer clipboard text retains complete long details")
{
  const std::vector<std::string> headers{"Item", "Details"};
  const auto detail = std::string(16'384, 'x') + " / Grüße aus 東京";
  const std::vector<TransferClipboardRow> rows{{"report.txt", detail}};

  CHECK(FormatTransferClipboardText(headers, rows) ==
        "Item\tDetails\r\nreport.txt\t" + detail + "\r\n");
}

TEST_CASE("transfer CSV uses a UTF-8 BOM quoted fields and CRLF records", "[reports][csv]")
{
  const std::vector<std::string> headers{"Verbindung", "Element", "Details"};
  const std::vector<TransferClipboardRow> rows{
      {"Büro", "Grüße, \"東京\".txt", ""},
      {"", "資料🔌.txt", "Completed"}};

  CHECK(FormatTransferCsvText(headers, rows) ==
        "\xef\xbb\xbf"
        "\"Verbindung\",\"Element\",\"Details\"\r\n"
        "\"Büro\",\"Grüße, \"\"東京\"\".txt\",\"\"\r\n"
        "\"\",\"資料🔌.txt\",\"Completed\"\r\n");
}

TEST_CASE("transfer CSV produces no report when no rows are selected", "[reports][csv]")
{
  const std::vector<std::string> headers{"Item", "State"};

  CHECK(FormatTransferCsvText(headers, {}).empty());
  CHECK(FormatTransferCsvText({}, {}).empty());

  const std::vector<std::string> emptyHeaders{"", ""};
  const std::vector<TransferClipboardRow> emptyCells{{"", ""}};

  CHECK(FormatTransferCsvText(emptyHeaders, emptyCells) ==
        "\xef\xbb\xbf\"\",\"\"\r\n\"\",\"\"\r\n");
}

TEST_CASE("transfer CSV sanitizes each field before quoting", "[reports][csv]")
{
  const std::vector<std::string> headers{"Item\tName", "Error\r\nDetails"};
  const std::vector<TransferClipboardRow> rows{
      {"file\tname\r\n.txt", std::string{"before\0after", 12}}};

  CHECK(FormatTransferCsvText(headers, rows) ==
        "\xef\xbb\xbf\"Item Name\",\"Error  Details\"\r\n"
        "\"file name  .txt\",\"before after\"\r\n");

  std::string controls;

  for (unsigned int byte = 0; byte < 0x20U; ++byte)
  {
    controls.push_back(static_cast<char>(byte));
  }

  controls.push_back('\x7f');

  const std::vector<std::string> controlHeader{controls};
  const std::vector<TransferClipboardRow> controlRows{{controls}};
  const std::string expected(controls.size(), ' ');

  CHECK(FormatTransferCsvText(controlHeader, controlRows) ==
        "\xef\xbb\xbf\"" + expected + "\"\r\n\"" + expected + "\"\r\n");
}

TEST_CASE("transfer CSV protects formula prefixes without changing clipboard copy", "[reports][csv]")
{
  const std::vector<std::string> headers{" =Header", "Item"};
  const std::vector<TransferClipboardRow> rows{
      {"=SUM(1,2)", "\t +1"}, {"-2", "  @example"}, {"normal-name.txt", "name=ordinary.txt"}, {"'already text", "   "}, {"\xc2\xa0=Unicode space", "\xe3\x80\x80+Unicode space"}};

  CHECK(FormatTransferCsvText(headers, rows) ==
        "\xef\xbb\xbf\"' =Header\",\"Item\"\r\n"
        "\"'=SUM(1,2)\",\"'  +1\"\r\n"
        "\"'-2\",\"'  @example\"\r\n"
        "\"normal-name.txt\",\"name=ordinary.txt\"\r\n"
        "\"'already text\",\"   \"\r\n"
        "\"'\xc2\xa0=Unicode space\",\"'\xe3\x80\x80+Unicode space\"\r\n");
  CHECK(FormatTransferClipboardText(headers, rows).starts_with(" =Header\tItem\r\n=SUM(1,2)\t  +1\r\n-2\t  @example\r\n"));
}

TEST_CASE("transfer CSV redacts credentials independently and leaves its source unchanged", "[reports][csv]")
{
  const std::vector<std::string> headers{"password=header-secret", "Address", "Command"};
  const std::vector<TransferClipboardRow> rows{
      {"token\t= token-secret", "ftp://alice:url-secret@example.test/path", "PASS command-secret"},
      {"passphrase\x7f= phrase-secret", "sftp://example.test/path", "Safe text"}};
  const auto originalHeaders = headers;
  const auto originalRows = rows;

  CHECK(FormatTransferCsvText(headers, rows) ==
        "\xef\xbb\xbf\"password=<redacted>\",\"Address\",\"Command\"\r\n"
        "\"token = <redacted>\",\"ftp://<credentials>@example.test/path\",\"PASS <redacted>\"\r\n"
        "\"passphrase = <redacted>\",\"sftp://example.test/path\",\"Safe text\"\r\n");
  CHECK(headers == originalHeaders);
  CHECK(rows == originalRows);
}

TEST_CASE("transfer CSV retains full long details", "[reports][csv]")
{
  const std::vector<std::string> headers{"Details"};
  const auto detail = std::string(16'384, 'x') + " / Grüße aus 東京";
  const std::vector<TransferClipboardRow> rows{{detail}};

  CHECK(FormatTransferCsvText(headers, rows) ==
        "\xef\xbb\xbf\"Details\"\r\n\"" + detail + "\"\r\n");
}

TEST_CASE("completed empty transfers display one hundred percent")
{
  TransferProgress progress;
  progress.bytesTransferred = 0U;
  progress.totalBytes = 0U;

  CHECK(TransferProgressPercent(TransferState::Completed, progress) == 100U);
}

TEST_CASE("active transfers without a measurable total use byte progress")
{
  TransferProgress progress;

  CHECK_FALSE(TransferProgressPercent(TransferState::Running, progress));

  progress.totalBytes = 0U;

  CHECK_FALSE(TransferProgressPercent(TransferState::Running, progress));
  CHECK_FALSE(TransferProgressPercent(TransferState::Failed, progress));
}

TEST_CASE("measurable transfer progress is a bounded percentage")
{
  TransferProgress progress;
  progress.bytesTransferred = 42U;
  progress.totalBytes = 100U;

  CHECK(TransferProgressPercent(TransferState::Running, progress) == 42U);

  progress.bytesTransferred = 150U;

  CHECK(TransferProgressPercent(TransferState::Running, progress) == 100U);

  progress.bytesTransferred = std::numeric_limits<std::uint64_t>::max() / 2U;
  progress.totalBytes = std::numeric_limits<std::uint64_t>::max();

  CHECK(TransferProgressPercent(TransferState::Running, progress) == 49U);
}

TEST_CASE("transfer speed uses active bytes rather than logical resume progress")
{
  TransferProgress progress{
      .jobId = "resumed",
      .bytesTransferred = 900U,
      .totalBytes = 1'100U,
      .activeBytesTransferred = 400U,
      .elapsed = 2s};

  CHECK(TransferSpeedBytesPerSecond(TransferState::Running, progress) ==
        200U);
  CHECK(TransferEstimatedTimeRemaining(TransferState::Running, progress) ==
        1s);
}

TEST_CASE("transfer ETA rounds remaining partial seconds up")
{
  TransferProgress progress{
      .jobId = "job",
      .bytesTransferred = 10U,
      .totalBytes = 111U,
      .activeBytesTransferred = 100U,
      .elapsed = 10s};

  REQUIRE(TransferSpeedBytesPerSecond(TransferState::Running, progress) ==
          10U);
  CHECK(TransferEstimatedTimeRemaining(TransferState::Running, progress) ==
        11s);
}

TEST_CASE("transfer metrics require active measurable work")
{
  TransferProgress progress{
      .jobId = "job",
      .bytesTransferred = 25U,
      .totalBytes = 100U,
      .activeBytesTransferred = 25U,
      .elapsed = 1s};

  for (const auto state : {TransferState::Queued,
                           TransferState::Enumerating,
                           TransferState::Paused,
                           TransferState::Completed,
                           TransferState::Failed,
                           TransferState::Cancelled})
  {
    CHECK_FALSE(TransferSpeedBytesPerSecond(state, progress));
    CHECK_FALSE(TransferEstimatedTimeRemaining(state, progress));
  }

  progress.elapsed = {};

  CHECK_FALSE(TransferSpeedBytesPerSecond(TransferState::Running, progress));
  CHECK_FALSE(TransferEstimatedTimeRemaining(TransferState::Running, progress));

  progress.elapsed = 1s;
  progress.activeBytesTransferred = 0U;

  CHECK_FALSE(TransferSpeedBytesPerSecond(TransferState::Running, progress));
}

TEST_CASE("transfer ETA requires remaining bytes and a known total")
{
  TransferProgress progress{
      .jobId = "job",
      .bytesTransferred = 100U,
      .totalBytes = std::nullopt,
      .activeBytesTransferred = 100U,
      .elapsed = 1s};

  CHECK(TransferSpeedBytesPerSecond(TransferState::Running, progress) ==
        100U);
  CHECK_FALSE(TransferEstimatedTimeRemaining(TransferState::Running, progress));

  progress.totalBytes = 100U;

  CHECK_FALSE(TransferEstimatedTimeRemaining(TransferState::Running, progress));
}

TEST_CASE("very slow and extreme transfer rates remain representable")
{
  TransferProgress slow{
      .jobId = "slow",
      .bytesTransferred = 1U,
      .totalBytes = 3U,
      .activeBytesTransferred = 1U,
      .elapsed = 2s};

  CHECK(TransferSpeedBytesPerSecond(TransferState::Running, slow) == 1U);
  CHECK(TransferEstimatedTimeRemaining(TransferState::Running, slow) == 2s);

  TransferProgress fast{
      .jobId = "fast",
      .bytesTransferred = std::numeric_limits<std::uint64_t>::max() - 1U,
      .totalBytes = std::numeric_limits<std::uint64_t>::max(),
      .activeBytesTransferred = std::numeric_limits<std::uint64_t>::max(),
      .elapsed = std::chrono::nanoseconds{1}};

  REQUIRE(TransferSpeedBytesPerSecond(TransferState::Running, fast));
  CHECK(TransferEstimatedTimeRemaining(TransferState::Running, fast) == 1s);
}

TEST_CASE("a newly completed download requests a local browser refresh")
{
  const std::vector previous{
      Transfer("download", TransferDirection::Download,
               TransferState::Running)};

  const std::vector current{
      Transfer("download", TransferDirection::Download,
               TransferState::Completed)};

  CHECK(HasNewlyCompletedDownload(previous, current));
  CHECK(IsNewlyCompletedTransfer(previous, current.front()));
}

TEST_CASE("the first snapshot of an already completed download requests a refresh")
{
  const std::vector<QueuedTransfer> previous;
  const std::vector current{
      Transfer("download", TransferDirection::Download,
               TransferState::Completed)};

  CHECK(HasNewlyCompletedDownload(previous, current));
  CHECK(IsNewlyCompletedTransfer(previous, current.front()));
}

TEST_CASE("completed downloads refresh only once")
{
  const std::vector previous{
      Transfer("download", TransferDirection::Download,
               TransferState::Completed)};

  const std::vector current{
      Transfer("download", TransferDirection::Download,
               TransferState::Completed)};

  CHECK_FALSE(HasNewlyCompletedDownload(previous, current));
  CHECK_FALSE(IsNewlyCompletedTransfer(previous, current.front()));
}

TEST_CASE("uploads and unfinished downloads do not request a local refresh")
{
  const std::vector previous{
      Transfer("upload", TransferDirection::Upload, TransferState::Running),
      Transfer("download", TransferDirection::Download,
               TransferState::Running)};

  SECTION("completed upload")
  {
    const std::vector current{
        Transfer("upload", TransferDirection::Upload,
                 TransferState::Completed),
        Transfer("download", TransferDirection::Download,
                 TransferState::Running)};

    CHECK_FALSE(HasNewlyCompletedDownload(previous, current));
    REQUIRE(current.front().job.direction == TransferDirection::Upload);
    CHECK(IsNewlyCompletedTransfer(previous, current.front()));
  }

  SECTION("failed download")
  {
    const std::vector current{
        Transfer("upload", TransferDirection::Upload, TransferState::Running),
        Transfer("download", TransferDirection::Download,
                 TransferState::Failed)};

    CHECK_FALSE(HasNewlyCompletedDownload(previous, current));
    CHECK_FALSE(IsNewlyCompletedTransfer(previous, current.back()));
  }

  SECTION("cancelled download")
  {
    const std::vector current{
        Transfer("upload", TransferDirection::Upload, TransferState::Running),
        Transfer("download", TransferDirection::Download,
                 TransferState::Cancelled)};

    CHECK_FALSE(HasNewlyCompletedDownload(previous, current));
    CHECK_FALSE(IsNewlyCompletedTransfer(previous, current.back()));
  }
}

TEST_CASE("new transfer failures request a notification after an observed queue state")
{
  const auto direction = GENERATE(
      TransferDirection::Upload, TransferDirection::Download);

  CAPTURE(direction);

  auto current = Transfer("item", direction, TransferState::Failed);
  current.error = RemoteError{.code = RemoteErrorCode::PermissionDenied,
                              .message = "Transfer permission denied"};

  for (const auto state : {TransferState::Queued,
                           TransferState::Enumerating,
                           TransferState::Running,
                           TransferState::Paused})
  {
    const std::vector previous{
        Transfer("item", direction, state)};

    CAPTURE(state);
    CHECK(IsNewlyFailedTransfer(previous, current));
  }
}

TEST_CASE("initial and restored transfer failures do not request a notification")
{
  const auto direction = GENERATE(
      TransferDirection::Upload, TransferDirection::Download);

  CAPTURE(direction);

  const auto current = Transfer("restored", direction, TransferState::Failed);

  SECTION("initial snapshot")
  {
    const std::vector<QueuedTransfer> previous;

    CHECK_FALSE(IsNewlyFailedTransfer(previous, current));
  }

  SECTION("a different job with the same paths was previously observed")
  {
    const std::vector previous{
        Transfer("another-item", direction, TransferState::Running)};

    CHECK_FALSE(IsNewlyFailedTransfer(previous, current));
  }
}

TEST_CASE("unchanged transfer failures notify only once")
{
  const auto direction = GENERATE(
      TransferDirection::Upload, TransferDirection::Download);

  CAPTURE(direction);

  auto current = Transfer("item", direction, TransferState::Failed);
  current.attempt = 2;

  const std::vector previous{current};

  CHECK_FALSE(IsNewlyFailedTransfer(previous, current));

  current.error = RemoteError{.code = RemoteErrorCode::RemoteIo,
                              .message = "Updated failure details"};

  CHECK_FALSE(IsNewlyFailedTransfer(previous, current));

  current.attempt = 1;

  CHECK_FALSE(IsNewlyFailedTransfer(previous, current));
}

TEST_CASE("a failed transfer retry notifies even when its running snapshot was missed")
{
  const auto direction = GENERATE(
      TransferDirection::Upload, TransferDirection::Download);

  CAPTURE(direction);

  auto current = Transfer("item", direction, TransferState::Failed);
  current.attempt = 1;

  const std::vector previous{current};

  current.attempt = 2;

  CHECK(IsNewlyFailedTransfer(previous, current));

  const std::vector notified{current};

  CHECK_FALSE(IsNewlyFailedTransfer(notified, current));
}

TEST_CASE("nonfailed transfers do not request failure notifications")
{
  const auto direction = GENERATE(
      TransferDirection::Upload, TransferDirection::Download);

  CAPTURE(direction);

  const std::vector previous{
      Transfer("item", direction, TransferState::Running)};

  for (const auto state : {TransferState::Queued,
                           TransferState::Enumerating,
                           TransferState::Running,
                           TransferState::Paused,
                           TransferState::Completed,
                           TransferState::Cancelled})
  {
    CAPTURE(state);
    CHECK_FALSE(IsNewlyFailedTransfer(
        previous, Transfer("item", direction, state)));
  }
}

TEST_CASE("pause and cancellation errors never request transfer failure notifications")
{
  const auto direction = GENERATE(
      TransferDirection::Upload, TransferDirection::Download);

  CAPTURE(direction);

  const std::vector previous{
      Transfer("item", direction, TransferState::Running)};

  auto current = Transfer("item", direction, TransferState::Failed);

  for (const auto code : {RemoteErrorCode::Paused,
                          RemoteErrorCode::Cancelled})
  {
    current.error = RemoteError{.code = code,
                                .message = "User interrupted the transfer"};

    CAPTURE(code);
    CHECK_FALSE(IsNewlyFailedTransfer(previous, current));
  }
}

TEST_CASE("stale failed attempts do not notify after a transfer has restarted")
{
  const auto direction = GENERATE(
      TransferDirection::Upload, TransferDirection::Download);

  CAPTURE(direction);

  auto running = Transfer("item", direction, TransferState::Running);
  running.attempt = 2;

  const std::vector previous{running};

  auto failed = Transfer("item", direction, TransferState::Failed);
  failed.attempt = 1;

  CHECK_FALSE(IsNewlyFailedTransfer(previous, failed));
}

TEST_CASE("connection loss during either transfer direction requests one failure notification")
{
  const auto direction = GENERATE(
      TransferDirection::Upload, TransferDirection::Download);

  CAPTURE(direction);

  auto current = Transfer("item", direction, TransferState::Running);
  current.attempt = 1;

  const std::vector previous{current};

  current.state = TransferState::Failed;
  current.error = RemoteError{
      .code = RemoteErrorCode::ConnectionLost,
      .message = "The FTP connection was interrupted: CURLcode 56, "
                 "schannel: server closed abruptly (missing close_notify)",
      .nativeCode = 56,
      .retryable = true};

  CHECK(IsNewlyFailedTransfer(previous, current));

  const std::vector notified{current};

  CHECK_FALSE(IsNewlyFailedTransfer(notified, current));
}
