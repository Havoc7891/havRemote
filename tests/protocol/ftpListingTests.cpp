// SPDX-License-Identifier: MIT

#include "protocol/ftpSession.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <array>
#include <optional>
#include <string>
#include <utility>

using namespace havremote;

TEST_CASE("MLSD parser retains names, kinds, sizes, modes, and timestamps")
{
  constexpr std::string_view listing =
      "type=cdir;modify=20260101000000; .\r\n"
      "type=pdir;modify=20260101000000; ..\r\n"
      "type=dir;modify=20260829112233;unix.mode=0755;unix.uid=1000;unix.owner=ftp;unix.ownername=alice;unix.gid=2000;unix.group=ftp;unix.groupname=staff; assets\r\n"
      "type=file;size=4294967297;modify=20260829112234.123;unix.mode=0100640;unix.ownername=;unix.owner=;unix.uid=1001;unix.groupname=;unix.group=;unix.gid=2001; big file.bin\r\n"
      "type=OS.unix=slink;modify=20260829112235; latest\r\n";

  auto result = ftp::ParseMlsdListing(listing, RemotePath{"/pub"});

  REQUIRE(result);
  REQUIRE(result->size() == 3);

  CHECK((*result)[0].name.Bytes() == "assets");
  CHECK((*result)[0].path.Bytes() == "/pub/assets");
  CHECK((*result)[0].kind == RemoteEntryKind::Directory);
  CHECK((*result)[0].permissions == 0755);
  CHECK((*result)[0].owner == "alice");
  CHECK((*result)[0].group == "staff");
  REQUIRE((*result)[0].modifiedAt);

  CHECK((*result)[1].name.Bytes() == "big file.bin");
  CHECK((*result)[1].kind == RemoteEntryKind::File);
  CHECK((*result)[1].size == 4'294'967'297ULL);
  CHECK((*result)[1].permissions == 0640);
  CHECK((*result)[1].owner == "1001");
  CHECK((*result)[1].group == "2001");
  CHECK((*result)[2].kind == RemoteEntryKind::Symlink);
  CHECK_FALSE((*result)[2].permissions);
  CHECK_FALSE((*result)[2].owner);
  CHECK_FALSE((*result)[2].group);
}

TEST_CASE("MLSD parser accepts Python-style prefixed octal modes")
{
  constexpr std::string_view listing =
      "type=file;unix.mode=0o640; lower-prefix.txt\r\n"
      "type=file;unix.mode=0O4755; upper-prefix.txt\r\n";

  const auto result = ftp::ParseMlsdListing(listing, RemotePath{"/"});

  REQUIRE(result);
  REQUIRE(result->size() == 2U);
  CHECK((*result)[0].permissions == 0640U);
  CHECK((*result)[1].permissions == 04755U);
}

TEST_CASE("Unix LIST parser handles spaces and does not follow symlink targets")
{
  constexpr std::string_view listing =
      "drwxr-xr-x 2 owner group 4096 Aug 29 12:34 directory name\n"
      "-rw-r----- 1 owner group 123 Aug 28 2025 report final.txt\n"
      "lrwxrwxrwx 1 owner group 7 Aug 29 12:30 latest -> release\n";

  auto result = ftp::ParseListListing(listing, RemotePath{"/"});

  REQUIRE(result);
  REQUIRE(result->size() == 3);
  CHECK((*result)[0].kind == RemoteEntryKind::Directory);
  CHECK((*result)[0].name.Bytes() == "directory name");
  CHECK((*result)[0].permissions == 0755);
  CHECK((*result)[0].owner == "owner");
  CHECK((*result)[0].group == "group");
  CHECK((*result)[1].kind == RemoteEntryKind::File);
  CHECK((*result)[1].size == 123);
  CHECK((*result)[1].permissions == 0640);
  CHECK((*result)[1].owner == "owner");
  CHECK((*result)[1].group == "group");
  CHECK((*result)[2].kind == RemoteEntryKind::Symlink);
  CHECK((*result)[2].name.Bytes() == "latest");
  CHECK((*result)[2].permissions == 0777);
  CHECK((*result)[2].owner == "owner");
  CHECK((*result)[2].group == "group");
}

TEST_CASE("Unix LIST parser retains special permission bits without inventing execute bits")
{
  constexpr std::string_view listing =
      "-rwSr-s--T 1 deploy group 7 Aug 29 12:30 protected.bin\n";

  auto result = ftp::ParseListListing(listing, RemotePath{"/"});

  REQUIRE(result);
  REQUIRE(result->size() == 1U);
  CHECK(result->front().permissions == 07650);
  CHECK(result->front().owner == "deploy");
  CHECK(result->front().group == "group");
}

TEST_CASE("DOS LIST parser handles directories and large files")
{
  constexpr std::string_view listing =
      "08-29-26  01:42PM       <DIR>          Uploads\r\n"
      "08-29-2026  12:01AM       5000000000 archive.bin\r\n";

  auto result = ftp::ParseListListing(listing, RemotePath{"/home"});

  REQUIRE(result);
  REQUIRE(result->size() == 2);
  CHECK((*result)[0].kind == RemoteEntryKind::Directory);
  CHECK((*result)[0].path.Bytes() == "/home/Uploads");
  CHECK_FALSE((*result)[0].permissions);
  CHECK_FALSE((*result)[0].owner);
  CHECK_FALSE((*result)[0].group);
  CHECK((*result)[1].kind == RemoteEntryKind::File);
  CHECK((*result)[1].size == 5'000'000'000ULL);
  CHECK_FALSE((*result)[1].permissions);
  CHECK_FALSE((*result)[1].owner);
  CHECK_FALSE((*result)[1].group);
}

TEST_CASE("unrecognized nonempty LIST data reports a parser error")
{
  auto result = ftp::ParseListListing("this server format is unknown\n", RemotePath{"/"});

  REQUIRE_FALSE(result);
  CHECK(result.error().code == RemoteErrorCode::ParseError);
}

TEST_CASE("empty directories produce an empty listing")
{
  auto mlsd = ftp::ParseMlsdListing({}, RemotePath{"/empty"});

  auto list = ftp::ParseListListing({}, RemotePath{"/empty"});

  REQUIRE(mlsd);
  REQUIRE(list);
  CHECK(mlsd->empty());
  CHECK(list->empty());
}

TEST_CASE("LIST supplements missing MLSD owner and group by exact raw name")
{
  auto mlsd = ftp::ParseMlsdListing(
      "type=file;size=99;modify=20260829112234;unix.ownername=alice; report.txt\r\n"
      "type=dir;size=123;modify=20260829112235; assets\r\n",
      RemotePath{"/pub"});

  auto list = ftp::ParseListListing(
      "-rw-r----- 1 different staff 7 Aug 28 2025 report.txt\n"
      "drwxr-x--- 2 deploy release 4096 Aug 29 12:34 assets\n",
      RemotePath{"/pub"});

  REQUIRE(mlsd);
  REQUIRE(list);

  const auto reportPath = (*mlsd)[0].path;
  const auto reportSize = (*mlsd)[0].size;
  const auto reportTime = (*mlsd)[0].modifiedAt;

  ftp::SupplementMlsdOwnerGroup(*mlsd, *list);

  CHECK((*mlsd)[0].owner == "alice"); // An MLSD value always wins
  CHECK((*mlsd)[0].group == "staff");
  CHECK((*mlsd)[0].path == reportPath);
  CHECK((*mlsd)[0].size == reportSize);
  CHECK((*mlsd)[0].modifiedAt == reportTime);
  CHECK_FALSE((*mlsd)[0].permissions); // LIST cannot replace other facts

  CHECK((*mlsd)[1].owner == "deploy");
  CHECK((*mlsd)[1].group == "release");
  CHECK((*mlsd)[1].size == 123U);
  CHECK_FALSE((*mlsd)[1].permissions);
}

TEST_CASE("LIST ownership supplement rejects ambiguous or inexact matches")
{
  const std::string rawLegacyName{"legacy-\xfc.txt", 12};

  auto makeEntry = [](RemotePath name,
                      const RemoteEntryKind kind,
                      std::optional<std::string> owner = {},
                      std::optional<std::string> group = {})
  {
    RemoteEntry entry;
    entry.name = std::move(name);
    entry.kind = kind;
    entry.owner = std::move(owner);
    entry.group = std::move(group);

    return entry;
  };

  auto mlsd = std::vector<RemoteEntry>{
      makeEntry(RemotePath{"same.txt"}, RemoteEntryKind::File),
      makeEntry(RemotePath{"same.txt"}, RemoteEntryKind::File),
      makeEntry(RemotePath{"case.txt"}, RemoteEntryKind::File),
      makeEntry(RemotePath{"kind.txt"}, RemoteEntryKind::File),
      makeEntry(RemotePath{"duplicate-list.txt"}, RemoteEntryKind::File),
      makeEntry(RemotePath{rawLegacyName, "legacy-ü.txt"}, RemoteEntryKind::File),
  };

  const auto list = std::vector<RemoteEntry>{
      makeEntry(RemotePath{"same.txt"}, RemoteEntryKind::File, "one", "one"),
      makeEntry(RemotePath{"Case.txt"}, RemoteEntryKind::File, "case", "case"),
      makeEntry(RemotePath{"kind.txt"}, RemoteEntryKind::Directory, "kind", "kind"),
      makeEntry(RemotePath{"duplicate-list.txt"}, RemoteEntryKind::File,
                "first", "first"),
      makeEntry(RemotePath{"duplicate-list.txt"}, RemoteEntryKind::File,
                "second", "second"),
      makeEntry(RemotePath{rawLegacyName, "different display text"},
                RemoteEntryKind::File, "raw-owner", "raw-group"),
  };

  ftp::SupplementMlsdOwnerGroup(mlsd, list);

  CHECK_FALSE(mlsd[0].owner);
  CHECK_FALSE(mlsd[1].owner);
  CHECK_FALSE(mlsd[2].owner);
  CHECK_FALSE(mlsd[3].owner);
  CHECK_FALSE(mlsd[4].owner);
  CHECK(mlsd[5].owner == "raw-owner");
  CHECK(mlsd[5].group == "raw-group");
}

TEST_CASE("undecodable FTP owner metadata does not discard valid group metadata")
{
  const std::string invalidOwner{"\xc3\x28", 2};

  const auto mlsd = ftp::ParseMlsdListing(
      "type=file;size=1;unix.ownername=" + invalidOwner +
          ";unix.groupname=readers" +
          "; readable.txt\r\n",
      RemotePath{"/"});

  REQUIRE(mlsd);
  REQUIRE(mlsd->size() == 1U);
  CHECK(mlsd->front().name.Bytes() == "readable.txt");
  CHECK_FALSE(mlsd->front().owner);
  CHECK(mlsd->front().group == "readers");

  const auto list = ftp::ParseListListing(
      "-rw-r--r-- 1 " + invalidOwner +
          " group 1 Aug 29 12:34 readable.txt\n",
      RemotePath{"/"});

  REQUIRE(list);
  REQUIRE(list->size() == 1U);
  CHECK(list->front().name.Bytes() == "readable.txt");
  CHECK_FALSE(list->front().owner);
  CHECK(list->front().group == "group");
}

TEST_CASE("undecodable FTP group metadata does not discard valid owner metadata")
{
  const std::string invalidGroup{"\xc3\x28", 2};

  const auto mlsd = ftp::ParseMlsdListing(
      "type=file;size=1;unix.ownername=alice;unix.groupname=" +
          invalidGroup + "; readable.txt\r\n",
      RemotePath{"/"});

  REQUIRE(mlsd);
  REQUIRE(mlsd->size() == 1U);
  CHECK(mlsd->front().owner == "alice");
  CHECK_FALSE(mlsd->front().group);

  const auto list = ftp::ParseListListing(
      "-rw-r--r-- 1 alice " + invalidGroup +
          " 1 Aug 29 12:34 readable.txt\n",
      RemotePath{"/"});

  REQUIRE(list);
  REQUIRE(list->size() == 1U);
  CHECK(list->front().owner == "alice");
  CHECK_FALSE(list->front().group);
}

TEST_CASE("FTP text encoding names are explicit and UTF-8 stays lossless")
{
  const auto utf8 = ftp::ResolveTextEncoding("utf8");

  REQUIRE(utf8);
  CHECK(utf8->canonicalName == "UTF-8");
  CHECK(utf8->windowsCodePage == 65001U);
  CHECK(utf8->utf8);

  const auto windows = ftp::ResolveTextEncoding("windows-1252");

  REQUIRE(windows);
  CHECK(windows->windowsCodePage == 1252U);
  CHECK(windows->canonicalName == "windows-1252");

  const auto iana = ftp::ResolveTextEncoding("ISO-8859-1");

  REQUIRE(iana);
  CHECK(iana->windowsCodePage == 28591U);
  CHECK(iana->canonicalName == "ISO-8859-1");

  constexpr std::array rejected{
      "",
      "UTF-16LE",
      "UTF-7",
      "ISO-2022-JP",
      "windows-99999",
      "not-a-code-page",
      "windows-1252\r\n",
  };

  for (const std::string_view name : rejected)
  {
    INFO("encoding=" << name);

    const auto result = ftp::ResolveTextEncoding(name);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == RemoteErrorCode::InvalidArgument);
  }

  constexpr std::string_view unicodeName = "Gr\xc3\xbc\xc3\x9f"
                                           "e.txt";

  const auto encoded = ftp::EncodeServerText(unicodeName, *utf8);

  REQUIRE(encoded);
  CHECK(*encoded == unicodeName);

  const auto decoded = ftp::DecodeServerText(*encoded, *utf8);

  REQUIRE(decoded);
  CHECK(*decoded == unicodeName);

  const std::string invalidUtf8{"\xc3\x28", 2};

  CHECK_FALSE(ftp::EncodeServerText(invalidUtf8, *utf8));
  CHECK_FALSE(ftp::DecodeServerText(invalidUtf8, *utf8));
}

TEST_CASE("legacy FTP listings retain raw bytes and expose display UTF-8")
{
  const auto encoding = ftp::ResolveTextEncoding("CP1252");

  REQUIRE(encoding);

  const std::string rawName =
      std::string{"Gr"} + static_cast<char>(0xfc) + static_cast<char>(0xdf) +
      "e.txt";

  constexpr std::string_view displayName = "Gr\xc3\xbc\xc3\x9f"
                                           "e.txt";

  const auto encoded = ftp::EncodeServerText(displayName, *encoding);

  REQUIRE(encoded);
  CHECK(*encoded == rawName);

  const auto decoded = ftp::DecodeServerText(rawName, *encoding);

  REQUIRE(decoded);
  CHECK(*decoded == displayName);

  const std::string rawOwner =
      std::string{"J"} + static_cast<char>(0xf6) + "rg";

  const std::string displayOwner =
      std::string{"J"} + static_cast<char>(0xc3) +
      static_cast<char>(0xb6) + "rg";

  const std::string rawGroup =
      std::string{"B"} + static_cast<char>(0xfc) + "ro";

  const std::string displayGroup =
      std::string{"B"} + static_cast<char>(0xc3) +
      static_cast<char>(0xbc) + "ro";

  const auto listing = "type=file;size=7;unix.uid=1000;unix.ownername=" +
                       rawOwner + ";unix.groupname=" + rawGroup +
                       "; " + rawName + "\r\n";

  const auto parsed = ftp::ParseMlsdListing(
      listing, RemotePath{"/daten", "/daten"}, *encoding);

  REQUIRE(parsed);
  REQUIRE(parsed->size() == 1U);
  CHECK(parsed->front().name.Bytes() == rawName);
  CHECK(parsed->front().name.DisplayUtf8() == displayName);
  CHECK(parsed->front().path.Bytes() == "/daten/" + rawName);
  CHECK(parsed->front().path.DisplayUtf8() == "/daten/" +
                                                  std::string{displayName});
  REQUIRE(parsed->front().owner);
  CHECK(*parsed->front().owner == displayOwner);
  REQUIRE(parsed->front().group);
  CHECK(*parsed->front().group == displayGroup);

  // CP1252 has no exact representation for this CJK character. Best-fit or
  // default-character substitution must never alter a typed remote path.
  const auto unrepresentable = ftp::EncodeServerText("\xe6\xbc\xa2", *encoding);

  REQUIRE_FALSE(unrepresentable);
  CHECK(unrepresentable.error().code == RemoteErrorCode::InvalidArgument);

  // EBCDIC does not preserve FTP's ASCII command/path syntax and is refused
  // even when the code page happens to be installed.
  CHECK_FALSE(ftp::ResolveTextEncoding("CP500"));
}

TEST_CASE("legacy FTP conversions reject invalid bytes and best-fit substitutions")
{
  const auto ascii = ftp::ResolveTextEncoding("ASCII");

  REQUIRE(ascii);

  const auto nonAscii = ftp::EncodeServerText("Gr\xc3\xbc\xc3\x9f"
                                              "e",
                                              *ascii);

  REQUIRE_FALSE(nonAscii);
  CHECK(nonAscii.error().code == RemoteErrorCode::InvalidArgument);

  const auto nonAsciiBytes = ftp::DecodeServerText(std::string{"\xfc", 1U}, *ascii);

  REQUIRE_FALSE(nonAsciiBytes);
  CHECK(nonAsciiBytes.error().code == RemoteErrorCode::ParseError);

  const auto empty = ftp::EncodeServerText("", *ascii);

  REQUIRE(empty);
  CHECK(empty->empty());

  const auto shiftJis = ftp::ResolveTextEncoding("CP932");

  REQUIRE(shiftJis);

  const auto invalid = ftp::DecodeServerText(std::string{"\x81", 1U}, *shiftJis);

  REQUIRE_FALSE(invalid);
  CHECK(invalid.error().code == RemoteErrorCode::ParseError);

  const auto japanese = ftp::EncodeServerText("\xe6\xbc\xa2", *shiftJis);

  REQUIRE(japanese);
  CHECK(*japanese == std::string{"\x8a\xbf", 2U});

  const auto decoded = ftp::DecodeServerText(*japanese, *shiftJis);

  REQUIRE(decoded);
  CHECK(*decoded == "\xe6\xbc\xa2");
}
