// SPDX-License-Identifier: MIT

#include "config/csonQueueRepository.hpp"
#include "core/session.hpp"
#include "platform/pathSafety.hpp"
#include "protocol/ftpSession.hpp"
#include "protocol/sftpSession.hpp"
#include "ui/remoteController.hpp"

#include <catch2/catch_test_macros.hpp>

#include <wx/app.h>
#include <wx/init.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
  using namespace havremote;
  using namespace havremote::ui;
  using namespace std::chrono_literals;

  constexpr std::string_view OriginalContents = "original contents";
  constexpr std::string_view IncomingContents = "replacement contents";

  RemoteError TestError(const RemoteErrorCode code, std::string message)
  {
    return RemoteError{.code = code, .message = std::move(message)};
  }

  void WriteFile(const std::filesystem::path &path, const std::string_view text)
  {
    if (!path.parent_path().empty())
    {
      std::filesystem::create_directories(path.parent_path());
    }

    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(text.data(), static_cast<std::streamsize>(text.size()));

    if (!output)
    {
      throw std::runtime_error{"Cannot write controller test fixture"};
    }
  }

  std::string ReadFile(const std::filesystem::path &path)
  {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
  }

  struct RemoteFile final
  {
    RemoteEntry entry;
    std::string contents;
  };

  RemoteFile MakeRemoteFile(const RemotePath &path, std::string contents,
                            const RemoteEntryKind kind = RemoteEntryKind::File)
  {
    RemoteEntry entry;
    entry.path = path;
    entry.name = path.Filename();
    entry.kind = kind;
    entry.size = contents.size();
    entry.modifiedAt = std::chrono::system_clock::time_point{1'700'000'000s};

    return {std::move(entry), std::move(contents)};
  }

  struct TransferCall final
  {
    std::filesystem::path local;
    RemotePath remote;
    std::uint64_t resumeOffset{};
    bool durableDestination{};
    bool durableUploadCheckpoint{};
  };

  struct FakeServer final
  {
    std::mutex mutex;
    std::map<std::string, RemoteFile> files;
    std::deque<std::optional<RemoteErrorCode>> outcomes;
    std::vector<TransferCall> calls;
    std::deque<std::optional<RemoteError>> permissionOutcomes;
    std::vector<RemotePath> permissionCalls;
    std::deque<std::optional<RemoteError>> listOutcomes;
    std::vector<RemotePath> listCalls;
    std::vector<config::PersistentQueueItem> durableItems;
  };

  // Test-owned protocol factories exercise RemoteController's scheduling,
  // conflict handling, and persistence without a server.
  std::shared_ptr<FakeServer> activeServer;

  class FakeSession final : public IRemoteSession
  {
  public:
    explicit FakeSession(std::shared_ptr<FakeServer> server)
        : mServer(std::move(server)) {}

    ProtocolKind Protocol() const noexcept override { return ProtocolKind::Ftp; }
    bool Connected() const noexcept override { return mConnected; }
    Result<void> Connect(const SiteProfile &, const SessionCallbacks &,
                         std::stop_token) override
    {
      mConnected = true;
      return {};
    }
    void Disconnect() noexcept override { mConnected = false; }

    Result<std::vector<RemoteEntry>> List(const RemotePath &path,
                                          std::stop_token) override
    {
      std::scoped_lock lock{mServer->mutex};

      mServer->listCalls.push_back(path);

      if (!mServer->listOutcomes.empty())
      {
        const auto error = std::move(mServer->listOutcomes.front());

        mServer->listOutcomes.pop_front();

        if (error)
        {
          return std::unexpected(*error);
        }
      }

      std::vector<RemoteEntry> result;

      for (const auto &[key, file] : mServer->files)
      {
        if (key != path.Bytes() && file.entry.path.Parent().Bytes() == path.Bytes())
        {
          result.push_back(file.entry);
        }
      }

      return result;
    }

    Result<RemoteEntry> Stat(const RemotePath &path, std::stop_token) override
    {
      std::scoped_lock lock{mServer->mutex};

      const auto found = mServer->files.find(path.Bytes());

      if (found == mServer->files.end())
      {
        return std::unexpected(TestError(RemoteErrorCode::NotFound, "Missing test file"));
      }

      return found->second.entry;
    }

    Result<void> Mkdir(const RemotePath &path, bool, std::stop_token) override
    {
      std::scoped_lock lock{mServer->mutex};

      mServer->files.try_emplace(path.Bytes(), MakeRemoteFile(path, {}, RemoteEntryKind::Directory));

      return {};
    }
    Result<void> CreateRemoteFile(const RemotePath &, bool, std::stop_token) override
    {
      return Unsupported();
    }
    Result<void> Rename(const RemotePath &, const RemotePath &, bool,
                        std::stop_token) override
    {
      return Unsupported();
    }
    Result<void> Remove(const RemotePath &, bool, std::stop_token) override
    {
      return Unsupported();
    }
    Result<void> SetPermissions(const RemotePath &path, std::uint32_t,
                                std::stop_token) override
    {
      std::scoped_lock lock{mServer->mutex};

      mServer->permissionCalls.push_back(path);

      if (mServer->permissionOutcomes.empty())
      {
        return Unsupported();
      }

      const auto error = std::move(mServer->permissionOutcomes.front());

      mServer->permissionOutcomes.pop_front();

      return error ? Result<void>{std::unexpected(*error)} : Result<void>{};
    }

    Result<void> Upload(const std::filesystem::path &local, const RemotePath &remote,
                        const TransferOptions &options, const ProgressCallback &,
                        std::stop_token) override
    {
      const auto contents = ReadFile(local);

      std::scoped_lock lock{mServer->mutex};

      const auto outcome = RecordCall(local, remote, options, TransferDirection::Upload);

      if (outcome)
      {
        if (options.temporaryRemotePath)
        {
          mServer->files[options.temporaryRemotePath->Bytes()] =
              MakeRemoteFile(*options.temporaryRemotePath, contents.substr(0, 3));
        }

        auto error = TestError(*outcome, "Injected interrupted upload");

        if (*outcome == RemoteErrorCode::RemoteIo)
        {
          // Match a confirmed FTP 552 refusal after some bytes were stored
          error.nativeCode = 552;
          error.operationMayHaveSucceeded = true;
        }

        return std::unexpected(std::move(error));
      }

      mServer->files[remote.Bytes()] = MakeRemoteFile(remote, contents);

      if (options.temporaryRemotePath)
      {
        mServer->files.erase(options.temporaryRemotePath->Bytes());
      }

      return {};
    }

    Result<void> Download(const RemotePath &remote, const std::filesystem::path &local,
                          const TransferOptions &options, const ProgressCallback &,
                          std::stop_token) override
    {
      std::scoped_lock lock{mServer->mutex};

      const auto outcome = RecordCall(local, remote, options, TransferDirection::Download);
      const auto contents = mServer->files.at(remote.Bytes()).contents;

      auto partial = local;
      partial += L".havremote.part";

      if (outcome)
      {
        WriteFile(partial, contents.substr(0, 3));

        auto error = TestError(*outcome, "Injected interrupted download");

        if (*outcome == RemoteErrorCode::TimedOut)
        {
          // Match a real libcurl low-speed timeout, including its advisory
          // retryable flag: the controller must still wait for user Retry.
          error.nativeCode = 28;
          error.retryable = true;
        }

        return std::unexpected(std::move(error));
      }

      WriteFile(local, contents);

      std::error_code ignored;

      std::filesystem::remove(partial, ignored);

      return {};
    }

  private:
    Result<void> Unsupported() const
    {
      return std::unexpected(TestError(RemoteErrorCode::Unsupported,
                                       "Operation not used by controller test"));
    }

    std::optional<RemoteErrorCode> RecordCall(
        const std::filesystem::path &local, const RemotePath &remote,
        const TransferOptions &options, const TransferDirection direction)
    {
      TransferCall call{.local = local, .remote = remote, .resumeOffset = options.resumeOffset};

      for (const auto &item : mServer->durableItems)
      {
        const auto &job = item.transfer.job;

        if (job.id != options.jobId)
        {
          continue;
        }

        call.durableDestination = job.localPath == local &&
                                  job.remotePath == remote;

        for (const auto &mapping : job.destinationOverrides)
        {
          call.durableDestination = call.durableDestination ||
                                    (mapping.localPath == local && mapping.remotePath == remote);
        }

        if (direction == TransferDirection::Upload)
        {
          call.durableUploadCheckpoint = std::ranges::any_of(
              item.resumeRecords, [&](const config::QueueResumeRecord &record)
              { return record.localPath == local && record.remotePath == remote &&
                       record.temporaryRemotePath == options.temporaryRemotePath; });
        }
      }

      mServer->calls.push_back(std::move(call));

      if (mServer->outcomes.empty())
      {
        return std::nullopt;
      }

      const auto outcome = mServer->outcomes.front();

      mServer->outcomes.pop_front();

      return outcome;
    }

    std::shared_ptr<FakeServer> mServer;
    bool mConnected{};
  };

  class ControllerHarness final
  {
  public:
    explicit ControllerHarness(const bool useRootAsWorkingDirectory = false)
        : root(std::filesystem::temp_directory_path() /
               ("havremote-controller-" + GenerateId())),
          repository(root / "queue.cson"), server(std::make_shared<FakeServer>())
    {
      if (!initializer.IsOk())
      {
        throw std::runtime_error{"Cannot initialize wxWidgets for controller tests"};
      }

      std::filesystem::create_directories(root);

      activeServer = server;

      site.id = "test-site";
      site.name = "Controller test";
      site.protocol = ProtocolKind::Ftp;
      site.host = "controller.invalid";
      site.port = 21;
      site.username = "test-user";
      site.initialRemoteDirectory = RemotePath{"/remote"};

      server->files.emplace("/remote", MakeRemoteFile(RemotePath{"/remote"}, {},
                                                      RemoteEntryKind::Directory));

      target.Bind(EVT_HAVREMOTE_CONTROLLER, [this](wxThreadEvent &event)
                  {
        const auto payload = event.GetPayload<ControllerEventEnvelopePtr>();

        if (const auto *connection = std::get_if<ConnectionEvent>(&payload->event))
        {
          connected = connection->connected;
        }

        if (const auto *error = std::get_if<OperationErrorEvent>(&payload->event))
        {
          operationErrors.push_back(*error);
        }

        if (const auto *inspection = std::get_if<RemoteInspectionEvent>(&payload->event))
        {
          inspectionIds.push_back(inspection->requestId);
        }

        if (std::holds_alternative<DirectoryEvent>(payload->event))
        {
          ++directoryEvents;
        }

        if (const auto *queue = std::get_if<QueueEvent>(&payload->event);
            queue && queue->durableChange && controller)
        {
          (void)Save();
        } });

      if (useRootAsWorkingDirectory)
      {
        mPreviousWorkingDirectory = std::filesystem::current_path();
        std::filesystem::current_path(root);
      }
    }

    ~ControllerHarness()
    {
      controller.reset();

      wxTheApp->RemovePendingEventHandler(&target);

      target.DeletePendingEvents();

      activeServer.reset();

      std::error_code ignored;

      if (mPreviousWorkingDirectory)
      {
        std::filesystem::current_path(*mPreviousWorkingDirectory, ignored);
      }

      std::filesystem::remove_all(root, ignored);
    }

    bool Start(std::vector<config::PersistentQueueItem> restored = {})
    {
      controller.reset();

      wxTheApp->RemovePendingEventHandler(&target);

      target.DeletePendingEvents();

      connected = false;

      UiInteractions interactions;
      interactions.resolveConflict = [this](const ConflictChallenge &challenge)
          -> Result<ConflictResolution>
      {
        resumeOffered.push_back(challenge.safeResumeAvailable);

        if (choices.empty())
        {
          return std::unexpected(TestError(RemoteErrorCode::Conflict,
                                           "Unexpected additional conflict prompt"));
        }

        const auto choice = choices.front();

        choices.pop_front();

        if (failSaveAfterRename && choice == ConflictPolicy::Rename)
        {
          failNextSave = true;
        }

        return ConflictResolution{choice, false};
      };

      interactions.persistQueue = [this](const std::string &)
      { return Save(); };

      mTransferRuntime = std::make_shared<TransferRuntime>(
          1, platform::CheckLocalPathConflict);

      controller = std::make_unique<RemoteController>(
          target, "test-connection", mTransferRuntime,
          root / "known_hosts", std::move(interactions), 2s, 2s, std::move(restored));

      (void)controller->Connect(site);

      return PumpUntil([this]
                       { return connected; });
    }

    Result<TransferRuntime::DestinationReservation> ReserveLocalDestination(
        const std::filesystem::path &path)
    {
      return mTransferRuntime->ReserveLocalDestination(path);
    }

    Result<void> Save()
    {
      if (std::exchange(failNextSave, false))
      {
        return std::unexpected(TestError(RemoteErrorCode::LocalIo,
                                         "Injected queue save failure"));
      }

      auto items = controller->PersistentQueueItems();

      const auto result = repository.Save(items);

      if (!result)
      {
        saveErrors.push_back(result.error().message);

        return std::unexpected(TestError(RemoteErrorCode::LocalIo, result.error().message));
      }

      std::scoped_lock lock{server->mutex};

      server->durableItems = std::move(items);

      return {};
    }

    template <typename Predicate>
    bool PumpUntil(Predicate predicate)
    {
      const auto deadline = std::chrono::steady_clock::now() + 5s;

      do
      {
        wxTheApp->ProcessPendingEvents();

        if (predicate())
        {
          wxTheApp->ProcessPendingEvents();

          return true;
        }

        std::this_thread::sleep_for(1ms);
      } while (std::chrono::steady_clock::now() < deadline);

      return false;
    }

    bool WaitFor(const TransferState state)
    {
      return PumpUntil([&]
                       {
        const auto items = controller->Transfers();
        return items.size() == 1 && items.front().state == state; });
    }

    TransferJob Job(const TransferDirection direction, const bool recursive) const
    {
      return TransferJob{.id = "test-job", .siteId = site.id, .siteEndpoint = EndpointIdentity(site), .direction = direction, .localPath = root / (recursive ? "tree" : "report.txt"), .remotePath = RemotePath{recursive ? "/remote/tree" : "/remote/report.txt"}, .recursive = recursive, .conflictPolicy = ConflictPolicy::Ask};
    }

    wxInitializer initializer;
    std::filesystem::path root;
    config::CsonQueueRepository repository;
    std::shared_ptr<FakeServer> server;
    wxEvtHandler target;
    SiteProfile site;
    std::unique_ptr<RemoteController> controller;
    std::deque<ConflictPolicy> choices;
    std::vector<bool> resumeOffered;
    std::vector<std::string> saveErrors;
    std::vector<OperationErrorEvent> operationErrors;
    std::vector<std::string> inspectionIds;
    std::size_t directoryEvents{};
    bool failSaveAfterRename{};
    bool failNextSave{};
    bool connected{};

  private:
    std::shared_ptr<TransferRuntime> mTransferRuntime;
    std::optional<std::filesystem::path> mPreviousWorkingDirectory;
  };
} // namespace

