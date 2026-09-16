// SPDX-License-Identifier: MIT

#include "ui/fileListModel.hpp"

#ifdef _WIN32
#include "ui/fileListCtrl.hpp"
#endif

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace havremote::ui;

#ifdef _WIN32
TEST_CASE("file-list Delete shortcut accepts only the initial unmodified key-down",
          "[ui][file-list][keyboard]")
{
  for (const int key : {WXK_DELETE, WXK_NUMPAD_DELETE})
  {
    CAPTURE(key);

    wxKeyEvent event{wxEVT_KEY_DOWN};
    event.m_keyCode = key;

    CHECK(IsFileListDeleteKey(event));

    event.m_isRepeat = true;

    CHECK_FALSE(IsFileListDeleteKey(event));
  }
}

TEST_CASE("file-list Delete shortcut leaves all modified Delete keys alone",
          "[ui][file-list][keyboard]")
{
  for (const int key : {WXK_DELETE, WXK_NUMPAD_DELETE})
  {
    for (unsigned modifiers = 1; modifiers < 16; ++modifiers)
    {
      CAPTURE(key, modifiers);

      wxKeyEvent event{wxEVT_KEY_DOWN};
      event.m_keyCode = key;
      event.SetControlDown((modifiers & 1U) != 0);
      event.SetShiftDown((modifiers & 2U) != 0);
      event.SetAltDown((modifiers & 4U) != 0);
      event.SetMetaDown((modifiers & 8U) != 0);

      CHECK_FALSE(IsFileListDeleteKey(event));
    }
  }
}

TEST_CASE("file-list Delete shortcut does not consume other keys or event types",
          "[ui][file-list][keyboard]")
{
  for (const int key : std::array<int, 6>{
           WXK_BACK, WXK_RETURN, WXK_F2, WXK_ESCAPE, WXK_NUMPAD_DECIMAL, 'D'})
  {
    CAPTURE(key);

    wxKeyEvent event{wxEVT_KEY_DOWN};
    event.m_keyCode = key;

    CHECK_FALSE(IsFileListDeleteKey(event));
  }

  for (const int key : {WXK_DELETE, WXK_NUMPAD_DELETE})
  {
    CAPTURE(key);

    wxKeyEvent character{wxEVT_CHAR};
    character.m_keyCode = key;

    CHECK_FALSE(IsFileListDeleteKey(character));

    wxKeyEvent released{wxEVT_KEY_UP};
    released.m_keyCode = key;

    CHECK_FALSE(IsFileListDeleteKey(released));
  }
}
#endif

namespace
{
  bool IsAsciiDigit(const char value) noexcept
  {
    return value >= '0' && value <= '9';
  }

  unsigned char FoldedAscii(const char value) noexcept
  {
    return static_cast<unsigned char>(
        std::tolower(static_cast<unsigned char>(value)));
  }

  int TestNaturalCompare(const std::string_view left,
                         const std::string_view right)
  {
    std::size_t leftIndex = 0;
    std::size_t rightIndex = 0;

    while (leftIndex < left.size() && rightIndex < right.size())
    {
      if (IsAsciiDigit(left[leftIndex]) && IsAsciiDigit(right[rightIndex]))
      {
        const auto leftDigits = leftIndex;
        const auto rightDigits = rightIndex;

        while (leftIndex < left.size() && IsAsciiDigit(left[leftIndex]))
        {
          ++leftIndex;
        }

        while (rightIndex < right.size() && IsAsciiDigit(right[rightIndex]))
        {
          ++rightIndex;
        }

        auto leftSignificant = leftDigits;
        auto rightSignificant = rightDigits;

        while (leftSignificant < leftIndex && left[leftSignificant] == '0')
        {
          ++leftSignificant;
        }

        while (rightSignificant < rightIndex && right[rightSignificant] == '0')
        {
          ++rightSignificant;
        }

        const auto leftLength = leftIndex - leftSignificant;
        const auto rightLength = rightIndex - rightSignificant;

        if (leftLength != rightLength)
        {
          return leftLength < rightLength ? -1 : 1;
        }

        const auto comparison = left.substr(leftSignificant, leftLength)
                                    .compare(right.substr(rightSignificant,
                                                          rightLength));

        if (comparison != 0)
        {
          return comparison < 0 ? -1 : 1;
        }

        continue;
      }

      const auto leftCharacter = FoldedAscii(left[leftIndex]);
      const auto rightCharacter = FoldedAscii(right[rightIndex]);

      if (leftCharacter != rightCharacter)
      {
        return leftCharacter < rightCharacter ? -1 : 1;
      }

      ++leftIndex;
      ++rightIndex;
    }

    if (leftIndex == left.size() && rightIndex == right.size())
    {
      return 0;
    }

    return leftIndex == left.size() ? -1 : 1;
  }

