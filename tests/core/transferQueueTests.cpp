// SPDX-License-Identifier: MIT

#include "core/transferQueue.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

using namespace havremote;

namespace
{
  TransferJob DownloadJob(std::string id = {})
  {
    return TransferJob{.id = std::move(id),
                       .siteId = "site-a",
                       .siteEndpoint = SiteEndpointIdentity{ProtocolKind::Sftp,
                                                            "example.test",
                                                            22,
                                                            "alice"},
                       .direction = TransferDirection::Download,
                       .localPath = "download.bin",
                       .remotePath = RemotePath{"/download.bin"},
                       .expectedRemoteRevision = std::nullopt};
  }

  QueuedTransfer RestoredTransfer(std::string id,
                                  const TransferState state)
  {
    auto job = DownloadJob(std::move(id));

    QueuedTransfer result{
        .job = std::move(job),
        .state = state,
        .progress = TransferProgress{.jobId = {},
                                     .bytesTransferred = 12,
                                     .totalBytes = 100,
                                     .activeBytesTransferred = 8,
                                     .elapsed = std::chrono::seconds{7}},
        .error = std::nullopt,
        .attempt = 3};

    if (state == TransferState::Failed)
    {
      result.error = RemoteError{
          .code = RemoteErrorCode::ConnectionLost,
          .message = "connection lost after a destructive request",
          .retryable = true,
          .operationMayHaveSucceeded = true};
    }

    return result;
  }

  TEST_CASE("queue only claims transfers for the selected site")
  {
    TransferQueue queue;

    auto other = DownloadJob("other");
    other.siteId = "site-b";

    REQUIRE(queue.Enqueue(std::move(other)));
    REQUIRE(queue.Enqueue(DownloadJob("selected")));

    const auto claimed = queue.TakeNext("site-a");

    REQUIRE(claimed);
    CHECK(claimed->id == "selected");
    CHECK(queue.Get("other")->state == TransferState::Queued);
  }

  TEST_CASE("queue does not retarget a stable site id after endpoint edits")
  {
    TransferQueue queue;

    REQUIRE(queue.Enqueue(DownloadJob("old-endpoint")));

    const SiteEndpointIdentity edited{ProtocolKind::Sftp,
                                      "replacement.example",
                                      22,
                                      "alice"};

    CHECK_FALSE(queue.TakeNext("site-a", edited));

    const SiteEndpointIdentity original{ProtocolKind::Sftp,
                                        "example.test",
                                        22,
                                        "alice"};

    REQUIRE(queue.TakeNext("site-a", original));
  }
} // namespace

TEST_CASE("queue claims transfers in insertion order")
{
  TransferQueue queue;

  const auto first = queue.Enqueue(DownloadJob("first"));
  const auto second = queue.Enqueue(DownloadJob("second"));

  REQUIRE(first);
  REQUIRE(second);

  const auto claimed = queue.TakeNext();

  REQUIRE(claimed);
  CHECK(claimed->id == "first");
  REQUIRE(queue.Get("first"));
  CHECK(queue.Get("first")->state == TransferState::Enumerating);
  CHECK(queue.Get("first")->attempt == 1);
  CHECK(queue.Get("second")->state == TransferState::Queued);
}

TEST_CASE("durability-gated transfers remain paused until explicitly activated")
{
  TransferQueue queue;

  const auto inserted = queue.EnqueuePaused(DownloadJob("durable"));

  REQUIRE(inserted);
  REQUIRE(queue.Get(*inserted));
  CHECK(queue.Get(*inserted)->state == TransferState::Paused);
  CHECK_FALSE(queue.TakeNext());

  REQUIRE(queue.Retry(*inserted));

  const auto claimed = queue.TakeNext();

  REQUIRE(claimed);
  CHECK(claimed->id == *inserted);
}

