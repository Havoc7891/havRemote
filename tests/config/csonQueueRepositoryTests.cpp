// SPDX-License-Identifier: MIT

#include "configTestSupport.hpp"

#include "config/csonQueueRepository.hpp"
#include "core/fileTime.hpp"

#include <catch2/catch_test_macros.hpp>
#include <havCSON.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <string_view>

namespace havremote::config
{
  namespace
  {
    using namespace std::chrono_literals;

    constexpr std::string_view QueueTemporaryId{
        "01234567-89ab-4cde-8f01-23456789abcd"};

    PersistentQueueItem FailedUpload(const std::filesystem::path &localPath)
    {
      std::string remoteBytes{"/data/"};
      remoteBytes.push_back(static_cast<char>(0xffU));
      remoteBytes += ".txt";

      const auto modified =
          std::chrono::system_clock::time_point{1'700'000'000s} +
          std::chrono::duration_cast<std::chrono::system_clock::duration>(123'456'700ns);

      const auto localModified = SystemTimeToFileTime(modified);

      constexpr std::uint64_t exactLargeValue = 9'007'199'254'740'993ULL;

      PersistentQueueItem result;
      result.connectionId = "connection-1";
      result.transfer.job = TransferJob{
          .id = "job-1",
          .siteId = "site-1",
          .siteEndpoint = SiteEndpointIdentity{ProtocolKind::Sftp,
                                               "example.test",
                                               22,
                                               "user"},
          .direction = TransferDirection::Upload,
          .localPath = localPath,
          .remotePath = RemotePath{remoteBytes, "/data/display.txt"},
          .recursive = false,
          .conflictPolicy = ConflictPolicy::Overwrite,
          .expectedRemoteRevision = RemoteFileRevision{
              .kind = RemoteEntryKind::File,
              .size = exactLargeValue,
              .modifiedAt = modified}};
      result.transfer.state = TransferState::Failed;
      result.transfer.progress = TransferProgress{
          .jobId = "job-1",
          .bytesTransferred = exactLargeValue,
          .totalBytes = exactLargeValue,
          .elapsed = 8s};
      result.transfer.error = RemoteError{
          .code = RemoteErrorCode::ProtocolError,
          .message = "Remote replacement failed",
          .nativeCode = -31,
          .retryable = true,
          .operationMayHaveSucceeded = false};
      result.transfer.attempt = 3;
      result.resumeRecords.push_back(QueueResumeRecord{
          .kind = QueueResumeKind::Upload,
          .localPath = localPath,
          .remotePath = RemotePath{remoteBytes, "/data/display.txt"},
          .remoteSize = 0,
          .remoteModifiedAt = std::nullopt,
          .partSize = 0,
          .partModifiedAt = std::nullopt,
          .localSize = exactLargeValue,
          .localModifiedAt = localModified,
          .temporaryRemotePath = RemotePath{
              remoteBytes + ".havremote." + std::string{QueueTemporaryId} +
                  ".part",
              "/data/display.txt.havremote." +
                  std::string{QueueTemporaryId} + ".part"}});

      return result;
    }

    QueueResumeRecord DownloadCheckpoint(const PersistentQueueItem &item)
    {
      const auto modified =
          std::chrono::system_clock::time_point{1'700'000'000s} +
          std::chrono::duration_cast<std::chrono::system_clock::duration>(123'456'700ns);

      return QueueResumeRecord{
          .kind = QueueResumeKind::Download,
          .localPath = item.transfer.job.localPath,
          .remotePath = item.transfer.job.remotePath,
          .remoteSize = 100,
          .remoteModifiedAt = modified,
          .partSize = 40,
          .partModifiedAt = SystemTimeToFileTime(modified),
          .localSize = 0,
          .localModifiedAt = std::nullopt,
          .temporaryRemotePath = std::nullopt,
      };
    }

    void SetUploadCheckpointDestination(QueueResumeRecord &checkpoint,
                                        const RemotePath &remote)
    {
      checkpoint.remotePath = remote;
      checkpoint.temporaryRemotePath = RemotePath{
          remote.Bytes() + ".havremote." + std::string{QueueTemporaryId} + ".part",
          remote.DisplayUtf8() + ".havremote." + std::string{QueueTemporaryId} + ".part"};
    }

    PersistentQueueItem RenamedRecursiveUpload(const std::filesystem::path &root)
    {
      auto item = FailedUpload(root / "display.txt");

      auto &job = item.transfer.job;

      const auto originalRemote = job.remotePath;

      const RemotePath renamedRemote{originalRemote.Bytes() + ".renamed",
                                     originalRemote.DisplayUtf8() + ".renamed"};

      job.destinationOverrides.push_back(TransferDestinationOverride{
          .originalLocalPath = job.localPath,
          .originalRemotePath = originalRemote,
          .localPath = job.localPath,
          .remotePath = renamedRemote});
      job.localPath = root;
      job.remotePath = RemotePath{"/data"};
      job.recursive = true;
      job.expectedRemoteRevision.reset();

      SetUploadCheckpointDestination(item.resumeRecords.front(), renamedRemote);

      return item;
    }
  } // namespace

  TEST_CASE("A missing persistent queue loads as empty without creating a file")
  {
    test::TempDirectory directory;

    const auto file = directory.Path() / "queue.cson";

    CsonQueueRepository repository(file);

    const auto loaded = repository.Load();

    REQUIRE(loaded.has_value());
    CHECK(loaded->items.empty());
    CHECK_FALSE(std::filesystem::exists(file));
    REQUIRE(repository.QueuePath().has_value());
    CHECK(*repository.QueuePath() == file);
  }

  TEST_CASE("Persistent queue round trips exact metadata and arbitrary remote bytes")
  {
    test::TempDirectory directory;

    const auto file = directory.Path() / "queue.cson";

    const auto localPath =
        directory.Path() / std::filesystem::path{std::u8string{u8"über.txt"}};

    CsonQueueRepository repository(file);

    const auto source = FailedUpload(localPath);

    REQUIRE(repository.Save({source}).has_value());

    const auto serialized = test::ReadText(file);

    CHECK(serialized.find("9007199254740993") != std::string::npos);
    CHECK(serialized.find("1700000000123456700") != std::string::npos);
    CHECK(serialized.find("FF2E747874") != std::string::npos);

    CsonQueueRepository reloaded(file);

    const auto loaded = reloaded.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->items.size() == 1);

    const auto &item = loaded->items.front();

    CHECK(item.connectionId == source.connectionId);
    CHECK(item.transfer.state == TransferState::Failed);
    CHECK(item.transfer.job.id == "job-1");
    CHECK(item.transfer.job.localPath == localPath);
    CHECK(item.transfer.job.remotePath.Bytes() ==
          source.transfer.job.remotePath.Bytes());
    CHECK(item.transfer.job.remotePath.DisplayUtf8() == "/data/display.txt");
    REQUIRE(item.transfer.job.expectedRemoteRevision.has_value());
    CHECK(item.transfer.job.expectedRemoteRevision->size ==
          9'007'199'254'740'993ULL);
    CHECK(item.transfer.job.expectedRemoteRevision->modifiedAt ==
          source.transfer.job.expectedRemoteRevision->modifiedAt);
    CHECK(item.transfer.progress.bytesTransferred ==
          9'007'199'254'740'993ULL);
    CHECK(item.transfer.progress.elapsed ==
          std::chrono::steady_clock::duration{});
    REQUIRE(item.transfer.error.has_value());
    CHECK(item.transfer.error->nativeCode == -31);
    REQUIRE(item.resumeRecords.size() == 1);
    CHECK(item.resumeRecords.front().kind == QueueResumeKind::Upload);
    CHECK(item.resumeRecords.front().localSize ==
          9'007'199'254'740'993ULL);
    CHECK(item.resumeRecords.front().localModifiedAt ==
          source.resumeRecords.front().localModifiedAt);
    REQUIRE(item.resumeRecords.front().temporaryRemotePath.has_value());
    CHECK(item.resumeRecords.front().temporaryRemotePath->Bytes() ==
          source.resumeRecords.front().temporaryRemotePath->Bytes());

    REQUIRE(reloaded.Save(loaded->items).has_value());
    CHECK(test::ReadText(file) == serialized);
  }

  TEST_CASE("Renamed single-file queue destinations remain synchronized across saves")
  {
    test::TempDirectory directory;

    const auto file = directory.Path() / "queue.cson";

    auto item = FailedUpload(directory.Path() / "source.txt");
    item.transfer.job.expectedRemoteRevision.reset();

    CsonQueueRepository repository(file);

    SECTION("upload")
    {
      for (const auto name : {"first.txt", "second.txt"})
      {
        item.transfer.job.remotePath = RemotePath{std::string{"/data/"} + name};

        SetUploadCheckpointDestination(item.resumeRecords.front(),
                                       item.transfer.job.remotePath);

        REQUIRE(repository.Save({item}).has_value());

        const auto loaded = CsonQueueRepository{file}.Load();

        REQUIRE(loaded.has_value());
        REQUIRE(loaded->items.size() == 1);

        const auto &restored = loaded->items.front();

        CHECK(restored.transfer.job.remotePath == item.transfer.job.remotePath);
        REQUIRE(restored.resumeRecords.size() == 1);
        CHECK(restored.resumeRecords.front().remotePath == restored.transfer.job.remotePath);
        CHECK(restored.resumeRecords.front().temporaryRemotePath ==
              item.resumeRecords.front().temporaryRemotePath);
        CHECK(restored.transfer.job.destinationOverrides.empty());
      }
    }

    SECTION("download")
    {
      item.transfer.job.direction = TransferDirection::Download;
      item.resumeRecords = {DownloadCheckpoint(item)};

      for (const auto name : {"first.txt", "second.txt"})
      {
        item.transfer.job.localPath = directory.Path() / name;
        item.resumeRecords.front().localPath = item.transfer.job.localPath;

        REQUIRE(repository.Save({item}).has_value());

        const auto loaded = CsonQueueRepository{file}.Load();

        REQUIRE(loaded.has_value());
        REQUIRE(loaded->items.size() == 1);

        const auto &restored = loaded->items.front();

        CHECK(restored.transfer.job.localPath == item.transfer.job.localPath);
        REQUIRE(restored.resumeRecords.size() == 1);
        CHECK(restored.resumeRecords.front().localPath == restored.transfer.job.localPath);
        CHECK(restored.resumeRecords.front().remotePath == restored.transfer.job.remotePath);
        CHECK(restored.transfer.job.destinationOverrides.empty());
      }
    }

    CHECK(test::ReadText(file).find("destinationOverrides") == std::string::npos);
  }

  TEST_CASE("Recursive queue rename mappings preserve original and resolved paths")
  {
    test::TempDirectory directory;

    const auto file = directory.Path() / "queue.cson";

    auto item = RenamedRecursiveUpload(directory.Path() / "tree");

    SECTION("upload preserves arbitrary remote bytes and display names") {}

    SECTION("download preserves an unchanged remote source and renamed local destination")
    {
      auto &job = item.transfer.job;
      job.direction = TransferDirection::Download;

      auto &destination = job.destinationOverrides.front();
      destination.remotePath = destination.originalRemotePath;
      destination.localPath = destination.originalLocalPath.parent_path() /
                              std::filesystem::path{std::u8string{u8"umbenannt-über.txt"}};

      auto checkpoint = DownloadCheckpoint(item);
      checkpoint.localPath = destination.localPath;
      checkpoint.remotePath = destination.remotePath;

      item.resumeRecords = {checkpoint};
    }

    for (unsigned cycle = 0; cycle < 3; ++cycle)
    {
      CsonQueueRepository repository(file);

      REQUIRE(repository.Save({item}).has_value());

      const auto loaded = CsonQueueRepository{file}.Load();

      REQUIRE(loaded.has_value());
      REQUIRE(loaded->items.size() == 1);

      const auto &restored = loaded->items.front();

      CHECK(restored.transfer.job.destinationOverrides == item.transfer.job.destinationOverrides);
      REQUIRE(restored.resumeRecords.size() == 1);
      CHECK(restored.resumeRecords.front().localPath == item.resumeRecords.front().localPath);
      CHECK(restored.resumeRecords.front().remotePath == item.resumeRecords.front().remotePath);

      item = restored;
    }

    CHECK(test::ReadText(file).find("FF2E747874") != std::string::npos);
  }

  TEST_CASE("Persistent queue rejects unsafe or stale rename mapping snapshots")
  {
    test::TempDirectory directory;

    const auto file = directory.Path() / "queue.cson";

    auto item = RenamedRecursiveUpload(directory.Path() / "tree");

    CsonQueueRepository repository(file);

    REQUIRE(repository.Save({item}).has_value());

    const auto original = test::ReadText(file);

    auto &job = item.transfer.job;

    auto &destination = job.destinationOverrides.front();

    SECTION("single-file jobs must use their canonical paths")
    {
      job.recursive = false;
      job.localPath = destination.localPath;
      job.remotePath = destination.remotePath;
    }
    SECTION("duplicate originals")
    {
      job.destinationOverrides.push_back(destination);
    }
    SECTION("original local source outside root")
    {
      destination.originalLocalPath = directory.Path() / "outside.txt";
      destination.localPath = destination.originalLocalPath;
    }
    SECTION("original remote destination outside root")
    {
      destination.originalRemotePath = RemotePath{"/other/original.txt"};
      destination.remotePath = RemotePath{"/other/renamed.txt"};
    }
    SECTION("renamed destination outside its parent")
    {
      destination.remotePath = RemotePath{"/other/renamed.txt"};
    }
    SECTION("upload cannot change its source")
    {
      destination.localPath = destination.originalLocalPath.parent_path() / "other.txt";
    }
    SECTION("stale checkpoint still names the original destination")
    {
      SetUploadCheckpointDestination(item.resumeRecords.front(), destination.originalRemotePath);
    }
    SECTION("unmapped checkpoint outside local root")
    {
      item.resumeRecords.front().localPath = directory.Path() / "outside.txt";
    }
    SECTION("unmapped checkpoint outside remote root")
    {
      job.destinationOverrides.clear();

      SetUploadCheckpointDestination(item.resumeRecords.front(), RemotePath{"/other/file.txt"});
    }

    const auto saved = repository.Save({item});

    REQUIRE_FALSE(saved.has_value());
    CHECK(saved.error().kind == ConfigErrorKind::Validation);
    CHECK(test::ReadText(file) == original);
  }

  TEST_CASE("Malformed persistent destination mappings are rejected while loading")
  {
    test::TempDirectory directory;

    const auto file = directory.Path() / "queue.cson";

    const auto item = RenamedRecursiveUpload(directory.Path() / "tree");

    REQUIRE(CsonQueueRepository{file}.Save({item}).has_value());

    havCSON::Value document;

    REQUIRE(havCSON::Parse(test::ReadText(file), document) == havCSON::ErrorCode::OK);

    auto &job = document.asObject().at("items").asArray().front().asObject().at("job").asObject();

    auto &destinations = job.at("destinationOverrides");

    SECTION("array required")
    {
      destinations = "not-an-array";
    }
    SECTION("mapping object required")
    {
      destinations.asArray().front() = false;
    }
    SECTION("all mapping members required")
    {
      destinations.asArray().front().asObject().erase("originalLocalPath");
    }
    SECTION("unknown secret-bearing member forbidden")
    {
      destinations.asArray().front().asObject().emplace("password", "must-not-be-accepted");
    }
    SECTION("local paths must be strings")
    {
      destinations.asArray().front().asObject().at("localPath") = 2.0;
    }
    SECTION("remote bytes require hexadecimal text")
    {
      destinations.asArray().front().asObject().at("remotePath").asObject().at("bytesHex") = "ZZ";
    }
    SECTION("duplicate mappings forbidden")
    {
      const auto copy = destinations.asArray().front();

      destinations.asArray().push_back(copy);
    }
    SECTION("bounded mapping count")
    {
      const auto copy = destinations.asArray().front();

      destinations.asArray().assign(10'001, copy);
    }

    test::WriteText(file, havCSON::ToString(document));

    CsonQueueRepository repository(file);

    const auto loaded = repository.Load();

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().kind == ConfigErrorKind::Validation);
    CHECK_FALSE(repository.Save({}).has_value());
  }

  TEST_CASE("Active persistent queue states recover paused and terminal states are omitted")
  {
    test::TempDirectory directory;

    const auto file = directory.Path() / "queue.cson";

    auto queued = FailedUpload(directory.Path() / "queued.txt");
    queued.transfer.job.id = "queued";
    queued.transfer.progress.jobId = "queued";
    queued.transfer.state = TransferState::Queued;
    queued.transfer.error.reset();

    auto enumerating = queued;
    enumerating.transfer.job.id = "enumerating";
    enumerating.transfer.progress.jobId = "enumerating";
    enumerating.transfer.state = TransferState::Enumerating;

    auto running = queued;
    running.transfer.job.id = "running";
    running.transfer.progress.jobId = "running";
    running.transfer.state = TransferState::Running;

    auto completed = queued;
    completed.transfer.job.id = "completed";
    completed.transfer.progress.jobId = "completed";
    completed.transfer.state = TransferState::Completed;

    auto canceled = queued;
    canceled.transfer.job.id = "canceled";
    canceled.transfer.progress.jobId = "canceled";
    canceled.transfer.state = TransferState::Cancelled;

    CsonQueueRepository repository(file);

    REQUIRE(repository.Save(
                          {queued, enumerating, running, completed, canceled})
                .has_value());

    CsonQueueRepository reloaded(file);

    const auto loaded = reloaded.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->items.size() == 3);

    for (const auto &item : loaded->items)
    {
      CHECK(item.transfer.state == TransferState::Paused);
    }
  }

  TEST_CASE("A failed queue load blocks overwriting the recoverable file")
  {
    test::TempDirectory directory;

    const auto file = directory.Path() / "queue.cson";

    const std::string unsupported = "formatVersion: 2\nitems: []\n";

    test::WriteText(file, unsupported);

    CsonQueueRepository repository(file);

    const auto loaded = repository.Load();

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().kind == ConfigErrorKind::UnsupportedVersion);

    const auto saved = repository.Save({});

    REQUIRE_FALSE(saved.has_value());
    CHECK(test::ReadText(file) == unsupported);

    test::WriteText(file, "formatVersion: 1\nitems: []\n");

    REQUIRE(repository.Load().has_value());
    REQUIRE(repository.Save({}).has_value());
  }

  TEST_CASE("Future queue versions are reported before their new root members")
  {
    test::TempDirectory directory;

    const auto file = directory.Path() / "future.cson";

    test::WriteText(
        file,
        "formatVersion: 2\nitems: []\nfutureQueueMetadata: true\n");

    CsonQueueRepository repository(file);

    const auto loaded = repository.Load();

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().kind == ConfigErrorKind::UnsupportedVersion);
  }

