// SPDX-License-Identifier: MIT

#include "platform/pathSafety.hpp"

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
#include <vector>

using namespace havremote;

namespace
{
  class PathFixture final
  {
  public:
    PathFixture()
        : path{std::filesystem::canonical(std::filesystem::temp_directory_path()) /
               ("havremote-path-tests-" + GenerateId())}
    {
      REQUIRE(std::filesystem::create_directory(path));
    }

    ~PathFixture()
    {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }

    PathFixture(const PathFixture &) = delete;
    PathFixture &operator=(const PathFixture &) = delete;

    const std::filesystem::path path;
  };

  class CurrentDirectoryGuard final
  {
  public:
    explicit CurrentDirectoryGuard(const std::filesystem::path &directory)
        : mOriginalPath{std::filesystem::current_path()}
    {
      std::filesystem::current_path(directory);
    }

    ~CurrentDirectoryGuard()
    {
      std::error_code ignored;
      std::filesystem::current_path(mOriginalPath, ignored);
    }

    CurrentDirectoryGuard(const CurrentDirectoryGuard &) = delete;
    CurrentDirectoryGuard &operator=(const CurrentDirectoryGuard &) = delete;

  private:
    const std::filesystem::path mOriginalPath;
  };

  [[nodiscard]] std::filesystem::path Utf8Path(const std::string_view value)
  {
    return std::filesystem::path{std::u8string{
        reinterpret_cast<const char8_t *>(value.data()), value.size()}};
  }

  [[nodiscard]] std::size_t CountEntries(const std::filesystem::path &directory)
  {
    return static_cast<std::size_t>(std::distance(
        std::filesystem::directory_iterator{directory}, std::filesystem::directory_iterator{}));
  }

  [[nodiscard]] std::filesystem::path FindOwnedProbe(const std::filesystem::path &root)
  {
    std::filesystem::path found;

    const auto prefix = std::filesystem::path{".havremote-name-check-"}.native();

    for (const auto &entry : std::filesystem::directory_iterator{root})
    {
      if (!entry.path().filename().native().starts_with(prefix))
      {
        continue;
      }

      REQUIRE(found.empty());
      REQUIRE(std::filesystem::is_directory(entry.symlink_status()));

      found = entry.path();
    }

    REQUIRE_FALSE(found.empty());

    return found;
  }

  void WriteSentinel(const std::filesystem::path &path)
  {
    REQUIRE_FALSE(std::filesystem::exists(path));

    std::ofstream output{path, std::ios::binary};

    REQUIRE(output.is_open());

    output << "test-owned sentinel\n";
    output.close();

    REQUIRE(output.good());
  }

  void CheckSentinel(const std::filesystem::path &path)
  {
    REQUIRE(std::filesystem::is_regular_file(std::filesystem::symlink_status(path)));

    std::ifstream input{path, std::ios::binary};

    REQUIRE(input.is_open());

    const std::string contents{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};

    CHECK(contents == "test-owned sentinel\n");
  }

  // Probe only empty, test-owned names in the actual destination directory.
  // All reference entries are removed before invoking the path safety API.
  [[nodiscard]] bool AllowsDistinctNames(const std::filesystem::path &parent,
                                         const std::string_view first,
                                         const std::string_view second)
  {
    const auto firstPath = parent / Utf8Path(first);
    const auto secondPath = parent / Utf8Path(second);

    REQUIRE(std::filesystem::create_directory(firstPath));

    std::error_code error;

    const bool distinct = std::filesystem::create_directory(secondPath, error);

    REQUIRE_FALSE(error);

    if (distinct)
    {
      REQUIRE(std::filesystem::remove(secondPath));
    }

    REQUIRE(std::filesystem::remove(firstPath));

    return distinct;
  }

  [[nodiscard]] bool AllowsName(const std::filesystem::path &parent,
                                const std::string_view name)
  {
    const auto reference = parent / Utf8Path(name);

    std::error_code error;

    const bool created = std::filesystem::create_directory(reference, error);
    if (created)
    {
      REQUIRE(std::filesystem::remove(reference));

      return true;
    }

    REQUIRE(error);

    return false;
  }

