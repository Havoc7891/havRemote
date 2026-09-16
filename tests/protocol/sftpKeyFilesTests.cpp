// SPDX-License-Identifier: MIT

#include "protocol/sftpKeyFiles.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

using namespace havremote;

namespace
{
  void AppendUint32(std::string &output, const std::uint32_t value)
  {
    output.push_back(static_cast<char>((value >> 24U) & 0xffU));
    output.push_back(static_cast<char>((value >> 16U) & 0xffU));
    output.push_back(static_cast<char>((value >> 8U) & 0xffU));
    output.push_back(static_cast<char>(value & 0xffU));
  }

  void AppendSshString(std::string &output, const std::string_view value)
  {
    AppendUint32(output, static_cast<std::uint32_t>(value.size()));
    output.append(value);
  }

  [[nodiscard]] std::string EncodeBase64(const std::string_view bytes)
  {
    constexpr std::string_view Alphabet{
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};

    std::string encoded;
    encoded.reserve((bytes.size() + 2U) / 3U * 4U);

    for (std::size_t offset = 0; offset < bytes.size(); offset += 3U)
    {
      const auto first = static_cast<unsigned char>(bytes[offset]);

      const auto second = offset + 1U < bytes.size()
                              ? static_cast<unsigned char>(bytes[offset + 1U])
                              : 0U;

      const auto third = offset + 2U < bytes.size()
                             ? static_cast<unsigned char>(bytes[offset + 2U])
                             : 0U;

      encoded.push_back(Alphabet[first >> 2U]);
      encoded.push_back(Alphabet[((first & 0x03U) << 4U) | (second >> 4U)]);
      encoded.push_back(offset + 1U < bytes.size()
                            ? Alphabet[((second & 0x0fU) << 2U) | (third >> 6U)]
                            : '=');
      encoded.push_back(offset + 2U < bytes.size() ? Alphabet[third & 0x3fU] : '=');
    }

    return encoded;
  }

  [[nodiscard]] std::string MakeOpenSshKey(const std::string_view cipher,
                                           const std::string_view kdf,
                                           const std::string_view kdfOptions,
                                           const std::string_view trailing = {},
                                           const std::uint32_t keyCount = 1U)
  {
    std::string binary{"openssh-key-v1\0", 15U};

    AppendSshString(binary, cipher);
    AppendSshString(binary, kdf);
    AppendSshString(binary, kdfOptions);

    AppendUint32(binary, keyCount);

    for (std::uint32_t index = 0; index < keyCount; ++index)
    {
      AppendSshString(binary, "public-key-fixture");
    }

    AppendSshString(binary, "private-key-fixture");

    binary.append(trailing);

    return "-----BEGIN OPENSSH PRIVATE KEY-----\n" + EncodeBase64(binary) +
           "\n-----END OPENSSH PRIVATE KEY-----\n";
  }

  [[nodiscard]] std::string MakePem(const std::string_view label,
                                    const std::string_view body)
  {
    return "-----BEGIN " + std::string{label} + "-----\r\n" +
           std::string{body} + "\r\n-----END " + std::string{label} + "-----\r\n";
  }
} // namespace

TEST_CASE("OpenSSH v1 private-key encryption is read from cipher and KDF fields",
          "[protocol][sftp][key-files]")
{
  CHECK(sftp::DetectPrivateKeyEncryption(MakeOpenSshKey("none", "none", {})) ==
        sftp::PrivateKeyEncryption::Unencrypted);
  CHECK(sftp::DetectPrivateKeyEncryption(
            MakeOpenSshKey("aes256-ctr", "bcrypt", "bcrypt-options")) ==
        sftp::PrivateKeyEncryption::Encrypted);
}

