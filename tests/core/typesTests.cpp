// SPDX-License-Identifier: MIT

#include "core/types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace havremote;

TEST_CASE("protocol and authentication values have stable configuration names")
{
  CHECK(ToString(ProtocolKind::Ftp) == "ftp");
  CHECK(ToString(ProtocolKind::FtpsExplicit) == "ftps-explicit");
  CHECK(ToString(ProtocolKind::FtpsImplicit) == "ftps-implicit");
  CHECK(ToString(ProtocolKind::Sftp) == "sftp");
  CHECK(ProtocolKindFromString("sftp") == ProtocolKind::Sftp);
  CHECK_FALSE(ProtocolKindFromString("SFTP"));
  CHECK(DefaultPort(ProtocolKind::Ftp) == 21);
  CHECK(DefaultPort(ProtocolKind::FtpsExplicit) == 21);
  CHECK(DefaultPort(ProtocolKind::FtpsImplicit) == 990);
  CHECK(DefaultPort(ProtocolKind::Sftp) == 22);

  CHECK(ToString(FtpDataConnectionMode::Passive) == "passive");
  CHECK(ToString(FtpDataConnectionMode::Active) == "active");
  CHECK(FtpDataConnectionModeFromString("passive") ==
        FtpDataConnectionMode::Passive);
  CHECK(FtpDataConnectionModeFromString("active") ==
        FtpDataConnectionMode::Active);
  CHECK_FALSE(FtpDataConnectionModeFromString("automatic"));

  CHECK(ToString(AuthenticationKind::PrivateKey) == "private-key");
  CHECK(ToString(AuthenticationKind::PasswordKeyboardInteractive) ==
        "password-keyboard-interactive");
  CHECK(AuthenticationKindFromString("agent") == AuthenticationKind::Agent);
  CHECK(AuthenticationKindFromString("password-keyboard-interactive") ==
        AuthenticationKind::PasswordKeyboardInteractive);
  CHECK_FALSE(AuthenticationKindFromString("unknown"));
}

TEST_CASE("remote paths preserve server bytes separately from display text")
{
  const std::string rawName{"bad\xFFname", 8};
  const RemotePath directory{"/incoming", "/incoming"};
  const RemotePath name{rawName, "bad�name"};
  const auto joined = directory.Joined(name);

  CHECK(joined.Bytes() == std::string{"/incoming/"} + rawName);
  CHECK(joined.DisplayUtf8() == "/incoming/bad�name");
  CHECK(joined.Parent() == directory);
  CHECK(joined.Filename() == name);
  CHECK(RemotePath{"/"}.IsRoot());
  CHECK(RemotePath{"relative"}.IsAbsolute() == false);
  CHECK(RemotePath{}.Empty());
  CHECK(RemotePath{std::string{}}.Empty());
  CHECK_FALSE(RemotePath{}.IsRoot());
}

TEST_CASE("remote child validation blocks traversal and path separators")
{
  CHECK(IsValidRemoteChildName("report.txt"));
  CHECK(IsValidRemoteChildName("line feed\nname"));
  CHECK_FALSE(IsValidRemoteChildName(""));
  CHECK_FALSE(IsValidRemoteChildName("."));
  CHECK_FALSE(IsValidRemoteChildName(".."));
  CHECK_FALSE(IsValidRemoteChildName("parent/child"));

  const std::string embeddedNull{"a\0b", 3};

  CHECK_FALSE(IsValidRemoteChildName(embeddedNull));
}

TEST_CASE("remote file revisions require exact verifiable file metadata")
{
  using namespace std::chrono_literals;

  const auto modified = std::chrono::system_clock::time_point{123456s};

  const RemoteEntry entry{
      .path = RemotePath{"/notes.txt"},
      .name = RemotePath{"notes.txt"},
      .kind = RemoteEntryKind::File,
      .size = 42,
      .modifiedAt = modified,
      .permissions = std::nullopt,
      .owner = std::nullopt,
      .group = std::nullopt,
      .hidden = false,
  };

  const auto expected = MakeRemoteFileRevision(entry);

  CHECK(expected.kind == RemoteEntryKind::File);
  CHECK(expected.size == 42);
  CHECK(expected.modifiedAt == modified);
  CHECK(CompareRemoteFileRevision(expected, std::optional{expected}) ==
        RemoteFileRevisionComparison::Matches);

  auto changed = expected;
  changed.size = 43;

  CHECK(CompareRemoteFileRevision(expected, std::optional{changed}) ==
        RemoteFileRevisionComparison::Changed);

  changed = expected;
  changed.modifiedAt = modified + 1s;

  CHECK(CompareRemoteFileRevision(expected, std::optional{changed}) ==
        RemoteFileRevisionComparison::Changed);

  auto unverifiable = expected;
  unverifiable.modifiedAt.reset();

  CHECK(CompareRemoteFileRevision(expected, std::optional{unverifiable}) ==
        RemoteFileRevisionComparison::Unverifiable);
  CHECK(CompareRemoteFileRevision(unverifiable, std::optional{expected}) ==
        RemoteFileRevisionComparison::Unverifiable);

  auto wrongKind = expected;
  wrongKind.kind = RemoteEntryKind::Directory;

  CHECK(CompareRemoteFileRevision(expected, std::optional{wrongKind}) ==
        RemoteFileRevisionComparison::WrongKind);
  CHECK(CompareRemoteFileRevision(wrongKind, std::optional{expected}) ==
        RemoteFileRevisionComparison::WrongKind);
  CHECK(CompareRemoteFileRevision(expected, std::nullopt) ==
        RemoteFileRevisionComparison::Missing);
}

