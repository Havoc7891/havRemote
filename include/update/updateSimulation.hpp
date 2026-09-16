// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UPDATE_UPDATE_SIMULATION_HPP
#define HAVREMOTE_INCLUDE_UPDATE_UPDATE_SIMULATION_HPP

#if defined(HAVREMOTE_UPDATE_SIMULATION)

#include "update/updateService.hpp"

#include <chrono>

namespace havremote::updates
{
  enum class UpdateSimulationScenario
  {
    Available,
    UpToDate,
    NoRelease,
    NetworkError,
    ServerError,
  };

  [[nodiscard]] std::expected<UpdateSimulationScenario, std::string>
  ParseUpdateSimulationScenario(std::string_view name);

  [[nodiscard]] std::string_view UpdateSimulationScenarioName(
      UpdateSimulationScenario scenario);

  // Debug-only, offline transport injection. The production release parser,
  // repository validation, and runtime-asset URL checks remain in use.
  // Optional response latency is limited to two seconds and is cancellable.
  [[nodiscard]] std::unique_ptr<IUpdateService> MakeSimulatedUpdateService(
      UpdateSimulationScenario scenario,
      GitHubUpdateSource source,
      std::chrono::milliseconds delay = std::chrono::milliseconds{0});
} // namespace havremote::updates

#endif // defined(HAVREMOTE_UPDATE_SIMULATION)

#endif // HAVREMOTE_INCLUDE_UPDATE_UPDATE_SIMULATION_HPP