TEST_CASE("Inconsistent or malformed OpenSSH v1 keys have unknown encryption",
          "[protocol][sftp][key-files]")
{
  CHECK(sftp::DetectPrivateKeyEncryption(
            MakeOpenSshKey("none", "bcrypt", "bcrypt-options")) ==
        sftp::PrivateKeyEncryption::Unknown);
  CHECK(sftp::DetectPrivateKeyEncryption(MakeOpenSshKey("none", "none", "unexpected")) ==
        sftp::PrivateKeyEncryption::Unknown);
  CHECK(sftp::DetectPrivateKeyEncryption(
            MakeOpenSshKey("none", "none", {}, {}, 2U)) ==
        sftp::PrivateKeyEncryption::Unknown);
  CHECK(sftp::DetectPrivateKeyEncryption(
            "-----BEGIN OPENSSH PRIVATE KEY-----\n!!!!\n"
            "-----END OPENSSH PRIVATE KEY-----\n") ==
        sftp::PrivateKeyEncryption::Unknown);

  auto truncated = MakeOpenSshKey("aes256-ctr", "bcrypt", "options");
  truncated.erase(truncated.find("-----END"));

  CHECK(sftp::DetectPrivateKeyEncryption(truncated) ==
        sftp::PrivateKeyEncryption::Unknown);
}

TEST_CASE("OpenSSH v1 AEAD private keys retain their authentication tag",
          "[protocol][sftp][key-files]")
{
  const std::string authenticationTag(16U, '\x5a');

  CHECK(sftp::DetectPrivateKeyEncryption(MakeOpenSshKey(
            "aes256-gcm@openssh.com", "bcrypt", "bcrypt-options",
            authenticationTag)) == sftp::PrivateKeyEncryption::Encrypted);

  CHECK(sftp::DetectPrivateKeyEncryption(MakeOpenSshKey(
            "aes256-gcm@openssh.com", "bcrypt", "bcrypt-options",
            authenticationTag.substr(1U))) == sftp::PrivateKeyEncryption::Unknown);
  CHECK(sftp::DetectPrivateKeyEncryption(MakeOpenSshKey(
            "aes256-ctr", "bcrypt", "bcrypt-options", authenticationTag)) ==
        sftp::PrivateKeyEncryption::Unknown);
}

TEST_CASE("Encrypted PKCS8 private keys are recognized",
          "[protocol][sftp][key-files]")
{
  CHECK(sftp::DetectPrivateKeyEncryption(
            MakePem("ENCRYPTED PRIVATE KEY", "MAMCAQE=")) ==
        sftp::PrivateKeyEncryption::Encrypted);
}

TEST_CASE("Legacy PEM Proc-Type encryption markers are recognized",
          "[protocol][sftp][key-files]")
{
  const auto encrypted = MakePem(
      "RSA PRIVATE KEY",
      "Proc-Type: 4,ENCRYPTED\r\n"
      "DEK-Info: AES-256-CBC,0011223344556677\r\n"
      "\r\n"
      "MAMCAQE=");

  CHECK(sftp::DetectPrivateKeyEncryption(encrypted) ==
        sftp::PrivateKeyEncryption::Encrypted);
}

TEST_CASE("Known unencrypted PEM private-key containers are recognized",
          "[protocol][sftp][key-files]")
{
  constexpr std::array labels{
      std::string_view{"PRIVATE KEY"}, std::string_view{"RSA PRIVATE KEY"},
      std::string_view{"EC PRIVATE KEY"}, std::string_view{"DSA PRIVATE KEY"}};

  for (const auto label : labels)
  {
    CAPTURE(label);
    CHECK(sftp::DetectPrivateKeyEncryption(MakePem(label, "MAMCAQE=")) ==
          sftp::PrivateKeyEncryption::Unencrypted);
  }
}

TEST_CASE("Unknown and malformed PEM input has unknown encryption",
          "[protocol][sftp][key-files]")
{
  CHECK(sftp::DetectPrivateKeyEncryption({}) ==
        sftp::PrivateKeyEncryption::Unknown);
  CHECK(sftp::DetectPrivateKeyEncryption(
            MakePem("CERTIFICATE", "MAMCAQE=")) ==
        sftp::PrivateKeyEncryption::Unknown);
  CHECK(sftp::DetectPrivateKeyEncryption(
            MakePem("PRIVATE KEY", "not base64")) ==
        sftp::PrivateKeyEncryption::Unknown);
  CHECK(sftp::DetectPrivateKeyEncryption(
            "-----BEGIN ENCRYPTED PRIVATE KEY-----\nMAMCAQE=\n") ==
        sftp::PrivateKeyEncryption::Unknown);
  CHECK(sftp::DetectPrivateKeyEncryption(MakePem(
            "EC PRIVATE KEY",
            "Proc-Type: 4,ENCRYPTED\n\nMAMCAQE=")) ==
        sftp::PrivateKeyEncryption::Unknown);
}
