// SPDX-License-Identifier: MIT

#include "platform/helpLauncher.hpp"

#include "core/types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using namespace havremote;

namespace
{
  class TemporaryHelpDirectory final
  {
  public:
    TemporaryHelpDirectory()
        : mPath(std::filesystem::temp_directory_path() /
                ("havremote-help-" + GenerateId()))
    {
      std::filesystem::create_directories(mPath / "en");

      std::ofstream{mPath / "en" / "index.html"} << "<!doctype html>";
    }

    ~TemporaryHelpDirectory()
    {
      std::error_code ignored;
      std::filesystem::remove_all(mPath, ignored);
    }

    TemporaryHelpDirectory(const TemporaryHelpDirectory &) = delete;
    TemporaryHelpDirectory &operator=(const TemporaryHelpDirectory &) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const noexcept
    {
      return mPath;
    }

  private:
    std::filesystem::path mPath;
  };
} // namespace

TEST_CASE("Offline help selects and normalizes the requested language",
          "[platform][help]")
{
  std::vector<std::filesystem::path> inspected;

  platform::HelpLaunchFunctions functions;

  functions.isRegularFile =
      [&](const std::filesystem::path &path, std::error_code &)
  {
    inspected.push_back(path);

    return path == std::filesystem::path{"help/de-de/index.html"};
  };

  const auto result =
      platform::ResolveHelpIndex("help", "DE-de", functions);

  REQUIRE(result);
  CHECK(*result == std::filesystem::path{"help/de-de/index.html"});
  REQUIRE(inspected.size() == 1);
  CHECK(inspected.front() == *result);
}

TEST_CASE("Offline help falls back to English when a language is unavailable",
          "[platform][help]")
{
  std::vector<std::filesystem::path> inspected;

  platform::HelpLaunchFunctions functions;

  functions.isRegularFile =
      [&](const std::filesystem::path &path, std::error_code &)
  {
    inspected.push_back(path);

    return path == std::filesystem::path{"help/en/index.html"};
  };

  const auto result = platform::ResolveHelpIndex("help", "de", functions);

  REQUIRE(result);
  CHECK(*result == std::filesystem::path{"help/en/index.html"});
  REQUIRE(inspected.size() == 2);
  CHECK(inspected[0] == std::filesystem::path{"help/de/index.html"});
  CHECK(inspected[1] == *result);
}

TEST_CASE("Offline help falls back through the production filesystem",
          "[platform][help]")
{
  TemporaryHelpDirectory help;

  const auto result = platform::ResolveHelpIndex(help.Path(), "de");

  REQUIRE(result);
  CHECK(*result == help.Path() / "en" / "index.html");
}

TEST_CASE("English offline help is inspected only once when it is missing",
          "[platform][help]")
{
  std::size_t inspections{};

  platform::HelpLaunchFunctions functions;
  functions.isRegularFile =
      [&](const std::filesystem::path &, std::error_code &)
  {
    ++inspections;

    return false;
  };

  const auto result = platform::ResolveHelpIndex("help", "EN", functions);

  REQUIRE_FALSE(result);
  CHECK(result.error().code == platform::PlatformErrorCode::NotFound);
  CHECK(inspections == 1);
}

TEST_CASE("Offline help rejects unsafe language codes before inspecting files",
          "[platform][help]")
{
  const std::vector<std::string> unsafeCodes{
      "",
      "-en",
      "en-",
      "en--us",
      "../en",
      "en/us",
      "en_us",
      "d\xC3\xA9",
      std::string(36, 'a'),
  };

  for (const auto &code : unsafeCodes)
  {
    CAPTURE(code);

    bool inspected{};

    platform::HelpLaunchFunctions functions;
    functions.isRegularFile =
        [&](const std::filesystem::path &, std::error_code &)
    {
      inspected = true;

      return true;
    };

    const auto result = platform::ResolveHelpIndex("help", code, functions);

    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          platform::PlatformErrorCode::InvalidArgument);
    CHECK_FALSE(inspected);
  }
}

TEST_CASE("Offline help accepts ASCII alphanumeric language subtags",
          "[platform][help]")
{
  platform::HelpLaunchFunctions functions;
  functions.isRegularFile =
      [](const std::filesystem::path &, std::error_code &)
  { return true; };

  const auto result = platform::ResolveHelpIndex("help", "ZH-Hant-123", functions);

  REQUIRE(result);
  CHECK(*result == std::filesystem::path{"help/zh-hant-123/index.html"});
}

TEST_CASE("Offline help reports filesystem inspection failures",
          "[platform][help]")
{
  platform::HelpLaunchFunctions functions;
  functions.isRegularFile =
      [](const std::filesystem::path &, std::error_code &error)
  {
    error = std::make_error_code(std::errc::permission_denied);

    return false;
  };

  const auto result = platform::ResolveHelpIndex("help", "de", functions);

  REQUIRE_FALSE(result);
  CHECK(result.error().code == platform::PlatformErrorCode::AccessDenied);
  CHECK(result.error().nativeCode != 0);
}

TEST_CASE("Offline help launches the resolved local file",
          "[platform][help]")
{
  platform::HelpLaunchFunctions functions;
  functions.isRegularFile =
      [](const std::filesystem::path &, std::error_code &)
  { return true; };

  std::filesystem::path launched;

  functions.launchBrowser = [&](const std::filesystem::path &path)
  {
    launched = path;

    return true;
  };

  const auto result = platform::LaunchOfflineHelp("help", "DE", functions);

  REQUIRE(result);
  CHECK(launched == std::filesystem::path{"help/de/index.html"});
}

TEST_CASE("Offline help distinguishes browser launch failures",
          "[platform][help]")
{
  platform::HelpLaunchFunctions functions;
  functions.isRegularFile =
      [](const std::filesystem::path &, std::error_code &)
  { return true; };
  functions.launchBrowser =
      [](const std::filesystem::path &)
  { return false; };

  const auto result = platform::LaunchOfflineHelp("help", "en", functions);

  REQUIRE_FALSE(result);
  CHECK(result.error().code == platform::PlatformErrorCode::Unavailable);
}

TEST_CASE("Offline help preserves Unicode installation paths",
          "[platform][help]")
{
  const auto root = std::filesystem::path{u8"Grüße 猫/havRemote help"};

  platform::HelpLaunchFunctions functions;
  functions.isRegularFile =
      [](const std::filesystem::path &, std::error_code &)
  { return true; };

  std::filesystem::path launched;

  functions.launchBrowser = [&](const std::filesystem::path &path)
  {
    launched = path;

    return true;
  };

  const auto result = platform::LaunchOfflineHelp(root, "de", functions);

  REQUIRE(result);
  CHECK(launched == root / "de" / "index.html");
}
