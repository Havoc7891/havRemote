// SPDX-License-Identifier: MIT

#include "update/updateSimulationWorkspace.hpp"

#if defined(HAVREMOTE_UPDATE_SIMULATION)

#include "core/types.hpp"

#include <catch2/catch_test_macros.hpp>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

using namespace havremote;

namespace
{
  class SimulationDirectory final
  {
  public:
    SimulationDirectory()
        : path{std::filesystem::temp_directory_path() / ("havremote-simulation-" + GenerateId())}
    {
      REQUIRE(std::filesystem::create_directory(path));
    }

    ~SimulationDirectory()
    {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }

    const std::filesystem::path path;
  };

  void WriteSimulationFile(const std::filesystem::path &path, const std::string_view text)
  {
    std::ofstream stream{path, std::ios::binary};

    REQUIRE(stream.is_open());

    stream.write(text.data(), static_cast<std::streamsize>(text.size()));

    REQUIRE(stream.good());
  }

  std::string ReadSimulationFile(const std::filesystem::path &path)
  {
    std::ifstream stream{path, std::ios::binary};

    REQUIRE(stream.is_open());

    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
  }

  constexpr std::array<const char *, 3> StateFilenames{
      "havRemote.cson", "queue.cson", "known_hosts"};
} // namespace

TEST_CASE("update simulation workspaces are isolated by scenario and preserve state",
          "[update][simulation][workspace]")
{
  const SimulationDirectory executable;

  // Even files alongside the executable are not imported into simulation
  WriteSimulationFile(executable.path / "havRemote.cson", "unrelated settings");

  const auto available = updates::PrepareUpdateSimulationWorkspace(
      executable.path, updates::UpdateSimulationScenario::Available);

  REQUIRE(available);
  CHECK(*available == executable.path / "update-simulation" / "available");
  CHECK(std::filesystem::is_empty(*available));

  for (const auto *filename : StateFilenames)
  {
    WriteSimulationFile(*available / filename, "# retained comment\nexisting state\n");
  }

  const auto repeated = updates::PrepareUpdateSimulationWorkspace(
      executable.path, updates::UpdateSimulationScenario::Available);

  REQUIRE(repeated);
  CHECK(*repeated == *available);

  for (const auto *filename : StateFilenames)
  {
    CHECK(ReadSimulationFile(*repeated / filename) == "# retained comment\nexisting state\n");
  }

  CHECK(std::distance(std::filesystem::directory_iterator{*repeated},
                      std::filesystem::directory_iterator{}) == 3);

  const auto upToDate = updates::PrepareUpdateSimulationWorkspace(
      executable.path, updates::UpdateSimulationScenario::UpToDate);

  REQUIRE(upToDate);
  CHECK(*upToDate == executable.path / "update-simulation" / "up-to-date");
  CHECK(std::filesystem::is_empty(*upToDate));
  CHECK(ReadSimulationFile(executable.path / "havRemote.cson") == "unrelated settings");
}

TEST_CASE("update simulation rejects invalid executable directories and scenarios",
          "[update][simulation][workspace]")
{
  const SimulationDirectory executable;

  constexpr auto scenario = updates::UpdateSimulationScenario::Available;

  CHECK_FALSE(updates::PrepareUpdateSimulationWorkspace({}, scenario));
  CHECK_FALSE(updates::PrepareUpdateSimulationWorkspace("relative", scenario));
  CHECK_FALSE(updates::PrepareUpdateSimulationWorkspace(executable.path / "missing", scenario));
  CHECK_FALSE(updates::PrepareUpdateSimulationWorkspace(executable.path / "..", scenario));
  CHECK_FALSE(updates::PrepareUpdateSimulationWorkspace(
      executable.path, static_cast<updates::UpdateSimulationScenario>(-1)));

  auto native = executable.path.native();
  native.push_back(std::filesystem::path::value_type{});
  native += std::filesystem::path{"suffix"}.native();

  CHECK_FALSE(updates::PrepareUpdateSimulationWorkspace(std::filesystem::path{native}, scenario));
  CHECK(std::filesystem::is_empty(executable.path));
}

TEST_CASE("update simulation rejects non-directory workspaces and non-file state",
          "[update][simulation][workspace]")
{
  const SimulationDirectory executable;

  const auto root = executable.path / "update-simulation";

  constexpr auto scenario = updates::UpdateSimulationScenario::Available;

  SECTION("workspace root is a file")
  {
    WriteSimulationFile(root, "untouched");

    CHECK_FALSE(updates::PrepareUpdateSimulationWorkspace(executable.path, scenario));
    CHECK(ReadSimulationFile(root) == "untouched");
  }
  SECTION("scenario directory is a file")
  {
    REQUIRE(std::filesystem::create_directory(root));

    WriteSimulationFile(root / "available", "untouched");

    CHECK_FALSE(updates::PrepareUpdateSimulationWorkspace(executable.path, scenario));
    CHECK(ReadSimulationFile(root / "available") == "untouched");
  }
  SECTION("state target is a directory")
  {
    const auto workspace = updates::PrepareUpdateSimulationWorkspace(executable.path, scenario);

    REQUIRE(workspace);

    for (const auto *filename : StateFilenames)
    {
      REQUIRE(std::filesystem::create_directory(*workspace / filename));
      CHECK_FALSE(updates::PrepareUpdateSimulationWorkspace(executable.path, scenario));
      REQUIRE(std::filesystem::remove(*workspace / filename));
    }
  }
}