TEST_CASE("restored queue state remains owned and requires explicit retry")
{
  TransferQueue queue;

  REQUIRE(queue.Restore({RestoredTransfer("queued", TransferState::Queued),
                         RestoredTransfer("enumerating", TransferState::Enumerating),
                         RestoredTransfer("running", TransferState::Running),
                         RestoredTransfer("paused", TransferState::Paused),
                         RestoredTransfer("failed", TransferState::Failed)}));

  const auto restored = queue.Snapshot();

  REQUIRE(restored.size() == 5);
  CHECK(queue.Get("enumerating")->state == TransferState::Paused);
  CHECK(queue.Get("running")->state == TransferState::Paused);
  CHECK(queue.Get("running")->progress.jobId == "running");
  CHECK(queue.Get("running")->progress.bytesTransferred == 12);
  CHECK(queue.Get("running")->progress.activeBytesTransferred == 0);
  CHECK(queue.Get("running")->progress.elapsed ==
        std::chrono::steady_clock::duration{});
  CHECK(queue.Get("running")->attempt == 3);

  const SiteEndpointIdentity wrongEndpoint{ProtocolKind::Sftp,
                                           "replacement.example",
                                           22,
                                           "alice"};

  CHECK_FALSE(queue.TakeNext("site-a", wrongEndpoint));

  const SiteEndpointIdentity originalEndpoint{ProtocolKind::Sftp,
                                              "example.test",
                                              22,
                                              "alice"};

  const auto claimed = queue.TakeNext("site-a", originalEndpoint);

  REQUIRE(claimed);
  CHECK(claimed->id == "queued");

  CHECK_FALSE(queue.TakeNext("site-a", originalEndpoint));
  REQUIRE(queue.Retry("running"));

  const auto retriedActive = queue.TakeNext("site-a", originalEndpoint);

  REQUIRE(retriedActive);
  CHECK(retriedActive->id == "running");

  const auto failed = queue.Get("failed");

  REQUIRE(failed);
  REQUIRE(failed->error);
  CHECK(failed->error->operationMayHaveSucceeded);
  CHECK(failed->attempt == 3);
  REQUIRE(queue.Retry("failed"));
  CHECK_FALSE(queue.Get("failed")->error);
}

TEST_CASE("queue restore is all-or-nothing for duplicate identities")
{
  TransferQueue queue;

  REQUIRE(queue.Enqueue(DownloadJob("existing")));

  SECTION("duplicate within restored snapshot")
  {
    const auto restored = queue.Restore(
        {RestoredTransfer("new", TransferState::Paused),
         RestoredTransfer("new", TransferState::Failed)});

    REQUIRE_FALSE(restored);
    CHECK(restored.error().code == RemoteErrorCode::Conflict);
  }

  SECTION("duplicate with live queue")
  {
    const auto restored = queue.Restore(
        {RestoredTransfer("new", TransferState::Paused),
         RestoredTransfer("existing", TransferState::Paused)});

    REQUIRE_FALSE(restored);
    CHECK(restored.error().code == RemoteErrorCode::Conflict);
  }

  REQUIRE(queue.Snapshot().size() == 1);
  CHECK(queue.Get("existing").has_value());
  CHECK_FALSE(queue.Get("new").has_value());
}

TEST_CASE("queue restore rejects malformed actionable state atomically")
{
  TransferQueue queue;

  const auto requireRejected = [&](QueuedTransfer invalid)
  {
    const auto restored = queue.Restore(
        {RestoredTransfer("valid", TransferState::Paused),
         std::move(invalid)});

    REQUIRE_FALSE(restored);
    CHECK(restored.error().code == RemoteErrorCode::InvalidArgument);
    CHECK(queue.Snapshot().empty());
  };

  SECTION("terminal state")
  {
    requireRejected(RestoredTransfer("invalid", TransferState::Completed));
  }
  SECTION("missing endpoint")
  {
    auto invalid = RestoredTransfer("invalid", TransferState::Paused);
    invalid.job.siteEndpoint.reset();

    requireRejected(std::move(invalid));
  }
  SECTION("mismatched progress identity")
  {
    auto invalid = RestoredTransfer("invalid", TransferState::Paused);
    invalid.progress.jobId = "different";

    requireRejected(std::move(invalid));
  }
  SECTION("progress beyond total")
  {
    auto invalid = RestoredTransfer("invalid", TransferState::Paused);
    invalid.progress.bytesTransferred = 101;

    requireRejected(std::move(invalid));
  }
  SECTION("failed state without an error")
  {
    auto invalid = RestoredTransfer("invalid", TransferState::Failed);
    invalid.error.reset();

    requireRejected(std::move(invalid));
  }
  SECTION("non-failed state with an error")
  {
    auto invalid = RestoredTransfer("invalid", TransferState::Paused);
    invalid.error = RemoteError{.code = RemoteErrorCode::RemoteIo,
                                .message = "stale error"};

    requireRejected(std::move(invalid));
  }
  SECTION("recursive guarded transfer")
  {
    auto invalid = RestoredTransfer("invalid", TransferState::Paused);
    invalid.job.recursive = true;
    invalid.job.expectedRemoteRevision = RemoteFileRevision{
        .kind = RemoteEntryKind::File, .size = 12, .modifiedAt = std::nullopt};

    requireRejected(std::move(invalid));
  }
}

