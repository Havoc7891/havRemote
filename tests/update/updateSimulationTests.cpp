// SPDX-License-Identifier: MIT

#include "update/updateSimulation.hpp"

#if defined(HAVREMOTE_UPDATE_SIMULATION)

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <limits>
#include <thread>
#include <utility>

using namespace havremote::updates;
using namespace std::chrono_literals;

namespace
{
  GitHubUpdateSource SimulationSource(
      const Version current = {.major = 0, .minor = 1, .patch = 0})
  {
    return GitHubUpdateSource{
        .owner = "Example-Owner",
        .repository = "havRemote-test",
        .currentVersion = current,
        .target = {ReleasePlatform::Windows, ReleaseArchitecture::X86_64},
    };
  }
} // namespace

TEST_CASE("Update simulation scenario names parse strictly and round trip",
          "[update][simulation]")
{
  constexpr std::array scenarios{
      std::pair{"available", UpdateSimulationScenario::Available},
      std::pair{"up-to-date", UpdateSimulationScenario::UpToDate},
      std::pair{"no-release", UpdateSimulationScenario::NoRelease},
      std::pair{"network-error", UpdateSimulationScenario::NetworkError},
      std::pair{"server-error", UpdateSimulationScenario::ServerError},
  };

  for (const auto &[name, expected] : scenarios)
  {
    CAPTURE(name);

    const auto parsed = ParseUpdateSimulationScenario(name);

    REQUIRE(parsed);
    CHECK(*parsed == expected);
    CHECK(UpdateSimulationScenarioName(*parsed) == name);
  }

  for (const std::string_view invalid : {
           "", "Available", "AVAILABLE", " available", "available ",
           "up_to_date", "upToDate", "latest", "no-release\n"})
  {
    CAPTURE(invalid);

    const auto parsed = ParseUpdateSimulationScenario(invalid);

    REQUIRE_FALSE(parsed);
    CHECK_FALSE(parsed.error().empty());
  }
}

TEST_CASE("Available update simulation uses real runtime metadata validation",
          "[update][simulation]")
{
  const auto source = SimulationSource();
  const auto service = MakeSimulatedUpdateService(
      UpdateSimulationScenario::Available, source);
  const auto result = service->CheckLatest({});

  REQUIRE(result);
  CHECK(result->disposition == UpdateDisposition::Available);
  REQUIRE(result->release);

  const auto &release = *result->release;

  CHECK((release.version == Version{.major = 0, .minor = 1, .patch = 1}));
  CHECK(release.version > source.currentVersion);
  CHECK(release.canonicalVersion == "0.1.1");
  CHECK(release.tagName == "v0.1.1");
  CHECK(release.releasePageUrl ==
        "https://github.com/Example-Owner/havRemote-test/releases/tag/v0.1.1");
  REQUIRE(release.runtimeAsset);
  CHECK(release.runtimeAsset->name == "havRemote-0.1.1-windows-x86_64.zip");
  CHECK(release.runtimeAsset->downloadUrl ==
        "https://github.com/Example-Owner/havRemote-test/releases/download/"
        "v0.1.1/havRemote-0.1.1-windows-x86_64.zip");
  CHECK(release.runtimeAsset->size > 0U);
  CHECK_FALSE(release.runtimeAsset->sha256);
  CHECK(release.title.starts_with("Simulated "));
  CHECK(release.notes.find("DEBUG SIMULATION") != std::string::npos);
  CHECK(release.notes.find("No request was sent to GitHub") !=
        std::string::npos);
}

TEST_CASE("Up-to-date update simulation reports the current version",
          "[update][simulation]")
{
  for (const auto current : {
           Version{.major = 0, .minor = 1, .patch = 0},
           Version{.major = 2, .minor = 14, .patch = 39},
           Version{.major = 0, .minor = 0, .patch = 0}})
  {
    CAPTURE(ReleaseVersionText(current));

    const auto service = MakeSimulatedUpdateService(
        UpdateSimulationScenario::UpToDate, SimulationSource(current));
    const auto result = service->CheckLatest({});

    REQUIRE(result);
    CHECK(result->disposition == UpdateDisposition::UpToDate);
    CHECK_FALSE(result->release);
  }
}

