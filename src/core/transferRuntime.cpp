// SPDX-License-Identifier: MIT

#include "core/transferRuntime.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace havremote
{
  namespace
  {
    using namespace std::chrono_literals;

    RemoteError RuntimeError(const RemoteErrorCode code, std::string message)
    {
      return RemoteError{.code = code, .message = std::move(message)};
    }

    std::optional<std::filesystem::path> LocalDestinationKey(
        const std::filesystem::path &path)
    {
      std::error_code error;

      auto absolute = std::filesystem::absolute(path, error).lexically_normal();

      if (error || absolute.empty())
      {
        return std::nullopt;
      }

      return absolute;
    }

    std::string NormalizedEndpointHost(std::string_view host)
    {
      std::string normalized{host};

      std::ranges::transform(normalized, normalized.begin(), [](const unsigned char character)
                             { return character >= 'A' && character <= 'Z'
                                          ? static_cast<char>(character - 'A' + 'a')
                                          : static_cast<char>(character); });

      return normalized;
    }

    std::string RemoteDestinationKey(const SiteEndpointIdentity &endpoint,
                                     const RemotePath &path)
    {
      const auto append = [](std::string &output, const std::string_view value)
      {
        output += std::to_string(value.size());
        output.push_back(':');
        output.append(value);
      };

      std::string key = std::to_string(static_cast<unsigned int>(endpoint.protocol));

      key.push_back('|');
      append(key, NormalizedEndpointHost(endpoint.host));

      key.push_back('|');
      key += std::to_string(endpoint.port);

      key.push_back('|');
      append(key, endpoint.username);

      key.push_back('|');
      append(key, path.Bytes());

      return key;
    }
  } // namespace

  struct TransferRuntime::State final
  {
    explicit State(const std::size_t limit,
                   LocalPathConflictCheck localPathConflictCheck)
        : concurrencyLimit(limit),
          mLocalPathConflictCheck(std::move(localPathConflictCheck)) {}

    const std::size_t concurrencyLimit;
    const LocalPathConflictCheck mLocalPathConflictCheck;
    std::mutex mutex;
    std::condition_variable slotAvailable;
    std::size_t activeTransfers{};
    std::unordered_set<std::filesystem::path> activeLocalDestinations;
    std::unordered_set<std::string> activeRemoteDestinations;
  };

  TransferRuntime::TransferLease::TransferLease(std::shared_ptr<State> state) noexcept
      : mState(std::move(state)) {}

  TransferRuntime::TransferLease::TransferLease(TransferLease &&other) noexcept
      : mState(std::move(other.mState)) {}

  TransferRuntime::TransferLease &TransferRuntime::TransferLease::operator=(
      TransferLease &&other) noexcept
  {
    if (this != &other)
    {
      Release();

      mState = std::move(other.mState);
    }

    return *this;
  }

  TransferRuntime::TransferLease::~TransferLease() { Release(); }

  void TransferRuntime::TransferLease::Release() noexcept
  {
    if (!mState)
    {
      return;
    }

    {
      std::scoped_lock lock{mState->mutex};

      if (mState->activeTransfers > 0U)
      {
        --mState->activeTransfers;
      }
    }

    mState->slotAvailable.notify_one();

    mState.reset();
  }

  TransferRuntime::DestinationReservation::DestinationReservation(
      std::shared_ptr<State> state,
      const Kind kind,
      std::filesystem::path localKey,
      std::string remoteKey) noexcept
      : mState(std::move(state)),
        mKind(kind),
        mLocalKey(std::move(localKey)),
        mRemoteKey(std::move(remoteKey)) {}

  TransferRuntime::DestinationReservation::DestinationReservation(
      DestinationReservation &&other) noexcept
      : mState(std::move(other.mState)),
        mKind(other.mKind),
        mLocalKey(std::move(other.mLocalKey)),
        mRemoteKey(std::move(other.mRemoteKey)) {}

  TransferRuntime::DestinationReservation &
  TransferRuntime::DestinationReservation::operator=(
      DestinationReservation &&other) noexcept
  {
    if (this != &other)
    {
      Release();

      mState = std::move(other.mState);
      mKind = other.mKind;
      mLocalKey = std::move(other.mLocalKey);
      mRemoteKey = std::move(other.mRemoteKey);
    }

    return *this;
  }

  TransferRuntime::DestinationReservation::~DestinationReservation() { Release(); }

  void TransferRuntime::DestinationReservation::Release() noexcept
  {
    if (!mState)
    {
      return;
    }

    {
      std::scoped_lock lock{mState->mutex};

      if (mKind == Kind::Local)
      {
        mState->activeLocalDestinations.erase(mLocalKey);
      }
      else
      {
        mState->activeRemoteDestinations.erase(mRemoteKey);
      }
    }

    mState.reset();
    mLocalKey.clear();
    mRemoteKey.clear();
  }

  TransferRuntime::TransferRuntime(
      const std::size_t concurrencyLimit,
      LocalPathConflictCheck localPathConflictCheck)
  {
    if (concurrencyLimit == 0U || concurrencyLimit > 16U)
    {
      throw std::invalid_argument{"Transfer concurrency must be between 1 and 16"};
    }

    mState = std::make_shared<State>(concurrencyLimit,
                                     std::move(localPathConflictCheck));
  }

  std::size_t TransferRuntime::ConcurrencyLimit() const noexcept
  {
    return mState->concurrencyLimit;
  }

  std::optional<TransferRuntime::TransferLease> TransferRuntime::AcquireTransfer(
      const std::stop_token stopToken) const
  {
    std::unique_lock lock{mState->mutex};

    while (mState->activeTransfers >= mState->concurrencyLimit)
    {
      if (stopToken.stop_requested())
      {
        return std::nullopt;
      }

      mState->slotAvailable.wait_for(lock, 25ms);
    }

    if (stopToken.stop_requested())
    {
      return std::nullopt;
    }

    ++mState->activeTransfers;

    return TransferLease{mState};
  }

  Result<TransferRuntime::DestinationReservation>
  TransferRuntime::ReserveLocalDestination(const std::filesystem::path &path) const
  {
    auto key = LocalDestinationKey(path);
    if (!key)
    {
      return std::unexpected(RuntimeError(
          RemoteErrorCode::LocalIo,
          "Could not normalize the local transfer destination"));
    }

    {
      std::scoped_lock lock{mState->mutex};

      bool hasConflict = mState->activeLocalDestinations.contains(*key);
      if (!hasConflict && mState->mLocalPathConflictCheck)
      {
        for (const auto &active : mState->activeLocalDestinations)
        {
          auto conflict = mState->mLocalPathConflictCheck(*key, active);

          if (!conflict)
          {
            return std::unexpected(std::move(conflict.error()));
          }

          if (*conflict)
          {
            hasConflict = true;

            break;
          }
        }
      }

      if (hasConflict)
      {
        return std::unexpected(RuntimeError(
            RemoteErrorCode::Conflict,
            "Another transfer is already writing this local destination"));
      }

      mState->activeLocalDestinations.insert(*key);
    }

    return DestinationReservation{
        mState, DestinationReservation::Kind::Local, std::move(*key), {}};
  }

  Result<TransferRuntime::DestinationReservation>
  TransferRuntime::ReserveRemoteDestination(const SiteEndpointIdentity &endpoint,
                                            const RemotePath &path) const
  {
    auto key = RemoteDestinationKey(endpoint, path);
    {
      std::scoped_lock lock{mState->mutex};

      if (!mState->activeRemoteDestinations.insert(key).second)
      {
        return std::unexpected(RuntimeError(
            RemoteErrorCode::Conflict,
            "Another transfer is already writing this remote destination"));
      }
    }

    return DestinationReservation{
        mState, DestinationReservation::Kind::Remote, {}, std::move(key)};
  }
} // namespace havremote
