// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UI_CONTROLLER_EVENTS_HPP
#define HAVREMOTE_INCLUDE_UI_CONTROLLER_EVENTS_HPP

#include "core/transferQueue.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace havremote::ui
{
  enum class ConnectionAttemptKind
  {
    Browser,
    Transfer,
  };

  struct ConnectionAttemptEvent final
  {
    std::string siteId;
    std::uint64_t generation{};
    SiteEndpointIdentity endpoint;
    ConnectionAttemptKind kind{ConnectionAttemptKind::Browser};
    std::uint32_t workerNumber{};
    std::uint32_t transferAttempt{};
  };

  struct ConnectionEvent final
  {
    bool connected{};
    std::string siteId;
    std::uint64_t generation{};
    std::string message;
  };

  struct DirectoryEvent final
  {
    std::string siteId;
    std::uint64_t generation{};
    RemotePath directory;
    std::vector<RemoteEntry> entries;
    std::optional<std::string> preferredSelectionIdentity;
  };

  struct RemoteInspectionEvent final
  {
    std::string requestId;
    std::string siteId;
    std::uint64_t generation{};
    RemotePath path;
    Result<RemoteEntry> result;
  };

  struct QueueEvent final
  {
    std::vector<QueuedTransfer> transfers;
    // True when the UI handler must perform an atomic save. Changes already
    // covered by a synchronous durability barrier, and plain progress events,
    // do not request a second write here.
    bool durableChange{};
  };

  struct OperationErrorEvent final
  {
    std::string siteId;
    std::uint64_t generation{};
    std::string operation;
    RemoteError error;
  };

  struct DiagnosticEvent final
  {
    DiagnosticLevel level{DiagnosticLevel::Information};
    std::string message;
  };

  using ControllerEvent = std::variant<ConnectionAttemptEvent,
                                       ConnectionEvent,
                                       DirectoryEvent,
                                       RemoteInspectionEvent,
                                       QueueEvent,
                                       OperationErrorEvent,
                                       DiagnosticEvent>;

  // Every controller instance has a distinct runtime connection ID. Site IDs
  // identify saved profiles and generations identify reconnect epochs, neither
  // of which can distinguish two tabs connected to the same profile.
  struct ControllerEventEnvelope final
  {
    std::string connectionId;
    ControllerEvent event;
  };

  // The const pointer makes a posted event immutable while wxWidgets transfers
  // ownership of its lightweight shared payload to the event thread.
  using ControllerEventEnvelopePtr =
      std::shared_ptr<const ControllerEventEnvelope>;
} // namespace havremote::ui

#endif // HAVREMOTE_INCLUDE_UI_CONTROLLER_EVENTS_HPP