namespace havremote
{
  RemoteSessionPtr MakeFtpSession() { return std::make_unique<FakeSession>(activeServer); }
  RemoteSessionPtr MakeSftpSession() { return std::make_unique<FakeSession>(activeServer); }
} // namespace havremote

TEST_CASE("controller persists renamed upload destinations before writing and reuses them",
          "[controller][queue][rename]")
{
  bool recursive{};
  bool restore{};

  auto interruption = RemoteErrorCode::ProtocolError;

  SECTION("single file retries a failed upload") {}
  SECTION("single file retries a paused upload") { interruption = RemoteErrorCode::Paused; }
  SECTION("single file resumes after restoration") { restore = true; }
  SECTION("recursive upload resumes its chosen child after restoration")
  {
    recursive = true;
    restore = true;
  }

  ControllerHarness harness;

  const auto job = harness.Job(TransferDirection::Upload, recursive);
  const auto local = recursive ? job.localPath / "report.txt" : job.localPath;
  const auto original = recursive ? job.remotePath.Joined(RemotePath{"report.txt"}) : job.remotePath;
  const auto renamed = original.Parent().Joined(RemotePath{"report (2).txt"});

  WriteFile(local, IncomingContents);

  harness.server->files[original.Bytes()] = MakeRemoteFile(original, std::string{OriginalContents});
  harness.server->outcomes = {interruption, std::nullopt};
  harness.choices = {ConflictPolicy::Rename, ConflictPolicy::Resume};

  REQUIRE(harness.Start());
  REQUIRE(harness.controller->Enqueue(job));
  REQUIRE(harness.WaitFor(interruption == RemoteErrorCode::Paused
                              ? TransferState::Paused
                              : TransferState::Failed));
  REQUIRE(harness.Save());
  REQUIRE(harness.saveErrors.empty());

  auto loaded = harness.repository.Load();

  REQUIRE(loaded);
  REQUIRE(loaded->items.size() == 1);
  REQUIRE(loaded->items.front().resumeRecords.size() == 1);
  CHECK(loaded->items.front().resumeRecords.front().remotePath == renamed);

  if (recursive)
  {
    CHECK(loaded->items.front().transfer.job.remotePath == job.remotePath);
    REQUIRE(loaded->items.front().transfer.job.destinationOverrides.size() == 1);
    CHECK(loaded->items.front().transfer.job.destinationOverrides.front().remotePath == renamed);
  }
  else
  {
    CHECK(loaded->items.front().transfer.job.remotePath == renamed);
  }

  if (restore)
  {
    REQUIRE(harness.Start(std::move(loaded->items)));
  }

  REQUIRE(harness.controller->Retry(job.id));
  REQUIRE(harness.WaitFor(TransferState::Completed));
  REQUIRE(harness.saveErrors.empty());
  REQUIRE(harness.resumeOffered.size() == 2);
  CHECK(harness.resumeOffered.back());

  std::scoped_lock lock{harness.server->mutex};

  REQUIRE(harness.server->calls.size() == 2);

  for (const auto &call : harness.server->calls)
  {
    CHECK(call.remote == renamed);
    CHECK(call.local == local);
    CHECK(call.durableDestination);
    CHECK(call.durableUploadCheckpoint);
  }

  CHECK(harness.server->calls.back().resumeOffset == 3);
  CHECK(harness.server->files.at(original.Bytes()).contents == OriginalContents);
  CHECK(harness.server->files.at(renamed.Bytes()).contents == IncomingContents);
}