  FileListRow Row(std::string identity,
                  std::string name,
                  const FileListRowKind kind = FileListRowKind::File)
  {
    FileListRow result;
    result.stableIdentity = std::move(identity);
    result.kind = kind;
    result.name = std::move(name);

    return result;
  }

  std::vector<std::string> Identities(const std::vector<FileListRow> &rows)
  {
    std::vector<std::string> result;
    result.reserve(rows.size());

    for (const auto &item : rows)
    {
      result.push_back(item.stableIdentity);
    }

    return result;
  }
} // namespace

TEST_CASE("file permissions format symbolic and special mode bits")
{
  CHECK(FormatFilePermissions(std::nullopt).empty());
  CHECK(FormatFilePermissions(0U) == "---------");
  CHECK(FormatFilePermissions(0754U) == "rwxr-xr--");

  // SFTP includes file-type bits in the same mode value
  CHECK(FormatFilePermissions(0100755U) == "rwxr-xr-x");

  CHECK(FormatFilePermissions(04755U) == "rwsr-xr-x");
  CHECK(FormatFilePermissions(04644U) == "rwSr--r--");
  CHECK(FormatFilePermissions(02750U) == "rwxr-s---");
  CHECK(FormatFilePermissions(02640U) == "rw-r-S---");
  CHECK(FormatFilePermissions(01777U) == "rwxrwxrwt");
  CHECK(FormatFilePermissions(01666U) == "rw-rw-rwT");
}

TEST_CASE("file owner and group format without stray separators")
{
  CHECK(FormatFileOwnerGroup({}, {}).empty());
  CHECK(FormatFileOwnerGroup("alice", {}) == "alice");
  CHECK(FormatFileOwnerGroup({}, "staff") == "staff");
  CHECK(FormatFileOwnerGroup("alice", "staff") == "alice/staff");
  CHECK(FormatFileOwnerGroup("1000", "1001") == "1000/1001");
}

TEST_CASE("permission octal input is exact and covers all twelve bits")
{
  CHECK(ParseFilePermissionsOctal("000") == 0U);
  CHECK(ParseFilePermissionsOctal("755") == 0755U);
  CHECK(ParseFilePermissionsOctal("0000") == 0U);
  CHECK(ParseFilePermissionsOctal("4755") == 04755U);
  CHECK(ParseFilePermissionsOctal("7777") == 07777U);

  CHECK_FALSE(ParseFilePermissionsOctal(""));
  CHECK_FALSE(ParseFilePermissionsOctal("77"));
  CHECK_FALSE(ParseFilePermissionsOctal("07555"));
  CHECK_FALSE(ParseFilePermissionsOctal("0o755"));
  CHECK_FALSE(ParseFilePermissionsOctal(" 755"));
  CHECK_FALSE(ParseFilePermissionsOctal("755 "));
  CHECK_FALSE(ParseFilePermissionsOctal("0788"));
  CHECK_FALSE(ParseFilePermissionsOctal("-755"));

  CHECK(FormatFilePermissionsOctal(0U) == "0000");
  CHECK(FormatFilePermissionsOctal(0755U) == "0755");
  CHECK(FormatFilePermissionsOctal(04755U) == "4755");
  CHECK(FormatFilePermissionsOctal(07777U) == "7777");

  // File-type bits supplied by SFTP are never presented as permission bits
  CHECK(FormatFilePermissionsOctal(0100755U) == "0755");
}

