// SPDX-License-Identifier: MIT

#include "platform/reportExport.hpp"

#include "core/types.hpp"

#include <catch2/catch_test_macros.hpp>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace havremote;

namespace
{
  class TemporaryReportDirectory final
  {
  public:
    explicit TemporaryReportDirectory(
        const std::filesystem::path &parent = std::filesystem::temp_directory_path())
        : path{parent / ("havremote-report-tests-" + GenerateId())}
    {
      REQUIRE(std::filesystem::create_directory(path));
    }

    ~TemporaryReportDirectory()
    {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }

    const std::filesystem::path path;
  };

  void WriteExistingFile(const std::filesystem::path &path, const std::string_view bytes)
  {
    std::ofstream stream{path, std::ios::binary};

    REQUIRE(stream.is_open());

    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));

    REQUIRE(stream.good());
  }

  std::string ReadFile(const std::filesystem::path &path)
  {
    std::ifstream stream{path, std::ios::binary};

    REQUIRE(stream.is_open());

    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
  }

  std::size_t CountEntries(const std::filesystem::path &directory)
  {
    return static_cast<std::size_t>(std::distance(
        std::filesystem::directory_iterator{directory}, std::filesystem::directory_iterator{}));
  }
} // namespace

TEST_CASE("report export preserves exact UTF-8 bytes at a Unicode path", "[platform][reports]")
{
  const TemporaryReportDirectory directory;

  const auto destination = directory.path / std::filesystem::path{u8"Übertragungen 東京.csv"};

  std::string text{"\xef\xbb\xbf\"Element\",\"Details\"\r\n\"Grüße.txt\",\"東京🔌\"\r\n"};
  text += std::string(2U * 1024U * 1024U + 5U, 'x');
  text.push_back('\0');
  text += "exact binary tail\r\n";

  const auto saved = platform::WriteReportAtomic(destination, text);

  REQUIRE(saved);
  CHECK(ReadFile(destination) == text);
  CHECK(CountEntries(directory.path) == 1U);
}

TEST_CASE("report export replaces existing reports without temporary leftovers", "[platform][reports]")
{
  const TemporaryReportDirectory directory;

  const auto destination = directory.path / "queue.csv";

  WriteExistingFile(destination, "previous report");

  const std::string replacement{"\xef\xbb\xbf\"Item\"\r\n\"new.txt\"\r\n"};

  REQUIRE(platform::WriteReportAtomic(destination, replacement));
  CHECK(ReadFile(destination) == replacement);
  CHECK(CountEntries(directory.path) == 1U);
}

TEST_CASE("empty report exports neither create nor overwrite files", "[platform][reports]")
{
  const TemporaryReportDirectory directory;

  const auto existing = directory.path / "existing.csv";
  const auto absent = directory.path / "absent.csv";

  WriteExistingFile(existing, "retain this report");

  CHECK_FALSE(platform::WriteReportAtomic(absent, {}));
  CHECK_FALSE(std::filesystem::exists(absent));
  CHECK_FALSE(platform::WriteReportAtomic(existing, {}));
  CHECK(ReadFile(existing) == "retain this report");
  CHECK(CountEntries(directory.path) == 1U);
}

TEST_CASE("report export rejects missing parents and nonregular destinations", "[platform][reports]")
{
  const TemporaryReportDirectory directory;

  const auto existing = directory.path / "existing.csv";

  WriteExistingFile(existing, "retain this report");

  const auto missing = directory.path / "missing" / "report.csv";

  CHECK_FALSE(platform::WriteReportAtomic(missing, "new report"));
  CHECK_FALSE(std::filesystem::exists(missing.parent_path()));
  CHECK_FALSE(platform::WriteReportAtomic(existing / "child.csv", "new report"));
  CHECK_FALSE(platform::WriteReportAtomic(directory.path, "new report"));
  CHECK_FALSE(platform::WriteReportAtomic({}, "new report"));
  CHECK(ReadFile(existing) == "retain this report");
  CHECK(CountEntries(directory.path) == 1U);
}

TEST_CASE("report export rejects embedded NUL path components", "[platform][reports]")
{
  const TemporaryReportDirectory directory;

  const auto existing = directory.path / "existing.csv";

  WriteExistingFile(existing, "retain this report");

  auto native = existing.native();
  native.push_back(std::filesystem::path::value_type{});
  native += std::filesystem::path{"hidden-suffix"}.native();

  const auto result = platform::WriteReportAtomic(std::filesystem::path{native}, "new report");

  REQUIRE_FALSE(result);
  CHECK(result.error().code == platform::PlatformErrorCode::InvalidArgument);
  CHECK(ReadFile(existing) == "retain this report");
  CHECK(CountEntries(directory.path) == 1U);
}