  void CheckPathConflict(const std::filesystem::path &left,
                         const std::filesystem::path &right,
                         const bool expected)
  {
    CAPTURE(left, right);

    const auto forward = platform::CheckLocalPathConflict(left, right);

    REQUIRE(forward);

    CHECK(*forward == expected);

    const auto reverse = platform::CheckLocalPathConflict(right, left);

    REQUIRE(reverse);
    CHECK(*reverse == expected);
  }

#if defined(_WIN32)
  [[nodiscard]] bool SetCaseSensitivity(const std::filesystem::path &directory,
                                        const bool sensitive)
  {
    const HANDLE handle = CreateFileW(
        directory.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);

    if (handle == INVALID_HANDLE_VALUE)
    {
      return false;
    }

    FILE_CASE_SENSITIVE_INFO info{};
    info.Flags = sensitive ? FILE_CS_FLAG_CASE_SENSITIVE_DIR : 0;

    const bool changed = SetFileInformationByHandle(
                             handle, FileCaseSensitiveInfo, &info, sizeof(info)) != FALSE;

    CloseHandle(handle);

    return changed;
  }
#endif
} // namespace

TEST_CASE("Local filename syntax follows the host platform", "[platform][paths]")
{
  CHECK(platform::IsLocalSafeFilename("ordinary.txt"));
  CHECK(platform::IsLocalSafeFilename("Grüße 東京 🔌.txt"));
  CHECK_FALSE(platform::IsLocalSafeFilename(""));
  CHECK_FALSE(platform::IsLocalSafeFilename("."));
  CHECK_FALSE(platform::IsLocalSafeFilename(".."));
  CHECK_FALSE(platform::IsLocalSafeFilename("parent/child"));

  for (const auto name : {"CON", "com1.txt", "CONIN$", "conout$.txt", "COM¹.log", "LPT³",
                          "with:colon", "with\\backslash", "trailing.", "trailing ", "a<b"})
  {
    CAPTURE(name);

#if defined(_WIN32)
    CHECK_FALSE(platform::IsLocalSafeFilename(name));
#else
    CHECK(platform::IsLocalSafeFilename(name));
#endif
  }
}

TEST_CASE("Local filename syntax rejects NUL and malformed UTF-8", "[platform][paths]")
{
  const std::vector<std::string> invalid{
      std::string{"prefix\0suffix", 13},
      "\x80", "\xc0\xaf", "\xc2", "\xe2\x28\xa1", "\xe0\x80\x80",
      "\xed\xa0\x80", "\xf0\x80\x80\xaf", "\xf4\x90\x80\x80", "\xff"};

  for (const auto &name : invalid)
  {
    CHECK_FALSE(platform::IsLocalSafeFilename(name));
  }
}

TEST_CASE("Download mapping rejects traversal absolute paths and malformed names",
          "[platform][paths]")
{
  const PathFixture fixture;
  {
    platform::SafeDownloadMapper mapper{fixture.path};

    const std::vector<std::string> invalid{
        "", ".", "..", "../outside.txt", "folder/../../outside.txt", "folder/../file.txt",
        "/absolute.txt", "folder/./file.txt", "folder//file.txt", "folder/",
        std::string{"prefix\0suffix", 13}, "folder/\xc0\xaf", "folder/\xed\xa0\x80"};

    for (const auto &name : invalid)
    {
      CHECK_FALSE(mapper.Map(RemotePath{name}));
    }

#if defined(_WIN32)
    CHECK_FALSE(mapper.Map(RemotePath{"C:/absolute.txt"}));
    CHECK_FALSE(mapper.Map(RemotePath{"\\\\server\\share\\file.txt"}));
#endif

    const auto mapped = mapper.Map(RemotePath{"folder/report.txt"});

    REQUIRE(mapped);
    CHECK(*mapped == fixture.path / "folder" / "report.txt");
    CHECK_FALSE(std::filesystem::exists(fixture.path / "folder"));
  }

  CHECK(CountEntries(fixture.path) == 0U);
}

