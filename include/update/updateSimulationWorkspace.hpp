// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UPDATE_UPDATE_SIMULATION_WORKSPACE_HPP
#define HAVREMOTE_INCLUDE_UPDATE_UPDATE_SIMULATION_WORKSPACE_HPP

#if defined(HAVREMOTE_UPDATE_SIMULATION)

#include "platform/credentialStore.hpp"
#include "update/updateSimulation.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace havremote::updates
{
  // Simulation settings persist separately for each scenario. Existing state
  // is never copied, reset, or migrated from the normal application directory.
  [[nodiscard]] std::expected<std::filesystem::path, std::string>
  PrepareUpdateSimulationWorkspace(
      const std::filesystem::path &executableDirectory,
      UpdateSimulationScenario scenario);

  // Simulation must never access the user's actual stored credentials.
  [[nodiscard]] std::unique_ptr<platform::ICredentialStore>
  MakeUpdateSimulationCredentialStore();
} // namespace havremote::updates

#endif // HAVREMOTE_UPDATE_SIMULATION

#endif // HAVREMOTE_INCLUDE_UPDATE_UPDATE_SIMULATION_WORKSPACE_HPP