TEST_CASE("queue enforces transfer state transitions")
{
  TransferQueue queue;

  REQUIRE(queue.Enqueue(DownloadJob("job")));
  CHECK_FALSE(queue.Complete("job"));
  REQUIRE(queue.MarkEnumerating("job"));
  REQUIRE(queue.MarkRunning("job"));
  REQUIRE(queue.UpdateProgress(TransferProgress{
      .jobId = "job", .bytesTransferred = 42, .totalBytes = {}}));
  REQUIRE(queue.Pause("job"));
  CHECK(queue.Get("job")->state == TransferState::Paused);
  REQUIRE(queue.Retry("job"));
  CHECK(queue.Get("job")->state == TransferState::Queued);
  CHECK(queue.Get("job")->progress.bytesTransferred == 0);
}

TEST_CASE("failed transfers retain an error until retry")
{
  TransferQueue queue;

  REQUIRE(queue.Enqueue(DownloadJob("job")));
  REQUIRE(queue.MarkEnumerating("job"));
  REQUIRE(queue.MarkRunning("job"));
  REQUIRE(queue.Fail("job", RemoteError{.code = RemoteErrorCode::ConnectionLost,
                                        .message = "network lost",
                                        .retryable = true}));

  const auto failed = queue.Get("job");

  REQUIRE(failed);
  REQUIRE(failed->error);
  CHECK(failed->state == TransferState::Failed);
  CHECK(failed->error->retryable);
  REQUIRE(queue.Retry("job"));
  CHECK_FALSE(queue.Get("job")->error);
}

TEST_CASE("uncertain failures remain failed until an explicit retry", "[queue][uncertainty]")
{
  TransferQueue queue;

  REQUIRE(queue.Enqueue(DownloadJob("uncertain")));
  REQUIRE(queue.TakeNext());
  REQUIRE(queue.MarkRunning("uncertain"));
  REQUIRE(queue.Fail(
      "uncertain",
      RemoteError{.code = RemoteErrorCode::ConnectionLost,
                  .message = "connection lost after a destructive request",
                  .retryable = true,
                  .operationMayHaveSucceeded = true}));

  const auto failed = queue.Get("uncertain");

  REQUIRE(failed);
  REQUIRE(failed->error);
  CHECK(failed->state == TransferState::Failed);
  CHECK(failed->attempt == 1);
  CHECK(failed->error->operationMayHaveSucceeded);
  CHECK_FALSE(queue.TakeNext());

  REQUIRE(queue.Retry("uncertain"));

  const auto retried = queue.TakeNext();

  REQUIRE(retried);
  CHECK(retried->id == "uncertain");
  CHECK(queue.Get("uncertain")->attempt == 2);
}

TEST_CASE("progress cannot move backwards")
{
  TransferQueue queue;

  REQUIRE(queue.Enqueue(DownloadJob("job")));
  REQUIRE(queue.MarkEnumerating("job"));
  REQUIRE(queue.MarkRunning("job"));
  REQUIRE(queue.UpdateProgress(TransferProgress{
      .jobId = "job", .bytesTransferred = 100, .totalBytes = {}}));

  const auto backwards = queue.UpdateProgress(
      TransferProgress{.jobId = "job", .bytesTransferred = 99, .totalBytes = {}});

  REQUIRE_FALSE(backwards);
  CHECK(backwards.error().code == RemoteErrorCode::InvalidArgument);
}

TEST_CASE("active transfer measurements are monotonic and physically bounded")
{
  TransferQueue queue;

  REQUIRE(queue.Enqueue(DownloadJob("job")));
  REQUIRE(queue.TakeNext());
  REQUIRE(queue.MarkRunning("job"));
  REQUIRE(queue.UpdateProgress(TransferProgress{
      .jobId = "job",
      .bytesTransferred = 100,
      .totalBytes = 200,
      .activeBytesTransferred = 80,
      .elapsed = std::chrono::seconds{4}}));

  SECTION("active bytes cannot exceed logical progress")
  {
    const auto invalid = queue.UpdateProgress(TransferProgress{
        .jobId = "job",
        .bytesTransferred = 100,
        .totalBytes = 200,
        .activeBytesTransferred = 101,
        .elapsed = std::chrono::seconds{5}});

    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == RemoteErrorCode::InvalidArgument);
  }

  SECTION("active bytes cannot move backwards")
  {
    const auto invalid = queue.UpdateProgress(TransferProgress{
        .jobId = "job",
        .bytesTransferred = 110,
        .totalBytes = 200,
        .activeBytesTransferred = 79,
        .elapsed = std::chrono::seconds{5}});

    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == RemoteErrorCode::InvalidArgument);
  }

  SECTION("active elapsed time cannot move backwards")
  {
    const auto invalid = queue.UpdateProgress(TransferProgress{
        .jobId = "job",
        .bytesTransferred = 110,
        .totalBytes = 200,
        .activeBytesTransferred = 90,
        .elapsed = std::chrono::seconds{3}});

    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == RemoteErrorCode::InvalidArgument);
  }

  CHECK(queue.Get("job")->progress.activeBytesTransferred == 80);
  CHECK(queue.Get("job")->progress.elapsed == std::chrono::seconds{4});
}