TEST_CASE("Download mapping preserves a Unicode native path without creating targets",
          "[platform][paths]")
{
  const PathFixture fixture;

  const std::string name = "Grüße 東京/Übertragung 🔌.txt";
  {
    platform::SafeDownloadMapper mapper{fixture.path};

    const auto mapped = mapper.Map(RemotePath{name});

    REQUIRE(mapped);
    CHECK(*mapped == fixture.path / Utf8Path(name));
    CHECK_FALSE(std::filesystem::exists(fixture.path / Utf8Path("Grüße 東京")));
  }

  CHECK(CountEntries(fixture.path) == 0U);
}

TEST_CASE("Download roots accept terminal separators and dot components",
          "[platform][paths]")
{
  const PathFixture fixture;

  const auto target = fixture.path / "nested" / "file.txt";

  for (const auto &root : {fixture.path / "", fixture.path / "."})
  {
    CAPTURE(root);

    CHECK(platform::ValidateLocalWriteTarget(root, target));
    {
      platform::SafeDownloadMapper mapper{root};

      const auto mapped = mapper.Map(RemotePath{"nested/file.txt"});

      REQUIRE(mapped);
      CHECK(*mapped == target);
      CHECK_FALSE(std::filesystem::exists(fixture.path / "nested"));
    }

    CHECK(CountEntries(fixture.path) == 0U);
  }
}

TEST_CASE("Download mapping preserves relative current-directory destinations",
          "[platform][paths]")
{
  const PathFixture fixture;

  const CurrentDirectoryGuard currentDirectory{fixture.path};

  const std::filesystem::path relative{"nested/file.txt"};

  CHECK(platform::ValidateLocalWriteTarget(".", relative));
  CHECK(platform::ValidateLocalWriteTarget(".", fixture.path / relative));
  {
    platform::SafeDownloadMapper mapper{"."};

    const auto mapped = mapper.Map(RemotePath{"nested/file.txt"});

    REQUIRE(mapped);
    CHECK(*mapped == relative);
    CHECK(mapped->is_relative());
    CHECK_FALSE(std::filesystem::exists(fixture.path / "nested"));
  }

  CHECK(CountEntries(fixture.path) == 0U);
}

TEST_CASE("Local write validation preserves filesystem roots",
          "[platform][paths]")
{
  const PathFixture fixture;

  const auto root = fixture.path.root_path();

  REQUIRE_FALSE(root.empty());
  CHECK(platform::ValidateLocalWriteTarget(root, root));
  CHECK(platform::ValidateLocalWriteTarget(root, fixture.path / "file.txt"));
  CHECK(CountEntries(fixture.path) == 0U);
}

#if !defined(_WIN32)
TEST_CASE("POSIX download mapping preserves names that Windows syntax would reject",
          "[platform][paths]")
{
  const PathFixture fixture;

  for (const auto name : {"CON", "with:colon", "\\leading-backslash", "trailing.", "trailing "})
  {
    CAPTURE(name);

    const bool allowed = AllowsName(fixture.path, name);
    {
      platform::SafeDownloadMapper mapper{fixture.path};

      const auto mapped = mapper.Map(RemotePath{name});

      CHECK(static_cast<bool>(mapped) == allowed);

      if (mapped)
      {
        CHECK(*mapped == fixture.path / Utf8Path(name));
      }
    }

    CHECK(CountEntries(fixture.path) == 0U);
  }
}
#endif

TEST_CASE("Download leaf aliases follow actual filesystem case and normalization rules",
          "[platform][paths]")
{
  const PathFixture fixture;

  const std::array<std::array<std::string, 2>, 3> pairs{{{"Readme.txt", "README.TXT"},
                                                         {"Ä.txt", "ä.txt"},
                                                         {"\xc3\xa9.txt", "e\xcc\x81.txt"}}};

  for (const auto &pair : pairs)
  {
    CAPTURE(pair[0], pair[1]);

    const bool distinct = AllowsDistinctNames(fixture.path, pair[0], pair[1]);
    {
      platform::SafeDownloadMapper mapper{fixture.path};

      REQUIRE(mapper.Map(RemotePath{pair[0]}));
      CHECK(static_cast<bool>(mapper.Map(RemotePath{pair[1]})) == distinct);
      CHECK_FALSE(std::filesystem::exists(fixture.path / Utf8Path(pair[0])));
      CHECK_FALSE(std::filesystem::exists(fixture.path / Utf8Path(pair[1])));
    }

    CHECK(CountEntries(fixture.path) == 0U);
  }
}