TEST_CASE("controller persists renamed download destinations across interruption and restoration",
          "[controller][queue][rename]")
{
  bool recursive{};

  SECTION("single file") {}
  SECTION("recursive child") { recursive = true; }

  ControllerHarness harness;

  const auto job = harness.Job(TransferDirection::Download, recursive);
  const auto original = recursive ? job.localPath / "report.txt" : job.localPath;
  const auto renamed = original.parent_path() / "report (2).txt";
  const auto remote = recursive ? job.remotePath.Joined(RemotePath{"report.txt"}) : job.remotePath;

  WriteFile(original, OriginalContents);

  if (recursive)
  {
    harness.server->files[job.remotePath.Bytes()] = MakeRemoteFile(job.remotePath, {}, RemoteEntryKind::Directory);
  }

  harness.server->files[remote.Bytes()] = MakeRemoteFile(remote, std::string{IncomingContents});
  harness.server->outcomes = {RemoteErrorCode::Paused, std::nullopt};
  harness.choices = {ConflictPolicy::Rename, ConflictPolicy::Resume};

  REQUIRE(harness.Start());
  REQUIRE(harness.controller->Enqueue(job));
  REQUIRE(harness.WaitFor(TransferState::Paused));
  REQUIRE(harness.Save());
  REQUIRE(harness.saveErrors.empty());

  auto loaded = harness.repository.Load();

  REQUIRE(loaded);
  REQUIRE(loaded->items.size() == 1);
  REQUIRE(loaded->items.front().resumeRecords.size() == 1);
  CHECK(loaded->items.front().resumeRecords.front().localPath == renamed);
  REQUIRE(harness.Start(std::move(loaded->items)));
  REQUIRE(harness.controller->Retry(job.id));
  REQUIRE(harness.WaitFor(TransferState::Completed));
  REQUIRE(harness.saveErrors.empty());
  CHECK(ReadFile(original) == OriginalContents);
  CHECK(ReadFile(renamed) == IncomingContents);
  REQUIRE(harness.resumeOffered.size() == 2);
  CHECK(harness.resumeOffered.back());

  std::scoped_lock lock{harness.server->mutex};

  REQUIRE(harness.server->calls.size() == 2);

  for (const auto &call : harness.server->calls)
  {
    CHECK(call.local == renamed);
    CHECK(call.remote == remote);
    CHECK(call.durableDestination);
  }

  CHECK(harness.server->calls.back().resumeOffset == 3);
}