TEST_CASE("paused transfers are not scheduled until explicitly retried")
{
  TransferQueue queue;

  REQUIRE(queue.Enqueue(DownloadJob("paused")));
  REQUIRE(queue.Pause("paused"));
  CHECK_FALSE(queue.TakeNext());

  REQUIRE(queue.Retry("paused"));

  const auto claimed = queue.TakeNext();

  REQUIRE(claimed);
  CHECK(claimed->id == "paused");
  CHECK(queue.Get("paused")->state == TransferState::Enumerating);
}

TEST_CASE("completed and canceled transfers are terminal")
{
  TransferQueue queue;

  REQUIRE(queue.Enqueue(DownloadJob("completed")));
  REQUIRE(queue.TakeNext());
  REQUIRE(queue.MarkRunning("completed"));
  REQUIRE(queue.Complete("completed"));
  CHECK_FALSE(queue.Retry("completed"));
  CHECK_FALSE(queue.Cancel("completed"));

  REQUIRE(queue.Enqueue(DownloadJob("canceled")));
  REQUIRE(queue.Cancel("canceled"));
  CHECK_FALSE(queue.Retry("canceled"));
  CHECK_FALSE(queue.MarkEnumerating("canceled"));
}

TEST_CASE("transfer actions follow queue state and pending controls")
{
  using havremote::AvailableTransferActions;
  using havremote::TransferActionAvailability;
  using havremote::TransferControl;
  using havremote::TransferState;

  const TransferActionAvailability active{
      .pause = true, .cancel = true, .retry = false};

  CHECK(AvailableTransferActions(TransferState::Queued) == active);
  CHECK(AvailableTransferActions(TransferState::Enumerating) == active);
  CHECK(AvailableTransferActions(TransferState::Running) == active);

  const TransferActionAvailability resumable{
      .pause = false, .cancel = true, .retry = true};

  CHECK(AvailableTransferActions(TransferState::Paused) == resumable);
  CHECK(AvailableTransferActions(TransferState::Failed) == resumable);

  CHECK(AvailableTransferActions(TransferState::Completed) ==
        TransferActionAvailability{});
  CHECK(AvailableTransferActions(TransferState::Cancelled) ==
        TransferActionAvailability{});

  const TransferActionAvailability pauseRequested{
      .pause = false, .cancel = true, .retry = false};

  CHECK(AvailableTransferActions(TransferState::Running,
                                 TransferControl::Pause) ==
        pauseRequested);
  CHECK(AvailableTransferActions(TransferState::Running,
                                 TransferControl::Cancel) ==
        TransferActionAvailability{});
  CHECK(AvailableTransferActions(TransferState::Paused,
                                 TransferControl::Pause) ==
        resumable);
  CHECK(AvailableTransferActions(TransferState::Failed,
                                 TransferControl::Cancel) ==
        resumable);
}

TEST_CASE("conditional UI actions never relabel active transfers")
{
  TransferQueue queue;

  REQUIRE(queue.Enqueue(DownloadJob("active")));
  REQUIRE(queue.TakeNext());

  const auto paused = queue.PauseIfQueued("active");

  REQUIRE(paused);
  CHECK_FALSE(*paused);
  CHECK(queue.Get("active")->state == TransferState::Enumerating);

  const auto canceled = queue.CancelIfInactive("active");

  REQUIRE(canceled);
  CHECK_FALSE(*canceled);
  CHECK(queue.Get("active")->state == TransferState::Enumerating);

  REQUIRE(queue.MarkRunning("active"));
  REQUIRE(queue.Pause("active"));

  const auto canceledPaused = queue.CancelIfInactive("active");

  REQUIRE(canceledPaused);
  CHECK(*canceledPaused);
  CHECK(queue.Get("active")->state == TransferState::Cancelled);
}

TEST_CASE("progress rejects bytes beyond the declared total")
{
  TransferQueue queue;

  REQUIRE(queue.Enqueue(DownloadJob("bounded")));
  REQUIRE(queue.TakeNext());
  REQUIRE(queue.MarkRunning("bounded"));

  const auto invalid = queue.UpdateProgress(TransferProgress{
      .jobId = "bounded", .bytesTransferred = 11, .totalBytes = 10});

  REQUIRE_FALSE(invalid);
  CHECK(invalid.error().code == RemoteErrorCode::InvalidArgument);
  CHECK(queue.Get("bounded")->progress.bytesTransferred == 0);
}