TEST_CASE("Update simulation passes HTTP failures through the real service",
          "[update][simulation]")
{
  const std::array scenarios{
      std::pair{UpdateSimulationScenario::NoRelease, 404L},
      std::pair{UpdateSimulationScenario::ServerError, 500L},
  };

  for (const auto &[scenario, status] : scenarios)
  {
    CAPTURE(UpdateSimulationScenarioName(scenario));

    const auto service = MakeSimulatedUpdateService(scenario,
                                                    SimulationSource());
    const auto result = service->CheckLatest({});

    REQUIRE_FALSE(result);
    CHECK(result.error().kind == UpdateErrorKind::HttpStatus);
    CHECK(result.error().httpStatus == status);
    CHECK_FALSE(result.error().curlCode);

    if (status == 404)
    {
      CHECK(result.error().message ==
            "GitHub has no published stable havRemote release");
    }
    else
    {
      CHECK(result.error().message == "GitHub returned HTTP status 500");
    }
  }
}

TEST_CASE("Update simulation reports an offline fabricated network error",
          "[update][simulation]")
{
  const auto service = MakeSimulatedUpdateService(
      UpdateSimulationScenario::NetworkError, SimulationSource());
  const auto result = service->CheckLatest({});

  REQUIRE_FALSE(result);
  CHECK(result.error().kind == UpdateErrorKind::Network);
  CHECK(result.error().curlCode == 7);
  CHECK_FALSE(result.error().httpStatus);
  CHECK(result.error().message.find("simulated connection failure") !=
        std::string::npos);
}

TEST_CASE("Update simulation retains repository validation",
          "[update][simulation][security]")
{
  for (const std::string_view invalid : {
           "", "owner/path", "https://example.test", "owner\"", "owner name"})
  {
    for (const bool invalidOwner : {false, true})
    {
      CAPTURE(invalid, invalidOwner);

      auto source = SimulationSource();

      (invalidOwner ? source.owner : source.repository) = invalid;

      const auto service = MakeSimulatedUpdateService(
          UpdateSimulationScenario::Available, std::move(source));

      const auto result = service->CheckLatest({});

      REQUIRE_FALSE(result);
      CHECK(result.error().kind == UpdateErrorKind::Initialization);
      CHECK(result.error().message ==
            "The configured GitHub repository identifier is invalid");
    }
  }
}

TEST_CASE("Update simulation derives the release from the supplied source",
          "[update][simulation]")
{
  const auto service = MakeSimulatedUpdateService(
      UpdateSimulationScenario::Available,
      GitHubUpdateSource{
          .owner = "Another.Owner",
          .repository = "client_test",
          .currentVersion = {.major = 2, .minor = 14, .patch = 39},
          .target = {ReleasePlatform::Windows, ReleaseArchitecture::X86_64},
      });

  const auto result = service->CheckLatest({});

  REQUIRE(result);
  REQUIRE(result->release);
  CHECK(result->release->canonicalVersion == "2.14.40");
  CHECK(result->release->releasePageUrl ==
        "https://github.com/Another.Owner/client_test/releases/tag/v2.14.40");
  REQUIRE(result->release->runtimeAsset);
  CHECK(result->release->runtimeAsset->downloadUrl ==
        "https://github.com/Another.Owner/client_test/releases/download/"
        "v2.14.40/havRemote-2.14.40-windows-x86_64.zip");
}