TEST_CASE("controller downloads to relative local destinations",
          "[controller][queue][paths]")
{
  bool recursive{};

  std::filesystem::path destination{"report.txt"};

  SECTION("a bare relative filename") {}
  SECTION("a file beneath a relative directory")
  {
    destination = std::filesystem::path{"downloads"} / "report.txt";
  }
  SECTION("a recursive relative root")
  {
    recursive = true;
    destination = "tree";
  }
  SECTION("a recursive current-directory root")
  {
    recursive = true;
    destination = ".";
  }

  ControllerHarness harness{true};

  auto job = harness.Job(TransferDirection::Download, recursive);
  job.localPath = destination;

  auto remote = job.remotePath;
  auto expected = harness.root / destination;

  if (recursive)
  {
    harness.server->files[job.remotePath.Bytes()] =
        MakeRemoteFile(job.remotePath, {}, RemoteEntryKind::Directory);

    const auto nested = job.remotePath.Joined(RemotePath{"nested"});

    harness.server->files[nested.Bytes()] =
        MakeRemoteFile(nested, {}, RemoteEntryKind::Directory);

    remote = nested.Joined(RemotePath{"report.txt"});

    expected /= std::filesystem::path{"nested"} / "report.txt";
  }

  expected = expected.lexically_normal();

  harness.server->files[remote.Bytes()] =
      MakeRemoteFile(remote, std::string{IncomingContents});

  REQUIRE(harness.Start());
  REQUIRE(harness.controller->Enqueue(job));
  REQUIRE(harness.PumpUntil([&]
                            {
    const auto items = harness.controller->Transfers();

    return items.size() == 1U &&
           (items.front().state == TransferState::Completed ||
            items.front().state == TransferState::Failed); }));

  const auto items = harness.controller->Transfers();

  REQUIRE(items.size() == 1U);
  INFO("Transfer result: " << (items.front().error ? items.front().error->message : "No transfer error"));
  REQUIRE(items.front().state == TransferState::Completed);
  CHECK(harness.saveErrors.empty());
  CHECK(ReadFile(expected) == IncomingContents);

  std::scoped_lock lock{harness.server->mutex};

  REQUIRE(harness.server->calls.size() == 1U);
  CHECK(std::filesystem::absolute(harness.server->calls.front().local).lexically_normal() ==
        expected);
  CHECK(harness.server->calls.front().remote == remote);
}