TEST_CASE("report export does not follow symbolic-link destinations", "[platform][reports]")
{
  const TemporaryReportDirectory directory;

  const auto original = directory.path / "original.csv";
  const auto link = directory.path / "link.csv";

  WriteExistingFile(original, "retain this report");

  std::error_code error;

  std::filesystem::create_symlink(original, link, error);

  if (error)
  {
    SKIP("Creating symbolic links is unavailable: " + error.message());
  }

  const auto result = platform::WriteReportAtomic(link, "new report");

  REQUIRE_FALSE(result);
  CHECK(result.error().code == platform::PlatformErrorCode::UnsafeFileType);
  CHECK(ReadFile(original) == "retain this report");
  CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(link)));

  const std::vector<std::filesystem::path> protectedPaths{original};

  CHECK(platform::IsProtectedExportPath(link, protectedPaths));
  CHECK(CountEntries(directory.path) == 2U);
}

TEST_CASE("report export does not follow symbolic-link parent directories", "[platform][reports]")
{
  const TemporaryReportDirectory directory;

  const auto actual = directory.path / "actual";
  const auto link = directory.path / "linked";

  REQUIRE(std::filesystem::create_directory(actual));

  std::error_code error;

  std::filesystem::create_directory_symlink(actual, link, error);

  if (error)
  {
    SKIP("Creating directory links is unavailable: " + error.message());
  }

  const auto result = platform::WriteReportAtomic(link / "report.csv", "new report");

  REQUIRE_FALSE(result);
  CHECK(result.error().code == platform::PlatformErrorCode::UnsafeFileType);
  CHECK(CountEntries(actual) == 0U);
  CHECK(CountEntries(directory.path) == 2U);
}

