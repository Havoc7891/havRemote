// SPDX-License-Identifier: MIT

#include "ui/localDirectoryEvents.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

TEST_CASE("local directory results distinguish identical loader generations")
{
  using namespace havremote::ui;

  const auto makeResult = [](std::string connectionId)
  {
    return LocalDirectoryResultPtr{
        std::make_shared<const LocalDirectoryResult>(LocalDirectoryResult{
            .connectionId = std::move(connectionId),
            .generation = 1,
            .directory = "C:/same-directory",
            .entries = {},
            .warnings = {},
            .error = std::nullopt,
        })};
  };

  const auto first = makeResult("connection-a");
  const auto second = makeResult("connection-b");

  STATIC_REQUIRE(std::is_const_v<LocalDirectoryResultPtr::element_type>);
  REQUIRE(first);
  REQUIRE(second);
  CHECK(first->generation == second->generation);
  CHECK(first->directory == second->directory);
  CHECK(first->connectionId != second->connectionId);
}