TEST_CASE("controller reserves the partial download destination before writing",
          "[controller][queue][paths]")
{
  ControllerHarness harness;

  const auto job = harness.Job(TransferDirection::Download, false);

  auto partial = job.localPath;
  partial += ".havremote.part";

  harness.server->files[job.remotePath.Bytes()] =
      MakeRemoteFile(job.remotePath, std::string{IncomingContents});

  REQUIRE(harness.Start());

  {
    auto activeWriter = harness.ReserveLocalDestination(partial);

    REQUIRE(activeWriter);
    REQUIRE(harness.controller->Enqueue(job));
    REQUIRE(harness.WaitFor(TransferState::Failed));

    const auto items = harness.controller->Transfers();

    REQUIRE(items.size() == 1U);
    REQUIRE(items.front().error);
    CHECK(items.front().error->code == RemoteErrorCode::Conflict);
    CHECK_FALSE(std::filesystem::exists(job.localPath));
    CHECK_FALSE(std::filesystem::exists(partial));

    std::scoped_lock lock{harness.server->mutex};

    CHECK(harness.server->calls.empty());
  }

  REQUIRE(harness.controller->Retry(job.id));
  REQUIRE(harness.WaitFor(TransferState::Completed));
  CHECK(harness.saveErrors.empty());
  CHECK(ReadFile(job.localPath) == IncomingContents);
  CHECK_FALSE(std::filesystem::exists(partial));

  std::scoped_lock lock{harness.server->mutex};

  REQUIRE(harness.server->calls.size() == 1U);
  CHECK(harness.server->calls.front().local == job.localPath);
  CHECK(harness.server->calls.front().remote == job.remotePath);
}

TEST_CASE("controller does not upload when saving a renamed destination fails",
          "[controller][queue][rename]")
{
  ControllerHarness harness;

  const auto job = harness.Job(TransferDirection::Upload, false);

  WriteFile(job.localPath, IncomingContents);

  harness.server->files[job.remotePath.Bytes()] = MakeRemoteFile(job.remotePath, std::string{OriginalContents});
  harness.choices = {ConflictPolicy::Rename};
  harness.failSaveAfterRename = true;

  REQUIRE(harness.Start());
  REQUIRE(harness.controller->Enqueue(job));
  REQUIRE(harness.WaitFor(TransferState::Failed));

  {
    std::scoped_lock lock{harness.server->mutex};

    CHECK(harness.server->calls.empty());
  }

  const RemotePath renamed{"/remote/report (2).txt"};

  CHECK(harness.controller->Transfers().front().job.remotePath == renamed);
  REQUIRE(harness.controller->Retry(job.id));
  REQUIRE(harness.WaitFor(TransferState::Completed));

  std::scoped_lock lock{harness.server->mutex};

  REQUIRE(harness.server->calls.size() == 1);
  CHECK(harness.server->calls.front().remote == renamed);
  CHECK(harness.server->calls.front().durableDestination);
  CHECK(harness.server->calls.front().durableUploadCheckpoint);
}

TEST_CASE("controller retains a timed-out download until explicit retry and resumes its partial",
          "[controller][queue][timeout]")
{
  ControllerHarness harness;

  const auto job = harness.Job(TransferDirection::Download, false);
  auto part = job.localPath;
  part += L".havremote.part";

  WriteFile(job.localPath, OriginalContents);

  const auto source = MakeRemoteFile(job.remotePath, std::string{IncomingContents});

  harness.server->files[job.remotePath.Bytes()] = source;
  harness.server->outcomes = {RemoteErrorCode::TimedOut, std::nullopt, std::nullopt};
  harness.choices = {ConflictPolicy::Overwrite, ConflictPolicy::Resume};

  REQUIRE(harness.Start());
  REQUIRE(harness.controller->Enqueue(job));
  REQUIRE(harness.WaitFor(TransferState::Failed));

  const auto transfer = [&](const std::string_view id)
  {
    const auto items = harness.controller->Transfers();
    const auto found = std::ranges::find_if(items, [id](const auto &item)
                                            { return item.job.id == id; });

    REQUIRE(found != items.end());

    return *found;
  };

  const auto failed = transfer(job.id);

  REQUIRE(failed.error);
  CHECK(failed.error->code == RemoteErrorCode::TimedOut);
  CHECK(failed.error->nativeCode == 28);
  CHECK(failed.error->retryable);
  CHECK_FALSE(failed.error->operationMayHaveSucceeded);
  CHECK(failed.attempt == 1U);
  CHECK(ReadFile(job.localPath) == OriginalContents);
  CHECK(ReadFile(part) == IncomingContents.substr(0, 3));

  REQUIRE(harness.Save());
  REQUIRE(harness.saveErrors.empty());

  const auto saved = harness.repository.Load();

  REQUIRE(saved);
  REQUIRE(saved->items.size() == 1U);

  const auto &savedItem = saved->items.front();

  CHECK(savedItem.transfer.state == TransferState::Failed);
  REQUIRE(savedItem.transfer.error);
  CHECK(savedItem.transfer.error->code == RemoteErrorCode::TimedOut);
  CHECK(savedItem.transfer.error->nativeCode == 28);
  CHECK(savedItem.transfer.error->retryable);
  REQUIRE(savedItem.resumeRecords.size() == 1U);

  const auto &record = savedItem.resumeRecords.front();

  CHECK(record.kind == config::QueueResumeKind::Download);
  CHECK(record.localPath == job.localPath);
  CHECK(record.remotePath == job.remotePath);
  CHECK(record.remoteSize == IncomingContents.size());
  CHECK(record.remoteModifiedAt == source.entry.modifiedAt);
  CHECK(record.partSize == 3U);
  CHECK(record.partModifiedAt == std::filesystem::last_write_time(part));

  // Give the worker real subsequent work instead of relying on a short sleep
  // to infer that it did not automatically retry the failed transfer.
  auto probe = harness.Job(TransferDirection::Download, false);
  probe.id = "subsequent-job";
  probe.localPath = harness.root / "subsequent.txt";
  probe.remotePath = RemotePath{"/remote/subsequent.txt"};
  {
    std::scoped_lock lock{harness.server->mutex};

    harness.server->files[probe.remotePath.Bytes()] =
        MakeRemoteFile(probe.remotePath, "subsequent download");
  }

  REQUIRE(harness.controller->Enqueue(probe));
  REQUIRE(harness.PumpUntil([&]
                            { return transfer(probe.id).state == TransferState::Completed; }));

  const auto stillFailed = transfer(job.id);

  CHECK(stillFailed.state == TransferState::Failed);
  CHECK(stillFailed.attempt == 1U);
  REQUIRE(stillFailed.error);
  CHECK(stillFailed.error->nativeCode == 28);
  CHECK(ReadFile(job.localPath) == OriginalContents);
  CHECK(ReadFile(part) == IncomingContents.substr(0, 3));
  {
    std::scoped_lock lock{harness.server->mutex};

    REQUIRE(harness.server->calls.size() == 2U);
    CHECK(harness.server->calls.front().remote == job.remotePath);
    CHECK(harness.server->calls.back().remote == probe.remotePath);
    CHECK(harness.server->outcomes.size() == 1U);
  }

  REQUIRE(harness.controller->Retry(job.id));
  REQUIRE(harness.PumpUntil([&]
                            { return transfer(job.id).state == TransferState::Completed; }));

  const auto completed = transfer(job.id);

  CHECK(completed.attempt == 2U);
  CHECK_FALSE(completed.error);
  CHECK(ReadFile(job.localPath) == IncomingContents);
  CHECK_FALSE(std::filesystem::exists(part));
  REQUIRE(harness.resumeOffered.size() == 2U);
  CHECK_FALSE(harness.resumeOffered.front());
  CHECK(harness.resumeOffered.back());
  REQUIRE(harness.saveErrors.empty());

  std::scoped_lock lock{harness.server->mutex};

  REQUIRE(harness.server->calls.size() == 3U);
  CHECK(harness.server->calls.back().remote == job.remotePath);
  CHECK(harness.server->calls.back().local == job.localPath);
  CHECK(harness.server->calls.back().resumeOffset == 3U);
}

