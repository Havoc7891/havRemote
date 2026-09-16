// SPDX-License-Identifier: MIT

#include "protocol/sftpHostKeyPolicy.hpp"
#include "protocol/sftpSession.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

using namespace havremote;

TEST_CASE("SSH authentication method matching uses complete tokens")
{
  CHECK(sftp::AuthenticationMethodOffered(
      "password,keyboard-interactive,publickey", "password"));
  CHECK(sftp::AuthenticationMethodOffered(
      "password,keyboard-interactive,publickey", "keyboard-interactive"));
  CHECK(sftp::AuthenticationMethodOffered("keyboard-interactive",
                                          "keyboard-interactive"));
  CHECK_FALSE(sftp::AuthenticationMethodOffered(
      "password-vendor,keyboard-interactive-extra", "password"));
  CHECK_FALSE(sftp::AuthenticationMethodOffered("password", "pass"));
  CHECK_FALSE(sftp::AuthenticationMethodOffered("password,", ""));
}

TEST_CASE("SFTP long entries provide named owner and group metadata")
{
  const auto parsed = sftp::ParseLongEntryOwnerGroup(
      "-rw-r----- 1 alice developers 123 Aug 30 12:34 report.txt");

  REQUIRE(parsed);
  CHECK(parsed->owner == "alice");
  CHECK(parsed->group == "developers");
}

TEST_CASE("SFTP long-entry ownership accepts standard mode suffixes and UTF-8")
{
  const auto parsed = sftp::ParseLongEntryOwnerGroup(
      "drwxr-sr-x+ 2 Ren\xc3\xa9 Entwickler 4096 Aug 30 12:34 Projekt");

  REQUIRE(parsed);
  CHECK(parsed->owner == "Ren\xc3\xa9");
  CHECK(parsed->group == "Entwickler");
}

TEST_CASE("SFTP long-entry ownership parser rejects ambiguous metadata")
{
  const std::string invalidUtf8{"alice\xff", 6U};

  CHECK_FALSE(sftp::ParseLongEntryOwnerGroup("report.txt"));
  CHECK_FALSE(sftp::ParseLongEntryOwnerGroup(
      "-rw-r----- links alice developers 123 Aug 30 report.txt"));
  CHECK_FALSE(sftp::ParseLongEntryOwnerGroup(
      "not-a-mode 1 alice developers 123 Aug 30 report.txt"));
  CHECK_FALSE(sftp::ParseLongEntryOwnerGroup(
      "-rw-r----- 1 ? developers 123 Aug 30 report.txt"));
  CHECK_FALSE(sftp::ParseLongEntryOwnerGroup(
      "-rw-r----- 1 " + invalidUtf8 +
      " developers 123 Aug 30 report.txt"));
}

TEST_CASE("SFTP host-key policy promotes Ed25519 without removing fallbacks")
{
  constexpr std::array supported{
      "ecdsa-sha2-nistp256",
      "ecdsa-sha2-nistp384",
      "ecdsa-sha2-nistp521",
      "ecdsa-sha2-nistp256-cert-v01@openssh.com",
      "ssh-ed25519",
      "ssh-ed25519-cert-v01@openssh.com",
      "rsa-sha2-512",
      "rsa-sha2-256",
      "ssh-rsa",
      "ssh-dss",
  };

  CHECK(sftp::MakeHostKeyAlgorithmPreference(supported) ==
        "ssh-ed25519,"
        "ecdsa-sha2-nistp256,"
        "ecdsa-sha2-nistp384,"
        "ecdsa-sha2-nistp521,"
        "ecdsa-sha2-nistp256-cert-v01@openssh.com,"
        "ssh-ed25519-cert-v01@openssh.com,"
        "rsa-sha2-512,"
        "rsa-sha2-256,"
        "ssh-rsa,"
        "ssh-dss");
}

TEST_CASE("SFTP host-key policy preserves an already preferred or absent Ed25519")
{
  constexpr std::array alreadyPreferred{
      "ssh-ed25519", "ecdsa-sha2-nistp256", "rsa-sha2-512"};

  constexpr std::array withoutEd25519{
      "ecdsa-sha2-nistp256", "rsa-sha2-512", "ssh-rsa"};

  constexpr std::array<const char *, 0> empty{};

  CHECK(sftp::MakeHostKeyAlgorithmPreference(alreadyPreferred) ==
        "ssh-ed25519,ecdsa-sha2-nistp256,rsa-sha2-512");
  CHECK(sftp::MakeHostKeyAlgorithmPreference(withoutEd25519) ==
        "ecdsa-sha2-nistp256,rsa-sha2-512,ssh-rsa");
  CHECK(sftp::MakeHostKeyAlgorithmPreference(empty).empty());
}
