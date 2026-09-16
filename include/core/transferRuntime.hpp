// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_CORE_TRANSFER_RUNTIME_HPP
#define HAVREMOTE_INCLUDE_CORE_TRANSFER_RUNTIME_HPP

#include "core/types.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace havremote
{
  // Process-wide transfer coordination shared by every connection controller.
  // Controllers may own any number of candidate worker threads, but only the
  // configured number of leases can perform transfer work concurrently.
  class TransferRuntime final
  {
    struct State;

  public:
    using LocalPathConflictCheck = std::function<Result<bool>(
        const std::filesystem::path &, const std::filesystem::path &)>;

    class TransferLease final
    {
    public:
      TransferLease(const TransferLease &) = delete;
      TransferLease &operator=(const TransferLease &) = delete;
      TransferLease(TransferLease &&other) noexcept;
      TransferLease &operator=(TransferLease &&other) noexcept;
      ~TransferLease();

    private:
      friend class TransferRuntime;
      explicit TransferLease(std::shared_ptr<State> state) noexcept;
      void Release() noexcept;

      std::shared_ptr<State> mState;
    };

    class DestinationReservation final
    {
    public:
      DestinationReservation(const DestinationReservation &) = delete;
      DestinationReservation &operator=(const DestinationReservation &) = delete;
      DestinationReservation(DestinationReservation &&other) noexcept;
      DestinationReservation &operator=(DestinationReservation &&other) noexcept;
      ~DestinationReservation();

    private:
      enum class Kind
      {
        Local,
        Remote
      };

      friend class TransferRuntime;
      DestinationReservation(std::shared_ptr<State> state,
                             Kind kind,
                             std::filesystem::path localKey,
                             std::string remoteKey) noexcept;
      void Release() noexcept;

      std::shared_ptr<State> mState;
      Kind mKind{Kind::Local};
      std::filesystem::path mLocalKey;
      std::string mRemoteKey;
    };

    // Without a conflict check, local reservations compare only normalized
    // absolute path spellings. The callback receives candidate and active paths
    // under the runtime mutex. True or an error rejects the reservation. It must
    // not call back into this runtime.
    explicit TransferRuntime(
        std::size_t concurrencyLimit,
        LocalPathConflictCheck localPathConflictCheck = {});

    TransferRuntime(const TransferRuntime &) = delete;
    TransferRuntime &operator=(const TransferRuntime &) = delete;

    [[nodiscard]] std::size_t ConcurrencyLimit() const noexcept;

    // Returns no lease when cancellation is requested while waiting for a
    // global transfer slot. The bounded polling interval also keeps this safe
    // on standard libraries whose condition-variable stop integration varies.
    [[nodiscard]] std::optional<TransferLease> AcquireTransfer(
        std::stop_token stopToken) const;

    [[nodiscard]] Result<DestinationReservation> ReserveLocalDestination(
        const std::filesystem::path &path) const;
    [[nodiscard]] Result<DestinationReservation> ReserveRemoteDestination(
        const SiteEndpointIdentity &endpoint,
        const RemotePath &path) const;

  private:
    std::shared_ptr<State> mState;
  };
} // namespace havremote

#endif // HAVREMOTE_INCLUDE_CORE_TRANSFER_RUNTIME_HPP
