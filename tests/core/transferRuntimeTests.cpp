// SPDX-License-Identifier: MIT

#include "core/transferRuntime.hpp"
#include "ui/controllerEvents.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <variant>

using namespace std::chrono_literals;

TEST_CASE("controller event envelopes distinguish identical site epochs")
{
  using namespace havremote::ui;

  const auto makePayload = [](std::string connectionId)
  {
    return ControllerEventEnvelopePtr{
        std::make_shared<const ControllerEventEnvelope>(
            ControllerEventEnvelope{
                .connectionId = std::move(connectionId),
                .event = ConnectionEvent{.connected = true,
                                         .siteId = "same-site",
                                         .generation = 2,
                                         .message = "Connected"}})};
  };

  const auto first = makePayload("connection-a");
  const auto second = makePayload("connection-b");

  REQUIRE(first);
  REQUIRE(second);
  CHECK(first->connectionId != second->connectionId);
  REQUIRE(std::holds_alternative<ConnectionEvent>(first->event));
  REQUIRE(std::holds_alternative<ConnectionEvent>(second->event));

  const auto &firstEvent = std::get<ConnectionEvent>(first->event);
  const auto &secondEvent = std::get<ConnectionEvent>(second->event);

  CHECK(firstEvent.siteId == secondEvent.siteId);
  CHECK(firstEvent.generation == secondEvent.generation);
}

TEST_CASE("connection attempt events distinguish browser and transfer sessions")
{
  using namespace havremote::ui;

  const havremote::SiteEndpointIdentity endpoint{
      .protocol = havremote::ProtocolKind::Sftp,
      .host = "example.com",
      .port = 22,
      .username = "user"};

  const ConnectionAttemptEvent browser{
      .siteId = "site-a",
      .generation = 4,
      .endpoint = endpoint,
      .kind = ConnectionAttemptKind::Browser};

  const ConnectionAttemptEvent transfer{
      .siteId = "site-a",
      .generation = 4,
      .endpoint = endpoint,
      .kind = ConnectionAttemptKind::Transfer,
      .workerNumber = 2,
      .transferAttempt = 3};

  CHECK(browser.kind == ConnectionAttemptKind::Browser);
  CHECK(browser.workerNumber == 0);
  CHECK(browser.transferAttempt == 0);
  CHECK(transfer.kind == ConnectionAttemptKind::Transfer);
  CHECK(transfer.workerNumber == 2);
  CHECK(transfer.transferAttempt == 3);
  CHECK(transfer.endpoint == browser.endpoint);
}

TEST_CASE("remote inspection events preserve request correlation and errors")
{
  using namespace havremote::ui;

  const RemoteInspectionEvent success{
      .requestId = "inspect-success",
      .siteId = "site-a",
      .generation = 7,
      .path = havremote::RemotePath{"/notes.txt"},
      .result = havremote::RemoteEntry{
          .path = havremote::RemotePath{"/notes.txt"},
          .name = havremote::RemotePath{"notes.txt"},
          .kind = havremote::RemoteEntryKind::File,
          .size = 12,
          .modifiedAt = std::nullopt,
          .permissions = std::nullopt,
          .owner = std::nullopt,
          .group = std::nullopt,
          .hidden = false,
      },
  };

  REQUIRE(success.result);
  CHECK(success.requestId == "inspect-success");
  CHECK(success.result->path == success.path);

  const RemoteInspectionEvent missing{
      .requestId = "inspect-missing",
      .siteId = "site-a",
      .generation = 7,
      .path = havremote::RemotePath{"/deleted.txt"},
      .result = std::unexpected(havremote::RemoteError{
          .code = havremote::RemoteErrorCode::NotFound,
          .message = "Not found",
      }),
  };

  REQUIRE_FALSE(missing.result);
  CHECK(missing.result.error().code == havremote::RemoteErrorCode::NotFound);
}

TEST_CASE("transfer runtime globally gates workers and cancels waiters")
{
  havremote::TransferRuntime runtime{1};

  const std::stop_source neverStop;

  auto first = runtime.AcquireTransfer(neverStop.get_token());

  REQUIRE(first.has_value());

  std::stop_source waiterStop;

  std::promise<void> startedPromise;

  auto started = startedPromise.get_future();

  std::promise<bool> resultPromise;

  auto result = resultPromise.get_future();

  std::jthread waiter([&]
                      {
        startedPromise.set_value();
        resultPromise.set_value(
            runtime.AcquireTransfer(waiterStop.get_token()).has_value()); });

  REQUIRE(started.wait_for(1s) == std::future_status::ready);
  CHECK(result.wait_for(100ms) == std::future_status::timeout);

  waiterStop.request_stop();

  REQUIRE(result.wait_for(1s) == std::future_status::ready);
  CHECK_FALSE(result.get());

  first.reset();

  CHECK(runtime.AcquireTransfer(neverStop.get_token()).has_value());
}