TEST_CASE("file-list name sorting is natural and keeps directories first")
{
  auto rows = std::vector{
      Row("file-10", "file10.txt"),
      Row("directory-z", "z", FileListRowKind::Directory),
      Row("file-2", "file2.txt"),
      Row("directory-a", "a", FileListRowKind::Directory),
  };

  SortFileListRows(rows, FileListColumn::Name, true, TestNaturalCompare);

  CHECK(Identities(rows) ==
        std::vector<std::string>{"directory-a", "directory-z",
                                 "file-2", "file-10"});

  SortFileListRows(rows, FileListColumn::Name, false, TestNaturalCompare);

  CHECK(Identities(rows) ==
        std::vector<std::string>{"directory-z", "directory-a",
                                 "file-10", "file-2"});
}

TEST_CASE("file-list column widths normalize by profile and logical column")
{
  const std::array supplied{
      FileListColumnWidth{FileListColumn::Modified, 300},
      FileListColumnWidth{FileListColumn::Name, 250},
  };

  CHECK(NormalizeFileListColumnWidths(FileListColumnProfile::Local, supplied) ==
        FileListColumnWidths{
            {FileListColumn::Name, 250},
            {FileListColumn::Size, 85},
            {FileListColumn::Type, 100},
            {FileListColumn::Modified, 300},
        });

  CHECK(NormalizeFileListColumnWidths(FileListColumnProfile::Remote, supplied) ==
        FileListColumnWidths{
            {FileListColumn::Name, 250},
            {FileListColumn::Size, 85},
            {FileListColumn::Type, 100},
            {FileListColumn::Modified, 300},
            {FileListColumn::Permissions, 110},
            {FileListColumn::Owner, 110},
        });
}

TEST_CASE("file-list column widths clamp and reject foreign columns")
{
  const std::array supplied{
      FileListColumnWidth{FileListColumn::Name, 0},
      FileListColumnWidth{FileListColumn::Size,
                          MaximumFileListColumnWidthDips + 1U},
      FileListColumnWidth{FileListColumn::Permissions, 222},
      FileListColumnWidth{static_cast<FileListColumn>(99), 333},
  };

  CHECK(NormalizeFileListColumnWidths(FileListColumnProfile::Local, supplied) ==
        FileListColumnWidths{
            {FileListColumn::Name, MinimumFileListColumnWidthDips},
            {FileListColumn::Size, MaximumFileListColumnWidthDips},
            {FileListColumn::Type, 100},
            {FileListColumn::Modified, 145},
        });
}

TEST_CASE("file-list column width normalization is deterministic for duplicates")
{
  const std::array supplied{
      FileListColumnWidth{FileListColumn::Name, 240},
      FileListColumnWidth{FileListColumn::Name, 480},
      FileListColumnWidth{FileListColumn::Owner, 175},
  };

  const auto normalized =
      NormalizeFileListColumnWidths(FileListColumnProfile::Remote, supplied);

  REQUIRE(normalized.size() == 6U);
  CHECK(normalized.front() ==
        FileListColumnWidth{FileListColumn::Name, 240});
  CHECK(normalized.back() ==
        FileListColumnWidth{FileListColumn::Owner, 175});
}

TEST_CASE("file-list parent navigation row remains first for every sort")
{
  constexpr std::array columns{
      FileListColumn::Name,
      FileListColumn::Size,
      FileListColumn::Type,
      FileListColumn::Modified,
      FileListColumn::Permissions,
      FileListColumn::Owner,
  };

  for (const auto column : columns)
  {
    for (const bool ascending : {true, false})
    {
      CAPTURE(column, ascending);

      auto rows = std::vector{
          Row("file", "file.txt", FileListRowKind::File),
          Row("parent", "..", FileListRowKind::ParentDirectory),
          Row("directory", "folder", FileListRowKind::Directory),
      };
      rows[0].sourceIndex = 7U;
      rows[0].rawSize = 1U;
      rows[0].modifiedAt =
          std::chrono::system_clock::time_point{std::chrono::seconds{1}};

      SortFileListRows(rows, column, ascending, TestNaturalCompare);

      REQUIRE(rows.size() == 3U);
      CHECK(rows.front().kind == FileListRowKind::ParentDirectory);
      CHECK_FALSE(rows.front().sourceIndex.has_value());

      const auto file = std::ranges::find(rows, "file",
                                          &FileListRow::stableIdentity);

      REQUIRE(file != rows.end());
      CHECK(file->sourceIndex == 7U);
    }
  }
}