TEST_CASE("duplicate ids and missing paths are rejected")
{
  TransferQueue queue;

  REQUIRE(queue.Enqueue(DownloadJob("duplicate")));

  const auto duplicate = queue.Enqueue(DownloadJob("duplicate"));

  REQUIRE_FALSE(duplicate);
  CHECK(duplicate.error().code == RemoteErrorCode::Conflict);

  auto invalid = DownloadJob();
  invalid.localPath.clear();

  CHECK_FALSE(queue.Enqueue(std::move(invalid)));
}

TEST_CASE("terminal transfers can be removed")
{
  TransferQueue queue;

  REQUIRE(queue.Enqueue(DownloadJob("completed")));
  REQUIRE(queue.TakeNext());
  REQUIRE(queue.MarkRunning("completed"));
  REQUIRE(queue.Complete("completed"));

  REQUIRE(queue.Enqueue(DownloadJob("failed")));
  REQUIRE(queue.TakeNext());
  REQUIRE(queue.Fail(
      "failed",
      RemoteError{.code = RemoteErrorCode::RemoteIo,
                  .message = "failed"}));

  REQUIRE(queue.Enqueue(DownloadJob("cancelled")));
  REQUIRE(queue.Cancel("cancelled"));

  REQUIRE(queue.RemoveTerminal("completed"));
  CHECK_FALSE(queue.Get("completed"));

  const std::vector<std::string> remaining{"failed", "cancelled"};

  REQUIRE(queue.RemoveTerminal(remaining));
  CHECK(queue.Snapshot().empty());
}

TEST_CASE("terminal bulk removal is all-or-nothing")
{
  TransferQueue queue;

  REQUIRE(queue.Enqueue(DownloadJob("failed")));
  REQUIRE(queue.TakeNext());
  REQUIRE(queue.Fail(
      "failed",
      RemoteError{.code = RemoteErrorCode::RemoteIo,
                  .message = "failed"}));
  REQUIRE(queue.Enqueue(DownloadJob("queued")));

  SECTION("an actionable transfer rejects the complete removal")
  {
    const std::vector<std::string> ids{"failed", "queued"};
    const auto removed = queue.RemoveTerminal(ids);

    REQUIRE_FALSE(removed);
    CHECK(removed.error().code == RemoteErrorCode::Conflict);
  }

  SECTION("a missing transfer rejects the complete removal")
  {
    const std::vector<std::string> ids{"failed", "missing"};
    const auto removed = queue.RemoveTerminal(ids);

    REQUIRE_FALSE(removed);
    CHECK(removed.error().code == RemoteErrorCode::NotFound);
  }

  SECTION("duplicate input is rejected")
  {
    const std::vector<std::string> ids{"failed", "failed"};
    const auto removed = queue.RemoveTerminal(ids);

    REQUIRE_FALSE(removed);
    CHECK(removed.error().code == RemoteErrorCode::InvalidArgument);
  }

  CHECK(queue.Get("failed"));
  CHECK(queue.Get("queued"));
}

TEST_CASE("single-file conflict renames update the canonical job across retries")
{
  auto job = DownloadJob("renamed");
  job.expectedRemoteRevision = RemoteFileRevision{
      RemoteEntryKind::File, 123, std::chrono::system_clock::time_point{}};

  TransferDestinationOverride resolution{
      job.localPath, job.remotePath, job.localPath, job.remotePath};

  SECTION("download changes only the local destination")
  {
    resolution.localPath = "download (2).bin";
  }
  SECTION("upload changes only the remote destination")
  {
    job.direction = TransferDirection::Upload;

    resolution.remotePath = RemotePath{"/download (2).bin"};
  }

  TransferQueue queue;

  REQUIRE(queue.Enqueue(job));
  REQUIRE(queue.TakeNext());
  REQUIRE(queue.MarkRunning(job.id));

  const auto resolved = queue.ResolveDestination(job.id, resolution);

  REQUIRE(resolved);
  CHECK(resolved->localPath == resolution.localPath);
  CHECK(resolved->remotePath == resolution.remotePath);
  CHECK(resolved->id == job.id);
  CHECK(resolved->siteId == job.siteId);
  CHECK(resolved->siteEndpoint == job.siteEndpoint);
  CHECK(resolved->direction == job.direction);
  CHECK(resolved->conflictPolicy == job.conflictPolicy);
  CHECK(resolved->expectedRemoteRevision == job.expectedRemoteRevision);
  CHECK(resolved->destinationOverrides.empty());

  REQUIRE(queue.Fail(job.id, RemoteError{
                                 .code = RemoteErrorCode::ConnectionLost,
                                 .message = "interrupted"}));

  TransferQueue restored;

  REQUIRE(restored.Restore(queue.Snapshot()));
  REQUIRE(restored.Retry(job.id));

  const auto retried = restored.TakeNext();

  REQUIRE(retried);
  CHECK(retried->localPath == resolution.localPath);
  CHECK(retried->remotePath == resolution.remotePath);
  CHECK(retried->expectedRemoteRevision == job.expectedRemoteRevision);
  CHECK(ResolveTransferPaths(*retried, retried->localPath, retried->remotePath));
  CHECK_FALSE(ResolveTransferPaths(*retried, job.localPath, job.remotePath));
}

