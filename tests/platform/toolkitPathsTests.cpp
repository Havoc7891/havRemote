// SPDX-License-Identifier: MIT

#include "platform/toolkitPaths.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <string_view>

using namespace havremote;

namespace
{
  wxString ToolkitText(const std::u8string_view text)
  {
    return wxString::FromUTF8(reinterpret_cast<const char *>(text.data()),
                              text.size());
  }
}

TEST_CASE("Toolkit paths preserve empty and relative native paths",
          "[platform][toolkit-paths]")
{
  CHECK(platform::FromToolkitPath(wxString{}).empty());
  CHECK(platform::ToToolkitPath(std::filesystem::path{}).empty());

  for (const auto &path : {std::filesystem::path{"."},
                           std::filesystem::path{"../directory/file name.txt"},
                           std::filesystem::path{"directory/"}})
  {
    const auto converted = platform::ToToolkitPath(path);

    CHECK(platform::FromToolkitPath(converted).native() == path.native());
  }
}

TEST_CASE("Toolkit paths round-trip Unicode without relying on the locale",
          "[platform][toolkit-paths]")
{
  constexpr std::array<std::u8string_view, 4> names{
      u8"Ordner/Grüße.txt", u8"資料/測試.txt", u8"📁/🔑.txt", u8"folder/a b ' \".txt"};

  for (const auto name : names)
  {
    const std::filesystem::path path{name};

    const auto text = ToolkitText(name);

    CHECK(platform::FromToolkitPath(text).native() == path.native());
    CHECK(platform::ToToolkitPath(path) == text);
    CHECK(platform::FromToolkitPath(platform::ToToolkitPath(path)).native() ==
          path.native());
  }
}

TEST_CASE("Toolkit conversion does not normalize case or Unicode spelling",
          "[platform][toolkit-paths]")
{
  constexpr std::array<std::u8string_view, 4> names{
      u8"Folder/File.txt", u8"folder/file.txt", u8"caf\u00e9.txt", u8"cafe\u0301.txt"};

  for (const auto name : names)
  {
    const auto text = ToolkitText(name);

    CHECK(platform::ToToolkitPath(platform::FromToolkitPath(text)) == text);
    CHECK(platform::FromToolkitPath(text).generic_u8string() == name);
  }
}

#if defined(_WIN32)
TEST_CASE("Toolkit paths retain drive and UNC spellings",
          "[platform][toolkit-paths]")
{
  for (const auto name : {L"C:\\Folder\\file.txt",
                          L"\\\\server\\share\\file.txt",
                          L"\\\\?\\C:\\Folder\\file.txt"})
  {
    const std::filesystem::path path{name};

    CHECK(platform::FromToolkitPath(platform::ToToolkitPath(path)).native() ==
          path.native());
  }
}
#else
TEST_CASE("Toolkit paths retain POSIX backslashes and punctuation",
          "[platform][toolkit-paths]")
{
  const std::filesystem::path path{u8"/tmp/資料/a\\b:c.txt"};

  CHECK(platform::FromToolkitPath(platform::ToToolkitPath(path)).native() ==
        path.native());
}
#endif