  TEST_CASE("Malformed existing queue files block every later save")
  {
    struct Scenario final
    {
      std::string_view name;
      std::string_view contents;
      ConfigErrorKind expectedKind;
    };

    constexpr std::array scenarios{
        Scenario{"parse", "formatVersion: 1\nitems: \"\\q\"\n",
                 ConfigErrorKind::Parse},
        Scenario{"validation", "formatVersion: 1\nitems: {}\n",
                 ConfigErrorKind::Validation},
        Scenario{"empty", "", ConfigErrorKind::Validation},
    };

    test::TempDirectory directory;

    for (const auto &scenario : scenarios)
    {
      CAPTURE(scenario.name);

      const auto file = directory.Path() /
                        (std::string{scenario.name} + ".cson");

      test::WriteText(file, scenario.contents);

      const auto original = test::ReadText(file);

      CsonQueueRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      CHECK(loaded.error().kind == scenario.expectedKind);
      REQUIRE_FALSE(repository.Save({}).has_value());
      CHECK(test::ReadText(file) == original);
    }
  }

  TEST_CASE("Queue checked fields report the offending value in the correct item")
  {
    struct Scenario final
    {
      std::string_view member;
      std::string_view replacement;
      havCSON::ValuePath path;
    };

    const std::array scenarios{
        Scenario{"attempt", "1.5", {"attempt"}},
        Scenario{"attempt", "4294967296", {"attempt"}},
        Scenario{"port", "65536", {"job", "endpoint", "port"}},
        Scenario{"recursive", "'yes'", {"job", "recursive"}},
        Scenario{"localSize", "'01'", {"resumeRecords", std::size_t{0}, "localSize"}},
        Scenario{"localModifiedAtUnixNanoseconds", "'001'", {"resumeRecords", std::size_t{0}, "localModifiedAtUnixNanoseconds"}},
        Scenario{"state", "'unknown'", {"state"}},
    };

    test::TempDirectory directory;

    const auto file = directory.Path() / "queue.cson";

    const auto first = FailedUpload(directory.Path() / "first.txt");

    auto second = FailedUpload(directory.Path() / "second.txt");
    second.transfer.job.id = "job-2";
    second.transfer.progress.jobId = "job-2";

    REQUIRE(CsonQueueRepository{file}.Save({first, second}).has_value());

    const auto valid = test::ReadText(file);

    REQUIRE(CsonQueueRepository{file}.Load().has_value());

    havCSON::Value parsed;
    havCSON::Error parseError;

    REQUIRE(havCSON::Parse(valid, parsed, &parseError,
                           {.trackSourceLocations = true}) == havCSON::ErrorCode::OK);

    for (const auto &scenario : scenarios)
    {
      CAPTURE(scenario.member, scenario.replacement);

      auto contents = valid;

      const auto member = havCSON::ValueView{parsed}.Member("items").At(1).AtPath(scenario.path).Get();

      REQUIRE(member.has_value());

      const auto *source = member->get().Source();

      REQUIRE(source != nullptr);

      const auto valuePosition = source->valueSpan.begin.byteOffset;

      contents.replace(valuePosition, source->valueSpan.end.byteOffset - valuePosition,
                       scenario.replacement);

      const auto line = 1U + static_cast<std::size_t>(std::count(
                                 contents.begin(), contents.begin() + static_cast<std::ptrdiff_t>(valuePosition), '\n'));

      const auto lineStart = contents.rfind('\n', valuePosition);

      const auto column = valuePosition - lineStart;

      test::WriteText(file, contents);

      CsonQueueRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      INFO(loaded.error().message);
      CHECK(loaded.error().kind == ConfigErrorKind::Validation);
      CHECK(loaded.error().path == file);
      CHECK(loaded.error().line == line);
      CHECK(loaded.error().column == column);
      CHECK(loaded.error().message.find("items[1]") != std::string::npos);
      CHECK(loaded.error().message.find(scenario.member) != std::string::npos);
      REQUIRE_FALSE(repository.Save({}).has_value());
      CHECK(test::ReadText(file) == contents);
    }
  }