TEST_CASE("controller retains a storage-rejected upload for explicit resumable retry",
          "[controller][queue][upload-storage]")
{
  ControllerHarness harness;

  const auto job = harness.Job(TransferDirection::Upload, false);

  WriteFile(job.localPath, IncomingContents);

  harness.server->files[job.remotePath.Bytes()] =
      MakeRemoteFile(job.remotePath, std::string{OriginalContents});
  harness.server->outcomes = {RemoteErrorCode::RemoteIo, std::nullopt, std::nullopt};
  harness.choices = {ConflictPolicy::Overwrite, ConflictPolicy::Resume};

  REQUIRE(harness.Start());
  REQUIRE(harness.controller->Enqueue(job));
  REQUIRE(harness.WaitFor(TransferState::Failed));
  REQUIRE(harness.Save());

  const auto saved = harness.repository.Load();

  REQUIRE(saved);
  REQUIRE(saved->items.size() == 1U);

  const auto &item = saved->items.front();

  CHECK(item.transfer.state == TransferState::Failed);
  CHECK(item.transfer.attempt == 1U);
  REQUIRE(item.transfer.error);
  CHECK(item.transfer.error->code == RemoteErrorCode::RemoteIo);
  CHECK(item.transfer.error->nativeCode == 552);
  CHECK_FALSE(item.transfer.error->retryable);
  CHECK(item.transfer.error->operationMayHaveSucceeded);
  REQUIRE(item.resumeRecords.size() == 1U);

  const auto &record = item.resumeRecords.front();

  CHECK(record.kind == config::QueueResumeKind::Upload);
  CHECK(record.localPath == job.localPath);
  CHECK(record.remotePath == job.remotePath);
  CHECK(record.localSize == IncomingContents.size());
  CHECK(record.localModifiedAt == std::filesystem::last_write_time(job.localPath));
  REQUIRE(record.temporaryRemotePath);
  CHECK(*record.temporaryRemotePath != job.remotePath);
  {
    std::scoped_lock lock{harness.server->mutex};

    REQUIRE(harness.server->calls.size() == 1U);
    CHECK(harness.server->calls.front().durableDestination);
    CHECK(harness.server->calls.front().durableUploadCheckpoint);
    CHECK(harness.server->files.at(job.remotePath.Bytes()).contents == OriginalContents);
    CHECK(harness.server->files.at(record.temporaryRemotePath->Bytes()).contents ==
          IncomingContents.substr(0, 3));
  }

  // Advance the same worker through another queued operation. A failed
  // storage-limited upload must neither be retried nor published meanwhile.
  auto probe = harness.Job(TransferDirection::Upload, false);
  probe.id = "subsequent-upload";
  probe.localPath = harness.root / "subsequent.txt";
  probe.remotePath = RemotePath{"/remote/subsequent.txt"};

  WriteFile(probe.localPath, "subsequent upload");

  const auto transfer = [&](const std::string_view id)
  {
    const auto items = harness.controller->Transfers();
    const auto found = std::ranges::find_if(items, [id](const auto &entry)
                                            { return entry.job.id == id; });

    REQUIRE(found != items.end());

    return *found;
  };

  REQUIRE(harness.controller->Enqueue(probe));
  REQUIRE(harness.PumpUntil([&]
                            { return transfer(probe.id).state == TransferState::Completed; }));
  CHECK(transfer(job.id).state == TransferState::Failed);
  CHECK(transfer(job.id).attempt == 1U);
  {
    std::scoped_lock lock{harness.server->mutex};

    REQUIRE(harness.server->calls.size() == 2U);
    CHECK(harness.server->calls.back().remote == probe.remotePath);
    CHECK(harness.server->files.at(job.remotePath.Bytes()).contents == OriginalContents);
    CHECK(harness.server->files.at(record.temporaryRemotePath->Bytes()).contents ==
          IncomingContents.substr(0, 3));
  }

  REQUIRE(harness.controller->Retry(job.id));
  REQUIRE(harness.PumpUntil([&]
                            { return transfer(job.id).state == TransferState::Completed; }));
  CHECK(transfer(job.id).attempt == 2U);
  CHECK_FALSE(transfer(job.id).error);
  CHECK(ReadFile(job.localPath) == IncomingContents);
  REQUIRE(harness.resumeOffered.size() == 2U);
  CHECK_FALSE(harness.resumeOffered.front());
  CHECK(harness.resumeOffered.back());
  REQUIRE(harness.saveErrors.empty());

  std::scoped_lock lock{harness.server->mutex};

  REQUIRE(harness.server->calls.size() == 3U);

  const auto &resumed = harness.server->calls.back();

  CHECK(resumed.local == job.localPath);
  CHECK(resumed.remote == job.remotePath);
  CHECK(resumed.resumeOffset == 3U);
  CHECK(resumed.durableUploadCheckpoint);
  CHECK(harness.server->files.at(job.remotePath.Bytes()).contents == IncomingContents);
  CHECK_FALSE(harness.server->files.contains(record.temporaryRemotePath->Bytes()));
}