TEST_CASE("Download mapping checks aliases in directory prefixes", "[platform][paths]")
{
  const PathFixture fixture;

  const std::array<std::array<std::string, 2>, 2> pairs{{{"A", "a"}, {"\xc3\xa9", "e\xcc\x81"}}};

  for (const auto &pair : pairs)
  {
    CAPTURE(pair[0], pair[1]);

    const bool distinct = AllowsDistinctNames(fixture.path, pair[0], pair[1]);
    {
      platform::SafeDownloadMapper mapper{fixture.path};

      REQUIRE(mapper.Map(RemotePath{pair[0] + "/first.txt"}));
      CHECK(static_cast<bool>(mapper.Map(RemotePath{pair[1] + "/second.txt"})) == distinct);
      // The rejected alias must not discard the first prefix's reservation
      REQUIRE(mapper.Map(RemotePath{pair[0] + "/third.txt"}));
    }

    CHECK(CountEntries(fixture.path) == 0U);
  }
}

TEST_CASE("Download mapping permits repeated exact prefixes but rejects duplicate full paths",
          "[platform][paths]")
{
  const PathFixture fixture;
  {
    platform::SafeDownloadMapper mapper{fixture.path};

    REQUIRE(mapper.Map(RemotePath{"shared/first.txt"}));
    REQUIRE(mapper.Map(RemotePath{"shared/second.txt"}));
    CHECK_FALSE(mapper.Map(RemotePath{"shared/first.txt"}));
    REQUIRE(mapper.Map(RemotePath{"shared/third.txt"}));
  }

  CHECK(CountEntries(fixture.path) == 0U);
}

TEST_CASE("Download mapping follows the existing child directory's filesystem",
          "[platform][paths]")
{
  const PathFixture fixture;

  const auto child = fixture.path / "existing";

  REQUIRE(std::filesystem::create_directory(child));

  const bool distinct = AllowsDistinctNames(child, "A", "a");
  {
    platform::SafeDownloadMapper mapper{fixture.path};

    REQUIRE(mapper.Map(RemotePath{"existing/A/first.txt"}));
    CHECK(static_cast<bool>(mapper.Map(RemotePath{"existing/a/second.txt"})) == distinct);
    CHECK_FALSE(std::filesystem::exists(child / "A"));
  }

  CHECK(CountEntries(fixture.path) == 1U);
  CHECK(CountEntries(child) == 0U);
}

#if defined(_WIN32)
TEST_CASE("Download mapping respects a child's distinct Windows case sensitivity",
          "[platform][paths][windows]")
{
  const PathFixture fixture;

  const bool parentSensitive = AllowsDistinctNames(fixture.path, "Parent", "parent");

  const auto child = fixture.path / "existing";

  REQUIRE(std::filesystem::create_directory(child));

  if (!SetCaseSensitivity(child, !parentSensitive))
  {
    SKIP("Changing case sensitivity on the test-owned child directory is unavailable");
  }

  const bool childSensitive = AllowsDistinctNames(child, "A", "a");
  if (childSensitive == parentSensitive)
  {
    SKIP("Directory creation does not expose a distinct child case policy");
  }

  {
    platform::SafeDownloadMapper mapper{fixture.path};

    REQUIRE(mapper.Map(RemotePath{"existing/A/first.txt"}));
    CHECK(static_cast<bool>(mapper.Map(RemotePath{"existing/a/second.txt"})) == childSensitive);
  }

  CheckPathConflict(child / "A.txt", child / "a.txt", !childSensitive);

  CHECK(CountEntries(fixture.path) == 1U);
  CHECK(CountEntries(child) == 0U);
}
#endif

TEST_CASE("A missing download root remains absent after mapping and cleanup",
          "[platform][paths]")
{
  const PathFixture fixture;

  const auto missingRoot = fixture.path / "not-created";
  {
    platform::SafeDownloadMapper mapper{missingRoot};

    const auto mapped = mapper.Map(RemotePath{"nested/file.txt"});

    REQUIRE(mapped);
    CHECK(*mapped == missingRoot / "nested" / "file.txt");
    CHECK_FALSE(std::filesystem::exists(missingRoot));
  }

  CHECK_FALSE(std::filesystem::exists(missingRoot));
  CHECK(CountEntries(fixture.path) == 0U);
}