TEST_CASE("destination resolution requires an active existing transfer")
{
  TransferQueue queue;

  const auto job = DownloadJob("inactive");
  const TransferDestinationOverride resolution{
      job.localPath, job.remotePath, "download (2).bin", job.remotePath};

  REQUIRE(queue.Enqueue(job));
  SECTION("queued") {}
  SECTION("paused") { REQUIRE(queue.Pause(job.id)); }
  SECTION("cancelled") { REQUIRE(queue.Cancel(job.id)); }
  SECTION("failed")
  {
    REQUIRE(queue.TakeNext());
    REQUIRE(queue.Fail(job.id, RemoteError{.message = "failed"}));
  }
  SECTION("completed")
  {
    REQUIRE(queue.TakeNext());
    REQUIRE(queue.MarkRunning(job.id));
    REQUIRE(queue.Complete(job.id));
  }
  CHECK_FALSE(queue.ResolveDestination(job.id, resolution));
  CHECK_FALSE(queue.ResolveDestination("missing", resolution));

  const auto unchanged = queue.Get(job.id);

  REQUIRE(unchanged);
  CHECK(unchanged->job.localPath == job.localPath);
  CHECK(unchanged->job.remotePath == job.remotePath);
}

TEST_CASE("single-file destination resolution rejects source and directory changes")
{
  auto job = DownloadJob("guarded");

  TransferDestinationOverride resolution{
      job.localPath, job.remotePath, "download (2).bin", job.remotePath};

  SECTION("download source changed")
  {
    resolution.remotePath = RemotePath{"/different.bin"};
  }
  SECTION("download destination moved to another directory")
  {
    resolution.localPath = "other/download (2).bin";
  }
  SECTION("download destination changes its parent directory's case identity")
  {
    job.localPath = "Parent/download.bin";

    resolution.originalLocalPath = job.localPath;
    resolution.localPath = "parent/download (2).bin";
  }
  SECTION("download destination traverses directories")
  {
    resolution.localPath = "other/../download (2).bin";
  }
  SECTION("original identity is stale")
  {
    resolution.originalLocalPath = "stale.bin";
  }
  SECTION("upload source changed")
  {
    job.direction = TransferDirection::Upload;

    resolution.localPath = "changed.bin";
    resolution.remotePath = RemotePath{"/download (2).bin"};
  }
  SECTION("upload destination moved to another directory")
  {
    job.direction = TransferDirection::Upload;

    resolution.localPath = job.localPath;
    resolution.remotePath = RemotePath{"/other/download (2).bin"};
  }
  SECTION("upload destination contains traversal")
  {
    job.direction = TransferDirection::Upload;

    resolution.localPath = job.localPath;
    resolution.remotePath = RemotePath{"/../download (2).bin"};
  }
  SECTION("upload destination contains command injection")
  {
    job.direction = TransferDirection::Upload;

    resolution.localPath = job.localPath;
    resolution.remotePath = RemotePath{"/download.bin\r\nDELE important"};
  }
  SECTION("upload destination display moves to another directory")
  {
    job.direction = TransferDirection::Upload;

    resolution.localPath = job.localPath;
    resolution.remotePath = RemotePath{"/download (2).bin", "/other/download (2).bin"};
  }
  SECTION("upload destination display contains traversal")
  {
    job.direction = TransferDirection::Upload;

    resolution.localPath = job.localPath;
    resolution.remotePath = RemotePath{"/download (2).bin", "/../download (2).bin"};
  }
  SECTION("upload destination display contains control characters")
  {
    job.direction = TransferDirection::Upload;

    resolution.localPath = job.localPath;
    resolution.remotePath = RemotePath{"/download (2).bin", "/download\t(2).bin"};
  }

  TransferQueue queue;

  REQUIRE(queue.Enqueue(job));
  REQUIRE(queue.TakeNext());

  const auto rejected = queue.ResolveDestination(job.id, resolution);

  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == RemoteErrorCode::InvalidArgument);
  CHECK(queue.Get(job.id)->job.localPath == job.localPath);
  CHECK(queue.Get(job.id)->job.remotePath == job.remotePath);
}