TEST_CASE("controller discards obsolete checkpoints when a retry chooses Rename",
          "[controller][queue][rename]")
{
  auto direction = TransferDirection::Upload;

  SECTION("upload") {}
  SECTION("download") { direction = TransferDirection::Download; }

  ControllerHarness harness;

  const auto job = harness.Job(direction, false);

  WriteFile(job.localPath, direction == TransferDirection::Upload
                               ? IncomingContents
                               : OriginalContents);

  harness.server->files[job.remotePath.Bytes()] = MakeRemoteFile(
      job.remotePath, std::string{direction == TransferDirection::Upload
                                      ? OriginalContents
                                      : IncomingContents});
  harness.server->outcomes = {RemoteErrorCode::ProtocolError,
                              RemoteErrorCode::Paused, std::nullopt};
  harness.choices = {ConflictPolicy::Overwrite, ConflictPolicy::Rename,
                     ConflictPolicy::Resume};

  REQUIRE(harness.Start());
  REQUIRE(harness.controller->Enqueue(job));
  REQUIRE(harness.WaitFor(TransferState::Failed));
  REQUIRE(harness.Save());
  REQUIRE(harness.controller->PersistentQueueItems().front().resumeRecords.size() == 1);
  REQUIRE(harness.controller->Retry(job.id));
  REQUIRE(harness.WaitFor(TransferState::Paused));
  REQUIRE(harness.Save());
  REQUIRE(harness.saveErrors.empty());

  auto loaded = harness.repository.Load();

  REQUIRE(loaded);
  REQUIRE(loaded->items.size() == 1);
  REQUIRE(loaded->items.front().resumeRecords.size() == 1);

  const auto &resolvedJob = loaded->items.front().transfer.job;
  const auto &record = loaded->items.front().resumeRecords.front();

  CHECK(record.localPath == resolvedJob.localPath);
  CHECK(record.remotePath == resolvedJob.remotePath);

  if (direction == TransferDirection::Upload)
  {
    CHECK(record.remotePath == RemotePath{"/remote/report (2).txt"});
  }
  else
  {
    CHECK(record.localPath == job.localPath.parent_path() / "report (2).txt");
  }

  REQUIRE(harness.Start(std::move(loaded->items)));
  REQUIRE(harness.controller->Retry(job.id));
  REQUIRE(harness.WaitFor(TransferState::Completed));
  REQUIRE(harness.saveErrors.empty());
  REQUIRE(harness.resumeOffered.size() == 3);
  CHECK(harness.resumeOffered[1]);
  CHECK(harness.resumeOffered[2]);

  std::scoped_lock lock{harness.server->mutex};

  REQUIRE(harness.server->calls.size() == 3);
  CHECK(harness.server->calls.back().resumeOffset == 3);
}

TEST_CASE("controller stops permission batches on session failures without a follow-up listing",
          "[controller][permissions]")
{
  for (const auto code : {RemoteErrorCode::AuthenticationFailed,
                          RemoteErrorCode::CredentialUnavailable,
                          RemoteErrorCode::TrustRejected,
                          RemoteErrorCode::HostKeyChanged,
                          RemoteErrorCode::CertificateInvalid,
                          RemoteErrorCode::NameResolutionFailed,
                          RemoteErrorCode::ConnectionFailed,
                          RemoteErrorCode::ConnectionLost,
                          RemoteErrorCode::NotConnected,
                          RemoteErrorCode::TimedOut})
  {
    CAPTURE(code);

    ControllerHarness harness;

    auto error = TestError(code, "Injected session failure");
    error.operationMayHaveSucceeded = code == RemoteErrorCode::TimedOut;

    harness.server->permissionOutcomes = {error, std::nullopt};

    REQUIRE(harness.Start());
    REQUIRE(harness.PumpUntil([&]
                              { return harness.directoryEvents == 1; }));

    harness.controller->SetPermissions(
        {RemotePath{"/remote/first.txt"}, RemotePath{"/remote/second.txt"}}, 0644U);

    REQUIRE(harness.PumpUntil([&]
                              { return !harness.connected; }));
    REQUIRE(harness.operationErrors.size() == 1);
    CHECK(harness.operationErrors.front().operation == "Change permissions");
    CHECK(harness.operationErrors.front().error.code == code);
    CHECK(harness.operationErrors.front().error.message == "Injected session failure");
    CHECK(harness.operationErrors.front().error.operationMayHaveSucceeded ==
          error.operationMayHaveSucceeded);

    std::scoped_lock lock{harness.server->mutex};

    REQUIRE(harness.server->permissionCalls.size() == 1);
    CHECK(harness.server->permissionCalls.front() == RemotePath{"/remote/first.txt"});
    CHECK(harness.server->permissionOutcomes.size() == 1);
    CHECK(harness.server->listCalls.size() == 1);
  }
}