TEST_CASE("Download probe cleanup preserves an unexpected regular file",
          "[platform][paths]")
{
  const PathFixture fixture;

  std::filesystem::path probe, sentinel;
  {
    platform::SafeDownloadMapper mapper{fixture.path};

    REQUIRE(mapper.Map(RemotePath{"planned/file.txt"}));

    probe = FindOwnedProbe(fixture.path);

    sentinel = probe / "sentinel.txt";

    WriteSentinel(sentinel);
  }

  CheckSentinel(sentinel);

  CHECK(CountEntries(probe) == 1U);
  CHECK(CountEntries(fixture.path) == 1U);
  CHECK_FALSE(std::filesystem::exists(fixture.path / "planned"));
}

#if !defined(_WIN32)
TEST_CASE("Download probe cleanup does not follow a replaced child symlink",
          "[platform][paths]")
{
  const PathFixture fixture;

  const auto replacement = fixture.path / "replacement-target";

  REQUIRE(std::filesystem::create_directory(replacement));
  REQUIRE(std::filesystem::create_directory(replacement / "leaf.txt"));

  const auto sentinel = replacement / "sentinel.txt";

  WriteSentinel(sentinel);

  std::filesystem::path probe;
  {
    platform::SafeDownloadMapper mapper{fixture.path};

    REQUIRE(mapper.Map(RemotePath{"branch/leaf.txt"}));

    probe = FindOwnedProbe(fixture.path);

    const auto branch = probe / "branch";

    REQUIRE(std::filesystem::is_directory(std::filesystem::symlink_status(branch)));

    std::filesystem::rename(branch, fixture.path / "saved-marker");

    std::error_code error;

    std::filesystem::create_directory_symlink(replacement, branch, error);

    if (error)
    {
      SKIP("Creating a test-owned replacement symlink is unavailable: " + error.message());
    }

    CHECK_FALSE(mapper.Map(RemotePath{"branch/later.txt"}));
  }

  CheckSentinel(sentinel);

  CHECK(std::filesystem::is_directory(replacement / "leaf.txt"));
  CHECK_FALSE(std::filesystem::exists(replacement / "later.txt"));
  CHECK(CountEntries(replacement) == 2U);
  CHECK_FALSE(std::filesystem::exists(probe));
  CHECK_FALSE(std::filesystem::exists(fixture.path / "branch"));
}
#endif

TEST_CASE("Failed download mapping does not reserve prefixes for later requests",
          "[platform][paths]")
{
  const PathFixture fixture;

  const std::string overlong(4096, 'x');

  REQUIRE_FALSE(AllowsName(fixture.path, overlong));
  {
    platform::SafeDownloadMapper mapper{fixture.path};

    CHECK_FALSE(mapper.Map(RemotePath{"Fresh/" + overlong}));
    REQUIRE(mapper.Map(RemotePath{"fresh/valid.txt"}));
    CHECK_FALSE(std::filesystem::exists(fixture.path / "Fresh"));
    CHECK_FALSE(std::filesystem::exists(fixture.path / "fresh"));
  }

  CHECK(CountEntries(fixture.path) == 0U);
}

TEST_CASE("Download component lengths follow actual filesystem acceptance",
          "[platform][paths]")
{
  const PathFixture fixture;

  std::string unicodeName;

  for (std::size_t index = 0; index < 130U; ++index)
  {
    unicodeName += "ä";
  }

  unicodeName += ".txt";

  const std::array names{unicodeName, std::string(300, 'x')};

  for (const auto &name : names)
  {
    INFO("UTF-8 filename bytes: " << name.size());

    // Syntax checking must not impose a guessed volume component limit
    CHECK(platform::IsLocalSafeFilename(name));

    const bool allowed = AllowsName(fixture.path, name);
    {
      platform::SafeDownloadMapper mapper{fixture.path};

      CHECK(static_cast<bool>(mapper.Map(RemotePath{name})) == allowed);
    }

    CHECK(CountEntries(fixture.path) == 0U);
  }
}