TEST_CASE("Update simulation uses the requested native package target",
          "[update][simulation][platform]")
{
  for (const auto target : {
           ReleaseTarget{ReleasePlatform::Linux, ReleaseArchitecture::X86_64},
           ReleaseTarget{ReleasePlatform::MacOS, ReleaseArchitecture::Arm64},
           ReleaseTarget{}})
  {
    auto source = SimulationSource();
    source.target = target;

    const auto service = MakeSimulatedUpdateService(
        UpdateSimulationScenario::Available, source);

    const auto result = service->CheckLatest({});

    REQUIRE(result);
    REQUIRE(result->release);

    const auto expectedAsset = RuntimeReleaseAssetName({0, 1, 1}, target);

    CHECK(result->release->runtimeAsset.has_value() == expectedAsset.has_value());

    if (expectedAsset)
    {
      REQUIRE(result->release->runtimeAsset);
      CHECK(result->release->runtimeAsset->name == *expectedAsset);
    }

    CHECK(result->release->releasePageUrl ==
          "https://github.com/Example-Owner/havRemote-test/releases/tag/v0.1.1");
  }
}

TEST_CASE("Update simulation never wraps version components",
          "[update][simulation]")
{
  constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();

  const std::array versions{
      std::pair{Version{.major = 2, .minor = 14, .patch = maximum},
                Version{.major = 2, .minor = 15, .patch = 0}},
      std::pair{Version{.major = 2, .minor = maximum, .patch = maximum},
                Version{.major = 3, .minor = 0, .patch = 0}},
  };

  for (const auto &[current, expected] : versions)
  {
    CAPTURE(ReleaseVersionText(current));

    const auto service = MakeSimulatedUpdateService(
        UpdateSimulationScenario::Available, SimulationSource(current));
    const auto result = service->CheckLatest({});

    REQUIRE(result);
    REQUIRE(result->release);
    CHECK(result->release->version == expected);
  }

  const auto service = MakeSimulatedUpdateService(
      UpdateSimulationScenario::Available,
      SimulationSource({.major = maximum, .minor = maximum, .patch = maximum}));

  const auto result = service->CheckLatest({});

  REQUIRE_FALSE(result);
  CHECK(result.error().kind == UpdateErrorKind::Initialization);
  CHECK(result.error().message.find("cannot represent a newer release") !=
        std::string::npos);
}

TEST_CASE("Update simulation honors cancellation before every scenario",
          "[update][simulation]")
{
  std::stop_source stop;
  stop.request_stop();

  for (const auto scenario : {
           UpdateSimulationScenario::Available,
           UpdateSimulationScenario::UpToDate,
           UpdateSimulationScenario::NoRelease,
           UpdateSimulationScenario::NetworkError,
           UpdateSimulationScenario::ServerError})
  {
    CAPTURE(UpdateSimulationScenarioName(scenario));

    const auto service = MakeSimulatedUpdateService(scenario,
                                                    SimulationSource(), 2s);
    const auto result = service->CheckLatest(stop.get_token());

    REQUIRE_FALSE(result);
    CHECK(result.error().kind == UpdateErrorKind::Cancelled);
  }
}

TEST_CASE("Update simulation latency remains cancellable",
          "[update][simulation]")
{
  const auto service = MakeSimulatedUpdateService(
      UpdateSimulationScenario::Available, SimulationSource(), 2s);

  std::stop_source stop;
  std::jthread canceller{[&stop]
                         {
                           std::this_thread::sleep_for(20ms);
                           stop.request_stop();
                         }};

  const auto result = service->CheckLatest(stop.get_token());

  REQUIRE_FALSE(result);
  CHECK(result.error().kind == UpdateErrorKind::Cancelled);
}

TEST_CASE("Negative update simulation latency is treated as zero",
          "[update][simulation]")
{
  const auto service = MakeSimulatedUpdateService(
      UpdateSimulationScenario::UpToDate, SimulationSource(), -1ms);

  const auto result = service->CheckLatest({});

  REQUIRE(result);
  CHECK(result->disposition == UpdateDisposition::UpToDate);
}

#endif // defined(HAVREMOTE_UPDATE_SIMULATION)
