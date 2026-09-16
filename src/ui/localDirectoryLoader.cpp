// SPDX-License-Identifier: MIT

#include "ui/localDirectoryLoader.hpp"
#include "ui/localDirectoryWorkerLimiter.hpp"
#include "core/fileTime.hpp"

#include <wx/app.h>

#include <atomic>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace havremote::ui
{
  wxDEFINE_EVENT(EVT_HAVREMOTE_LOCAL_DIRECTORY, wxThreadEvent);

  struct LocalDirectoryLoader::SharedState final
  {
    SharedState(wxEvtHandler &target, std::string runtimeConnectionId)
        : connectionId(std::move(runtimeConnectionId)), eventTarget(&target)
    {
      if (connectionId.empty())
      {
        throw std::invalid_argument{
            "LocalDirectoryLoader requires a connection id"};
      }
    }

    const std::string connectionId;
    std::atomic<std::uint64_t> generation{};
    std::mutex eventTargetMutex;
    wxEvtHandler *eventTarget{};
  };

  namespace
  {
    constexpr std::size_t MaxConcurrentDirectoryWorkers = 4U;

    detail::LocalDirectoryWorkerLimiter &ProcessDirectoryWorkerLimiter()
    {
      static detail::LocalDirectoryWorkerLimiter limiter{MaxConcurrentDirectoryWorkers};
      return limiter;
    }

    LocalEntryKind Classify(const std::filesystem::file_status status) noexcept
    {
      if (std::filesystem::is_symlink(status))
      {
        return LocalEntryKind::Symlink;
      }

      if (std::filesystem::is_directory(status))
      {
        return LocalEntryKind::Directory;
      }

      if (std::filesystem::is_regular_file(status))
      {
        return LocalEntryKind::File;
      }

      return LocalEntryKind::Other;
    }

    LocalDirectoryIssue Issue(std::filesystem::path path,
                              const LocalDirectoryOperation operation,
                              const std::error_code error,
                              std::string message = {})
    {
      if (message.empty())
      {
        message = error.message();
      }

      return LocalDirectoryIssue{.path = std::move(path),
                                 .operation = operation,
                                 .error = error,
                                 .message = std::move(message)};
    }

    std::chrono::system_clock::time_point ToSystemTime(
        const std::filesystem::file_time_type value)
    {
      return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
          FileTimeToSystemTime(value));
    }
  } // namespace

  LocalDirectoryLoader::LocalDirectoryLoader(wxEvtHandler &eventTarget,
                                             std::string connectionId)
      : mState(std::make_shared<SharedState>(eventTarget,
                                             std::move(connectionId))) {}

  LocalDirectoryLoader::~LocalDirectoryLoader()
  {
    Cancel();

    const std::lock_guard lock{mState->eventTargetMutex};

    mState->eventTarget = nullptr;
  }

  std::uint64_t LocalDirectoryLoader::Load(std::filesystem::path directory)
  {
    std::stop_source nextStopSource;

    mCurrentStopSource.request_stop();

    auto workerPermit = ProcessDirectoryWorkerLimiter().TryAcquire();
    if (!workerPermit)
    {
      mState->generation.fetch_add(1U, std::memory_order_acq_rel);

      throw std::system_error{
          std::make_error_code(std::errc::resource_unavailable_try_again),
          "Too many local directory requests are still blocked"};
    }

    const auto requestGeneration = mState->generation.fetch_add(1, std::memory_order_acq_rel) + 1;

    mCurrentStopSource = std::move(nextStopSource);

    const auto stopToken = mCurrentStopSource.get_token();

    auto state = mState;

    std::thread worker{
        [state,
         directory = std::move(directory),
         requestGeneration,
         stopToken,
         activeWorker = std::move(*workerPermit)]() mutable
        {
          // The process-wide permit is released by RAII on every worker exit
          (void)activeWorker;

          try
          {
            Enumerate(state,
                      std::move(directory),
                      requestGeneration,
                      stopToken);
          }
          catch (...)
          {
            // Enumeration errors are normally captured in the result.
            // Allocation failures and other exceptional failures must
            // not terminate the process from a detached worker.
          }
        }};

    try
    {
      worker.detach();
    }
    catch (...)
    {
      mCurrentStopSource.request_stop();

      if (worker.joinable())
      {
        worker.join();
      }

      throw;
    }

    return requestGeneration;
  }

  void LocalDirectoryLoader::Cancel()
  {
    mCurrentStopSource.request_stop();
    mState->generation.fetch_add(1, std::memory_order_acq_rel);
  }

  std::uint64_t LocalDirectoryLoader::CurrentGeneration() const noexcept
  {
    return mState->generation.load(std::memory_order_acquire);
  }

  void LocalDirectoryLoader::Enumerate(std::shared_ptr<SharedState> state,
                                       std::filesystem::path directory,
                                       const std::uint64_t requestGeneration,
                                       const std::stop_token stopToken)
  {
    auto result = LocalDirectoryResult{.connectionId = state->connectionId,
                                       .generation = requestGeneration,
                                       .directory = std::move(directory),
                                       .entries = {},
                                       .warnings = {},
                                       .error = std::nullopt};

    try
    {
      std::error_code iteratorError;

      auto iterator = std::filesystem::directory_iterator{
          result.directory,
          std::filesystem::directory_options::skip_permission_denied,
          iteratorError};

      if (iteratorError)
      {
        result.error = Issue(result.directory,
                             LocalDirectoryOperation::OpenDirectory,
                             iteratorError);
      }
      else
      {
        const auto end = std::filesystem::directory_iterator{};

        while (iterator != end && !stopToken.stop_requested())
        {
          auto snapshot = LocalDirectoryEntrySnapshot{
              .path = iterator->path(),
              .symlinkStatus = {},
              .kind = LocalEntryKind::Other,
              .exactSize = std::nullopt,
              .modifiedAt = std::nullopt,
          };

          std::error_code statusError;

          snapshot.symlinkStatus = iterator->symlink_status(statusError);

          if (statusError)
          {
            result.warnings.push_back(Issue(snapshot.path,
                                            LocalDirectoryOperation::ReadStatus,
                                            statusError));
          }
          else
          {
            snapshot.kind = Classify(snapshot.symlinkStatus);

            // file_size() and last_write_time() may follow links, so
            // neither is queried for an entry classified as a link.
            if (snapshot.kind == LocalEntryKind::File)
            {
              std::error_code sizeError;

              const auto size = iterator->file_size(sizeError);

              if (sizeError)
              {
                result.warnings.push_back(Issue(snapshot.path,
                                                LocalDirectoryOperation::ReadSize,
                                                sizeError));
              }
              else if (size > std::numeric_limits<std::uint64_t>::max())
              {
                result.warnings.push_back(Issue(
                    snapshot.path,
                    LocalDirectoryOperation::ReadSize,
                    std::make_error_code(std::errc::value_too_large)));
              }
              else
              {
                snapshot.exactSize = static_cast<std::uint64_t>(size);
              }
            }

            if (snapshot.kind != LocalEntryKind::Symlink)
            {
              std::error_code modifiedError;

              const auto modified = iterator->last_write_time(modifiedError);

              if (modifiedError)
              {
                result.warnings.push_back(Issue(
                    snapshot.path,
                    LocalDirectoryOperation::ReadModifiedTime,
                    modifiedError));
              }
              else
              {
                snapshot.modifiedAt = ToSystemTime(modified);
              }
            }
          }

          result.entries.push_back(std::move(snapshot));

          iterator.increment(iteratorError);

          if (iteratorError)
          {
            result.warnings.push_back(Issue(result.directory,
                                            LocalDirectoryOperation::AdvanceIterator,
                                            iteratorError));

            break;
          }
        }
      }
    }
    catch (const std::exception &exception)
    {
      result.error = Issue(result.directory,
                           LocalDirectoryOperation::Enumerate,
                           std::make_error_code(std::errc::io_error),
                           exception.what());
    }
    catch (...)
    {
      result.error = Issue(result.directory,
                           LocalDirectoryOperation::Enumerate,
                           std::make_error_code(std::errc::io_error),
                           "Unknown error while enumerating the directory");
    }

    if (stopToken.stop_requested() ||
        state->generation.load(std::memory_order_acquire) !=
            requestGeneration)
    {
      return;
    }

    const LocalDirectoryResultPtr payload =
        std::make_shared<const LocalDirectoryResult>(std::move(result));

    // Queueing and target invalidation share this mutex. This lets the UI
    // destroy the loader without joining a worker that may be blocked in the
    // filesystem, while preventing a late worker from touching a dead target.
    const std::lock_guard lock{state->eventTargetMutex};

    if (stopToken.stop_requested() ||
        state->generation.load(std::memory_order_acquire) !=
            requestGeneration ||
        !state->eventTarget)
    {
      return;
    }

    auto *event = new wxThreadEvent(EVT_HAVREMOTE_LOCAL_DIRECTORY);
    event->SetPayload(payload);

    wxQueueEvent(state->eventTarget, event);
  }
} // namespace havremote::ui