TEST_CASE("Remote containment checks component boundaries and traversal", "[platform][paths]")
{
  CHECK(platform::IsRemotePathWithin(RemotePath{"/home/user"}, RemotePath{"/home/user/file"}));
  CHECK(platform::IsRemotePathWithin(RemotePath{"/home/user/"}, RemotePath{"/home/user"}));
  CHECK(platform::IsRemotePathWithin(RemotePath{"/"}, RemotePath{"/anywhere/file"}));
  CHECK_FALSE(platform::IsRemotePathWithin(RemotePath{"/home/user"}, RemotePath{"/home/username"}));
  CHECK_FALSE(platform::IsRemotePathWithin(RemotePath{"/home/user"}, RemotePath{"/home/user/../escape"}));
  CHECK_FALSE(platform::IsRemotePathWithin(RemotePath{"/home/user"}, RemotePath{"/home/user/./file"}));
  CHECK_FALSE(platform::IsRemotePathWithin(RemotePath{"/home/../user"}, RemotePath{"/home/user/file"}));
  CHECK_FALSE(platform::IsRemotePathWithin(RemotePath{"/"}, RemotePath{"relative/file"}));
}

TEST_CASE("Local path conflicts follow actual filesystem aliases for absent leaves",
          "[platform][paths]")
{
  const PathFixture fixture;

  const auto parent = fixture.path / Utf8Path("Übertragungen 東京");

  REQUIRE(std::filesystem::create_directory(parent));

  const std::array<std::array<std::string, 2>, 3> pairs{{{"Readme.txt", "README.TXT"},
                                                         {"Ä.txt", "ä.txt"},
                                                         {"\xc3\xa9.txt", "e\xcc\x81.txt"}}};

  for (const auto &pair : pairs)
  {
    CAPTURE(pair[0], pair[1]);

    const bool distinct = AllowsDistinctNames(parent, pair[0], pair[1]);

    CheckPathConflict(parent / Utf8Path(pair[0]), parent / Utf8Path(pair[1]), !distinct);

    CHECK(CountEntries(parent) == 0U);
    CHECK(CountEntries(fixture.path) == 1U);
  }
}

TEST_CASE("Local path conflicts distinguish exact names different destinations and ancestors",
          "[platform][paths]")
{
  const PathFixture fixture;

  const auto firstParent = fixture.path / Utf8Path("Grüße 東京");
  const auto secondParent = fixture.path / Utf8Path("Andere Übertragungen");

  REQUIRE(std::filesystem::create_directory(firstParent));
  REQUIRE(std::filesystem::create_directory(secondParent));

  const auto first = firstParent / Utf8Path("Übertragung 🔌.txt");
  const auto second = firstParent / Utf8Path("別の名前.txt");
  const auto nested = firstParent / "missing" / Utf8Path("Übertragung 🔌.txt");

  CheckPathConflict(first, first, true);
  CheckPathConflict(nested, nested, true);
  CheckPathConflict(first, second, false);
  CheckPathConflict(first, secondParent / first.filename(), false);
  CheckPathConflict(firstParent, first, false);
  CheckPathConflict(firstParent / "missing", nested, false);
  CheckPathConflict(fixture.path, nested, false);

  CHECK(CountEntries(firstParent) == 0U);
  CHECK(CountEntries(secondParent) == 0U);
  CHECK(CountEntries(fixture.path) == 2U);
}

TEST_CASE("Local path conflicts ignore terminal separators and dot components",
          "[platform][paths]")
{
  const PathFixture fixture;

  const auto destination = fixture.path / "missing";

  REQUIRE_FALSE(std::filesystem::exists(destination));

  CheckPathConflict(destination, destination / "", true);
  CheckPathConflict(destination, destination / ".", true);

  CHECK(CountEntries(fixture.path) == 0U);
}

