// SPDX-License-Identifier: MIT

#include "ui/remoteController.hpp"

#include "core/logSanitizer.hpp"
#include "platform/pathSafety.hpp"
#include "protocol/ftpSession.hpp"
#include "protocol/sftpSession.hpp"

#include <wx/app.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace havremote::ui
{
  wxDEFINE_EVENT(EVT_HAVREMOTE_CONTROLLER, wxThreadEvent);

  namespace
  {
    using namespace std::chrono_literals;

    RemoteError ControllerError(const RemoteErrorCode code,
                                std::string message,
                                const bool retryable = false)
    {
      return RemoteError{.code = code, .message = std::move(message), .retryable = retryable};
    }

    RemoteSessionPtr MakeSession(const ProtocolKind protocol)
    {
      return protocol == ProtocolKind::Sftp ? MakeSftpSession() : MakeFtpSession();
    }

    std::filesystem::path PartialPath(const std::filesystem::path &finalPath)
    {
      auto value = finalPath;
      value += ".havremote.part";
      return value;
    }

    std::string GenericUtf8(const std::filesystem::path &path)
    {
      const auto value = path.generic_u8string();
      return {reinterpret_cast<const char *>(value.data()), value.size()};
    }

    std::string RelativeRemoteText(const std::string_view root,
                                   const std::string_view child)
    {
      auto rootBytes = std::string{root};

      while (rootBytes.size() > 1 && rootBytes.back() == '/')
      {
        rootBytes.pop_back();
      }

      if (rootBytes == "/")
      {
        return child.starts_with('/') ? std::string{child.substr(1)} : std::string{child};
      }

      if (child == rootBytes)
      {
        return {};
      }

      if (child.size() > rootBytes.size() && child.starts_with(rootBytes) &&
          child[rootBytes.size()] == '/')
      {
        return std::string{child.substr(rootBytes.size() + 1)};
      }

      return {};
    }

    std::string AddConflictSuffix(const std::string_view path,
                                  const unsigned int suffix)
    {
      const auto slash = path.find_last_of('/');
      const auto nameOffset = slash == std::string_view::npos ? 0U : slash + 1U;
      const auto name = path.substr(nameOffset);
      const auto dot = name.find_last_of('.');
      const auto extensionOffset = dot == std::string_view::npos || dot == 0U
                                       ? name.size()
                                       : dot;

      std::string candidate{path.substr(0U, nameOffset)};
      candidate.append(name.substr(0U, extensionOffset));
      candidate.append(" (");
      candidate.append(std::to_string(suffix));
      candidate.push_back(')');
      candidate.append(name.substr(extensionOffset));

      return candidate;
    }

    RemotePath RemoteConflictCandidate(const RemotePath &path,
                                       const unsigned int suffix)
    {
      return RemotePath{AddConflictSuffix(path.Bytes(), suffix),
                        AddConflictSuffix(path.DisplayUtf8(), suffix)};
    }
  } // namespace

  struct RemoteController::Impl final
  {
    struct UiDispatchState final
    {
      std::atomic_bool enabled{true};
      std::atomic<std::uint64_t> generation{};
      std::mutex promptMutex;
      std::condition_variable_any promptCv;
      bool promptActive{};
    };

    struct Browse final
    {
      RemotePath path;
    };

    struct Refresh final
    {
    };

    struct Inspect final
    {
      RemotePath path;
      std::string requestId;
    };

    struct Mkdir final
    {
      RemotePath path;
      bool recursive{};
    };

    struct CreateFile final
    {
      RemotePath path;
    };

    struct Rename final
    {
      RemotePath source;
      RemotePath destination;
      bool overwrite{};
    };

    struct Remove final
    {
      RemotePath path;
      bool recursive{};
    };

    struct SetPermissions final
    {
      std::vector<RemotePath> paths;
      std::uint32_t permissions{};
    };

    using BrowserCommand = std::variant<Browse,
                                        Refresh,
                                        Inspect,
                                        Mkdir,
                                        CreateFile,
                                        Rename,
                                        Remove,
                                        SetPermissions>;

    struct PlannedFile final
    {
      std::filesystem::path local;
      RemotePath remote;
      std::uint64_t size{};
      std::optional<std::chrono::system_clock::time_point> modifiedAt;
    };

    struct TransferPlan final
    {
      std::vector<std::pair<std::filesystem::path, RemotePath>> directories;
      std::vector<PlannedFile> files;
      std::uint64_t totalBytes{};
      std::filesystem::path localDestinationRoot;
    };

    struct DownloadResume final
    {
      std::string jobId;
      std::string siteId;
      SiteEndpointIdentity siteEndpoint;
      std::filesystem::path localPath;
      RemotePath remotePath;
      std::uint64_t remoteSize{};
      std::optional<std::chrono::system_clock::time_point> remoteModified;
      std::uint64_t partSize{};
      std::filesystem::file_time_type partModified;
    };

    struct UploadResume final
    {
      std::string jobId;
      std::string siteId;
      SiteEndpointIdentity siteEndpoint;
      std::filesystem::path localPath;
      std::uint64_t localSize{};
      std::filesystem::file_time_type localModified;
      RemotePath remotePath;
      RemotePath temporaryPath;
    };

    struct RemainingConflictChoice final
    {
      ConflictPolicy policy{ConflictPolicy::Skip};
      std::unordered_set<std::string> jobIds;
    };

    struct CommittedUploadRevision final
    {
      std::uint64_t generation{};
      RemoteFileRevision revision;
    };

    Impl(wxEvtHandler &target,
         std::string runtimeConnectionId,
         std::shared_ptr<TransferRuntime> sharedTransferRuntime,
         std::filesystem::path knownHosts,
         UiInteractions callbacks,
         const std::chrono::seconds connectTimeout,
         const std::chrono::seconds idleTimeout,
         std::vector<config::PersistentQueueItem> restoredItems)
        : eventTarget(target),
          connectionId(std::move(runtimeConnectionId)),
          transferRuntime(std::move(sharedTransferRuntime)),
          knownHostsFile(std::move(knownHosts)),
          interactions(std::move(callbacks)),
          workerCount(transferRuntime ? transferRuntime->ConcurrencyLimit() : 0U),
          connectionTimeout(std::max(connectTimeout, 1s)),
          commandIdleTimeout(std::max(idleTimeout, 1s))
    {
      if (connectionId.empty())
      {
        throw std::invalid_argument{"RemoteController requires a connection id"};
      }

      if (!transferRuntime)
      {
        throw std::invalid_argument{"RemoteController requires a shared transfer runtime"};
      }

      std::vector<QueuedTransfer> restoredTransfers;
      restoredTransfers.reserve(restoredItems.size());

      for (auto &item : restoredItems)
      {
        if (item.connectionId != connectionId)
        {
          throw std::invalid_argument{
              "A restored transfer belongs to a different connection tab"};
        }

        restoredTransfers.push_back(item.transfer);
      }

      if (auto restored = queue.Restore(std::move(restoredTransfers)); !restored)
      {
        throw std::invalid_argument{restored.error().message};
      }

      for (const auto &item : restoredItems)
      {
        const auto &job = item.transfer.job;
        controls[job.id] = TransferControl::Continue;

        for (const auto &record : item.resumeRecords)
        {
          if (record.kind == config::QueueResumeKind::Download)
          {
            if (!record.partModifiedAt)
            {
              throw std::invalid_argument{
                  "A restored download checkpoint has no partial-file timestamp"};
            }

            const auto key = job.siteId + "\n" + job.id + "\n" +
                             record.remotePath.Bytes() + "\n" +
                             GenericUtf8(record.localPath);

            downloadResumes.emplace(
                key,
                DownloadResume{job.id,
                               job.siteId,
                               *job.siteEndpoint,
                               record.localPath,
                               record.remotePath,
                               record.remoteSize,
                               record.remoteModifiedAt,
                               record.partSize,
                               *record.partModifiedAt});
          }
          else
          {
            if (!record.localModifiedAt || !record.temporaryRemotePath)
            {
              throw std::invalid_argument{
                  "A restored upload checkpoint is incomplete"};
            }

            const auto key = job.siteId + "\n" + job.id + "\n" +
                             record.remotePath.Bytes();

            uploadResumes.emplace(
                key,
                UploadResume{job.id,
                             job.siteId,
                             *job.siteEndpoint,
                             record.localPath,
                             record.localSize,
                             *record.localModifiedAt,
                             record.remotePath,
                             *record.temporaryRemotePath});
          }
        }
      }
    }

    ~Impl() { StopWorkers(); }

    void Post(ControllerEvent event)
    {
      if (shuttingDown.load(std::memory_order_acquire))
      {
        return;
      }

      auto *queued = new wxThreadEvent(EVT_HAVREMOTE_CONTROLLER);

      queued->SetPayload(ControllerEventEnvelopePtr{
          std::make_shared<const ControllerEventEnvelope>(
              ControllerEventEnvelope{connectionId, std::move(event)})});

      wxQueueEvent(&eventTarget, queued);
    }

    void PostQueue(const bool durableChange = false)
    {
      // Snapshot and enqueue as one ordered operation. Transfer workers and
      // UI controls can otherwise post an older snapshot after a newer one.
      std::scoped_lock lock{queuePostMutex};

      Post(QueueEvent{queue.Snapshot(), durableChange});
    }

    void PostError(std::string operation, RemoteError error)
    {
      std::string siteId;
      {
        std::scoped_lock lock{siteMutex};

        if (site)
        {
          siteId = site->id;
        }
      }

      Post(OperationErrorEvent{std::move(siteId),
                               uiDispatch->generation.load(std::memory_order_acquire),
                               std::move(operation),
                               std::move(error)});
    }

    void Diagnostic(const DiagnosticLevel level, std::string_view message)
    {
      Post(DiagnosticEvent{level, SanitizeDiagnosticText(message)});
    }

    template <typename T, typename Argument, typename Callback>
    Result<T> InvokeUi(const Callback &callback,
                       Argument argument,
                       const std::stop_token stopToken,
                       const std::uint64_t expectedGeneration,
                       std::string missingMessage)
    {
      if (!callback)
      {
        return std::unexpected(
            ControllerError(RemoteErrorCode::Unsupported, std::move(missingMessage)));
      }

      const auto dispatch = uiDispatch;
      {
        std::unique_lock promptLock{dispatch->promptMutex};

        const auto ready = dispatch->promptCv.wait(
            promptLock,
            stopToken,
            [&]
            {
              return !dispatch->promptActive ||
                     !dispatch->enabled.load(std::memory_order_acquire) ||
                     dispatch->generation.load(std::memory_order_acquire) !=
                         expectedGeneration;
            });

        if (!ready || stopToken.stop_requested() ||
            !dispatch->enabled.load(std::memory_order_acquire) ||
            dispatch->generation.load(std::memory_order_acquire) != expectedGeneration)
        {
          return std::unexpected(
              ControllerError(RemoteErrorCode::Cancelled, "UI request canceled"));
        }

        dispatch->promptActive = true;
      }

      struct Invocation final
      {
        std::promise<Result<T>> promise;
        std::atomic_bool delivered{};
        std::atomic_bool abandoned{};
      };

      auto invocation = std::make_shared<Invocation>();

      auto future = invocation->promise.get_future();

      eventTarget.CallAfter([dispatch,
                             invocation,
                             callback,
                             argument = std::move(argument),
                             expectedGeneration]() mutable
                            {
            const auto finish = [&invocation](Result<T> value) {
                bool expected = false;
                if (invocation->delivered.compare_exchange_strong(expected, true)) {
                    invocation->promise.set_value(std::move(value));
                }
            };

            const auto releasePrompt = [&dispatch] {
                {
                    std::scoped_lock lock{dispatch->promptMutex};
                    dispatch->promptActive = false;
                }
                dispatch->promptCv.notify_all();
            };

            if (invocation->abandoned.load(std::memory_order_acquire) ||
                !dispatch->enabled.load(std::memory_order_acquire) ||
                dispatch->generation.load(std::memory_order_acquire) != expectedGeneration) {
                releasePrompt();
                finish(std::unexpected(
                    ControllerError(RemoteErrorCode::Cancelled, "Obsolete UI request canceled")));

                return;
            }

            try {
                auto value = callback(argument);

                releasePrompt();

                if (invocation->abandoned.load(std::memory_order_acquire) ||
                    dispatch->generation.load(std::memory_order_acquire) != expectedGeneration) {
                    finish(std::unexpected(ControllerError(
                        RemoteErrorCode::Cancelled, "Obsolete UI request canceled")));
                } else {
                    finish(std::move(value));
                }
            } catch (const std::exception& exception) {
                releasePrompt();

                finish(std::unexpected(ControllerError(
                    RemoteErrorCode::Unknown,
                    std::string{"UI interaction failed: "} + exception.what())));
            } catch (...) {
                releasePrompt();

                finish(std::unexpected(
                    ControllerError(RemoteErrorCode::Unknown, "UI interaction failed")));
            } });

      while (future.wait_for(25ms) != std::future_status::ready)
      {
        if (stopToken.stop_requested())
        {
          invocation->abandoned.store(true, std::memory_order_release);

          bool expected = false;

          if (invocation->delivered.compare_exchange_strong(expected, true))
          {
            invocation->promise.set_value(std::unexpected(
                ControllerError(RemoteErrorCode::Cancelled, "Operation canceled")));
          }
        }
      }

      return future.get();
    }

    Result<void> PersistQueueDirect(const std::string &reason)
    {
      if (!interactions.persistQueue)
      {
        return {};
      }

      try
      {
        return interactions.persistQueue(reason);
      }
      catch (const std::exception &exception)
      {
        return std::unexpected(ControllerError(
            RemoteErrorCode::LocalIo,
            std::string{"Queue persistence failed: "} + exception.what()));
      }
      catch (...)
      {
        return std::unexpected(ControllerError(
            RemoteErrorCode::LocalIo,
            "Queue persistence failed"));
      }
    }

    Result<void> PersistQueueOnUi(const std::string_view reason,
                                  const std::stop_token stopToken,
                                  const std::uint64_t expectedGeneration)
    {
      if (!interactions.persistQueue)
      {
        return {};
      }

      return InvokeUi<void>(interactions.persistQueue,
                            std::string{reason},
                            stopToken,
                            expectedGeneration,
                            "No queue persistence handler is available");
    }

    SessionCallbacks BuildSessionCallbacks(const std::string &tlsPinnedPublicKey,
                                           const std::uint64_t expectedGeneration)
    {
      SessionCallbacks callbacks;
      callbacks.knownHostsFile = knownHostsFile;
      {
        std::scoped_lock lock{timeoutMutex};
        callbacks.connectionTimeout = connectionTimeout;
        callbacks.commandIdleTimeout = commandIdleTimeout;
      }
      callbacks.tlsPinnedPublicKey = tlsPinnedPublicKey;
      callbacks.diagnostic = [this](const DiagnosticLevel level, const std::string_view message)
      {
        Diagnostic(level, message);
      };
      callbacks.requestCredential = [this, expectedGeneration](const CredentialRequest &request,
                                                               const std::stop_token stopToken)
      {
        return InvokeUi<std::string>(interactions.requestCredential,
                                     request,
                                     stopToken,
                                     expectedGeneration,
                                     "No credential handler is available");
      };
      callbacks.verifyTrust = [this, expectedGeneration](const TrustChallenge &challenge,
                                                         const std::stop_token stopToken)
      {
        return InvokeUi<TrustDecision>(interactions.verifyTrust,
                                       challenge,
                                       stopToken,
                                       expectedGeneration,
                                       "No trust handler is available");
      };
      return callbacks;
    }

    std::uint64_t Start(SiteProfile selectedSite)
    {
      StopWorkers();

      shuttingDown.store(false, std::memory_order_release);

      uiDispatch->enabled.store(true, std::memory_order_release);

      const auto currentGeneration =
          uiDispatch->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
      {
        std::scoped_lock lock{conflictMutex};

        remainingConflictChoice.reset();
      }

      std::string tlsPinnedPublicKey;

      if ((selectedSite.protocol == ProtocolKind::FtpsExplicit ||
           selectedSite.protocol == ProtocolKind::FtpsImplicit) &&
          interactions.tlsPinnedPublicKey)
      {
        if (auto pin = interactions.tlsPinnedPublicKey(selectedSite.host, selectedSite.port))
        {
          tlsPinnedPublicKey = std::move(*pin);
        }
      }

      {
        std::scoped_lock lock{siteMutex};
        site = selectedSite;
      }

      if (selectedSite.protocol == ProtocolKind::Ftp)
      {
        Diagnostic(DiagnosticLevel::Warning,
                   "Plain FTP sends credentials and file contents without encryption.");
      }

      browser = std::jthread(
          [this, selectedSite, currentGeneration, tlsPinnedPublicKey](std::stop_token stop)
          {
            BrowserLoop(stop, selectedSite, currentGeneration, tlsPinnedPublicKey);
          });

      transferWorkers.reserve(workerCount);

      for (std::size_t index = 0; index < workerCount; ++index)
      {
        transferWorkers.emplace_back(
            [this, selectedSite, currentGeneration, index,
             tlsPinnedPublicKey](std::stop_token stop)
            {
              TransferLoop(stop,
                           selectedSite,
                           currentGeneration,
                           index,
                           tlsPinnedPublicKey);
            });
      }

      return currentGeneration;
    }

    void StopWorkers()
    {
      // Invalidate queued events and UI callbacks before asking the workers
      // to stop. The following connect receives its own distinct token.
      uiDispatch->generation.fetch_add(1, std::memory_order_acq_rel);
      uiDispatch->promptCv.notify_all();

      if (browser.joinable())
      {
        browser.request_stop();
      }

      for (auto &worker : transferWorkers)
      {
        worker.request_stop();
      }

      browserCv.notify_all();
      transferCv.notify_all();

      if (browser.joinable())
      {
        browser.join();
      }

      for (auto &worker : transferWorkers)
      {
        if (worker.joinable())
        {
          worker.join();
        }
      }

      transferWorkers.clear();
      {
        std::scoped_lock lock{browserMutex};

        browserCommands.clear();
      }

      {
        std::scoped_lock lock{siteMutex};

        site.reset();

        connectedGeneration.reset();
      }

      {
        std::scoped_lock lock{committedUploadMutex};

        committedUploadRevisions.clear();
      }
    }

    void Shutdown()
    {
      if (shuttingDown.exchange(true, std::memory_order_acq_rel))
      {
        return;
      }

      uiDispatch->enabled.store(false, std::memory_order_release);
      {
        std::scoped_lock actionLock{transferActionMutex};

        for (const auto &item : queue.Snapshot())
        {
          if (item.state == TransferState::Queued ||
              item.state == TransferState::Enumerating ||
              item.state == TransferState::Running)
          {
            {
              std::scoped_lock lock{controlMutex};
              controls[item.job.id] = TransferControl::Pause;
            }

            // Queued work can be made inert immediately. Active work reaches
            // its finalizer after StopWorkers() requests cancellation and is
            // mapped to Pause by CurrentControl().
            (void)queue.PauseIfQueued(item.job.id);
          }
        }
      }

      StopWorkers();
    }

    void PushBrowser(BrowserCommand command)
    {
      {
        std::scoped_lock lock{browserMutex};

        if (std::holds_alternative<Refresh>(command) &&
            std::ranges::any_of(browserCommands, [](const auto &queued)
                                { return std::holds_alternative<Refresh>(queued); }))
        {
          return;
        }

        browserCommands.push_back(std::move(command));
      }

      browserCv.notify_one();
    }

    void BrowserLoop(const std::stop_token stopToken,
                     const SiteProfile &selectedSite,
                     const std::uint64_t currentGeneration,
                     const std::string &tlsPinnedPublicKey)
    {
      auto session = MakeSession(selectedSite.protocol);

      Post(ConnectionAttemptEvent{
          .siteId = selectedSite.id,
          .generation = currentGeneration,
          .endpoint = EndpointIdentity(selectedSite),
          .kind = ConnectionAttemptKind::Browser});

      auto connected =
          session->Connect(selectedSite,
                           BuildSessionCallbacks(tlsPinnedPublicKey, currentGeneration),
                           stopToken);

      if (!connected)
      {
        if (!stopToken.stop_requested())
        {
          Post(ConnectionEvent{false,
                               selectedSite.id,
                               currentGeneration,
                               connected.error().message});

          PostError("Connect", connected.error());
        }

        return;
      }

      {
        std::scoped_lock lock{siteMutex};

        if (site && site->id == selectedSite.id &&
            uiDispatch->generation.load(std::memory_order_acquire) ==
                currentGeneration)
        {
          connectedGeneration = currentGeneration;
        }
      }

      Post(ConnectionEvent{true, selectedSite.id, currentGeneration, "Connected"});

      RemotePath currentDirectory = selectedSite.initialRemoteDirectory;

      if (!ListAndPost(*session,
                       selectedSite.id,
                       selectedSite.initialRemoteDirectory,
                       stopToken,
                       currentGeneration,
                       &currentDirectory))
      {
        session->Disconnect();
        {
          std::scoped_lock lock{siteMutex};

          if (connectedGeneration == currentGeneration)
          {
            connectedGeneration.reset();
          }
        }

        if (!stopToken.stop_requested())
        {
          Post(ConnectionEvent{false,
                               selectedSite.id,
                               currentGeneration,
                               "Connection lost"});
        }

        return;
      }

      while (!stopToken.stop_requested())
      {
        BrowserCommand command;
        {
          std::unique_lock lock{browserMutex};

          if (!browserCv.wait(lock, stopToken, [this]
                              { return !browserCommands.empty(); }))
          {
            break;
          }

          command = std::move(browserCommands.front());

          browserCommands.pop_front();
        }

        const bool keepSession = std::visit(
            [this,
             &session,
             &selectedSite,
             &stopToken,
             &currentDirectory,
             currentGeneration](auto &&operation)
            {
              using Operation = std::decay_t<decltype(operation)>;

              if constexpr (std::is_same_v<Operation, Browse>)
              {
                return ListAndPost(*session,
                                   selectedSite.id,
                                   operation.path,
                                   stopToken,
                                   currentGeneration,
                                   &currentDirectory);
              }
              else if constexpr (std::is_same_v<Operation, Refresh>)
              {
                return ListAndPost(*session,
                                   selectedSite.id,
                                   currentDirectory,
                                   stopToken,
                                   currentGeneration,
                                   &currentDirectory);
              }
              else if constexpr (std::is_same_v<Operation, Inspect>)
              {
                auto result = session->Stat(operation.path, stopToken);

                const bool usable = result || SessionUsableAfter(result.error());
                {
                  std::scoped_lock lock{committedUploadMutex};

                  const auto committed = committedUploadRevisions.find(
                      operation.path.Bytes());

                  if (committed != committedUploadRevisions.end() &&
                      committed->second.generation == currentGeneration)
                  {
                    if (result &&
                        MakeRemoteFileRevision(*result) !=
                            committed->second.revision)
                    {
                      result = std::unexpected(ControllerError(
                          RemoteErrorCode::Conflict,
                          "The remote file changed after the edited upload was committed"));
                    }
                    else if (!result &&
                             result.error().code ==
                                 RemoteErrorCode::NotFound)
                    {
                      result = std::unexpected(ControllerError(
                          RemoteErrorCode::Conflict,
                          "The remote file was deleted after the edited upload was committed"));
                    }

                    committedUploadRevisions.erase(committed);
                  }
                }

                if (uiDispatch->generation.load(std::memory_order_acquire) ==
                    currentGeneration)
                {
                  Post(RemoteInspectionEvent{
                      .requestId = std::move(operation.requestId),
                      .siteId = selectedSite.id,
                      .generation = currentGeneration,
                      .path = std::move(operation.path),
                      .result = std::move(result),
                  });
                }

                return usable;
              }
              else if constexpr (std::is_same_v<Operation, Mkdir>)
              {
                auto result = session->Mkdir(operation.path, operation.recursive, stopToken);
                if (!result)
                {
                  PostError("Create directory", result.error());

                  return SessionUsableAfter(result.error());
                }
                else
                {
                  return ListAndPost(*session,
                                     selectedSite.id,
                                     operation.path.Parent(),
                                     stopToken,
                                     currentGeneration,
                                     &currentDirectory);
                }
              }
              else if constexpr (std::is_same_v<Operation, CreateFile>)
              {
                RemotePath destination = operation.path;

                bool overwrite = false;
                bool findingRenamedDestination = false;

                unsigned int renameSuffix = 2U;

                while (!stopToken.stop_requested())
                {
                  auto result = session->CreateRemoteFile(
                      destination, overwrite, stopToken);

                  if (result)
                  {
                    return ListAndPost(*session,
                                       selectedSite.id,
                                       destination.Parent(),
                                       stopToken,
                                       currentGeneration,
                                       &currentDirectory,
                                       destination.Bytes());
                  }

                  if (result.error().code !=
                          RemoteErrorCode::AlreadyExists ||
                      overwrite)
                  {
                    PostError("Create file", result.error());

                    return SessionUsableAfter(result.error());
                  }

                  if (findingRenamedDestination)
                  {
                    if (renameSuffix >= 10'000U)
                    {
                      auto error = ControllerError(
                          RemoteErrorCode::Conflict,
                          "Could not find an unused remote filename");

                      PostError("Create file", error);

                      return true;
                    }

                    destination = RemoteConflictCandidate(
                        operation.path, renameSuffix++);

                    continue;
                  }

                  auto existing = session->Stat(destination, stopToken);
                  if (!existing)
                  {
                    PostError("Create file", existing.error());

                    return SessionUsableAfter(existing.error());
                  }

                  if (existing->kind != RemoteEntryKind::File)
                  {
                    auto error = ControllerError(
                        existing->kind == RemoteEntryKind::Directory
                            ? RemoteErrorCode::IsDirectory
                            : RemoteErrorCode::Unsupported,
                        existing->kind == RemoteEntryKind::Directory
                            ? "A remote directory already uses that name"
                            : "A non-file remote item already uses that name");

                    PostError("Create file", error);

                    return true;
                  }

                  auto resolution = CreateFileConflictResolution(
                      selectedSite,
                      destination,
                      existing->size,
                      stopToken,
                      currentGeneration);
                  if (!resolution)
                  {
                    if (resolution.error().code ==
                        RemoteErrorCode::Cancelled)
                    {
                      return true;
                    }

                    PostError("Create file", resolution.error());

                    return SessionUsableAfter(resolution.error());
                  }

                  switch (resolution->policy)
                  {
                  case ConflictPolicy::Overwrite:
                    overwrite = true;
                    break;

                  case ConflictPolicy::Skip:
                    return ListAndPost(*session,
                                       selectedSite.id,
                                       destination.Parent(),
                                       stopToken,
                                       currentGeneration,
                                       &currentDirectory,
                                       destination.Bytes());

                  case ConflictPolicy::Rename:
                    findingRenamedDestination = true;
                    destination = RemoteConflictCandidate(
                        operation.path, renameSuffix++);
                    break;

                  case ConflictPolicy::Resume:
                  case ConflictPolicy::Ask:
                  {
                    auto error = ControllerError(
                        RemoteErrorCode::Conflict,
                        "The selected conflict action cannot create an empty file");

                    PostError("Create file", error);

                    return true;
                  }
                  }
                }

                return true;
              }
              else if constexpr (std::is_same_v<Operation, Rename>)
              {
                auto result = session->Rename(operation.source,
                                              operation.destination,
                                              operation.overwrite,
                                              stopToken);

                if (!result)
                {
                  PostError("Rename", result.error());

                  return SessionUsableAfter(result.error());
                }
                else
                {
                  return ListAndPost(*session,
                                     selectedSite.id,
                                     operation.destination.Parent(),
                                     stopToken,
                                     currentGeneration,
                                     &currentDirectory);
                }
              }
              else if constexpr (std::is_same_v<Operation, Remove>)
              {
                auto result = session->Remove(operation.path, operation.recursive, stopToken);
                if (!result)
                {
                  PostError("Delete", result.error());

                  return SessionUsableAfter(result.error());
                }
                else
                {
                  return ListAndPost(*session,
                                     selectedSite.id,
                                     operation.path.Parent(),
                                     stopToken,
                                     currentGeneration,
                                     &currentDirectory);
                }
              }
              else if constexpr (std::is_same_v<Operation, SetPermissions>)
              {
                if (operation.paths.empty())
                {
                  return true;
                }

                std::optional<RemoteError> aggregateError;

                std::size_t failureCount{};

                bool refreshNeeded{};

                bool usable = true;

                for (const auto &path : operation.paths)
                {
                  if (stopToken.stop_requested())
                  {
                    break;
                  }

                  auto result = session->SetPermissions(
                      path, operation.permissions, stopToken);

                  if (result)
                  {
                    refreshNeeded = true;

                    continue;
                  }

                  refreshNeeded = refreshNeeded || result.error().operationMayHaveSucceeded;

                  ++failureCount;

                  if (!aggregateError)
                  {
                    aggregateError = result.error();
                  }
                  else
                  {
                    aggregateError->retryable =
                        aggregateError->retryable || result.error().retryable;
                    aggregateError->operationMayHaveSucceeded =
                        aggregateError->operationMayHaveSucceeded ||
                        result.error().operationMayHaveSucceeded;
                  }

                  usable = SessionUsableAfter(result.error());

                  if (!usable)
                  {
                    break;
                  }
                }

                if (aggregateError)
                {
                  if (failureCount > 1U)
                  {
                    aggregateError->message =
                        "Changing permissions failed for " +
                        std::to_string(failureCount) +
                        " remote items. First error: " +
                        aggregateError->message;
                  }

                  PostError("Change permissions", std::move(*aggregateError));
                }

                if (!usable)
                {
                  return false;
                }

                // A wholly rejected batch changed nothing. Refresh only when
                // permissions changed or an uncertain result needs observing.
                if (stopToken.stop_requested() || !refreshNeeded)
                {
                  return true;
                }

                return ListAndPost(*session,
                                   selectedSite.id,
                                   currentDirectory,
                                   stopToken,
                                   currentGeneration,
                                   &currentDirectory);
              }
            },
            std::move(command));

        if (!keepSession)
        {
          session->Disconnect();
          {
            std::scoped_lock lock{siteMutex};

            if (connectedGeneration == currentGeneration)
            {
              connectedGeneration.reset();
            }
          }

          if (!stopToken.stop_requested())
          {
            Post(ConnectionEvent{false,
                                 selectedSite.id,
                                 currentGeneration,
                                 "Connection lost"});
          }

          return;
        }
      }

      session->Disconnect();
      {
        std::scoped_lock lock{siteMutex};

        if (connectedGeneration == currentGeneration)
        {
          connectedGeneration.reset();
        }
      }

      if (uiDispatch->generation.load(std::memory_order_acquire) == currentGeneration &&
          !shuttingDown.load(std::memory_order_acquire))
      {
        Post(ConnectionEvent{false, selectedSite.id, currentGeneration, "Disconnected"});
      }
    }

    static bool SessionUsableAfter(const RemoteError &error) noexcept
    {
      switch (error.code)
      {
      case RemoteErrorCode::ConnectionLost:
      case RemoteErrorCode::NotConnected:
      case RemoteErrorCode::AuthenticationFailed:
      case RemoteErrorCode::CredentialUnavailable:
      case RemoteErrorCode::TrustRejected:
      case RemoteErrorCode::HostKeyChanged:
      case RemoteErrorCode::CertificateInvalid:
      case RemoteErrorCode::NameResolutionFailed:
      case RemoteErrorCode::ConnectionFailed:
        return false;

      case RemoteErrorCode::TimedOut:
        return !error.operationMayHaveSucceeded;

      default:
        return true;
      }
    }

    static Result<std::optional<RemoteFileRevision>> ObserveRemoteRevision(
        IRemoteSession &session,
        const RemotePath &path,
        const std::stop_token stopToken)
    {
      auto current = session.Stat(path, stopToken);

      if (current)
      {
        return std::optional{MakeRemoteFileRevision(*current)};
      }

      if (current.error().code == RemoteErrorCode::NotFound)
      {
        return std::optional<RemoteFileRevision>{};
      }

      return std::unexpected(current.error());
    }

    bool ListAndPost(IRemoteSession &session,
                     const std::string_view siteId,
                     const RemotePath &directory,
                     const std::stop_token stopToken,
                     const std::uint64_t currentGeneration,
                     RemotePath *const currentDirectory = nullptr,
                     std::optional<std::string> preferredSelectionIdentity =
                         std::nullopt)
    {
      auto result = session.List(directory, stopToken);

      if (!result)
      {
        if (!stopToken.stop_requested())
        {
          PostError("List directory", result.error());
        }

        return SessionUsableAfter(result.error());
      }

      if (currentDirectory != nullptr)
      {
        *currentDirectory = directory;
      }

      if (uiDispatch->generation.load(std::memory_order_acquire) == currentGeneration)
      {
        Post(DirectoryEvent{std::string{siteId},
                            currentGeneration,
                            directory,
                            std::move(*result),
                            std::move(preferredSelectionIdentity)});
      }

      return true;
    }

    bool HasQueuedTransfer(const std::string_view siteId,
                           const SiteEndpointIdentity &endpoint) const
    {
      return std::ranges::any_of(queue.Snapshot(), [&](const QueuedTransfer &item)
                                 { return item.state == TransferState::Queued && item.job.siteId == siteId &&
                                          item.job.siteEndpoint == endpoint; });
    }

    void TransferLoop(const std::stop_token stopToken,
                      const SiteProfile &selectedSite,
                      const std::uint64_t currentGeneration,
                      const std::size_t workerIndex,
                      const std::string &tlsPinnedPublicKey)
    {
      RemoteSessionPtr session;

      const auto selectedEndpoint = EndpointIdentity(selectedSite);

      while (!stopToken.stop_requested())
      {
        {
          std::unique_lock lock{transferMutex};

          if (!transferCv.wait(lock, stopToken, [this, &selectedSite, &selectedEndpoint]
                               { return HasQueuedTransfer(selectedSite.id, selectedEndpoint); }))
          {
            break;
          }
        }

        auto transferLease = transferRuntime->AcquireTransfer(stopToken);

        if (!transferLease)
        {
          break;
        }

        auto next = queue.TakeNext(selectedSite.id, selectedEndpoint);

        if (!next)
        {
          continue;
        }

        PostQueue();

        if (!session || !session->Connected())
        {
          session = MakeSession(selectedSite.protocol);

          const auto queued = queue.Get(next->id);

          Post(ConnectionAttemptEvent{
              .siteId = selectedSite.id,
              .generation = currentGeneration,
              .endpoint = selectedEndpoint,
              .kind = ConnectionAttemptKind::Transfer,
              .workerNumber = static_cast<std::uint32_t>(workerIndex + 1U),
              .transferAttempt = queued ? queued->attempt : 1U});

          auto connected = session->Connect(
              selectedSite,
              BuildSessionCallbacks(tlsPinnedPublicKey, currentGeneration),
              stopToken);

          if (!connected)
          {
            {
              std::scoped_lock actionLock{transferActionMutex};

              const auto current = queue.Get(next->id);

              if (current && current->state != TransferState::Paused &&
                  current->state != TransferState::Cancelled)
              {
                const auto control = CurrentControl(next->id, stopToken);

                if (control == TransferControl::Cancel)
                {
                  (void)queue.Cancel(next->id);
                }
                else if (control == TransferControl::Pause)
                {
                  (void)queue.Pause(next->id);
                }
                else
                {
                  (void)queue.Fail(next->id, connected.error());
                }
              }
            }

            PostQueue(true);

            session.reset();

            continue;
          }

          Diagnostic(DiagnosticLevel::Debug,
                     "Transfer worker " + std::to_string(workerIndex + 1) + " connected");
        }

        auto result = ProcessTransfer(*session, *next, stopToken, currentGeneration);
        {
          // Serialize final state selection with Pause/Cancel/Retry. A
          // request either becomes visible to this finalizer or observes
          // the state this finalizer committed. It cannot fall between
          // the control read and the queue transition.
          std::scoped_lock actionLock{transferActionMutex};

          const auto current = queue.Get(next->id);

          if (current && current->state != TransferState::Paused &&
              current->state != TransferState::Cancelled)
          {
            if (result)
            {
              if (queue.Complete(next->id) &&
                  next->direction == TransferDirection::Upload)
              {
                PushBrowser(Refresh{});
              }
            }
            else
            {
              // A later Cancel must dominate a Pause result already
              // returned by the protocol operation. Successful work
              // still completes above because it may have committed
              // its final rename before the request arrived.
              const auto control = CurrentControl(next->id, stopToken);

              if (control == TransferControl::Cancel)
              {
                (void)queue.Cancel(next->id);
              }
              else if (control == TransferControl::Pause)
              {
                (void)queue.Pause(next->id);
              }
              else if (result.error().code == RemoteErrorCode::Cancelled)
              {
                (void)queue.Cancel(next->id);
              }
              else if (result.error().code == RemoteErrorCode::Paused)
              {
                (void)queue.Pause(next->id);
              }
              else
              {
                (void)queue.Fail(next->id, result.error());
              }
            }
          }
        }

        if (!result &&
            result.error().code == RemoteErrorCode::ConnectionLost)
        {
          session->Disconnect();

          session.reset();
        }

        PostQueue(true);
      }

      if (session)
      {
        session->Disconnect();
      }
    }

    Result<void> CheckJobControl(const std::string &jobId,
                                 const std::stop_token stopToken) const
    {
      const auto control = CurrentControl(jobId, stopToken);

      if (control == TransferControl::Continue)
      {
        return {};
      }

      return std::unexpected(ControllerError(
          control == TransferControl::Pause ? RemoteErrorCode::Paused
                                            : RemoteErrorCode::Cancelled,
          control == TransferControl::Pause ? "Transfer paused" : "Transfer canceled"));
    }

    Result<TransferPlan> PlanUpload(IRemoteSession &,
                                    const TransferJob &job,
                                    const std::stop_token stopToken)
    {
      if (auto control = CheckJobControl(job.id, stopToken); !control)
      {
        return std::unexpected(control.error());
      }

      std::error_code error;

      const auto status = std::filesystem::symlink_status(job.localPath, error);

      if (error)
      {
        return std::unexpected(ControllerError(
            RemoteErrorCode::LocalIo, "Cannot inspect local source: " + error.message()));
      }

      if (std::filesystem::is_symlink(status))
      {
        return std::unexpected(ControllerError(
            RemoteErrorCode::Unsupported, "Symbolic links are not followed during transfers"));
      }

      if (auto safe = platform::ValidateLocalWriteTarget(job.localPath, job.localPath);
          !safe)
      {
        return std::unexpected(ControllerError(
            RemoteErrorCode::Unsupported,
            "The upload source is a link or reparse point: " + safe.error().message));
      }

      TransferPlan plan;

      if (std::filesystem::is_regular_file(status))
      {
        const auto size = std::filesystem::file_size(job.localPath, error);

        if (error)
        {
          return std::unexpected(ControllerError(
              RemoteErrorCode::LocalIo, "Cannot read local file size: " + error.message()));
        }

        plan.files.push_back(PlannedFile{job.localPath, job.remotePath, size, std::nullopt});
        plan.totalBytes = size;

        return plan;
      }

      if (!std::filesystem::is_directory(status))
      {
        return std::unexpected(ControllerError(RemoteErrorCode::Unsupported,
                                               "The selected local item is not a file or directory"));
      }

      if (!job.recursive)
      {
        return std::unexpected(ControllerError(
            RemoteErrorCode::IsDirectory, "Recursive transfer was not enabled"));
      }

      plan.directories.emplace_back(job.localPath, job.remotePath);

      std::filesystem::recursive_directory_iterator iterator{
          job.localPath, std::filesystem::directory_options::none, error};

      const std::filesystem::recursive_directory_iterator end;

      if (error)
      {
        return std::unexpected(ControllerError(
            RemoteErrorCode::LocalIo,
            "Cannot enumerate local directory: " + error.message()));
      }

      for (; iterator != end;)
      {
        if (auto control = CheckJobControl(job.id, stopToken); !control)
        {
          return std::unexpected(control.error());
        }

        const auto itemStatus = iterator->symlink_status(error);

        if (error)
        {
          return std::unexpected(ControllerError(
              RemoteErrorCode::LocalIo, "Cannot inspect local item: " + error.message()));
        }

        const auto safeItem =
            platform::ValidateLocalWriteTarget(iterator->path(), iterator->path());

        if (std::filesystem::is_symlink(itemStatus))
        {
          iterator.disable_recursion_pending();

          Diagnostic(DiagnosticLevel::Warning,
                     "Skipped local symbolic link: " + GenericUtf8(iterator->path()));

          iterator.increment(error);

          if (error)
          {
            return std::unexpected(ControllerError(
                RemoteErrorCode::LocalIo,
                "Cannot enumerate local directory: " + error.message()));
          }

          continue;
        }

        if (!safeItem)
        {
          return std::unexpected(ControllerError(
              RemoteErrorCode::Unsupported,
              "The upload source contains a junction, reparse point, or item that cannot "
              "be inspected safely: " +
                  safeItem.error().message));
        }

        const auto relative = std::filesystem::relative(iterator->path(), job.localPath, error);

        if (error)
        {
          return std::unexpected(ControllerError(
              RemoteErrorCode::LocalIo, "Cannot calculate local relative path"));
        }

        const auto remote = job.remotePath.Joined(RemotePath{GenericUtf8(relative)});

        if (std::filesystem::is_directory(itemStatus))
        {
          plan.directories.emplace_back(iterator->path(), remote);
        }
        else if (std::filesystem::is_regular_file(itemStatus))
        {
          const auto size = iterator->file_size(error);

          if (error || (std::numeric_limits<std::uint64_t>::max)() - plan.totalBytes < size)
          {
            return std::unexpected(ControllerError(
                RemoteErrorCode::LocalIo, "Cannot determine total transfer size"));
          }

          plan.totalBytes += size;

          plan.files.push_back(PlannedFile{iterator->path(), remote, size, std::nullopt});
        }

        iterator.increment(error);

        if (error)
        {
          return std::unexpected(ControllerError(
              RemoteErrorCode::LocalIo,
              "Cannot enumerate local directory: " + error.message()));
        }
      }

      return plan;
    }

    Result<TransferPlan> PlanDownload(IRemoteSession &session,
                                      const TransferJob &job,
                                      const std::stop_token stopToken)
    {
      if (auto control = CheckJobControl(job.id, stopToken); !control)
      {
        return std::unexpected(control.error());
      }

      auto root = session.Stat(job.remotePath, stopToken);
      if (!root)
      {
        if (job.expectedRemoteRevision &&
            root.error().code == RemoteErrorCode::NotFound)
        {
          return std::unexpected(ControllerError(
              RemoteErrorCode::Conflict,
              "The remote file selected for editing no longer exists"));
        }

        return std::unexpected(root.error());
      }

      if (job.expectedRemoteRevision &&
          CompareRemoteFileRevision(
              *job.expectedRemoteRevision,
              std::optional{MakeRemoteFileRevision(*root)}) !=
              RemoteFileRevisionComparison::Matches)
      {
        return std::unexpected(ControllerError(
            RemoteErrorCode::Conflict,
            "The remote file changed or could not be verified before downloading it for editing"));
      }

      if (root->kind == RemoteEntryKind::Symlink)
      {
        return std::unexpected(ControllerError(
            RemoteErrorCode::Unsupported, "Symbolic links are not followed during transfers"));
      }

      TransferPlan plan;

      if (root->kind == RemoteEntryKind::File)
      {
        plan.localDestinationRoot = job.localPath.parent_path();

        if (plan.localDestinationRoot.empty())
        {
          std::error_code error;

          plan.localDestinationRoot = std::filesystem::current_path(error);

          if (error)
          {
            return std::unexpected(ControllerError(
                RemoteErrorCode::LocalIo,
                "Cannot resolve the local destination directory: " + error.message()));
          }
        }

        plan.files.push_back(
            PlannedFile{job.localPath, job.remotePath, root->size, root->modifiedAt});

        plan.totalBytes = root->size;

        return plan;
      }

      if (root->kind != RemoteEntryKind::Directory)
      {
        return std::unexpected(ControllerError(
            RemoteErrorCode::Unsupported, "The selected remote item is not a file or directory"));
      }

      if (!job.recursive)
      {
        return std::unexpected(ControllerError(
            RemoteErrorCode::IsDirectory, "Recursive transfer was not enabled"));
      }

      platform::SafeDownloadMapper mapper{job.localPath};

      plan.localDestinationRoot = job.localPath;
      plan.directories.emplace_back(job.localPath, job.remotePath);

      std::deque<RemotePath> pending{job.remotePath};

      while (!pending.empty())
      {
        if (auto control = CheckJobControl(job.id, stopToken); !control)
        {
          return std::unexpected(control.error());
        }

        auto directory = std::move(pending.front());

        pending.pop_front();

        auto listing = session.List(directory, stopToken);
        if (!listing)
        {
          return std::unexpected(listing.error());
        }

        for (const auto &entry : *listing)
        {
          if (auto control = CheckJobControl(job.id, stopToken); !control)
          {
            return std::unexpected(control.error());
          }

          if (!platform::IsRemotePathWithin(job.remotePath, entry.path))
          {
            return std::unexpected(ControllerError(
                RemoteErrorCode::ProtocolError,
                "The server returned a path outside the selected directory"));
          }

          const auto relativeBytes = RelativeRemoteText(job.remotePath.Bytes(),
                                                        entry.path.Bytes());

          if (relativeBytes.empty())
          {
            return std::unexpected(ControllerError(
                RemoteErrorCode::ProtocolError, "The server returned an invalid child path"));
          }

          const auto relativeDisplay = RelativeRemoteText(
              job.remotePath.DisplayUtf8(), entry.path.DisplayUtf8());

          if (relativeDisplay.empty())
          {
            return std::unexpected(ControllerError(
                RemoteErrorCode::ProtocolError,
                "The server returned an invalid display path"));
          }

          auto local = mapper.Map(RemotePath{relativeBytes, relativeDisplay});
          if (!local)
          {
            return std::unexpected(ControllerError(RemoteErrorCode::InvalidArgument,
                                                   local.error().message));
          }

          if (entry.kind == RemoteEntryKind::Symlink)
          {
            Diagnostic(DiagnosticLevel::Warning,
                       "Skipped remote symbolic link: " + entry.path.DisplayUtf8());
          }
          else if (entry.kind == RemoteEntryKind::Directory)
          {
            plan.directories.emplace_back(*local, entry.path);

            pending.push_back(entry.path);
          }
          else if (entry.kind == RemoteEntryKind::File)
          {
            if ((std::numeric_limits<std::uint64_t>::max)() - plan.totalBytes < entry.size)
            {
              return std::unexpected(ControllerError(
                  RemoteErrorCode::ProtocolError, "Transfer size overflow"));
            }

            plan.totalBytes += entry.size;
            plan.files.push_back(
                PlannedFile{*local, entry.path, entry.size, entry.modifiedAt});
          }
        }
      }

      return plan;
    }

    Result<ConflictResolution> ResolveTransferConflict(const TransferJob &job,
                                                       const std::optional<std::uint64_t> existingSize,
                                                       const bool safeResume,
                                                       const std::stop_token stopToken,
                                                       const std::uint64_t currentGeneration)
    {
      // Two transfer workers may discover conflicts together. Serialize the
      // complete policy check/prompt/update sequence so wx never nests two
      // conflict dialogs and "apply to remaining" takes effect atomically.
      std::unique_lock promptLock{conflictPromptMutex, std::defer_lock};

      while (!promptLock.try_lock_for(25ms))
      {
        if (auto control = CheckJobControl(job.id, stopToken); !control)
        {
          return std::unexpected(control.error());
        }
      }

      if (auto control = CheckJobControl(job.id, stopToken); !control)
      {
        return std::unexpected(control.error());
      }

      if (job.conflictPolicy != ConflictPolicy::Ask)
      {
        return ConflictResolution{job.conflictPolicy, false};
      }

      {
        std::scoped_lock lock{conflictMutex};

        if (remainingConflictChoice &&
            remainingConflictChoice->jobIds.contains(job.id))
        {
          return ConflictResolution{remainingConflictChoice->policy, true};
        }
      }

      auto result = InvokeUi<ConflictResolution>(interactions.resolveConflict,
                                                 ConflictChallenge{
                                                     .job = job,
                                                     .existingSize = existingSize,
                                                     .safeResumeAvailable = safeResume,
                                                     .applyToRemainingAvailable = true,
                                                     .reason = ConflictReason::DestinationExists},
                                                 stopToken,
                                                 currentGeneration,
                                                 "No conflict handler is available");

      if (result && result->applyToRemainingQueue)
      {
        RemainingConflictChoice choice;
        choice.policy = result->policy;

        for (const auto &transfer : queue.Snapshot())
        {
          if (transfer.state == TransferState::Queued ||
              transfer.state == TransferState::Enumerating ||
              transfer.state == TransferState::Running ||
              transfer.state == TransferState::Paused)
          {
            choice.jobIds.insert(transfer.job.id);
          }
        }

        std::scoped_lock lock{conflictMutex};

        remainingConflictChoice = std::move(choice);
      }

      return result;
    }

    Result<ConflictResolution> RemoteChangedConflictResolution(
        const TransferJob &job,
        const std::optional<std::uint64_t> existingSize,
        const std::stop_token stopToken,
        const std::uint64_t currentGeneration)
    {
      // An external edit is bound to one captured remote revision. Never let
      // a queue-wide policy or a prior "apply to remaining" answer authorize
      // replacing a different revision.
      std::unique_lock promptLock{conflictPromptMutex, std::defer_lock};

      while (!promptLock.try_lock_for(25ms))
      {
        if (auto control = CheckJobControl(job.id, stopToken); !control)
        {
          return std::unexpected(control.error());
        }
      }

      if (auto control = CheckJobControl(job.id, stopToken); !control)
      {
        return std::unexpected(control.error());
      }

      auto promptJob = job;
      promptJob.conflictPolicy = ConflictPolicy::Ask;

      return InvokeUi<ConflictResolution>(
          interactions.resolveConflict,
          ConflictChallenge{
              .job = std::move(promptJob),
              .existingSize = existingSize,
              .safeResumeAvailable = false,
              .applyToRemainingAvailable = false,
              .reason = ConflictReason::RemoteChanged,
          },
          stopToken,
          currentGeneration,
          "No conflict handler is available");
    }

    Result<void> ConfirmRemoteChangedOverwrite(
        const TransferJob &job,
        const std::optional<RemoteFileRevision> &current,
        const std::stop_token stopToken,
        const std::uint64_t currentGeneration)
    {
      auto resolution = RemoteChangedConflictResolution(
          job,
          current ? std::optional<std::uint64_t>{current->size}
                  : std::nullopt,
          stopToken,
          currentGeneration);

      if (!resolution)
      {
        return std::unexpected(resolution.error());
      }

      if (resolution->policy != ConflictPolicy::Overwrite)
      {
        return std::unexpected(ControllerError(
            RemoteErrorCode::Conflict,
            "The edited remote file was not overwritten because its "
            "remote revision no longer matches"));
      }

      return {};
    }

    Result<ConflictResolution> CreateFileConflictResolution(
        const SiteProfile &selectedSite,
        const RemotePath &path,
        const std::optional<std::uint64_t> existingSize,
        const std::stop_token stopToken,
        const std::uint64_t currentGeneration)
    {
      // Share the transfer-conflict gate so a one-off create prompt cannot
      // appear on top of a transfer conflict from another worker.
      std::unique_lock promptLock{conflictPromptMutex, std::defer_lock};

      while (!promptLock.try_lock_for(25ms))
      {
        if (stopToken.stop_requested())
        {
          return std::unexpected(ControllerError(
              RemoteErrorCode::Cancelled,
              "Create-file conflict resolution was cancelled"));
        }
      }

      if (stopToken.stop_requested())
      {
        return std::unexpected(ControllerError(
            RemoteErrorCode::Cancelled,
            "Create-file conflict resolution was cancelled"));
      }

      TransferJob job{
          .id = GenerateId(),
          .siteId = selectedSite.id,
          .siteEndpoint = EndpointIdentity(selectedSite),
          .direction = TransferDirection::Upload,
          .localPath = {},
          .remotePath = path,
          .recursive = false,
          .conflictPolicy = ConflictPolicy::Ask,
          .expectedRemoteRevision = std::nullopt};

      return InvokeUi<ConflictResolution>(
          interactions.resolveConflict,
          ConflictChallenge{.job = std::move(job),
                            .existingSize = existingSize,
                            .safeResumeAvailable = false,
                            .applyToRemainingAvailable = false,
                            .reason = ConflictReason::DestinationExists},
          stopToken,
          currentGeneration,
          "No conflict handler is available");
    }

    TransferControl CurrentControl(const std::string &id,
                                   const std::stop_token stopToken) const
    {
      std::scoped_lock lock{controlMutex};

      const auto found = controls.find(id);

      if (found != controls.end() &&
          found->second != TransferControl::Continue)
      {
        return found->second;
      }

      return stopToken.stop_requested() ? TransferControl::Cancel
                                        : TransferControl::Continue;
    }

    Result<void> ValidatePlanDestinations(
        const TransferJob &job, const TransferPlan &plan,
        const TransferDestinationOverride *candidate = nullptr)
    {
      // Check the whole plan, including files and partial names not written yet
      TransferRuntime destinations{1};
      std::filesystem::path localRoot;

      if (job.direction == TransferDirection::Download)
      {
        std::error_code error;

        localRoot = std::filesystem::absolute(plan.localDestinationRoot, error).lexically_normal();

        if (error)
        {
          return std::unexpected(ControllerError(RemoteErrorCode::LocalIo,
                                                 "Cannot resolve the local destination directory: " + error.message()));
        }
      }

      platform::SafeDownloadMapper localDestinations{localRoot};

      std::vector<TransferRuntime::DestinationReservation> reservations;

      const auto reserve = [&](const std::filesystem::path &local,
                               const RemotePath &remote) -> Result<void>
      {
        if (job.direction == TransferDirection::Download)
        {
          std::error_code error;

          const auto absolute = std::filesystem::absolute(local, error).lexically_normal();

          if (error)
          {
            return std::unexpected(ControllerError(RemoteErrorCode::LocalIo,
                                                   "Cannot resolve the local destination: " + error.message()));
          }

          const auto relative = absolute.lexically_relative(localRoot);

          if (relative == ".")
          {
            return {};
          }

          auto mapped = localDestinations.Map(RemotePath{GenericUtf8(relative)});
          if (!mapped)
          {
            return std::unexpected(ControllerError(
                mapped.error().code == platform::PlatformErrorCode::Conflict
                    ? RemoteErrorCode::Conflict
                    : RemoteErrorCode::LocalIo,
                mapped.error().message));
          }

          return {};
        }

        auto reserved = destinations.ReserveRemoteDestination(*job.siteEndpoint, remote);
        if (!reserved)
        {
          return std::unexpected(reserved.error());
        }

        reservations.push_back(std::move(*reserved));

        return {};
      };

      for (const auto &[local, remote] : plan.directories)
      {
        if (auto reserved = reserve(local, remote); !reserved)
        {
          return reserved;
        }
      }

      for (const auto &file : plan.files)
      {
        auto resolved = candidate && candidate->originalLocalPath == file.local &&
                                candidate->originalRemotePath == file.remote
                            ? Result<TransferDestinationOverride>{*candidate}
                            : ResolveTransferPaths(job, file.local, file.remote);
        if (!resolved)
        {
          return std::unexpected(resolved.error());
        }

        if (job.direction == TransferDirection::Download)
        {
          if (!platform::IsLocalSafeFilename(GenericUtf8(resolved->localPath.filename())))
          {
            return std::unexpected(ControllerError(RemoteErrorCode::InvalidArgument,
                                                   "The resolved download filename is unsafe"));
          }

          if (auto safe = platform::ValidateLocalWriteTarget(
                  plan.localDestinationRoot, resolved->localPath);
              !safe)
          {
            return std::unexpected(ControllerError(RemoteErrorCode::LocalIo, safe.error().message));
          }
        }

        if (auto reserved = reserve(resolved->localPath, resolved->remotePath); !reserved)
        {
          return reserved;
        }

        if (job.direction == TransferDirection::Download)
        {
          if (auto reserved = reserve(PartialPath(resolved->localPath), resolved->remotePath);
              !reserved)
          {
            return reserved;
          }
        }
      }

      return {};
    }

    Result<void> RememberResolvedDestination(
        TransferJob &job, const TransferDestinationOverride &resolution,
        const std::stop_token stopToken, const std::uint64_t currentGeneration)
    {
      {
        std::scoped_lock actionLock{transferActionMutex};

        if (auto control = CheckJobControl(job.id, stopToken); !control)
        {
          return control;
        }

        // Persistence takes this same lock before its queue snapshot, so no
        // export can mix a renamed job with checkpoints for its former name.
        std::scoped_lock resumeLock{resumeMutex};

        auto updated = queue.ResolveDestination(job.id, resolution);
        if (!updated)
        {
          return std::unexpected(updated.error());
        }

        job = std::move(*updated);

        std::erase_if(uploadResumes, [&](const auto &entry)
                      {
          const auto &resume = entry.second;

          return resume.jobId == job.id &&
                 (!job.recursive || resume.localPath == resolution.originalLocalPath); });

        std::erase_if(downloadResumes, [&](const auto &entry)
                      {
          const auto &resume = entry.second;

          return resume.jobId == job.id &&
                 (!job.recursive || resume.remotePath == resolution.originalRemotePath); });
      }

      PostQueue();

      // Keep the chosen name in memory if this fails, but do not write a file.
      // Retry first persists the same coherent state before activating work.
      return PersistQueueOnUi("resolved transfer destination", stopToken, currentGeneration);
    }

    Result<void> ProcessTransfer(IRemoteSession &session,
                                 TransferJob job,
                                 const std::stop_token stopToken,
                                 const std::uint64_t currentGeneration)
    {
      Result<TransferPlan> planned = job.direction == TransferDirection::Upload
                                         ? PlanUpload(session, job, stopToken)
                                         : PlanDownload(session, job, stopToken);

      if (!planned)
      {
        return std::unexpected(planned.error());
      }

      if (!job.siteEndpoint)
      {
        return std::unexpected(ControllerError(RemoteErrorCode::Conflict,
                                               "The transfer has no endpoint identity"));
      }

      if (auto valid = ValidatePlanDestinations(job, *planned); !valid)
      {
        return valid;
      }

      if (auto running = queue.MarkRunning(job.id); !running)
      {
        return std::unexpected(running.error());
      }

      PostQueue();

      for (const auto &[local, remote] : planned->directories)
      {
        if (auto control = CheckJobControl(job.id, stopToken); !control)
        {
          return std::unexpected(control.error());
        }

        if (job.direction == TransferDirection::Upload)
        {
          auto created = session.Mkdir(remote, true, stopToken);
          if (!created)
          {
            if (created.error().code != RemoteErrorCode::AlreadyExists)
            {
              return created;
            }

            auto existing = session.Stat(remote, stopToken);
            if (!existing)
            {
              return std::unexpected(existing.error());
            }
            if (existing->kind != RemoteEntryKind::Directory)
            {
              return std::unexpected(ControllerError(
                  RemoteErrorCode::Conflict,
                  "A non-directory remote item blocks the upload directory: " +
                      remote.DisplayUtf8()));
            }
          }
        }
        else
        {
          auto safe = platform::ValidateLocalWriteTarget(planned->localDestinationRoot, local);
          if (!safe)
          {
            return std::unexpected(ControllerError(RemoteErrorCode::LocalIo,
                                                   safe.error().message));
          }

          std::error_code error;
          std::filesystem::create_directories(local, error);
          if (error)
          {
            return std::unexpected(ControllerError(
                RemoteErrorCode::LocalIo, "Cannot create local directory: " + error.message()));
          }

          safe = platform::ValidateLocalWriteTarget(planned->localDestinationRoot, local);
          if (!safe)
          {
            return std::unexpected(ControllerError(RemoteErrorCode::LocalIo,
                                                   safe.error().message));
          }
        }
      }

      std::uint64_t completedBytes = 0;
      std::uint64_t activeBytesTransferred = 0;

      auto activeElapsed = std::chrono::steady_clock::duration::zero();
      auto lastQueuePost = std::chrono::steady_clock::now() - 1s;

      for (auto file : planned->files)
      {
        const auto originalFile = file;

        auto resolved = ResolveTransferPaths(job, file.local, file.remote);
        if (!resolved)
        {
          return std::unexpected(resolved.error());
        }

        file.local = resolved->localPath;
        file.remote = resolved->remotePath;

        if (const auto control = CurrentControl(job.id, stopToken);
            control != TransferControl::Continue)
        {
          return std::unexpected(ControllerError(
              control == TransferControl::Pause ? RemoteErrorCode::Paused
                                                : RemoteErrorCode::Cancelled,
              control == TransferControl::Pause ? "Transfer paused" : "Transfer canceled"));
        }

        TransferOptions options{.jobId = job.id,
                                .resumeOffset = 0,
                                .overwrite = false,
                                .useTemporaryName = true,
                                .temporaryRemotePath = std::nullopt,
                                .beforeFinalize = {}};

        bool skip = false;

        if (job.direction == TransferDirection::Download)
        {
          std::error_code error;

          auto safe = platform::ValidateLocalWriteTarget(planned->localDestinationRoot, file.local);
          if (!safe)
          {
            return std::unexpected(ControllerError(RemoteErrorCode::LocalIo,
                                                   safe.error().message));
          }

          const bool destinationExists = std::filesystem::exists(file.local, error);
          const auto part = PartialPath(file.local);

          safe = platform::ValidateLocalWriteTarget(planned->localDestinationRoot, part);
          if (!safe)
          {
            return std::unexpected(ControllerError(RemoteErrorCode::LocalIo,
                                                   safe.error().message));
          }

          const bool partExists = std::filesystem::is_regular_file(part, error);

          std::uint64_t partSize = 0;

          bool safeResume = false;

          const auto downloadResumeKey = job.siteId + "\n" + job.id + "\n" +
                                         file.remote.Bytes() + "\n" +
                                         GenericUtf8(file.local);

          if (partExists)
          {
            partSize = std::filesystem::file_size(part, error);

            const auto partModified = std::filesystem::last_write_time(part, error);

            std::scoped_lock lock{resumeMutex};

            const auto recorded = downloadResumes.find(downloadResumeKey);

            safeResume = recorded != downloadResumes.end() && !error &&
                         recorded->second.siteId == job.siteId &&
                         job.siteEndpoint &&
                         recorded->second.siteEndpoint == *job.siteEndpoint &&
                         recorded->second.localPath == file.local &&
                         recorded->second.remotePath.Bytes() == file.remote.Bytes() &&
                         recorded->second.remoteSize == file.size &&
                         file.modifiedAt.has_value() &&
                         recorded->second.remoteModified == file.modifiedAt &&
                         recorded->second.partSize == partSize &&
                         recorded->second.partModified == partModified &&
                         partSize <= file.size;
          }

          if (destinationExists || partExists)
          {
            auto resolution = ResolveTransferConflict(
                job,
                destinationExists
                    ? std::optional<std::uint64_t>{std::filesystem::file_size(file.local, error)}
                    : std::optional<std::uint64_t>{partSize},
                safeResume,
                stopToken,
                currentGeneration);

            if (!resolution)
            {
              return std::unexpected(resolution.error());
            }

            switch (resolution->policy)
            {
            case ConflictPolicy::Skip:
              if (job.expectedRemoteRevision)
              {
                return std::unexpected(ControllerError(
                    RemoteErrorCode::Conflict,
                    "The remote edit download was skipped and has no valid local baseline"));
              }
              skip = true;
              break;

            case ConflictPolicy::Overwrite:
              options.overwrite = true;
              break;

            case ConflictPolicy::Resume:
              if (!safeResume)
              {
                return std::unexpected(ControllerError(
                    RemoteErrorCode::Conflict,
                    "Resume metadata no longer matches the source and partial file"));
              }
              options.resumeOffset = partSize;
              options.overwrite = destinationExists;
              break;

            case ConflictPolicy::Rename:
            {
              if (job.expectedRemoteRevision)
              {
                return std::unexpected(ControllerError(
                    RemoteErrorCode::Conflict,
                    "A guarded remote edit download cannot change its local destination"));
              }

              const auto stem = file.local.stem();
              const auto extension = file.local.extension();

              bool renamed = false;

              for (unsigned int suffix = 2; suffix < 10'000; ++suffix)
              {
                auto name = stem;
                name += " (" + std::to_string(suffix) + ")";
                name += extension;

                auto candidate = file.local.parent_path() / name;
                const bool exists = std::filesystem::exists(candidate, error);

                if (error)
                {
                  return std::unexpected(ControllerError(RemoteErrorCode::LocalIo, error.message()));
                }

                const bool partialExists = std::filesystem::exists(PartialPath(candidate), error);

                if (error)
                {
                  return std::unexpected(ControllerError(RemoteErrorCode::LocalIo, error.message()));
                }

                if (!exists && !partialExists)
                {
                  const TransferDestinationOverride choice{
                      originalFile.local, originalFile.remote, candidate, file.remote};

                  if (auto valid = ValidatePlanDestinations(job, *planned, &choice); !valid)
                  {
                    if (valid.error().code == RemoteErrorCode::Conflict)
                    {
                      continue;
                    }

                    return valid;
                  }

                  if (auto saved = RememberResolvedDestination(job, choice, stopToken,
                                                               currentGeneration);
                      !saved)
                  {
                    return saved;
                  }

                  file.local = std::move(candidate);

                  renamed = true;

                  break;
                }
              }

              if (!renamed)
              {
                return std::unexpected(ControllerError(
                    RemoteErrorCode::Conflict,
                    "Could not find an unused local download name"));
              }

              break;
            }

            case ConflictPolicy::Ask:
              break;
            }
          }

          error.clear();

          if (!file.local.parent_path().empty())
          {
            std::filesystem::create_directories(file.local.parent_path(), error);
          }

          if (error)
          {
            return std::unexpected(ControllerError(
                RemoteErrorCode::LocalIo, "Cannot create destination directory: " + error.message()));
          }

          safe = platform::ValidateLocalWriteTarget(planned->localDestinationRoot,
                                                    file.local);
          if (!safe)
          {
            return std::unexpected(ControllerError(RemoteErrorCode::LocalIo,
                                                   safe.error().message));
          }

          safe = platform::ValidateLocalWriteTarget(planned->localDestinationRoot,
                                                    PartialPath(file.local));
          if (!safe)
          {
            return std::unexpected(ControllerError(RemoteErrorCode::LocalIo,
                                                   safe.error().message));
          }

          if (job.expectedRemoteRevision)
          {
            options.beforeFinalize =
                [&session,
                 remote = file.remote,
                 expected = *job.expectedRemoteRevision,
                 stopToken](const bool overwrite) -> Result<bool>
            {
              auto observed = Impl::ObserveRemoteRevision(
                  session, remote, stopToken);
              if (!observed)
              {
                return std::unexpected(observed.error());
              }

              if (CompareRemoteFileRevision(expected, *observed) !=
                  RemoteFileRevisionComparison::Matches)
              {
                return std::unexpected(ControllerError(
                    RemoteErrorCode::Conflict,
                    "The remote file changed or could not be verified while it was being downloaded for editing"));
              }

              return overwrite;
            };
          }
        }
        else
        {
          std::error_code localError;

          const auto localSize = std::filesystem::file_size(file.local, localError);

          if (localError || localSize != file.size)
          {
            return std::unexpected(ControllerError(
                RemoteErrorCode::Conflict,
                "The local upload source changed after queue enumeration"));
          }

          const auto localModified = std::filesystem::last_write_time(file.local, localError);

          if (localError)
          {
            return std::unexpected(ControllerError(
                RemoteErrorCode::LocalIo,
                "Cannot read the local source timestamp: " + localError.message()));
          }

          auto existing = session.Stat(file.remote, stopToken);
          if (!existing && existing.error().code != RemoteErrorCode::NotFound)
          {
            return std::unexpected(existing.error());
          }

          auto uploadResumeKey = job.siteId + "\n" + job.id + "\n" + file.remote.Bytes();

          std::optional<UploadResume> recorded;
          {
            std::scoped_lock lock{resumeMutex};

            const auto found = uploadResumes.find(uploadResumeKey);

            if (found != uploadResumes.end())
            {
              recorded = found->second;
            }
          }

          bool safeResume = false;

          std::uint64_t partialSize = 0;

          if (recorded && recorded->siteId == job.siteId &&
              job.siteEndpoint && recorded->siteEndpoint == *job.siteEndpoint &&
              recorded->localPath == file.local &&
              recorded->localSize == localSize &&
              recorded->localModified == localModified &&
              recorded->remotePath.Bytes() == file.remote.Bytes())
          {
            auto partial = session.Stat(recorded->temporaryPath, stopToken);

            if (partial && partial->kind == RemoteEntryKind::File &&
                partial->size <= localSize)
            {
              safeResume = true;

              partialSize = partial->size;
            }
            else if (!partial && partial.error().code != RemoteErrorCode::NotFound)
            {
              return std::unexpected(partial.error());
            }
          }

          if (job.expectedRemoteRevision)
          {
            std::optional<RemoteFileRevision> currentRevision;

            if (existing)
            {
              currentRevision = MakeRemoteFileRevision(*existing);
            }

            const auto comparison = CompareRemoteFileRevision(
                *job.expectedRemoteRevision, currentRevision);

            if (comparison != RemoteFileRevisionComparison::Matches)
            {
              auto confirmed = ConfirmRemoteChangedOverwrite(
                  job,
                  currentRevision,
                  stopToken,
                  currentGeneration);

              if (!confirmed)
              {
                return std::unexpected(confirmed.error());
              }
            }

            // A matching revision authorizes this edit save without
            // the ordinary destination-exists prompt. A non-matching
            // revision reaches here only after an explicit one-off
            // Overwrite decision.
            options.overwrite = existing.has_value();

            if (safeResume && recorded)
            {
              options.resumeOffset = partialSize;
              options.temporaryRemotePath = recorded->temporaryPath;
            }

            options.beforeFinalize =
                [this,
                 &session,
                 path = file.remote,
                 guardedJob = job,
                 authorizedRevision = std::move(currentRevision),
                 stopToken,
                 currentGeneration](const bool) mutable -> Result<bool>
            {
              while (true)
              {
                auto observed = Impl::ObserveRemoteRevision(session, path, stopToken);
                if (!observed)
                {
                  return std::unexpected(observed.error());
                }

                // Once the user explicitly accepts an otherwise
                // unverifiable, missing, or wrong-kind state, its
                // exact observable fields become the commit
                // baseline. Re-prompt only if those fields change
                // again before the final rename.
                if (*observed == authorizedRevision)
                {
                  return observed->has_value();
                }

                auto confirmed = ConfirmRemoteChangedOverwrite(
                    guardedJob,
                    *observed,
                    stopToken,
                    currentGeneration);

                if (!confirmed)
                {
                  return std::unexpected(confirmed.error());
                }

                authorizedRevision = std::move(*observed);
              }
            };
          }
          else if (existing || safeResume)
          {
            const std::optional<std::uint64_t> conflictSize =
                existing ? std::optional<std::uint64_t>{existing->size}
                         : std::optional<std::uint64_t>{partialSize};

            auto resolution = ResolveTransferConflict(
                job,
                conflictSize,
                safeResume,
                stopToken,
                currentGeneration);
            if (!resolution)
            {
              return std::unexpected(resolution.error());
            }

            switch (resolution->policy)
            {
            case ConflictPolicy::Skip:
              skip = true;
              break;

            case ConflictPolicy::Overwrite:
              options.overwrite = existing.has_value();
              break;

            case ConflictPolicy::Rename:
            {
              bool renamed = false;

              for (unsigned int suffix = 2; suffix < 10'000; ++suffix)
              {
                auto candidate = RemoteConflictCandidate(file.remote, suffix);

                auto status = session.Stat(candidate, stopToken);
                if (!status && status.error().code == RemoteErrorCode::NotFound)
                {
                  const TransferDestinationOverride choice{
                      originalFile.local, originalFile.remote, file.local, candidate};

                  if (auto valid = ValidatePlanDestinations(job, *planned, &choice); !valid)
                  {
                    if (valid.error().code == RemoteErrorCode::Conflict)
                    {
                      continue;
                    }

                    return valid;
                  }

                  if (auto saved = RememberResolvedDestination(job, choice, stopToken,
                                                               currentGeneration);
                      !saved)
                  {
                    return saved;
                  }

                  file.remote = std::move(candidate);

                  renamed = true;

                  break;
                }

                if (!status && status.error().code != RemoteErrorCode::NotFound)
                {
                  return std::unexpected(status.error());
                }
              }

              if (!renamed)
              {
                return std::unexpected(ControllerError(
                    RemoteErrorCode::Conflict,
                    "Could not find an unused remote upload name"));
              }

              recorded.reset();

              safeResume = false;

              uploadResumeKey = job.siteId + "\n" + job.id + "\n" +
                                file.remote.Bytes();

              break;
            }

            case ConflictPolicy::Resume:
              if (!safeResume || !recorded)
              {
                return std::unexpected(ControllerError(
                    RemoteErrorCode::Conflict,
                    "Upload resume metadata no longer matches the source and partial file"));
              }
              options.resumeOffset = partialSize;
              options.temporaryRemotePath = recorded->temporaryPath;
              options.overwrite = existing.has_value();
              break;

            case ConflictPolicy::Ask:
              break;
            }
          }

          if (!skip && !options.temporaryRemotePath)
          {
            for (unsigned int attempt = 0; attempt < 32; ++attempt)
            {
              const auto token = GenerateId();

              RemotePath candidate{
                  file.remote.Bytes() + ".havremote." + token + ".part",
                  file.remote.DisplayUtf8() + ".havremote." + token + ".part"};

              auto status = session.Stat(candidate, stopToken);

              if (!status && status.error().code == RemoteErrorCode::NotFound)
              {
                options.temporaryRemotePath = std::move(candidate);

                break;
              }

              if (!status && status.error().code != RemoteErrorCode::NotFound)
              {
                return std::unexpected(status.error());
              }
            }

            if (!options.temporaryRemotePath)
            {
              return std::unexpected(ControllerError(
                  RemoteErrorCode::Conflict,
                  "Could not reserve a unique remote temporary upload name"));
            }
          }

          if (!skip && options.temporaryRemotePath)
          {
            std::scoped_lock lock{resumeMutex};

            uploadResumes[uploadResumeKey] = UploadResume{job.id,
                                                          job.siteId,
                                                          *job.siteEndpoint,
                                                          file.local,
                                                          localSize,
                                                          localModified,
                                                          file.remote,
                                                          *options.temporaryRemotePath};
          }

          if (!skip && options.temporaryRemotePath)
          {
            // Do not begin writing the remote temporary file until its exact
            // identity has reached the atomic queue repository.
            auto persisted = PersistQueueOnUi(
                "upload temporary path", stopToken, currentGeneration);

            if (!persisted)
            {
              return std::unexpected(persisted.error());
            }
          }
        }

        if (skip)
        {
          completedBytes += file.size;

          (void)queue.UpdateProgress(TransferProgress{
              .jobId = job.id,
              .bytesTransferred = completedBytes,
              .totalBytes = planned->totalBytes,
              .activeBytesTransferred = activeBytesTransferred,
              .elapsed = activeElapsed});

          continue;
        }

        std::optional<TransferRuntime::DestinationReservation> destinationReservation;
        std::optional<TransferRuntime::DestinationReservation> partialDestinationReservation;

        if (job.direction == TransferDirection::Download)
        {
          auto reserved = transferRuntime->ReserveLocalDestination(file.local);
          if (!reserved)
          {
            return std::unexpected(reserved.error());
          }

          destinationReservation.emplace(std::move(*reserved));

          auto partialReserved = transferRuntime->ReserveLocalDestination(PartialPath(file.local));
          if (!partialReserved)
          {
            return std::unexpected(partialReserved.error());
          }

          partialDestinationReservation.emplace(std::move(*partialReserved));
        }
        else
        {
          if (!job.siteEndpoint)
          {
            return std::unexpected(ControllerError(
                RemoteErrorCode::Conflict,
                "The upload has no immutable endpoint identity"));
          }

          auto reserved = transferRuntime->ReserveRemoteDestination(*job.siteEndpoint, file.remote);
          if (!reserved)
          {
            return std::unexpected(reserved.error());
          }

          destinationReservation.emplace(std::move(*reserved));
        }

        const auto fileBase = completedBytes;
        const auto activeBytesBase = activeBytesTransferred;
        const auto activeElapsedBase = activeElapsed;

        std::uint64_t fileActiveBytesTransferred{};

        auto fileActiveElapsed =
            std::chrono::steady_clock::duration::zero();

        const ProgressCallback progress =
            [this, &lastQueuePost, jobId = job.id, fileBase, total = planned->totalBytes,
             activeBytesBase, activeElapsedBase,
             &fileActiveBytesTransferred, &fileActiveElapsed,
             stopToken](const TransferProgress &value)
        {
          fileActiveBytesTransferred = value.activeBytesTransferred;
          fileActiveElapsed = value.elapsed;

          TransferProgress aggregate = value;
          aggregate.jobId = jobId;
          aggregate.bytesTransferred = fileBase + value.bytesTransferred;
          aggregate.totalBytes = total;
          aggregate.activeBytesTransferred = activeBytesBase + value.activeBytesTransferred;
          aggregate.elapsed = activeElapsedBase + value.elapsed;

          (void)queue.UpdateProgress(aggregate);

          const auto now = std::chrono::steady_clock::now();

          if (now - lastQueuePost >= 100ms)
          {
            PostQueue();

            lastQueuePost = now;
          }

          return CurrentControl(jobId, stopToken);
        };

        Result<void> transferred = job.direction == TransferDirection::Upload
                                       ? session.Upload(file.local,
                                                        file.remote,
                                                        options,
                                                        progress,
                                                        stopToken)
                                       : session.Download(file.remote,
                                                          file.local,
                                                          options,
                                                          progress,
                                                          stopToken);
        if (!transferred)
        {
          if (job.direction == TransferDirection::Download)
          {
            std::error_code error;

            const auto part = PartialPath(file.local);

            if (std::filesystem::is_regular_file(part, error))
            {
              const auto size = std::filesystem::file_size(part, error);
              const auto modified = std::filesystem::last_write_time(part, error);

              if (!error && file.modifiedAt && size <= file.size)
              {
                std::scoped_lock lock{resumeMutex};

                const auto key = job.siteId + "\n" + job.id + "\n" +
                                 file.remote.Bytes() + "\n" +
                                 GenericUtf8(file.local);

                downloadResumes[key] = DownloadResume{job.id,
                                                      job.siteId,
                                                      *job.siteEndpoint,
                                                      file.local,
                                                      file.remote,
                                                      file.size,
                                                      file.modifiedAt,
                                                      size,
                                                      modified};
              }
            }
          }

          PostQueue();

          return transferred;
        }

        activeBytesTransferred += fileActiveBytesTransferred;
        activeElapsed += fileActiveElapsed;

        if (job.direction == TransferDirection::Upload &&
            job.expectedRemoteRevision)
        {
          auto committed = session.Stat(file.remote, stopToken);
          if (!committed)
          {
            return std::unexpected(committed.error());
          }

          if (committed->kind != RemoteEntryKind::File ||
              committed->size != file.size)
          {
            return std::unexpected(ControllerError(
                RemoteErrorCode::Conflict,
                "The committed remote file does not match the edited local file"));
          }

          std::scoped_lock lock{committedUploadMutex};

          committedUploadRevisions[file.remote.Bytes()] =
              CommittedUploadRevision{
                  .generation = currentGeneration,
                  .revision = MakeRemoteFileRevision(*committed),
          };
        }

        {
          std::scoped_lock lock{resumeMutex};

          if (job.direction == TransferDirection::Upload)
          {
            uploadResumes.erase(job.siteId + "\n" + job.id + "\n" +
                                file.remote.Bytes());
          }
          else
          {
            downloadResumes.erase(job.siteId + "\n" + job.id + "\n" +
                                  file.remote.Bytes() + "\n" +
                                  GenericUtf8(file.local));
          }
        }

        PostQueue();

        completedBytes += file.size;

        TransferProgress aggregate{
            .jobId = job.id,
            .bytesTransferred = completedBytes,
            .totalBytes = planned->totalBytes,
            .activeBytesTransferred = activeBytesTransferred,
            .elapsed = activeElapsed};

        (void)queue.UpdateProgress(aggregate);
      }

      (void)queue.UpdateProgress(TransferProgress{
          .jobId = job.id,
          .bytesTransferred = planned->totalBytes,
          .totalBytes = planned->totalBytes,
          .activeBytesTransferred = activeBytesTransferred,
          .elapsed = activeElapsed});

      return {};
    }

    std::vector<config::PersistentQueueItem> PersistentQueueItems()
    {
      std::scoped_lock resumeLock{resumeMutex};

      auto transfers = queue.Snapshot();

      std::vector<config::PersistentQueueItem> result;
      result.reserve(transfers.size());

      for (auto &transfer : transfers)
      {
        if (transfer.state == TransferState::Completed ||
            transfer.state == TransferState::Cancelled)
        {
          continue;
        }

        if (transfer.state == TransferState::Enumerating ||
            transfer.state == TransferState::Running)
        {
          // A process restart can never continue a worker operation. Retain
          // its verified checkpoints, but require an explicit Retry.
          transfer.state = TransferState::Paused;
          transfer.error.reset();
        }

        transfer.progress.activeBytesTransferred = 0U;
        transfer.progress.elapsed = {};

        config::PersistentQueueItem item{
            .connectionId = connectionId,
            .transfer = std::move(transfer),
            .resumeRecords = {},
        };

        for (const auto &[key, resume] : downloadResumes)
        {
          (void)key;

          if (resume.jobId != item.transfer.job.id)
          {
            continue;
          }

          item.resumeRecords.push_back(config::QueueResumeRecord{
              .kind = config::QueueResumeKind::Download,
              .localPath = resume.localPath,
              .remotePath = resume.remotePath,
              .remoteSize = resume.remoteSize,
              .remoteModifiedAt = resume.remoteModified,
              .partSize = resume.partSize,
              .partModifiedAt = resume.partModified,
              .localSize = 0,
              .localModifiedAt = std::nullopt,
              .temporaryRemotePath = std::nullopt,
          });
        }

        for (const auto &[key, resume] : uploadResumes)
        {
          (void)key;

          if (resume.jobId != item.transfer.job.id)
          {
            continue;
          }

          item.resumeRecords.push_back(config::QueueResumeRecord{
              .kind = config::QueueResumeKind::Upload,
              .localPath = resume.localPath,
              .remotePath = resume.remotePath,
              .remoteSize = 0,
              .remoteModifiedAt = std::nullopt,
              .partSize = 0,
              .partModifiedAt = std::nullopt,
              .localSize = resume.localSize,
              .localModifiedAt = resume.localModified,
              .temporaryRemotePath = resume.temporaryPath,
          });
        }

        std::ranges::sort(
            item.resumeRecords,
            [](const config::QueueResumeRecord &left,
               const config::QueueResumeRecord &right)
            {
              if (left.kind != right.kind)
              {
                return left.kind < right.kind;
              }

              if (left.remotePath.Bytes() != right.remotePath.Bytes())
              {
                return left.remotePath.Bytes() < right.remotePath.Bytes();
              }

              return GenericUtf8(left.localPath) < GenericUtf8(right.localPath);
            });

        result.push_back(std::move(item));
      }

      return result;
    }

    wxEvtHandler &eventTarget;
    std::string connectionId;
    std::shared_ptr<TransferRuntime> transferRuntime;
    std::filesystem::path knownHostsFile;
    UiInteractions interactions;
    std::size_t workerCount{};
    mutable std::mutex timeoutMutex;
    std::chrono::seconds connectionTimeout;
    std::chrono::seconds commandIdleTimeout;
    std::atomic_bool shuttingDown{};
    std::shared_ptr<UiDispatchState> uiDispatch{std::make_shared<UiDispatchState>()};

    mutable std::mutex siteMutex;
    std::optional<SiteProfile> site;
    std::optional<std::uint64_t> connectedGeneration;
    std::jthread browser;
    std::vector<std::jthread> transferWorkers;

    std::mutex browserMutex;
    std::condition_variable_any browserCv;
    std::deque<BrowserCommand> browserCommands;

    TransferQueue queue;
    std::mutex queuePostMutex;
    mutable std::mutex transferActionMutex;
    mutable std::mutex transferMutex;
    std::condition_variable_any transferCv;

    mutable std::mutex controlMutex;
    std::unordered_map<std::string, TransferControl> controls;
    std::mutex conflictMutex;
    std::timed_mutex conflictPromptMutex;
    std::optional<RemainingConflictChoice> remainingConflictChoice;
    std::mutex resumeMutex;
    std::unordered_map<std::string, DownloadResume> downloadResumes;
    std::unordered_map<std::string, UploadResume> uploadResumes;
    std::mutex committedUploadMutex;
    std::unordered_map<std::string, CommittedUploadRevision>
        committedUploadRevisions;
  };

  RemoteController::RemoteController(wxEvtHandler &eventTarget,
                                     std::string connectionId,
                                     std::shared_ptr<TransferRuntime> transferRuntime,
                                     std::filesystem::path knownHostsFile,
                                     UiInteractions interactions,
                                     const std::chrono::seconds connectionTimeout,
                                     const std::chrono::seconds commandIdleTimeout,
                                     std::vector<config::PersistentQueueItem> restoredItems)
      : mImpl(std::make_unique<Impl>(eventTarget,
                                     std::move(connectionId),
                                     std::move(transferRuntime),
                                     std::move(knownHostsFile),
                                     std::move(interactions),
                                     connectionTimeout,
                                     commandIdleTimeout,
                                     std::move(restoredItems))) {}

  RemoteController::~RemoteController()
  {
    mImpl->Shutdown();
  }

  std::uint64_t RemoteController::Connect(SiteProfile site)
  {
    return mImpl->Start(std::move(site));
  }

  void RemoteController::Disconnect()
  {
    mImpl->StopWorkers();

    mImpl->Post(ConnectionEvent{false,
                                {},
                                mImpl->uiDispatch->generation.load(std::memory_order_acquire),
                                "Disconnected"});
  }

  void RemoteController::Browse(RemotePath directory)
  {
    mImpl->PushBrowser(Impl::Browse{std::move(directory)});
  }

  void RemoteController::Refresh()
  {
    mImpl->PushBrowser(Impl::Refresh{});
  }

  void RemoteController::Inspect(RemotePath path, std::string requestId)
  {
    mImpl->PushBrowser(
        Impl::Inspect{std::move(path), std::move(requestId)});
  }

  void RemoteController::CreateRemoteDirectory(RemotePath directory, const bool recursive)
  {
    mImpl->PushBrowser(Impl::Mkdir{std::move(directory), recursive});
  }

  void RemoteController::CreateRemoteFile(RemotePath path)
  {
    mImpl->PushBrowser(Impl::CreateFile{std::move(path)});
  }

  void RemoteController::Rename(RemotePath source,
                                RemotePath destination,
                                const bool overwrite)
  {
    mImpl->PushBrowser(Impl::Rename{std::move(source), std::move(destination), overwrite});
  }

  void RemoteController::Remove(RemotePath path, const bool recursive)
  {
    mImpl->PushBrowser(Impl::Remove{std::move(path), recursive});
  }

  void RemoteController::SetPermissions(std::vector<RemotePath> paths,
                                        const std::uint32_t permissions)
  {
    if (paths.empty())
    {
      return;
    }

    mImpl->PushBrowser(Impl::SetPermissions{std::move(paths), permissions});
  }

  Result<std::string> RemoteController::Enqueue(TransferJob job)
  {
    if (job.expectedRemoteRevision && job.recursive)
    {
      return std::unexpected(ControllerError(
          RemoteErrorCode::InvalidArgument,
          "An expected remote revision is only valid for a single-file transfer"));
    }

    {
      std::scoped_lock lock{mImpl->siteMutex};

      if (!mImpl->site)
      {
        return std::unexpected(ControllerError(
            RemoteErrorCode::NotConnected,
            "Connect to a site before queueing a transfer"));
      }

      if (job.siteId.empty())
      {
        job.siteId = mImpl->site->id;
      }
      else if (job.siteId != mImpl->site->id)
      {
        return std::unexpected(ControllerError(
            RemoteErrorCode::Conflict,
            "The transfer belongs to a different site"));
      }

      const auto selectedEndpoint = EndpointIdentity(*mImpl->site);
      if (!job.siteEndpoint)
      {
        job.siteEndpoint = selectedEndpoint;
      }
      else if (*job.siteEndpoint != selectedEndpoint)
      {
        return std::unexpected(ControllerError(
            RemoteErrorCode::Conflict,
            "The transfer belongs to a different site endpoint"));
      }
    }

    // Keep new work inert until its complete identity has been committed to
    // queue.cson. This closes the crash window between enqueue and the first
    // worker claim.
    auto result = mImpl->queue.EnqueuePaused(std::move(job));
    if (result)
    {
      {
        std::scoped_lock lock{mImpl->controlMutex};
        mImpl->controls[*result] = TransferControl::Continue;
      }

      if (auto persisted = mImpl->PersistQueueDirect("new transfer");
          !persisted)
      {
        {
          std::scoped_lock lock{mImpl->controlMutex};
          mImpl->controls[*result] = TransferControl::Pause;
        }

        mImpl->Diagnostic(DiagnosticLevel::Error, persisted.error().message);
        mImpl->PostQueue();

        return result;
      }

      if (auto activated = mImpl->queue.Retry(*result); !activated)
      {
        return std::unexpected(activated.error());
      }

      mImpl->PostQueue();
      mImpl->transferCv.notify_one();
    }

    return result;
  }

  Result<void> RemoteController::Pause(const std::string_view jobId)
  {
    Result<void> result;
    {
      std::scoped_lock actionLock{mImpl->transferActionMutex};

      const auto current = mImpl->queue.Get(jobId);
      if (!current)
      {
        return std::unexpected(ControllerError(RemoteErrorCode::NotFound,
                                               "Transfer not found"));
      }

      // Active work changes state only after the protocol acknowledges the
      // pause. If it already committed its final rename, the worker completes
      // it instead of presenting a dangerous retry of successful work.
      if (!IsTransferTransitionAllowed(current->state,
                                       TransferState::Paused))
      {
        return std::unexpected(ControllerError(
            RemoteErrorCode::Conflict,
            "Only pending or active transfers can be paused"));
      }

      {
        std::scoped_lock lock{mImpl->controlMutex};

        auto &control = mImpl->controls[std::string{jobId}];
        if (control == TransferControl::Cancel)
        {
          return std::unexpected(ControllerError(
              RemoteErrorCode::Conflict,
              "Transfer cancellation is already pending"));
        }

        control = TransferControl::Pause;
      }

      if (current->state == TransferState::Queued)
      {
        const auto paused = mImpl->queue.PauseIfQueued(jobId);
        if (!paused)
        {
          result = std::unexpected(paused.error());
        }
      }
    }

    mImpl->PostQueue(true);

    return result;
  }

  Result<void> RemoteController::Cancel(const std::string_view jobId)
  {
    Result<void> result;
    {
      std::scoped_lock actionLock{mImpl->transferActionMutex};

      const auto current = mImpl->queue.Get(jobId);
      if (!current)
      {
        return std::unexpected(ControllerError(RemoteErrorCode::NotFound,
                                               "Transfer not found"));
      }

      if (!IsTransferTransitionAllowed(current->state,
                                       TransferState::Cancelled))
      {
        return std::unexpected(ControllerError(
            RemoteErrorCode::Conflict,
            "Only unfinished transfers can be canceled"));
      }

      {
        // Cancellation always dominates an earlier pause request and must
        // be visible before a queued transfer can be claimed by a worker.
        std::scoped_lock lock{mImpl->controlMutex};

        mImpl->controls[std::string{jobId}] = TransferControl::Cancel;
      }

      if (current->state != TransferState::Enumerating &&
          current->state != TransferState::Running)
      {
        const auto cancelled = mImpl->queue.CancelIfInactive(jobId);
        if (!cancelled)
        {
          result = std::unexpected(cancelled.error());
        }
      }
    }

    mImpl->PostQueue(true);

    return result;
  }

  Result<void> RemoteController::Retry(const std::string_view jobId)
  {
    Result<void> result;
    {
      std::scoped_lock actionLock{mImpl->transferActionMutex};

      const auto queued = mImpl->queue.Get(jobId);
      if (!queued)
      {
        return std::unexpected(ControllerError(RemoteErrorCode::NotFound,
                                               "Transfer not found"));
      }

      if (!IsTransferTransitionAllowed(queued->state,
                                       TransferState::Queued))
      {
        return std::unexpected(ControllerError(
            RemoteErrorCode::Conflict,
            "Only paused or failed transfers can be retried"));
      }

      {
        std::scoped_lock lock{mImpl->siteMutex};

        const auto generation =
            mImpl->uiDispatch->generation.load(std::memory_order_acquire);

        if (!mImpl->site || !mImpl->connectedGeneration ||
            *mImpl->connectedGeneration != generation ||
            queued->job.siteId != mImpl->site->id ||
            queued->job.siteEndpoint != EndpointIdentity(*mImpl->site))
        {
          return std::unexpected(ControllerError(
              RemoteErrorCode::Conflict,
              "Reconnect to this transfer's original site before retrying it"));
        }
      }

      {
        // Clear the old Pause/Cancel request before exposing the job as
        // queued, otherwise a worker could immediately stop the retry.
        std::scoped_lock lock{mImpl->controlMutex};

        mImpl->controls[std::string{jobId}] = TransferControl::Continue;
      }

      if (auto persisted = mImpl->PersistQueueDirect("transfer retry");
          !persisted)
      {
        std::scoped_lock lock{mImpl->controlMutex};

        mImpl->controls[std::string{jobId}] = TransferControl::Pause;

        return std::unexpected(persisted.error());
      }

      result = mImpl->queue.Retry(jobId);
    }

    if (result)
    {
      mImpl->PostQueue();
      mImpl->transferCv.notify_one();
    }

    return result;
  }

  Result<void> RemoteController::RemoveTerminalTransfer(
      const std::string_view jobId)
  {
    const std::array jobIds{std::string{jobId}};

    return RemoveTerminalTransfers(
        std::span<const std::string>{jobIds});
  }

  Result<void> RemoteController::RemoveTerminalTransfers(
      const std::span<const std::string> jobIds)
  {
    if (jobIds.empty())
    {
      return {};
    }

    {
      std::scoped_lock actionLock{mImpl->transferActionMutex};

      auto removed = mImpl->queue.RemoveTerminal(jobIds);
      if (!removed)
      {
        return std::unexpected(removed.error());
      }

      const auto selected = [&](const std::string_view candidate)
      {
        return std::ranges::any_of(
            jobIds, [&](const std::string &jobId)
            { return jobId == candidate; });
      };

      {
        std::scoped_lock lock{mImpl->controlMutex};

        for (const auto &jobId : jobIds)
        {
          mImpl->controls.erase(jobId);
        }
      }

      {
        std::scoped_lock lock{mImpl->resumeMutex};

        std::erase_if(
            mImpl->downloadResumes,
            [&](const auto &entry)
            { return selected(entry.second.jobId); });

        std::erase_if(
            mImpl->uploadResumes,
            [&](const auto &entry)
            { return selected(entry.second.jobId); });
      }

      {
        std::scoped_lock lock{mImpl->conflictMutex};

        if (mImpl->remainingConflictChoice)
        {
          for (const auto &jobId : jobIds)
          {
            mImpl->remainingConflictChoice->jobIds.erase(jobId);
          }

          if (mImpl->remainingConflictChoice->jobIds.empty())
          {
            mImpl->remainingConflictChoice.reset();
          }
        }
      }
    }

    mImpl->PostQueue(true);

    return {};
  }

  Result<void> RemoteController::UpdateTimeouts(
      const std::chrono::seconds connectionTimeout,
      const std::chrono::seconds commandIdleTimeout)
  {
    if (connectionTimeout.count() <= 0 ||
        commandIdleTimeout.count() <= 0)
    {
      return std::unexpected(ControllerError(
          RemoteErrorCode::InvalidArgument,
          "Connection and command idle timeouts must be positive"));
    }

    std::scoped_lock lock{mImpl->timeoutMutex};

    mImpl->connectionTimeout = connectionTimeout;
    mImpl->commandIdleTimeout = commandIdleTimeout;

    return {};
  }

  std::vector<QueuedTransfer> RemoteController::Transfers() const
  {
    return mImpl->queue.Snapshot();
  }

  void RemoteController::PrepareForShutdown()
  {
    mImpl->Shutdown();
  }

  std::vector<config::PersistentQueueItem>
  RemoteController::PersistentQueueItems() const
  {
    return mImpl->PersistentQueueItems();
  }

  TransferActionAvailability RemoteController::TransferActions(
      const std::string_view jobId) const
  {
    std::scoped_lock actionLock{mImpl->transferActionMutex};

    const auto transfer = mImpl->queue.Get(jobId);
    if (!transfer)
    {
      return {};
    }

    auto result = AvailableTransferActions(
        transfer->state,
        mImpl->CurrentControl(std::string{jobId}, std::stop_token{}));
    if (!result.retry)
    {
      return result;
    }

    std::scoped_lock lock{mImpl->siteMutex};

    const auto generation =
        mImpl->uiDispatch->generation.load(std::memory_order_acquire);

    result.retry = mImpl->site && mImpl->connectedGeneration &&
                   *mImpl->connectedGeneration == generation &&
                   transfer->job.siteId == mImpl->site->id &&
                   transfer->job.siteEndpoint == EndpointIdentity(*mImpl->site);

    return result;
  }
} // namespace havremote::ui