  TEST_CASE("Missing queue fields refer to their actual containing item")
  {
    test::TempDirectory directory;

    const auto file = directory.Path() / "queue.cson";

    test::WriteText(file, "formatVersion: 1\nitems: [\n  {}\n]\n");

    const auto loaded = CsonQueueRepository{file}.Load();

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().kind == ConfigErrorKind::Validation);
    CHECK(loaded.error().line == 3);
    CHECK(loaded.error().column == 3);
    CHECK(loaded.error().message.find("connectionId") != std::string::npos);
  }

  TEST_CASE("Queue input limits reject unsafe documents without overwriting them")
  {
    test::TempDirectory directory;

    const auto file = directory.Path() / "queue.cson";

    SECTION("the library enforces the queue nesting limit")
    {
      const auto contents = std::string{"formatVersion: 1\nitems: "} +
                            std::string(256, '[') + "0" + std::string(256, ']') + "\n";

      test::WriteText(file, contents);

      CsonQueueRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      CHECK(loaded.error().kind == ConfigErrorKind::Validation);
      CHECK(loaded.error().line == 2);
      CHECK(loaded.error().column.has_value());
      REQUIRE_FALSE(repository.Save({}).has_value());
      CHECK(test::ReadText(file) == contents);
    }

    SECTION("the library enforces the existing 64 MiB input limit")
    {
      test::WriteText(file, "formatVersion: 1\nitems: []\n");

      constexpr std::uintmax_t oversizedLength = 64U * 1024U * 1024U + 1U;

      std::error_code resizeError;

      std::filesystem::resize_file(file, oversizedLength, resizeError);

      REQUIRE_FALSE(resizeError);

      CsonQueueRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      CHECK(loaded.error().kind == ConfigErrorKind::Validation);
      REQUIRE_FALSE(repository.Save({}).has_value());
      CHECK(std::filesystem::file_size(file) == oversizedLength);
    }
  }