#if defined(_WIN32)
TEST_CASE("Local path conflicts recognize Windows drive-letter aliases for absent leaves",
          "[platform][paths][windows]")
{
  const PathFixture fixture;

  auto upperDrive = fixture.path.root_name().native();

  if (upperDrive.size() != 2U || upperDrive[1] != L':' ||
      !((upperDrive[0] >= L'A' && upperDrive[0] <= L'Z') ||
        (upperDrive[0] >= L'a' && upperDrive[0] <= L'z')))
  {
    SKIP("The test fixture is not on a drive-letter path");
  }

  if (upperDrive[0] >= L'a' && upperDrive[0] <= L'z')
  {
    upperDrive[0] = static_cast<wchar_t>(upperDrive[0] - L'a' + L'A');
  }

  auto lowerDrive = upperDrive;
  lowerDrive[0] = static_cast<wchar_t>(lowerDrive[0] - L'A' + L'a');

  const auto suffix = fixture.path.relative_path() / Utf8Path("Übertragung 東京.txt");
  const auto upper = std::filesystem::path{upperDrive} / fixture.path.root_directory() / suffix;
  const auto lower = std::filesystem::path{lowerDrive} / fixture.path.root_directory() / suffix;

  REQUIRE(upper.root_name() != lower.root_name());
  REQUIRE_FALSE(std::filesystem::exists(upper));
  REQUIRE_FALSE(std::filesystem::exists(lower));

  CheckPathConflict(upper, lower, true);

  CHECK(CountEntries(fixture.path) == 0U);
}
#endif

TEST_CASE("Local path conflicts allow distinct leaves beneath missing aliased parents",
          "[platform][paths]")
{
  const PathFixture fixture;

  const std::array<std::array<std::string, 2>, 3> pairs{{{"A", "a"}, {"Ä", "ä"}, {"\xc3\xa9", "e\xcc\x81"}}};

  for (const auto &pair : pairs)
  {
    CAPTURE(pair[0], pair[1]);

    const bool distinctParents = AllowsDistinctNames(fixture.path, pair[0], pair[1]);
    const auto firstParent = fixture.path / Utf8Path(pair[0]);
    const auto secondParent = fixture.path / Utf8Path(pair[1]);
    const auto firstLeaf = pair[0] + ".txt";
    const auto secondLeaf = pair[1] + ".txt";

    // Measure the policy inherited by a new child, then remove every reference
    REQUIRE(std::filesystem::create_directory(firstParent));

    const bool distinctLeaves = AllowsDistinctNames(firstParent, firstLeaf, secondLeaf);

    REQUIRE(std::filesystem::remove(firstParent));

    CheckPathConflict(firstParent, secondParent / "child.txt", false);
    CheckPathConflict(firstParent / "one.txt", secondParent / "two.txt", false);
    CheckPathConflict(firstParent / "nested" / "one.txt",
                      secondParent / "nested" / "two.txt", false);
    CheckPathConflict(firstParent / "same.txt", secondParent / "same.txt", !distinctParents);
    CheckPathConflict(firstParent / Utf8Path(firstLeaf), secondParent / Utf8Path(secondLeaf),
                      !distinctParents && !distinctLeaves);

    CHECK(CountEntries(fixture.path) == 0U);
  }
}

TEST_CASE("Local path conflicts resolve existing aliased parents without creating leaves",
          "[platform][paths]")
{
  const std::array<std::array<std::string, 2>, 3> pairs{{{"Parent", "parent"}, {"Ä", "ä"}, {"\xc3\xa9", "e\xcc\x81"}}};

  for (const auto &pair : pairs)
  {
    const PathFixture fixture;

    CAPTURE(pair[0], pair[1]);

    const bool distinctParents = AllowsDistinctNames(fixture.path, pair[0], pair[1]);
    const auto firstParent = fixture.path / Utf8Path(pair[0]);
    const auto secondParent = fixture.path / Utf8Path(pair[1]);

    REQUIRE(std::filesystem::create_directory(firstParent));

    if (distinctParents)
    {
      REQUIRE(std::filesystem::create_directory(secondParent));
    }

    const bool distinctLeaves = AllowsDistinctNames(firstParent, "Ä.txt", "ä.txt");

    CheckPathConflict(firstParent / Utf8Path("Übertragung.txt"),
                      secondParent / Utf8Path("Übertragung.txt"), !distinctParents);
    CheckPathConflict(firstParent / Utf8Path("Ä.txt"), secondParent / Utf8Path("ä.txt"),
                      !distinctParents && !distinctLeaves);
    CheckPathConflict(firstParent / "one.txt", secondParent / "two.txt", false);

    CHECK(CountEntries(firstParent) == 0U);
    CHECK(CountEntries(secondParent) == 0U);
    CHECK(CountEntries(fixture.path) == (distinctParents ? 2U : 1U));
  }
}