TEST_CASE("SHA-256 host key fingerprints use OpenSSH notation")
{
  constexpr std::array data{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};

  CHECK(Sha256Fingerprint(data.data(), data.size()) ==
        "SHA256:ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0");
}

TEST_CASE("SHA-256 fingerprints match known digests across block boundaries")
{
  constexpr std::array<std::pair<std::size_t, std::string_view>, 13> vectors{{
      {0, "SHA256:47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU"},
      {1, "SHA256:ypeBEsobvcr6wjGzmiPcTaeG7/gUfE5yuYB3ha/uSLs"},
      {55, "SHA256:n0OQ+NMMLdkuyfCVtl4rmumwqSWlJY4kHJ8ekQ9zQxg"},
      {56, "SHA256:s1Q5pKxvCUi21vnjxq8PX1kM4g8b3nCQ73lwaG7Gc4o"},
      {63, "SHA256:fT50oF19sVvOStnsBljqmOPwbu7PFrTG//LaRX3cLzQ"},
      {64, "SHA256:/+BU/nrgy23GXDr5th1SCfQ5hR20PQulmXM33xVGaOs"},
      {65, "SHA256:Y1NhxIu56rFBmOduqKt/GkFoXWrWKqkUbTAdTxfrCuA"},
      {119, "SHA256:MeulHDE6XAgiat8Y1KNZz9/Y0ugWsT9K+VL36mWE3Ps"},
      {120, "SHA256:Lz0zVDLHC1gK8Ojhs2dKfAINaDql9zqq7f3FWvkEwhw"},
      {127, "SHA256:xX6SeK94+jyrOGZ770zinXg3h6L3MdThIgAnDwwyMgo"},
      {128, "SHA256:aDbPE7rEAOkQUHHNavRwhN+srU5eMCyUv+0k4BOvtz4"},
      {129, "SHA256:wSywJKLlVRzKDgj86PHF4xRVXMP+9jKe6ZSj23UhZq4"},
      {1'000'000, "SHA256:zcduXJkU+5KBocfihNc+Z/GAmkiklyAOBG05zMcRLNA"},
  }};

  for (const auto &[size, expected] : vectors)
  {
    CAPTURE(size);

    const std::string data(size, 'a');

    CHECK(Sha256Fingerprint(reinterpret_cast<const std::byte *>(data.data()),
                            data.size()) == expected);
  }

  CHECK(Sha256Fingerprint(nullptr, 0) == vectors.front().second);
}

TEST_CASE("SHA-256 fingerprints hash binary keys without text conversion")
{
  std::array<std::byte, 256> data{};

  for (std::size_t index = 0; index < data.size(); ++index)
  {
    data[index] = static_cast<std::byte>(index);
  }

  CHECK(Sha256Fingerprint(data.data(), data.size()) ==
        "SHA256:QK/y6dLYki5Hr9RkjmlnSXFYeF+9Hahw5xECZr+USIA");
}

TEST_CASE("generated ids are nonempty and unique")
{
  const auto first = GenerateId();
  const auto second = GenerateId();

  CHECK(first.size() == 36);
  CHECK(second.size() == 36);
  CHECK(first != second);
}

TEST_CASE("endpoint hosts exclude URL authority injection")
{
  CHECK(IsValidEndpointHost("example.com"));
  CHECK(IsValidEndpointHost("files.example.com."));
  CHECK(IsValidEndpointHost("127.0.0.1"));
  CHECK(IsValidEndpointHost("2001:db8::1"));
  CHECK(IsValidEndpointHost("::ffff:192.0.2.1"));

  CHECK_FALSE(IsValidEndpointHost("display.example@127.0.0.1"));
  CHECK_FALSE(IsValidEndpointHost("ftp://example.com"));
  CHECK_FALSE(IsValidEndpointHost("example.com:21"));
  CHECK_FALSE(IsValidEndpointHost("[2001:db8::1]"));
  CHECK_FALSE(IsValidEndpointHost("fe80::1%12"));
  CHECK_FALSE(IsValidEndpointHost("::1:"));
  CHECK_FALSE(IsValidEndpointHost("1:2:3:4:5:6:7:8:"));
  CHECK_FALSE(IsValidEndpointHost("999.0.0.1"));
  CHECK_FALSE(IsValidEndpointHost("bad host"));
  CHECK_FALSE(IsValidEndpointHost("täst.example"));
}

TEST_CASE("IP address literals exclude names and address decoration")
{
  CHECK(IsValidIpAddress("192.0.2.10"));
  CHECK(IsValidIpAddress("2001:db8::10"));
  CHECK(IsValidIpAddress("::ffff:192.0.2.10"));

  CHECK_FALSE(IsValidIpAddress(""));
  CHECK_FALSE(IsValidIpAddress("ftp.example"));
  CHECK_FALSE(IsValidIpAddress("[2001:db8::10]"));
  CHECK_FALSE(IsValidIpAddress("fe80::1%12"));
  CHECK_FALSE(IsValidIpAddress(" 192.0.2.10"));
  CHECK_FALSE(IsValidIpAddress("192.0.2.10:21"));
  CHECK_FALSE(IsValidIpAddress("ftp://192.0.2.10"));
}

TEST_CASE("endpoint identity treats host spelling case-insensitively")
{
  const SiteEndpointIdentity upper{
      ProtocolKind::Sftp, "Example.TEST", 22, "alice"};

  const SiteEndpointIdentity lower{
      ProtocolKind::Sftp, "example.test", 22, "alice"};

  CHECK(upper == lower);

  auto differentUser = lower;
  differentUser.username = "Alice";

  CHECK_FALSE(upper == differentUser);
}