TEST_CASE("file-list natural names are case-insensitive with stable tie breaks")
{
  auto rows = std::vector{
      Row("a-identity", "a2.txt"),
      Row("z-identity", "A2.txt"),
      Row("same-z", "same.txt"),
      Row("same-a", "same.txt"),
  };

  SortFileListRows(rows, FileListColumn::Name, true, TestNaturalCompare);

  CHECK(Identities(rows) ==
        std::vector<std::string>{"a-identity", "z-identity",
                                 "same-a", "same-z"});

  SortFileListRows(rows, FileListColumn::Name, false, TestNaturalCompare);

  CHECK(Identities(rows) ==
        std::vector<std::string>{"same-z", "same-a",
                                 "z-identity", "a-identity"});
}

TEST_CASE("file-list size sorting leaves unknown sizes last in both directions")
{
  auto rows = std::vector{
      Row("unknown", "unknown"),
      Row("maximum", "maximum"),
      Row("above-double", "above-double"),
      Row("above-four-gib", "above-four-gib"),
      Row("zero", "zero"),
  };
  rows[1].rawSize = (std::numeric_limits<std::uint64_t>::max)();
  rows[2].rawSize = (std::uint64_t{1} << 53U) + 1U;
  rows[3].rawSize = (std::uint64_t{4} << 30U) + 1U;
  rows[4].rawSize = 0U;

  SortFileListRows(rows, FileListColumn::Size, true, TestNaturalCompare);

  CHECK(Identities(rows) ==
        std::vector<std::string>{"zero", "above-four-gib", "above-double",
                                 "maximum", "unknown"});

  SortFileListRows(rows, FileListColumn::Size, false, TestNaturalCompare);

  CHECK(Identities(rows) ==
        std::vector<std::string>{"maximum", "above-double", "above-four-gib",
                                 "zero", "unknown"});
}

TEST_CASE("file-list sorting keeps source metadata attached to its backing row")
{
  auto rows = std::vector{
      Row("large", "file10.txt", FileListRowKind::Symlink),
      Row("small", "file2.txt", FileListRowKind::File),
  };
  rows[0].sourceIndex = 41U;
  rows[0].rawSize = 10U;
  rows[1].sourceIndex = 7U;
  rows[1].rawSize = 2U;

  SortFileListRows(rows, FileListColumn::Size, true, TestNaturalCompare);

  REQUIRE(rows.size() == 2U);
  CHECK(rows[0].stableIdentity == "small");
  CHECK(rows[0].sourceIndex == 7U);
  CHECK(rows[0].kind == FileListRowKind::File);
  CHECK(rows[1].stableIdentity == "large");
  CHECK(rows[1].sourceIndex == 41U);
  CHECK(rows[1].kind == FileListRowKind::Symlink);
}

TEST_CASE("file-list modified sorting leaves unknown times last in both directions")
{
  using namespace std::chrono_literals;

  auto rows = std::vector{
      Row("unknown", "unknown"),
      Row("later", "later"),
      Row("earlier", "earlier"),
  };
  rows[1].modifiedAt = std::chrono::system_clock::time_point{20s};
  rows[2].modifiedAt = std::chrono::system_clock::time_point{10s};

  SortFileListRows(rows, FileListColumn::Modified, true, TestNaturalCompare);

  CHECK(Identities(rows) ==
        std::vector<std::string>{"earlier", "later", "unknown"});

  SortFileListRows(rows, FileListColumn::Modified, false, TestNaturalCompare);

  CHECK(Identities(rows) ==
        std::vector<std::string>{"later", "earlier", "unknown"});
}