#if defined(_WIN32)
TEST_CASE("failed Windows report replacement preserves the destination and cleans up", "[platform][reports]")
{
  const TemporaryReportDirectory directory;

  const auto destination = directory.path / "locked.csv";

  WriteExistingFile(destination, "retain this report");

  const auto handle = CreateFileW(destination.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

  REQUIRE(handle != INVALID_HANDLE_VALUE);

  const auto result = platform::WriteReportAtomic(destination, "new report");

  const auto closed = CloseHandle(handle);

  CHECK(closed != FALSE);
  CHECK_FALSE(result);
  CHECK(ReadFile(destination) == "retain this report");
  CHECK(CountEntries(directory.path) == 1U);
}
#endif

TEST_CASE("report export protects state paths including absent files and normalized aliases", "[platform][reports]")
{
  const TemporaryReportDirectory directory;

  const auto configuration = directory.path / "havRemote.cson";
  const auto queue = directory.path / "queue.cson";

  const std::vector<std::filesystem::path> protectedPaths{configuration, queue};

  WriteExistingFile(configuration, "preserved configuration");

  CHECK(platform::IsProtectedExportPath(configuration, protectedPaths));
  CHECK(platform::IsProtectedExportPath(queue, protectedPaths));
  CHECK(platform::IsProtectedExportPath(directory.path / "." / "havRemote.cson", protectedPaths));
  CHECK(platform::IsProtectedExportPath(directory.path / "unused" / ".." / "queue.cson", protectedPaths));
  CHECK_FALSE(platform::IsProtectedExportPath(directory.path / "report.csv", protectedPaths));
  CHECK_FALSE(platform::IsProtectedExportPath(directory.path / "havRemote.cson.csv", protectedPaths));
  CHECK(platform::IsProtectedExportPath({}, protectedPaths));
  CHECK(ReadFile(configuration) == "preserved configuration");
  CHECK_FALSE(std::filesystem::exists(queue));
}

TEST_CASE("report export recognizes relative state aliases", "[platform][reports]")
{
  // Keep this fixture on the working directory's drive, so it has a relative
  // spelling on Windows too, without changing the process working directory.
  const TemporaryReportDirectory directory{std::filesystem::current_path()};

  const auto configuration = directory.path / "havRemote.cson";

  const auto relative = configuration.lexically_relative(std::filesystem::current_path());

  REQUIRE_FALSE(relative.empty());
  REQUIRE_FALSE(relative.is_absolute());

  const std::vector<std::filesystem::path> protectedPaths{configuration};

  CHECK(platform::IsProtectedExportPath(relative, protectedPaths));
}

TEST_CASE("report export recognizes existing hard-link state aliases", "[platform][reports]")
{
  const TemporaryReportDirectory directory;

  const auto configuration = directory.path / "havRemote.cson";
  const auto alias = directory.path / "alias.csv";

  WriteExistingFile(configuration, "preserved configuration");

  std::error_code error;

  std::filesystem::create_hard_link(configuration, alias, error);

  if (error)
  {
    SKIP("Creating hard links is unavailable: " + error.message());
  }

  const std::vector<std::filesystem::path> protectedPaths{configuration};

  CHECK(platform::IsProtectedExportPath(alias, protectedPaths));
  CHECK(ReadFile(configuration) == "preserved configuration");
}

#if defined(_WIN32)
TEST_CASE("report export protects case-insensitive Windows state names", "[platform][reports]")
{
  const TemporaryReportDirectory directory;

  const std::vector<std::filesystem::path> protectedPaths{
      directory.path / "havRemote.cson", directory.path / std::filesystem::path{u8"Grüße.cson"}};

  CHECK(platform::IsProtectedExportPath(directory.path / "HAVREMOTE.CSON", protectedPaths));
  CHECK(platform::IsProtectedExportPath(directory.path / std::filesystem::path{u8"GRÜßE.CSON"}, protectedPaths));
  CHECK_FALSE(platform::IsProtectedExportPath(directory.path / "report.csv", protectedPaths));
}

TEST_CASE("Windows exports reject ambiguous names even when protected state is absent", "[platform][reports]")
{
  const TemporaryReportDirectory directory;

  const auto queue = directory.path / "queue.cson";

  const std::vector<std::filesystem::path> protectedPaths{queue};

  for (const auto name : {"queue.cson.", "queue.cson ", "queue.cson. ",
                          "QUEUE.CSON...", "queue.cson:report", "NUL.csv",
                          "CON", "COM1.txt", "report?.csv"})
  {
    CAPTURE(name);

    const auto target = directory.path / name;

    CHECK(platform::IsProtectedExportPath(target, protectedPaths));

    const auto result = platform::WriteReportAtomic(target, "new report");

    REQUIRE_FALSE(result);
    CHECK(result.error().code == platform::PlatformErrorCode::InvalidArgument);
  }

  auto ambiguousParent = directory.path;
  ambiguousParent += L". ";

  CHECK(platform::IsProtectedExportPath(ambiguousParent / "queue.cson", protectedPaths));
  CHECK_FALSE(platform::WriteReportAtomic(ambiguousParent / "report.csv", "new report"));
  CHECK_FALSE(std::filesystem::exists(queue));
  CHECK(CountEntries(directory.path) == 0U);
  CHECK_FALSE(platform::IsProtectedExportPath(directory.path / "." / "report.csv", protectedPaths));

  WriteExistingFile(queue, "preserved queue");

  CHECK(platform::IsProtectedExportPath(directory.path / "queue.cson.", protectedPaths));
  CHECK_FALSE(platform::WriteReportAtomic(directory.path / "queue.cson.", "new report"));
  CHECK(ReadFile(queue) == "preserved queue");
  CHECK(CountEntries(directory.path) == 1U);
}

TEST_CASE("Windows exports reject device namespaces without touching existing files", "[platform][reports]")
{
  const TemporaryReportDirectory directory;

  const auto configuration = directory.path / "havRemote.cson";

  WriteExistingFile(configuration, "preserved configuration");

  const std::vector<std::filesystem::path> protectedPaths{configuration};

  const auto extended = std::filesystem::path{L"\\\\?\\" + configuration.native()};

  CHECK(platform::IsProtectedExportPath(extended, protectedPaths));
  CHECK_FALSE(platform::WriteReportAtomic(extended, "new report"));
  CHECK(platform::IsProtectedExportPath(std::filesystem::path{L"\\\\.\\NUL"}, protectedPaths));
  CHECK_FALSE(platform::WriteReportAtomic(std::filesystem::path{L"\\\\.\\NUL"}, "new report"));
  CHECK(ReadFile(configuration) == "preserved configuration");
  CHECK(CountEntries(directory.path) == 1U);
}
#endif