TEST_CASE("recursive conflict renames survive enumeration and replace earlier choices")
{
  auto job = DownloadJob("tree");
  job.recursive = true;
  job.localPath = "tree";
  job.remotePath = RemotePath{"/tree"};

  const auto local = std::filesystem::path{"tree/sub/file.txt"};

  const auto remote = RemotePath{"/tree/sub/file.txt"};

  TransferDestinationOverride first{local, remote, "tree/sub/file (2).txt", remote};

  auto replacement = first;
  replacement.localPath = "tree/sub/file (3).txt";

  SECTION("download") {}
  SECTION("upload")
  {
    job.direction = TransferDirection::Upload;

    first.localPath = local;
    first.remotePath = RemotePath{"/tree/sub/file (2).txt"};

    replacement.localPath = local;
    replacement.remotePath = RemotePath{"/tree/sub/file (3).txt"};
  }

  TransferQueue queue;

  REQUIRE(queue.Enqueue(job));
  REQUIRE(queue.TakeNext());
  REQUIRE(queue.ResolveDestination(job.id, first));

  const auto updated = queue.ResolveDestination(job.id, replacement);

  REQUIRE(updated);
  CHECK(updated->localPath == job.localPath);
  CHECK(updated->remotePath == job.remotePath);
  REQUIRE(updated->destinationOverrides.size() == 1);
  CHECK(updated->destinationOverrides.front() == replacement);

  REQUIRE(queue.Pause(job.id));

  TransferQueue restored;

  REQUIRE(restored.Restore(queue.Snapshot()));
  REQUIRE(restored.Retry(job.id));

  const auto retried = restored.TakeNext();

  REQUIRE(retried);

  const auto mapped = ResolveTransferPaths(*retried, local, remote);

  REQUIRE(mapped);
  CHECK(*mapped == replacement);

  const auto unrelated = ResolveTransferPaths(
      *retried, "tree/sub/other.txt", RemotePath{"/tree/sub/other.txt"});

  REQUIRE(unrelated);
  CHECK(unrelated->localPath == unrelated->originalLocalPath);
  CHECK(unrelated->remotePath == unrelated->originalRemotePath);
  CHECK_FALSE(ResolveTransferPaths(*retried, "outside/file.txt", remote));
  CHECK_FALSE(ResolveTransferPaths(*retried, local, RemotePath{"/tree-other/file.txt"}));
}

TEST_CASE("destination mapping validation rejects malformed persistent mappings atomically")
{
  auto job = DownloadJob("invalid-mapping");
  job.recursive = true;
  job.localPath = "tree";
  job.remotePath = RemotePath{"/tree"};
  job.destinationOverrides.push_back(TransferDestinationOverride{
      "tree/sub/file.txt", RemotePath{"/tree/sub/file.txt"},
      "tree/sub/file (2).txt", RemotePath{"/tree/sub/file.txt"}});

  SECTION("single-file jobs cannot retain mappings")
  {
    job.recursive = false;
  }
  SECTION("source changed")
  {
    job.destinationOverrides.front().remotePath = RemotePath{"/tree/sub/other.txt"};
  }
  SECTION("local original escapes root")
  {
    auto &mapping = job.destinationOverrides.front();
    mapping.originalLocalPath = "tree-other/file.txt";
    mapping.localPath = "tree-other/file (2).txt";
  }
  SECTION("remote source escapes root")
  {
    auto &mapping = job.destinationOverrides.front();
    mapping.originalRemotePath = RemotePath{"/tree-other/file.txt"};
    mapping.remotePath = mapping.originalRemotePath;
  }
  SECTION("remote source display escapes root")
  {
    auto &mapping = job.destinationOverrides.front();
    mapping.originalRemotePath = RemotePath{"/tree/sub/file.txt", "/outside/sub/file.txt"};
    mapping.remotePath = mapping.originalRemotePath;
  }
  SECTION("local target leaves its sibling directory")
  {
    job.destinationOverrides.front().localPath = "tree/other/file (2).txt";
  }
  SECTION("original identity is duplicated")
  {
    job.destinationOverrides.push_back(job.destinationOverrides.front());
  }
  SECTION("resolved destinations collide")
  {
    job.destinationOverrides.push_back(TransferDestinationOverride{
        "tree/sub/other.txt", RemotePath{"/tree/sub/other.txt"},
        "tree/sub/file (2).txt", RemotePath{"/tree/sub/other.txt"}});
  }
  SECTION("remote path has traversal")
  {
    auto &mapping = job.destinationOverrides.front();
    mapping.originalRemotePath = RemotePath{"/tree/../secret.txt"};
    mapping.remotePath = mapping.originalRemotePath;
  }
  SECTION("local root has traversal")
  {
    job.localPath = "outer/../tree";
  }

  CHECK_FALSE(ValidateTransferDestinations(job));

  TransferQueue queue;

  CHECK_FALSE(queue.Enqueue(job));

  auto restored = RestoredTransfer(job.id, TransferState::Paused);
  restored.job = job;

  CHECK_FALSE(queue.Restore({RestoredTransfer("valid", TransferState::Paused), restored}));
  CHECK(queue.Snapshot().empty());
}

