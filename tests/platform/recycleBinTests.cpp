// SPDX-License-Identifier: MIT

#include "platform/recycleBin.hpp"
#include "platform/recycleBinInternal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace havremote;

namespace
{
  std::size_t nativeCalls{};
  std::vector<std::filesystem::path> nativePaths;
  std::optional<platform::PlatformError> nativeFailure;

  class TrashFixture final
  {
  public:
    TrashFixture()
    {
      static std::atomic<unsigned> sequence{};

      const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();

      mPath = std::filesystem::temp_directory_path() /
              ("havremote-trash-validation-" + std::to_string(stamp) + "-" +
               std::to_string(sequence.fetch_add(1)));

      REQUIRE(std::filesystem::create_directory(mPath));

      nativeCalls = 0;
      nativePaths.clear();
      nativeFailure.reset();
    }

    ~TrashFixture()
    {
      std::error_code ignored;
      std::filesystem::remove_all(mPath, ignored);
    }

    [[nodiscard]] const std::filesystem::path &Path() const { return mPath; }

    [[nodiscard]] std::filesystem::path File(const std::filesystem::path &name) const
    {
      const auto path = mPath / name;

      std::ofstream file{path, std::ios::binary};

      REQUIRE(file.is_open());

      file << "keep these bytes";

      REQUIRE(file.good());

      return path;
    }

  private:
    std::filesystem::path mPath;
  };
}

// This executable links no native backend and never touches the user's Trash
platform::Result<void> platform::detail::MoveToSystemTrash(
    const std::span<const std::filesystem::path> paths)
{
  ++nativeCalls;

  nativePaths.assign(paths.begin(), paths.end());

  if (nativeFailure)
  {
    return std::unexpected(*nativeFailure);
  }

  return {};
}

TEST_CASE("Trash ignores an empty selection", "[platform][trash]")
{
  const TrashFixture fixture;

  REQUIRE(platform::MakeRecycleBin()->MoveToRecycleBin({}));
  CHECK(nativeCalls == 0U);
}

TEST_CASE("Trash rejects empty paths roots and dot entries", "[platform][trash]")
{
  const TrashFixture fixture;

  const std::array invalid{
      std::filesystem::path{}, fixture.Path().root_path(),
      fixture.Path() / ".", fixture.Path() / "..",
      fixture.Path() / "." / ""};

  for (const auto &path : invalid)
  {
    const auto result = platform::MakeRecycleBin()->MoveToRecycleBin(std::array{path});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == platform::PlatformErrorCode::InvalidArgument);
  }

  CHECK(nativeCalls == 0U);
}

TEST_CASE("Trash rejects embedded NUL without touching the matching prefix", "[platform][trash]")
{
  const TrashFixture fixture;

  const auto original = fixture.File("original.txt");

  auto native = original.native();
  native.push_back(std::filesystem::path::value_type{});
  native += std::filesystem::path{"suffix"}.native();

  const auto result = platform::MakeRecycleBin()->MoveToRecycleBin(
      std::array{std::filesystem::path{native}});

  REQUIRE_FALSE(result);
  CHECK(result.error().code == platform::PlatformErrorCode::InvalidArgument);
  CHECK(nativeCalls == 0U);
  CHECK(std::filesystem::exists(original));
}

TEST_CASE("Trash validates an entire batch before starting native operations", "[platform][trash]")
{
  const TrashFixture fixture;

  const auto original = fixture.File("keep.txt");

  for (const auto &missing : {fixture.Path() / "missing.txt",
                              fixture.Path() / "missing-directory" / "file.txt"})
  {
    const auto result = platform::MakeRecycleBin()->MoveToRecycleBin(
        std::array{original, missing});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == platform::PlatformErrorCode::NotFound);
  }

  CHECK(nativeCalls == 0U);
  CHECK(std::filesystem::exists(original));
}

TEST_CASE("Trash passes files directories and Unicode paths to the native backend", "[platform][trash]")
{
  const TrashFixture fixture;

  const auto file = fixture.File(std::filesystem::path{u8"Grüße 東京 🔌.txt"});

  const auto directory = fixture.Path() / "directory";

  REQUIRE(std::filesystem::create_directory(directory));

  const auto result = platform::MakeRecycleBin()->MoveToRecycleBin(
      std::array{file, directory / ""});

  REQUIRE(result);
  REQUIRE(nativeCalls == 1U);
  REQUIRE(nativePaths.size() == 2U);
  CHECK(nativePaths[0] == std::filesystem::canonical(file));
  CHECK(nativePaths[1] == std::filesystem::canonical(directory));
}