  TEST_CASE("Queue file read failures are I/O errors without invented source locations")
  {
    test::TempDirectory directory;

    CsonQueueRepository repository(directory.Path());

    const auto loaded = repository.Load();

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().kind == ConfigErrorKind::Io);
    CHECK(loaded.error().path == directory.Path());
    CHECK_FALSE(loaded.error().line.has_value());
    CHECK_FALSE(loaded.error().column.has_value());
    REQUIRE_FALSE(repository.Save({}).has_value());
    CHECK(std::filesystem::is_directory(directory.Path()));
  }

  TEST_CASE("Persistent queue rejects invalid typed enum values")
  {
    test::TempDirectory directory;

    const auto file = directory.Path() / "queue.cson";

    auto item = FailedUpload(directory.Path() / "source.txt");

    CsonQueueRepository repository(file);

    SECTION("transfer direction")
    {
      item.transfer.job.direction = static_cast<TransferDirection>(99);

      const auto saved = repository.Save({item});

      REQUIRE_FALSE(saved.has_value());
      CHECK(saved.error().kind == ConfigErrorKind::Validation);
    }

    SECTION("remote error code")
    {
      item.transfer.error->code = static_cast<RemoteErrorCode>(99);

      const auto saved = repository.Save({item});

      REQUIRE_FALSE(saved.has_value());
      CHECK(saved.error().kind == ConfigErrorKind::Validation);
    }

    SECTION("resume kind")
    {
      item.resumeRecords.front().kind = static_cast<QueueResumeKind>(99);

      const auto saved = repository.Save({item});

      REQUIRE_FALSE(saved.has_value());
      CHECK(saved.error().kind == ConfigErrorKind::Validation);
    }

    SECTION("endpoint protocol")
    {
      item.transfer.job.siteEndpoint->protocol = static_cast<ProtocolKind>(99);

      const auto saved = repository.Save({item});

      REQUIRE_FALSE(saved.has_value());
      CHECK(saved.error().kind == ConfigErrorKind::Validation);
    }
  }

  TEST_CASE("Persistent queue accepts only verifiable safe-resume checkpoints")
  {
    test::TempDirectory directory;

    const auto file = directory.Path() / "queue.cson";

    auto item = FailedUpload(directory.Path() / "source.txt");

    CsonQueueRepository repository(file);

    SECTION("download source timestamp is required")
    {
      item.transfer.job.direction = TransferDirection::Download;
      item.resumeRecords = {DownloadCheckpoint(item)};
      item.resumeRecords.front().remoteModifiedAt.reset();

      const auto saved = repository.Save({item});

      REQUIRE_FALSE(saved.has_value());
      CHECK(saved.error().kind == ConfigErrorKind::Validation);
    }

    SECTION("single-file local path must match")
    {
      item.resumeRecords.front().localPath = directory.Path() / "other.txt";

      const auto saved = repository.Save({item});

      REQUIRE_FALSE(saved.has_value());
      CHECK(saved.error().kind == ConfigErrorKind::Validation);
    }

    SECTION("single-file remote path must match")
    {
      item.resumeRecords.front().remotePath = RemotePath{"/data/other.txt"};

      const auto saved = repository.Save({item});

      REQUIRE_FALSE(saved.has_value());
      CHECK(saved.error().kind == ConfigErrorKind::Validation);
    }

    SECTION("temporary upload path must be distinct")
    {
      item.resumeRecords.front().temporaryRemotePath =
          item.resumeRecords.front().remotePath;

      const auto saved = repository.Save({item});

      REQUIRE_FALSE(saved.has_value());
      CHECK(saved.error().kind == ConfigErrorKind::Validation);
    }

    SECTION("temporary upload path must be a sibling")
    {
      item.resumeRecords.front().temporaryRemotePath = RemotePath{
          "/other/source.txt.havremote." + std::string{QueueTemporaryId} +
          ".part"};

      const auto saved = repository.Save({item});

      REQUIRE_FALSE(saved.has_value());
      CHECK(saved.error().kind == ConfigErrorKind::Validation);
    }

    SECTION("temporary upload path must contain a generated id")
    {
      const auto &remote = item.resumeRecords.front().remotePath;

      item.resumeRecords.front().temporaryRemotePath = RemotePath{
          remote.Bytes() + ".havremote.not-an-id.part",
          remote.DisplayUtf8() + ".havremote.not-an-id.part"};

      const auto saved = repository.Save({item});

      REQUIRE_FALSE(saved.has_value());
      CHECK(saved.error().kind == ConfigErrorKind::Validation);
    }
  }

  TEST_CASE("Persistent queue rejects duplicate keys and secret-bearing fields")
  {
    test::TempDirectory directory;

    SECTION("duplicate key")
    {
      const auto file = directory.Path() / "duplicate.cson";

      test::WriteText(
          file,
          "formatVersion: 1\nformatVersion: 1\nitems: []\n");

      CsonQueueRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      CHECK(loaded.error().kind == ConfigErrorKind::Parse);
      CHECK(loaded.error().line.has_value());
      CHECK(loaded.error().column.has_value());
    }

    SECTION("secret-bearing field")
    {
      const auto file = directory.Path() / "secret.cson";

      test::WriteText(
          file,
          "formatVersion: 1\nitems: []\npassword: \"do-not-store\"\n");

      CsonQueueRepository repository(file);

      const auto loaded = repository.Load();

      REQUIRE_FALSE(loaded.has_value());
      CHECK(loaded.error().kind == ConfigErrorKind::Validation);
    }
  }

  TEST_CASE("Persistent queue sanitizes failed transfer diagnostics before saving")
  {
    test::TempDirectory directory;

    const auto file = directory.Path() / "queue.cson";

    auto item = FailedUpload(directory.Path() / "source.txt");
    item.transfer.error->message =
        "Server rejected password: swordfish";

    CsonQueueRepository repository(file);

    REQUIRE(repository.Save({item}).has_value());

    const auto text = test::ReadText(file);

    CHECK(text.find("swordfish") == std::string::npos);
    CHECK(text.find("<redacted>") != std::string::npos);

    CsonQueueRepository reloaded(file);

    const auto loaded = reloaded.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->items.front().transfer.error.has_value());
    CHECK(loaded->items.front().transfer.error->message.find("<redacted>") !=
          std::string::npos);
  }
} // namespace havremote::config
