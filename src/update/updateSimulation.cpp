// SPDX-License-Identifier: MIT

#include "update/updateSimulation.hpp"

#if defined(HAVREMOTE_UPDATE_SIMULATION)

#include <algorithm>
#include <array>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <utility>

namespace havremote::updates
{
  namespace
  {
    constexpr std::array Scenarios{
        std::pair{std::string_view{"available"},
                  UpdateSimulationScenario::Available},
        std::pair{std::string_view{"up-to-date"},
                  UpdateSimulationScenario::UpToDate},
        std::pair{std::string_view{"no-release"},
                  UpdateSimulationScenario::NoRelease},
        std::pair{std::string_view{"network-error"},
                  UpdateSimulationScenario::NetworkError},
        std::pair{std::string_view{"server-error"},
                  UpdateSimulationScenario::ServerError},
    };

    UpdateError SimulationError(UpdateErrorKind kind, std::string message)
    {
      return UpdateError{
          .kind = kind,
          .message = std::move(message),
          .httpStatus = std::nullopt,
          .curlCode = std::nullopt,
      };
    }

    std::expected<Version, UpdateError> NextVersion(Version current)
    {
      constexpr auto Maximum = std::numeric_limits<std::uint32_t>::max();

      if (current.patch < Maximum)
      {
        ++current.patch;
      }
      else if (current.minor < Maximum)
      {
        ++current.minor;

        current.patch = 0;
      }
      else if (current.major < Maximum)
      {
        ++current.major;

        current.minor = 0;
        current.patch = 0;
      }
      else
      {
        return std::unexpected(SimulationError(
            UpdateErrorKind::Initialization,
            "The update simulation cannot represent a newer release version"));
      }

      return current;
    }

    std::string SimulatedReleaseJson(const GitHubUpdateSource &source,
                                     const Version &version)
    {
      const auto canonicalVersion = ReleaseVersionText(version);
      const auto tag = "v" + canonicalVersion;
      const auto repositoryUrl =
          "https://github.com/" + source.owner + '/' + source.repository;
      const auto asset = RuntimeReleaseAssetName(version, source.target);
      const auto assets = asset
                              ? "{\"name\":\"" + *asset +
                                    "\",\"browser_download_url\":\"" + repositoryUrl +
                                    "/releases/download/" + tag + '/' + *asset +
                                    "\",\"size\":4096,\"digest\":null}"
                              : std::string{};

      // The real service validates owner/repository before invoking this
      // transport. Their permitted characters need no JSON escaping.
      return "{\"tag_name\":\"" + tag +
             "\",\"html_url\":\"" + repositoryUrl + "/releases/tag/" + tag +
             "\",\"name\":\"Simulated havRemote " + canonicalVersion +
             "\",\"body\":\"DEBUG SIMULATION: This release is fictional. "
             "No request was sent to GitHub and no download is available.\"," +
             "\"assets\":[" + assets + "]}";
    }
  } // namespace

  std::expected<UpdateSimulationScenario, std::string>
  ParseUpdateSimulationScenario(const std::string_view name)
  {
    for (const auto &[candidate, scenario] : Scenarios)
    {
      if (name == candidate)
      {
        return scenario;
      }
    }

    return std::unexpected(
        "Unknown update simulation scenario. Expected available, up-to-date, "
        "no-release, network-error, or server-error.");
  }

  std::string_view UpdateSimulationScenarioName(
      const UpdateSimulationScenario scenario)
  {
    for (const auto &[name, candidate] : Scenarios)
    {
      if (scenario == candidate)
      {
        return name;
      }
    }

    return "unknown";
  }

  std::unique_ptr<IUpdateService> MakeSimulatedUpdateService(
      const UpdateSimulationScenario scenario,
      GitHubUpdateSource source,
      const std::chrono::milliseconds delay)
  {
    const auto boundedDelay = std::clamp(
        delay, std::chrono::milliseconds{0}, std::chrono::milliseconds{2000});

    UpdateTransport transport =
        [scenario, source, boundedDelay](std::string_view,
                                         std::string_view,
                                         const std::stop_token stopToken)
        -> std::expected<UpdateHttpResponse, UpdateError>
    {
      if (!stopToken.stop_requested() && boundedDelay.count() > 0)
      {
        std::mutex mutex;
        std::condition_variable_any ready;
        std::unique_lock lock{mutex};

        ready.wait_for(lock, stopToken, boundedDelay, []
                       { return false; });
      }

      if (stopToken.stop_requested())
      {
        return std::unexpected(SimulationError(
            UpdateErrorKind::Cancelled, "The update check was cancelled"));
      }

      switch (scenario)
      {
      case UpdateSimulationScenario::Available:
      {
        const auto next = NextVersion(source.currentVersion);
        if (!next)
        {
          return std::unexpected(next.error());
        }

        return UpdateHttpResponse{
            .status = 200,
            .body = SimulatedReleaseJson(source, *next),
        };
      }

      case UpdateSimulationScenario::UpToDate:
        return UpdateHttpResponse{
            .status = 200,
            .body = SimulatedReleaseJson(source, source.currentVersion),
        };

      case UpdateSimulationScenario::NoRelease:
        return UpdateHttpResponse{.status = 404, .body = "{}"};

      case UpdateSimulationScenario::NetworkError:
      {
        auto error = SimulationError(
            UpdateErrorKind::Network,
            "GitHub request failed: simulated connection failure "
            "(no network request was made)");
        error.curlCode = 7; // CURLE_COULDNT_CONNECT

        return std::unexpected(std::move(error));
      }

      case UpdateSimulationScenario::ServerError:
        return UpdateHttpResponse{.status = 500, .body = "{}"};
      }

      return std::unexpected(SimulationError(
          UpdateErrorKind::Initialization,
          "The update simulation scenario is invalid"));
    };

    return MakeGitHubUpdateService(std::move(source), std::move(transport));
  }
} // namespace havremote::updates

#endif // defined(HAVREMOTE_UPDATE_SIMULATION)