TEST_CASE("recursive destination mutation rejects collisions without replacing the prior mapping")
{
  auto job = DownloadJob("collision");
  job.recursive = true;
  job.localPath = "tree";
  job.remotePath = RemotePath{"/tree"};

  const TransferDestinationOverride first{
      "tree/first.txt", RemotePath{"/tree/first.txt"},
      "tree/chosen.txt", RemotePath{"/tree/first.txt"}};

  const TransferDestinationOverride second{
      "tree/second.txt", RemotePath{"/tree/second.txt"},
      "tree/chosen.txt", RemotePath{"/tree/second.txt"}};

  TransferQueue queue;

  REQUIRE(queue.Enqueue(job));
  REQUIRE(queue.TakeNext());
  REQUIRE(queue.ResolveDestination(job.id, first));
  CHECK_FALSE(queue.ResolveDestination(job.id, second));

  const auto canonical = queue.Get(job.id);

  REQUIRE(canonical);
  REQUIRE(canonical->job.destinationOverrides.size() == 1);
  CHECK(canonical->job.destinationOverrides.front() == first);
}

TEST_CASE("recursive destination metadata preserves case-distinct local identities")
{
  auto job = DownloadJob("case-distinct");
  job.recursive = true;
  job.localPath = "tree";
  job.remotePath = RemotePath{"/tree"};

  TransferDestinationOverride first{
      "tree/source.txt", RemotePath{"/tree/source.txt"},
      "tree/result.txt", RemotePath{"/tree/source.txt"}};

  TransferDestinationOverride second{
      "tree/other.txt", RemotePath{"/tree/other.txt"},
      "tree/RESULT.txt", RemotePath{"/tree/other.txt"}};

  SECTION("resolved destinations differ only in case") {}
  SECTION("original identities differ only in case")
  {
    second.originalLocalPath = "tree/SOURCE.txt";
    second.originalRemotePath = RemotePath{"/tree/SOURCE.txt"};
    second.remotePath = second.originalRemotePath;
    second.localPath = "tree/other-result.txt";
  }

  TransferQueue queue;

  REQUIRE(queue.Enqueue(job));
  REQUIRE(queue.TakeNext());
  REQUIRE(queue.ResolveDestination(job.id, first));

  const auto resolved = queue.ResolveDestination(job.id, second);

  REQUIRE(resolved);
  REQUIRE(resolved->destinationOverrides.size() == 2U);
  CHECK(resolved->destinationOverrides[0] == first);
  CHECK(resolved->destinationOverrides[1] == second);

  REQUIRE(queue.Pause(job.id));

  TransferQueue restored;

  REQUIRE(restored.Restore(queue.Snapshot()));

  const auto saved = restored.Get(job.id);

  REQUIRE(saved);

  for (const auto &expected : {first, second})
  {
    const auto mapped = ResolveTransferPaths(
        saved->job, expected.originalLocalPath, expected.originalRemotePath);

    REQUIRE(mapped);
    CHECK(*mapped == expected);
  }
}

TEST_CASE("destination resolution preserves non-UTF8 remote bytes and UTF8 display paths")
{
  auto job = DownloadJob("encoded");
  job.direction = TransferDirection::Upload;
  job.recursive = true;
  job.localPath = "tree";
  job.remotePath = RemotePath{"/caf\xe9", "/caf\xc3\xa9"};

  const TransferDestinationOverride resolution{
      "tree/file.txt", RemotePath{"/caf\xe9/\xe4.txt", "/caf\xc3\xa9/\xc3\xa4.txt"},
      "tree/file.txt", RemotePath{"/caf\xe9/\xe4 (2).txt", "/caf\xc3\xa9/\xc3\xa4 (2).txt"}};

  TransferQueue queue;

  REQUIRE(queue.Enqueue(job));
  REQUIRE(queue.TakeNext());

  const auto resolved = queue.ResolveDestination(job.id, resolution);

  REQUIRE(resolved);
  REQUIRE(ValidateTransferDestinations(*resolved));

  const auto mapped = ResolveTransferPaths(
      *resolved, resolution.originalLocalPath, resolution.originalRemotePath);

  REQUIRE(mapped);
  CHECK(*mapped == resolution);
}
