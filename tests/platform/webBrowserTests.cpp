// SPDX-License-Identifier: MIT

#include "platform/webBrowser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>

using namespace havremote;

TEST_CASE("HTTPS links are passed unchanged to the browser launcher",
          "[platform][browser]")
{
  constexpr std::string_view url =
      "https://github.com/Havoc7891/havRemote/releases/tag/v0.2.0?from=app#notes";

  std::string launched;

  const auto result = platform::LaunchHttpsUrl(
      url,
      [&](const std::string_view candidate)
      {
        launched = candidate;

        return true;
      });

  REQUIRE(result);
  CHECK(launched == url);
}

TEST_CASE("Unsafe browser links are rejected before invoking the launcher",
          "[platform][browser][security]")
{
  const std::string oversized =
      "https://example.test/" + std::string(4096U, 'a');

  const std::array invalid{
      std::string{},
      std::string{"http://github.com/Havoc7891/havRemote"},
      std::string{"HTTPS://github.com/Havoc7891/havRemote"},
      std::string{"https://"},
      std::string{"https:///releases/latest"},
      std::string{"https://?release=latest"},
      std::string{"https://user@github.com/Havoc7891/havRemote"},
      std::string{"https://github.com/Havoc7891/hav Remote"},
      std::string{"https://github.com\\Havoc7891\\havRemote"},
      std::string{"https://github.com/Havoc7891/havRemote\nnext"},
      oversized,
  };

  for (const auto &url : invalid)
  {
    CAPTURE(url);

    bool called{};

    const auto result = platform::LaunchHttpsUrl(
        url,
        [&](std::string_view)
        {
          called = true;

          return true;
        });

    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          platform::PlatformErrorCode::InvalidArgument);
    CHECK_FALSE(result.error().message.empty());
    CHECK_FALSE(called);
  }
}

TEST_CASE("A browser launch failure is reported as unavailable",
          "[platform][browser]")
{
  bool called{};

  const auto result = platform::LaunchHttpsUrl(
      "https://github.com/Havoc7891/havRemote/releases/latest",
      [&](std::string_view)
      {
        called = true;

        return false;
      });

  REQUIRE(called);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == platform::PlatformErrorCode::Unavailable);
  CHECK(result.error().nativeCode == 0);
  CHECK(result.error().message ==
        "The link could not be opened in the default browser");
}