TEST_CASE("file-list permission sorting is numeric and leaves unknown modes last")
{
  auto rows = std::vector{
      Row("unknown", "unknown"),
      Row("owner-only", "owner-only"),
      Row("private", "private"),
      Row("shared", "shared"),
  };
  rows[1].owner = "alice";
  // Display text is deliberately opposite to numeric order: the raw mode is
  // the authoritative sort key.
  rows[2].permissions = "zzz";
  rows[2].rawPermissions = 0600U;
  rows[3].permissions = "aaa";
  rows[3].rawPermissions = 0750U;

  SortFileListRows(rows, FileListColumn::Permissions, true,
                   TestNaturalCompare);

  CHECK(Identities(rows) ==
        std::vector<std::string>{"private", "shared", "owner-only",
                                 "unknown"});

  SortFileListRows(rows, FileListColumn::Permissions, false,
                   TestNaturalCompare);

  CHECK(Identities(rows) ==
        std::vector<std::string>{"shared", "private", "unknown",
                                 "owner-only"});
}

TEST_CASE("file-list owner sorting is natural and leaves unknown owners last")
{
  auto rows = std::vector{
      Row("unknown-z", "unknown-z"),
      Row("alice-10", "alice-10"),
      Row("unknown-a", "unknown-a"),
      Row("alice-2", "alice-2"),
  };
  rows[1].owner = "Alice10";
  rows[3].owner = "alice2";

  SortFileListRows(rows, FileListColumn::Owner, true, TestNaturalCompare);

  CHECK(Identities(rows) ==
        std::vector<std::string>{"alice-2", "alice-10", "unknown-a",
                                 "unknown-z"});

  SortFileListRows(rows, FileListColumn::Owner, false, TestNaturalCompare);

  CHECK(Identities(rows) ==
        std::vector<std::string>{"alice-10", "alice-2", "unknown-z",
                                 "unknown-a"});
}

TEST_CASE("file-list type sorting is semantic rather than translated text")
{
  auto rows = std::vector{
      Row("other", "other", FileListRowKind::Other),
      Row("directory", "directory", FileListRowKind::Directory),
      Row("symlink", "symlink", FileListRowKind::Symlink),
      Row("file", "file", FileListRowKind::File),
  };
  rows[0].type = "AAA";
  rows[1].type = "ZZZ";
  rows[2].type = "BBB";
  rows[3].type = "YYY";

  SortFileListRows(rows, FileListColumn::Type, true, TestNaturalCompare);

  CHECK(Identities(rows) ==
        std::vector<std::string>{"directory", "file", "symlink", "other"});

  SortFileListRows(rows, FileListColumn::Type, false, TestNaturalCompare);

  CHECK(Identities(rows) ==
        std::vector<std::string>{"directory", "other", "symlink", "file"});
}

TEST_CASE("file-list extensions use the final filename suffix without changing bytes",
          "[ui][file-list][type]")
{
  constexpr std::array cases{
      std::pair{std::string_view{""}, std::string_view{""}},
      std::pair{std::string_view{"README"}, std::string_view{""}},
      std::pair{std::string_view{"."}, std::string_view{""}},
      std::pair{std::string_view{".."}, std::string_view{""}},
      std::pair{std::string_view{".gitignore"}, std::string_view{""}},
      std::pair{std::string_view{".env.local"}, std::string_view{"local"}},
      std::pair{std::string_view{"archive.tar.gz"}, std::string_view{"gz"}},
      std::pair{std::string_view{"name."}, std::string_view{""}},
      std::pair{std::string_view{"name.txt."}, std::string_view{""}},
      std::pair{std::string_view{"name..txt"}, std::string_view{"txt"}},
      std::pair{std::string_view{"readme.txt"}, std::string_view{"txt"}},
      std::pair{std::string_view{"readme.TxT"}, std::string_view{"TxT"}},
      std::pair{std::string_view{"file.7z"}, std::string_view{"7z"}},
      std::pair{std::string_view{"my document.pdf"}, std::string_view{"pdf"}},
      std::pair{std::string_view{"Gr\xC3\xBC\xC3\x9F\x65.txt"}, std::string_view{"txt"}},
      std::pair{std::string_view{"file.\xE6\x96\x87\xE6\x9C\xAC"},
                std::string_view{"\xE6\x96\x87\xE6\x9C\xAC"}},
  };

  for (const auto &[filename, expected] : cases)
  {
    CAPTURE(filename);

    const auto extension = FileListExtension(filename);

    CHECK(extension == expected);

    if (!expected.empty())
    {
      CHECK(extension.data() == filename.data() + filename.size() - expected.size());
    }
  }
}

