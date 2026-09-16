// SPDX-License-Identifier: MIT

#include "core/types.hpp"
#include "platform/recycleBinInternal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <wx/init.h>
#include <wx/log.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>

using namespace havremote;

namespace
{
  class TrashErrorFixture final
  {
  public:
    TrashErrorFixture()
        : mPath(std::filesystem::temp_directory_path() /
                ("havremote-system-trash-error-" + GenerateId()))
    {
      REQUIRE(std::filesystem::create_directory(mPath));
    }

    ~TrashErrorFixture()
    {
      std::error_code ignored;
      std::filesystem::remove_all(mPath, ignored);
    }

    [[nodiscard]] const std::filesystem::path &Path() const { return mPath; }

  private:
    std::filesystem::path mPath;
  };
}

TEST_CASE("Trash captures system errors and stops before the next item",
          "[platform][trash][system-trash]")
{
  wxInitializer initializer;

  REQUIRE(initializer.IsOk());

  const TrashErrorFixture fixture;

  const auto missing = fixture.Path() / std::filesystem::path{u8"missing-ä.txt"};
  const auto untouched = fixture.Path() / "untouched.txt";
  {
    std::ofstream file{untouched};
    file << "untouched";

    REQUIRE(file.good());
  }

  REQUIRE_FALSE(std::filesystem::exists(missing));

  // Exercise the real library error path only. No existing item should reach Trash.
  wxLogCollector outerLog;

  auto *const originalTarget = wxLog::GetActiveTarget();

  const auto result = platform::detail::MoveToSystemTrash(
      std::array{missing, untouched});

  REQUIRE_FALSE(result);
  CHECK(result.error().code == platform::PlatformErrorCode::OperatingSystem);
  CHECK(result.error().nativeCode == 0UL);
  CHECK(result.error().message.find("missing-\xc3\xa4.txt") != std::string::npos);
  CHECK(outerLog.GetMessages().empty());
  CHECK(wxLog::GetActiveTarget() == originalTarget);
  REQUIRE(std::filesystem::exists(untouched));

  std::ifstream file{untouched};

  std::string contents;
  file >> contents;

  CHECK(contents == "untouched");
}

#if defined(__WXGTK__)
TEST_CASE("Trash rejects GTK filename conversions that change native bytes",
          "[platform][trash][system-trash]")
{
  wxInitializer initializer;

  REQUIRE(initializer.IsOk());

  const TrashErrorFixture fixture;
  const auto path = fixture.Path() / std::filesystem::path{"legacy-\xe4.txt"};
  {
    std::ofstream file{path};
    file << "untouched";

    REQUIRE(file.good());
  }

  const auto result = platform::detail::MoveToSystemTrash(std::array{path});

  REQUIRE_FALSE(result);
  CHECK(result.error().code == platform::PlatformErrorCode::InvalidArgument);
  CHECK(std::filesystem::exists(path));
}
#endif