TEST_CASE("transfer runtime reservations are shared and released by RAII")
{
  havremote::TransferRuntime runtime{2};

  const auto local = std::filesystem::current_path() / "target" / "file.bin";

  {
    auto first = runtime.ReserveLocalDestination(local);

    REQUIRE(first.has_value());

    const auto duplicate = runtime.ReserveLocalDestination(
        local.parent_path() / "." / local.filename());

    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error().code == havremote::RemoteErrorCode::Conflict);

    const auto caseDistinct = runtime.ReserveLocalDestination(
        local.parent_path() / "FILE.BIN");

    CHECK(caseDistinct.has_value());
  }

  CHECK(runtime.ReserveLocalDestination(local).has_value());

  const havremote::SiteEndpointIdentity endpoint{
      .protocol = havremote::ProtocolKind::Sftp,
      .host = "Example.COM",
      .port = 22,
      .username = "user"};

  auto firstRemote = runtime.ReserveRemoteDestination(
      endpoint, havremote::RemotePath{"/uploads/file.bin"});

  REQUIRE(firstRemote.has_value());

  auto sameEndpoint = endpoint;
  sameEndpoint.host = "example.com";

  const auto duplicateRemote = runtime.ReserveRemoteDestination(
      sameEndpoint, havremote::RemotePath{"/uploads/file.bin"});

  REQUIRE_FALSE(duplicateRemote.has_value());
  CHECK(duplicateRemote.error().code == havremote::RemoteErrorCode::Conflict);

  sameEndpoint.port = 2222;

  CHECK(runtime.ReserveRemoteDestination(
                   sameEndpoint, havremote::RemotePath{"/uploads/file.bin"})
            .has_value());
}

TEST_CASE("transfer runtime delegates local path equivalence to its callback")
{
  const auto local = std::filesystem::current_path() / "target" / "file.bin";
  const auto caseDistinct = local.parent_path() / "FILE.BIN";
  const auto other = local.parent_path() / "other.bin";

  std::size_t checks{};

  havremote::TransferRuntime runtime{
      2,
      [&](const std::filesystem::path &candidate,
          const std::filesystem::path &active) -> havremote::Result<bool>
      {
        ++checks;

        CHECK(candidate.is_absolute());
        CHECK(candidate == candidate.lexically_normal());
        CHECK((candidate == caseDistinct || candidate == other));
        CHECK((active == local || active == caseDistinct));

        return false;
      }};

  {
    auto first = runtime.ReserveLocalDestination(local);

    REQUIRE(first.has_value());
    CHECK(checks == 0);

    const auto duplicate = runtime.ReserveLocalDestination(
        local.parent_path() / "." / local.filename());

    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error().code == havremote::RemoteErrorCode::Conflict);
    CHECK(checks == 0);

    auto second = runtime.ReserveLocalDestination(
        caseDistinct.parent_path() / "." / caseDistinct.filename());

    REQUIRE(second.has_value());
    CHECK(checks == 1);

    auto third = runtime.ReserveLocalDestination(other);

    REQUIRE(third.has_value());
    CHECK(checks == 3);
  }

  CHECK(runtime.ReserveLocalDestination(caseDistinct).has_value());
  CHECK(checks == 3);
}

TEST_CASE("transfer runtime rejects callback aliases without retaining them")
{
  const auto local = std::filesystem::current_path() / "target" / "file.bin";
  const auto alias = local.parent_path() / "FILE.BIN";

  std::size_t checks{};

  havremote::TransferRuntime runtime{
      2,
      [&](const std::filesystem::path &candidate,
          const std::filesystem::path &active) -> havremote::Result<bool>
      {
        ++checks;

        CHECK(candidate == alias);
        CHECK(active == local);

        return true;
      }};

  {
    auto first = runtime.ReserveLocalDestination(local);

    REQUIRE(first.has_value());

    const auto duplicate = runtime.ReserveLocalDestination(alias);

    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error().code == havremote::RemoteErrorCode::Conflict);
    CHECK(checks == 1);
  }

  CHECK(runtime.ReserveLocalDestination(alias).has_value());
  CHECK(runtime.ReserveLocalDestination(local).has_value());
  CHECK(checks == 1);
}

TEST_CASE("transfer runtime propagates local conflict errors without reserving")
{
  const auto local = std::filesystem::current_path() / "target" / "file.bin";
  const auto other = local.parent_path() / "other.bin";

  bool failCheck = true;

  havremote::TransferRuntime runtime{
      2,
      [&](const std::filesystem::path &,
          const std::filesystem::path &) -> havremote::Result<bool>
      {
        if (failCheck)
        {
          return std::unexpected(havremote::RemoteError{
              .code = havremote::RemoteErrorCode::LocalIo,
              .message = "Could not inspect the destination",
              .nativeCode = 123});
        }

        return false;
      }};

  auto first = runtime.ReserveLocalDestination(local);

  REQUIRE(first.has_value());

  const auto failed = runtime.ReserveLocalDestination(other);

  REQUIRE_FALSE(failed.has_value());
  CHECK(failed.error().code == havremote::RemoteErrorCode::LocalIo);
  CHECK(failed.error().message == "Could not inspect the destination");
  CHECK(failed.error().nativeCode == 123);

  failCheck = false;

  auto recovered = runtime.ReserveLocalDestination(other);

  REQUIRE(recovered.has_value());

  const auto duplicate = runtime.ReserveLocalDestination(local);

  REQUIRE_FALSE(duplicate.has_value());
  CHECK(duplicate.error().code == havremote::RemoteErrorCode::Conflict);
}

TEST_CASE("transfer runtime reserves Unicode paths without locale conversion")
{
  havremote::TransferRuntime runtime{2};

  const auto local = std::filesystem::current_path() /
                     std::filesystem::path{u8"\u00e4\u65e5\U0001f4c4.bin"};

  {
    auto first = runtime.ReserveLocalDestination(local);

    REQUIRE(first.has_value());

    const auto duplicate = runtime.ReserveLocalDestination(
        local.parent_path() / "." / local.filename());

    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error().code == havremote::RemoteErrorCode::Conflict);
  }

  CHECK(runtime.ReserveLocalDestination(local).has_value());
}

TEST_CASE("transfer runtime rejects invalid concurrency limits")
{
  CHECK_THROWS_AS(havremote::TransferRuntime{0}, std::invalid_argument);
  CHECK_THROWS_AS(havremote::TransferRuntime{17}, std::invalid_argument);
}