TEST_CASE("Local path conflict errors do not silently permit invalid native paths",
          "[platform][paths]")
{
  const PathFixture fixture;

  const auto valid = fixture.path / "valid.txt";

  auto native = (fixture.path / "prefix").native();
  native.push_back(std::filesystem::path::value_type{});
  native += std::filesystem::path{"suffix"}.native();

  for (const auto &invalid : {std::filesystem::path{}, std::filesystem::path{native}})
  {
    const auto forward = platform::CheckLocalPathConflict(invalid, valid);

    REQUIRE_FALSE(forward);
    CHECK(forward.error().code == RemoteErrorCode::LocalIo);
    CHECK_FALSE(forward.error().message.empty());

    const auto reverse = platform::CheckLocalPathConflict(valid, invalid);

    REQUIRE_FALSE(reverse);
    CHECK(reverse.error().code == RemoteErrorCode::LocalIo);
  }

  CheckPathConflict(valid, fixture.path / "other.txt", false);

  CHECK(CountEntries(fixture.path) == 0U);
}

TEST_CASE("Local write validation rejects sibling roots and lexical escape",
          "[platform][paths]")
{
  const PathFixture fixture;
  const auto root = fixture.path / "destination";
  const auto sibling = fixture.path / "destination-other";

  REQUIRE(std::filesystem::create_directory(root));
  REQUIRE(std::filesystem::create_directory(sibling));
  CHECK(platform::ValidateLocalWriteTarget(root, root));
  CHECK(platform::ValidateLocalWriteTarget(root, root / "nested" / "file.txt"));
  CHECK_FALSE(platform::ValidateLocalWriteTarget(root, root / ".." / "outside.txt"));
  CHECK_FALSE(platform::ValidateLocalWriteTarget(root, sibling / "file.txt"));
  CHECK(CountEntries(root) == 0U);
  CHECK(CountEntries(sibling) == 0U);
}

TEST_CASE("Local write containment does not merge distinct roots by case",
          "[platform][paths]")
{
  const PathFixture fixture;

  if (!AllowsDistinctNames(fixture.path, "Root", "root"))
  {
    SKIP("The test filesystem aliases case variants of directory names");
  }

  const auto root = fixture.path / "Root";
  const auto other = fixture.path / "root";

  REQUIRE(std::filesystem::create_directory(root));
  REQUIRE(std::filesystem::create_directory(other));
  CHECK_FALSE(platform::ValidateLocalWriteTarget(root, other / "file.txt"));
}

TEST_CASE("Local write validation rejects symbolic-link parents and dangling targets",
          "[platform][paths]")
{
  const PathFixture fixture;

  const auto root = fixture.path / "destination";
  const auto outside = fixture.path / "outside";

  REQUIRE(std::filesystem::create_directory(root));
  REQUIRE(std::filesystem::create_directory(outside));

  const auto linked = root / "linked";

  std::error_code error;

  std::filesystem::create_directory_symlink(outside, linked, error);

  if (error)
  {
    SKIP("Creating symbolic links is unavailable: " + error.message());
  }

  CHECK_FALSE(platform::ValidateLocalWriteTarget(root, linked / "file.txt"));
  CHECK_FALSE(platform::ValidateLocalWriteTarget(linked, linked / "file.txt"));

  const auto dangling = root / "dangling";

  std::filesystem::create_symlink(outside / "missing.txt", dangling);

  CHECK_FALSE(platform::ValidateLocalWriteTarget(root, dangling));
  CHECK_FALSE(std::filesystem::exists(outside / "missing.txt"));
  CHECK(CountEntries(outside) == 0U);
}

TEST_CASE("Local write validation rejects embedded NUL without modifying its prefix",
          "[platform][paths]")
{
  const PathFixture fixture;

  auto native = (fixture.path / "prefix").native();
  native.push_back(std::filesystem::path::value_type{});
  native += std::filesystem::path{"suffix"}.native();

  CHECK_FALSE(platform::ValidateLocalWriteTarget(fixture.path, std::filesystem::path{native}));
  CHECK(CountEntries(fixture.path) == 0U);
}