TEST_CASE("update simulation refuses state files with hard-link aliases",
          "[update][simulation][workspace]")
{
  const SimulationDirectory executable;

  constexpr auto scenario = updates::UpdateSimulationScenario::Available;

  const auto workspace = updates::PrepareUpdateSimulationWorkspace(executable.path, scenario);

  REQUIRE(workspace);

  const auto original = executable.path / "original.cson";

  WriteSimulationFile(original, "preserve normal settings");

  for (const auto *filename : StateFilenames)
  {
    const auto alias = *workspace / filename;

    std::error_code error;

    std::filesystem::create_hard_link(original, alias, error);

    if (error)
    {
      SKIP("Creating hard links is unavailable: " + error.message());
    }

    CHECK_FALSE(updates::PrepareUpdateSimulationWorkspace(executable.path, scenario));
    CHECK(ReadSimulationFile(original) == "preserve normal settings");
    REQUIRE(std::filesystem::remove(alias));
  }
}

TEST_CASE("update simulation refuses symbolic-link state files including dangling links",
          "[update][simulation][workspace]")
{
  const SimulationDirectory executable;

  constexpr auto scenario = updates::UpdateSimulationScenario::Available;

  const auto workspace = updates::PrepareUpdateSimulationWorkspace(executable.path, scenario);

  REQUIRE(workspace);

  const auto original = executable.path / "original.cson";

  SECTION("existing target")
  {
    WriteSimulationFile(original, "preserve normal settings");
  }
  SECTION("absent target") {}

  for (const auto *filename : StateFilenames)
  {
    const auto alias = *workspace / filename;

    std::error_code error;

    std::filesystem::create_symlink(original, alias, error);

    if (error)
    {
      SKIP("Creating symbolic links is unavailable: " + error.message());
    }

    CHECK_FALSE(updates::PrepareUpdateSimulationWorkspace(executable.path, scenario));
    REQUIRE(std::filesystem::is_symlink(std::filesystem::symlink_status(alias)));
    REQUIRE(std::filesystem::remove(alias));
  }

  if (std::filesystem::exists(original))
  {
    CHECK(ReadSimulationFile(original) == "preserve normal settings");
  }
}

TEST_CASE("update simulation refuses redirected workspace directories",
          "[update][simulation][workspace]")
{
  const SimulationDirectory executable;

  const SimulationDirectory unrelated;

  const auto root = executable.path / "update-simulation";

  std::filesystem::path link;

  SECTION("workspace root") { link = root; }
  SECTION("scenario directory")
  {
    REQUIRE(std::filesystem::create_directory(root));

    link = root / "available";
  }
  SECTION("executable directory") { link = executable.path / "linked-executable"; }

  std::error_code error;

  std::filesystem::create_directory_symlink(unrelated.path, link, error);

  if (error)
  {
    SKIP("Creating directory links is unavailable: " + error.message());
  }

  const auto base = link.filename() == "linked-executable" ? link : executable.path;

  CHECK_FALSE(updates::PrepareUpdateSimulationWorkspace(
      base, updates::UpdateSimulationScenario::Available));
  CHECK(std::filesystem::is_empty(unrelated.path));
}

TEST_CASE("update simulation refuses read-only existing state without resetting it",
          "[update][simulation][workspace]")
{
  const SimulationDirectory executable;

  constexpr auto scenario = updates::UpdateSimulationScenario::Available;

  const auto workspace = updates::PrepareUpdateSimulationWorkspace(executable.path, scenario);

  REQUIRE(workspace);

  for (const auto *filename : StateFilenames)
  {
    const auto file = *workspace / filename;

    WriteSimulationFile(file, "retain read-only settings");

#if defined(_WIN32)
    const auto attributes = GetFileAttributesW(file.c_str());

    REQUIRE(attributes != INVALID_FILE_ATTRIBUTES);
    REQUIRE(SetFileAttributesW(file.c_str(), attributes | FILE_ATTRIBUTE_READONLY));

    const auto result = updates::PrepareUpdateSimulationWorkspace(executable.path, scenario);

    REQUIRE(SetFileAttributesW(file.c_str(), attributes));
#else
    const auto permissions = std::filesystem::status(file).permissions();

    std::filesystem::permissions(file, std::filesystem::perms::owner_read);

    const auto result = updates::PrepareUpdateSimulationWorkspace(executable.path, scenario);

    std::filesystem::permissions(file, permissions);
#endif
    CHECK_FALSE(result);
    CHECK(ReadSimulationFile(file) == "retain read-only settings");
  }
}

TEST_CASE("update simulation credential operations are always unavailable",
          "[update][simulation][workspace]")
{
  const auto credentials = updates::MakeUpdateSimulationCredentialStore();

  REQUIRE(credentials);

  const std::array secret{std::byte{'s'}, std::byte{'e'}, std::byte{'c'}};

  const auto stored = credentials->Store("simulated-only", "simulated-user", secret);
  const auto loaded = credentials->Load("simulated-only");
  const auto erased = credentials->Erase("simulated-only");

  REQUIRE_FALSE(stored);
  REQUIRE_FALSE(loaded);
  REQUIRE_FALSE(erased);

  for (const auto *error : {&stored.error(), &loaded.error(), &erased.error()})
  {
    CHECK(error->code == platform::PlatformErrorCode::Unavailable);
    CHECK(error->message == "Credential storage is unavailable in update simulation mode.");
    CHECK(error->nativeCode == 0);
  }
}

#endif // HAVREMOTE_UPDATE_SIMULATION