TEST_CASE("Trash resolves relative paths before invoking the native backend", "[platform][trash]")
{
  const TrashFixture fixture;

  const auto file = fixture.File("relative.txt");

  const auto relative = file.lexically_relative(std::filesystem::current_path());

  if (relative.empty())
  {
    SKIP("The temporary directory is on another filesystem root");
  }

  REQUIRE(platform::MakeRecycleBin()->MoveToRecycleBin(std::array{relative}));
  REQUIRE(nativePaths.size() == 1U);
  CHECK(nativePaths[0] == std::filesystem::canonical(file));
}

TEST_CASE("Trash preserves final symbolic links including dangling links", "[platform][trash]")
{
  const TrashFixture fixture;

  const auto target = fixture.File("target.txt");
  const auto link = fixture.Path() / "link.txt";
  const auto dangling = fixture.Path() / "dangling.txt";

  std::error_code error;

  std::filesystem::create_symlink(target, link, error);

  if (error)
  {
    SKIP("Creating symbolic links is unavailable: " + error.message());
  }

  std::filesystem::create_symlink(fixture.Path() / "absent.txt", dangling);

  REQUIRE(platform::MakeRecycleBin()->MoveToRecycleBin(std::array{link, dangling}));
  REQUIRE(nativePaths.size() == 2U);
  CHECK(nativePaths[0] == std::filesystem::canonical(fixture.Path()) / link.filename());
  CHECK(nativePaths[1] == std::filesystem::canonical(fixture.Path()) / dangling.filename());
  CHECK(std::filesystem::exists(target));
}

#if !defined(_WIN32)
TEST_CASE("Trash resolves parent links before dot-dot without changing the selected item",
          "[platform][trash]")
{
  const TrashFixture fixture;

  const auto actual = fixture.Path() / "actual";

  REQUIRE(std::filesystem::create_directories(actual / "nested"));

  const auto expected = fixture.File("actual/selected.txt");
  const auto decoy = fixture.File("selected.txt");
  const auto link = fixture.Path() / "directory-link";

  std::error_code error;

  std::filesystem::create_directory_symlink(actual / "nested", link, error);

  if (error)
  {
    SKIP("Creating directory links is unavailable: " + error.message());
  }

  REQUIRE(platform::MakeRecycleBin()->MoveToRecycleBin(
      std::array{link / ".." / "selected.txt"}));
  REQUIRE(nativePaths.size() == 1U);
  CHECK(nativePaths[0] == std::filesystem::canonical(expected));
  CHECK(nativePaths[0] != std::filesystem::canonical(decoy));
  CHECK(std::filesystem::exists(decoy));
}
#endif

TEST_CASE("Trash resolves directory links in parents but preserves the selected file link",
          "[platform][trash]")
{
  const TrashFixture fixture;

  const auto actual = fixture.Path() / "actual";

  REQUIRE(std::filesystem::create_directory(actual));

  const auto target = fixture.File("actual/target.txt");
  const auto fileLink = actual / "selected-link.txt";
  const auto parentLink = fixture.Path() / "parent-link";

  std::error_code error;

  std::filesystem::create_directory_symlink(actual, parentLink, error);

  if (error)
  {
    SKIP("Creating directory links is unavailable: " + error.message());
  }

  std::filesystem::create_symlink(target, fileLink, error);

  if (error)
  {
    SKIP("Creating file links is unavailable: " + error.message());
  }

  REQUIRE(platform::MakeRecycleBin()->MoveToRecycleBin(
      std::array{parentLink / fileLink.filename()}));
  REQUIRE(nativePaths.size() == 1U);
  CHECK(nativePaths[0] == std::filesystem::canonical(actual) / fileLink.filename());
  CHECK(std::filesystem::exists(target));
}

TEST_CASE("Trash propagates native failure without retrying", "[platform][trash]")
{
  const TrashFixture fixture;

  const auto source = fixture.File("keep.txt");

  nativeFailure = platform::PlatformError{
      platform::PlatformErrorCode::Unavailable, "Trash is unavailable on this volume", 42};

  const auto result = platform::MakeRecycleBin()->MoveToRecycleBin(std::array{source});

  REQUIRE_FALSE(result);
  CHECK(result.error().code == nativeFailure->code);
  CHECK(result.error().message == nativeFailure->message);
  CHECK(result.error().nativeCode == 42UL);
  CHECK(nativeCalls == 1U);
  CHECK(std::filesystem::exists(source));
}