TEST_CASE("file-list type sorting groups files by extension before their names",
          "[ui][file-list][type]")
{
  auto rows = std::vector{
      Row("txt", "aaa.txt"),
      Row("gz", "zzz.tar.gz"),
      Row("pdf", "middle.pdf"),
  };
  rows[0].type = "AAA";
  rows[1].type = "ZZZ";
  rows[2].type = "MMM";

  SortFileListRows(rows, FileListColumn::Type, true, TestNaturalCompare);

  CHECK(Identities(rows) == std::vector<std::string>{"gz", "pdf", "txt"});

  for (auto &row : rows)
  {
    row.type = "Datei";
  }

  SortFileListRows(rows, FileListColumn::Type, true, TestNaturalCompare);

  CHECK(Identities(rows) == std::vector<std::string>{"gz", "pdf", "txt"});

  SortFileListRows(rows, FileListColumn::Type, false, TestNaturalCompare);

  CHECK(Identities(rows) == std::vector<std::string>{"txt", "pdf", "gz"});
}

TEST_CASE("file-list extension groups retain natural case-insensitive name and identity ties",
          "[ui][file-list][type]")
{
  auto rows = std::vector{
      Row("txt-10", "file10.TxT"),
      Row("txt-2-z", "File2.TXT"),
      Row("txt-2-a", "file2.txt"),
      Row("type-10", "a.type10"),
      Row("type-2", "z.TYPE2"),
  };

  SortFileListRows(rows, FileListColumn::Type, true, TestNaturalCompare);

  CHECK(Identities(rows) ==
        std::vector<std::string>{"txt-2-a", "txt-2-z", "txt-10",
                                 "type-2", "type-10"});

  SortFileListRows(rows, FileListColumn::Type, false, TestNaturalCompare);

  CHECK(Identities(rows) ==
        std::vector<std::string>{"type-10", "type-2", "txt-10",
                                 "txt-2-z", "txt-2-a"});
}

TEST_CASE("file-list extensionless files form one type group",
          "[ui][file-list][type]")
{
  auto rows = std::vector{
      Row("txt", "a.txt"),
      Row("readme", "README"),
      Row("dotfile", ".gitignore"),
      Row("trailing-dot", "notes."),
      Row("hidden-extension", ".env.local"),
  };

  SortFileListRows(rows, FileListColumn::Type, true, TestNaturalCompare);

  CHECK(Identities(rows) ==
        std::vector<std::string>{"dotfile", "trailing-dot", "readme",
                                 "hidden-extension", "txt"});

  SortFileListRows(rows, FileListColumn::Type, false, TestNaturalCompare);

  CHECK(Identities(rows) ==
        std::vector<std::string>{"txt", "hidden-extension", "readme",
                                 "trailing-dot", "dotfile"});
}

TEST_CASE("file-list type sorting does not treat dotted directories or links as regular files",
          "[ui][file-list][type]")
{
  auto rows = std::vector{
      Row("directory-a", "a.zip", FileListRowKind::Directory),
      Row("file-a", "a.zip"),
      Row("link-a", "a.zip", FileListRowKind::Symlink),
      Row("directory-z", "z.txt", FileListRowKind::Directory),
      Row("file-z", "z.txt"),
      Row("link-z", "z.txt", FileListRowKind::Symlink),
      Row("other", "other.dat", FileListRowKind::Other),
      Row("parent", "..", FileListRowKind::ParentDirectory),
  };

  SortFileListRows(rows, FileListColumn::Type, true, TestNaturalCompare);

  CHECK(Identities(rows) ==
        std::vector<std::string>{"parent", "directory-a", "directory-z",
                                 "file-z", "file-a", "link-a", "link-z", "other"});

  SortFileListRows(rows, FileListColumn::Type, false, TestNaturalCompare);

  CHECK(Identities(rows) ==
        std::vector<std::string>{"parent", "directory-z", "directory-a",
                                 "other", "link-z", "link-a", "file-a", "file-z"});
}