TEST_CASE("controller aggregates definite per-file permission failures without refreshing",
          "[controller][permissions]")
{
  ControllerHarness harness;
  harness.server->permissionOutcomes = {
      TestError(RemoteErrorCode::PermissionDenied, "First file denied"),
      TestError(RemoteErrorCode::PermissionDenied, "Second file denied")};

  REQUIRE(harness.Start());
  REQUIRE(harness.PumpUntil([&]
                            { return harness.directoryEvents == 1; }));

  harness.controller->SetPermissions(
      {RemotePath{"/remote/first.txt"}, RemotePath{"/remote/second.txt"}}, 0644U);

  // This ordered read-only browser command proves the batch (and any
  // follow-up refresh) has finished, without relying on a timing delay.
  harness.controller->Inspect(RemotePath{"/remote"}, "permissions-finished");

  REQUIRE(harness.PumpUntil([&]
                            { return !harness.inspectionIds.empty(); }));
  CHECK(harness.connected);
  REQUIRE(harness.operationErrors.size() == 1);
  CHECK(harness.operationErrors.front().operation == "Change permissions");
  CHECK(harness.operationErrors.front().error.code == RemoteErrorCode::PermissionDenied);
  CHECK(harness.operationErrors.front().error.message ==
        "Changing permissions failed for 2 remote items. First error: First file denied");
  CHECK_FALSE(harness.operationErrors.front().error.operationMayHaveSucceeded);

  std::scoped_lock lock{harness.server->mutex};

  REQUIRE(harness.server->permissionCalls.size() == 2);
  CHECK(harness.server->permissionCalls[0] == RemotePath{"/remote/first.txt"});
  CHECK(harness.server->permissionCalls[1] == RemotePath{"/remote/second.txt"});
  CHECK(harness.server->listCalls.size() == 1);
}

TEST_CASE("controller refreshes permissions after successful or uncertain changes on usable sessions",
          "[controller][permissions]")
{
  std::deque<std::optional<RemoteError>> outcomes;

  std::size_t expectedErrors{};

  bool uncertain{};

  SECTION("all selected items succeeded") { outcomes = {std::nullopt, std::nullopt}; }
  SECTION("one item succeeded and another was denied")
  {
    outcomes = {std::nullopt, TestError(RemoteErrorCode::PermissionDenied, "File denied")};

    expectedErrors = 1;
  }
  SECTION("no confirmed success but a permission change may have reached the server")
  {
    auto error = TestError(RemoteErrorCode::RemoteIo, "Uncertain permission change");
    error.operationMayHaveSucceeded = true;

    outcomes = {error, TestError(RemoteErrorCode::PermissionDenied, "File denied")};

    expectedErrors = 1;

    uncertain = true;
  }

  ControllerHarness harness;
  harness.server->permissionOutcomes = std::move(outcomes);

  REQUIRE(harness.Start());
  REQUIRE(harness.PumpUntil([&]
                            { return harness.directoryEvents == 1; }));

  harness.controller->SetPermissions(
      {RemotePath{"/remote/first.txt"}, RemotePath{"/remote/second.txt"}}, 0644U);
  harness.controller->Inspect(RemotePath{"/remote"}, "permissions-finished");

  REQUIRE(harness.PumpUntil([&]
                            { return !harness.inspectionIds.empty(); }));
  CHECK(harness.connected);
  CHECK(harness.directoryEvents == 2);
  REQUIRE(harness.operationErrors.size() == expectedErrors);

  if (expectedErrors != 0)
  {
    CHECK(harness.operationErrors.front().operation == "Change permissions");
    CHECK(harness.operationErrors.front().error.operationMayHaveSucceeded == uncertain);
  }

  std::scoped_lock lock{harness.server->mutex};

  CHECK(harness.server->permissionCalls.size() == 2);
  CHECK(harness.server->listCalls.size() == 2);
}

TEST_CASE("controller preserves independent refresh failures after a partly successful permission batch",
          "[controller][permissions]")
{
  ControllerHarness harness;
  harness.server->permissionOutcomes = {
      std::nullopt, TestError(RemoteErrorCode::PermissionDenied, "File denied")};
  harness.server->listOutcomes = {
      std::nullopt, TestError(RemoteErrorCode::PermissionDenied, "Directory listing denied")};

  REQUIRE(harness.Start());
  REQUIRE(harness.PumpUntil([&]
                            { return harness.directoryEvents == 1; }));

  harness.controller->SetPermissions(
      {RemotePath{"/remote/first.txt"}, RemotePath{"/remote/second.txt"}}, 0644U);
  harness.controller->Inspect(RemotePath{"/remote"}, "permissions-finished");

  REQUIRE(harness.PumpUntil([&]
                            { return !harness.inspectionIds.empty(); }));
  REQUIRE(harness.operationErrors.size() == 2);
  CHECK(harness.operationErrors[0].operation == "Change permissions");
  CHECK(harness.operationErrors[0].error.message == "File denied");
  CHECK(harness.operationErrors[1].operation == "List directory");
  CHECK(harness.operationErrors[1].error.message == "Directory listing denied");
  CHECK(harness.connected);

  std::scoped_lock lock{harness.server->mutex};

  CHECK(harness.server->permissionCalls.size() == 2);
  CHECK(harness.server->listCalls.size() == 2);
}
