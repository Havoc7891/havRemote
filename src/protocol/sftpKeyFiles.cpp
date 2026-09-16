// SPDX-License-Identifier: MIT

#include "protocol/sftpKeyFiles.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
  constexpr std::size_t MaxDecodedKeySize = 1024U * 1024U;

  class DecodedBytes final
  {
  public:
    DecodedBytes() = default;

    ~DecodedBytes()
    {
      // The decoded PEM payload contains private key material. Volatile writes
      // keep this best-effort scrub from being optimized away before release.
      auto *const bytes = static_cast<volatile std::byte *>(mValue.data());

      for (std::size_t index = 0; index < mValue.size(); ++index)
      {
        bytes[index] = std::byte{};
      }
    }

    DecodedBytes(const DecodedBytes &) = delete;
    DecodedBytes &operator=(const DecodedBytes &) = delete;
    DecodedBytes(DecodedBytes &&) noexcept = default;
    DecodedBytes &operator=(DecodedBytes &&) = delete;

    void Reserve(const std::size_t size) { mValue.reserve(size); }
    void PushBack(const std::byte value) { mValue.push_back(value); }
    [[nodiscard]] const std::byte *Data() const noexcept { return mValue.data(); }
    [[nodiscard]] bool Empty() const noexcept { return mValue.empty(); }
    [[nodiscard]] std::size_t Size() const noexcept { return mValue.size(); }
    [[nodiscard]] std::span<const std::byte> Span() const noexcept { return mValue; }

  private:
    std::vector<std::byte> mValue;
  };

  [[nodiscard]] bool IsAsciiWhitespace(const char character) noexcept
  {
    return character == ' ' || character == '\t' || character == '\r' ||
           character == '\n';
  }

  [[nodiscard]] std::string_view TrimAsciiWhitespace(
      std::string_view value) noexcept
  {
    while (!value.empty() && IsAsciiWhitespace(value.front()))
    {
      value.remove_prefix(1U);
    }

    while (!value.empty() && IsAsciiWhitespace(value.back()))
    {
      value.remove_suffix(1U);
    }

    return value;
  }

  [[nodiscard]] char AsciiLower(const char character) noexcept
  {
    if (character >= 'A' && character <= 'Z')
    {
      return static_cast<char>(character + ('a' - 'A'));
    }

    return character;
  }

  [[nodiscard]] bool AsciiEqualIgnoreCase(std::string_view left,
                                          std::string_view right) noexcept
  {
    if (left.size() != right.size())
    {
      return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index)
    {
      if (AsciiLower(left[index]) != AsciiLower(right[index]))
      {
        return false;
      }
    }

    return true;
  }

  struct Armor final
  {
    std::string_view body;
  };

  [[nodiscard]] std::optional<Armor> ExtractArmor(
      const std::string_view contents,
      const std::string_view beginMarker,
      const std::string_view endMarker) noexcept
  {
    const auto trimmed = TrimAsciiWhitespace(contents);

    if (!trimmed.starts_with(beginMarker))
    {
      return std::nullopt;
    }

    std::size_t bodyBegin = beginMarker.size();

    if (bodyBegin >= trimmed.size())
    {
      return std::nullopt;
    }

    if (trimmed[bodyBegin] == '\r')
    {
      if (bodyBegin + 1U >= trimmed.size() || trimmed[bodyBegin + 1U] != '\n')
      {
        return std::nullopt;
      }

      bodyBegin += 2U;
    }
    else if (trimmed[bodyBegin] == '\n')
    {
      ++bodyBegin;
    }
    else
    {
      return std::nullopt;
    }

    const auto footer = trimmed.rfind(endMarker);

    if (footer == std::string_view::npos ||
        footer + endMarker.size() != trimmed.size() || footer <= bodyBegin ||
        (trimmed[footer - 1U] != '\n' && trimmed[footer - 1U] != '\r'))
    {
      return std::nullopt;
    }

    return Armor{.body = trimmed.substr(bodyBegin, footer - bodyBegin)};
  }

  [[nodiscard]] int DecodeBase64Character(const char character) noexcept
  {
    if (character >= 'A' && character <= 'Z')
    {
      return character - 'A';
    }

    if (character >= 'a' && character <= 'z')
    {
      return character - 'a' + 26;
    }

    if (character >= '0' && character <= '9')
    {
      return character - '0' + 52;
    }

    if (character == '+')
    {
      return 62;
    }

    if (character == '/')
    {
      return 63;
    }

    return -1;
  }

  [[nodiscard]] std::optional<DecodedBytes> DecodeBase64(
      const std::string_view encoded) noexcept
  {
    try
    {
      DecodedBytes decoded;
      decoded.Reserve(encoded.size() < MaxDecodedKeySize
                          ? encoded.size() * 3U / 4U
                          : MaxDecodedKeySize);

      std::array<char, 4U> quartet{};
      std::size_t quartetSize{};
      bool finished{};

      for (const char character : encoded)
      {
        if (IsAsciiWhitespace(character))
        {
          continue;
        }

        if (finished || (character != '=' && DecodeBase64Character(character) < 0))
        {
          return std::nullopt;
        }

        quartet[quartetSize++] = character;

        if (quartetSize != quartet.size())
        {
          continue;
        }

        if (quartet[0] == '=' || quartet[1] == '=')
        {
          return std::nullopt;
        }

        const bool thirdPadding = quartet[2] == '=';
        const bool fourthPadding = quartet[3] == '=';

        if (thirdPadding && !fourthPadding)
        {
          return std::nullopt;
        }

        const auto first = DecodeBase64Character(quartet[0]);
        const auto second = DecodeBase64Character(quartet[1]);
        const auto third = thirdPadding ? 0 : DecodeBase64Character(quartet[2]);
        const auto fourth = fourthPadding ? 0 : DecodeBase64Character(quartet[3]);

        if (first < 0 || second < 0 || third < 0 || fourth < 0 ||
            (thirdPadding && (second & 0x0f) != 0) ||
            (fourthPadding && !thirdPadding && (third & 0x03) != 0))
        {
          return std::nullopt;
        }

        const std::size_t bytesToAdd = thirdPadding ? 1U : (fourthPadding ? 2U : 3U);

        if (decoded.Size() > MaxDecodedKeySize - bytesToAdd)
        {
          return std::nullopt;
        }

        decoded.PushBack(static_cast<std::byte>((first << 2) | (second >> 4)));

        if (!thirdPadding)
        {
          decoded.PushBack(static_cast<std::byte>(((second & 0x0f) << 4) |
                                                  (third >> 2)));
        }

        if (!fourthPadding)
        {
          decoded.PushBack(static_cast<std::byte>(((third & 0x03) << 6) | fourth));
        }

        finished = thirdPadding || fourthPadding;

        quartetSize = 0U;
      }

      if (quartetSize != 0U || decoded.Empty())
      {
        return std::nullopt;
      }

      return std::optional<DecodedBytes>{std::move(decoded)};
    }
    catch (...)
    {
      return std::nullopt;
    }
  }

  class SshReader final
  {
  public:
    explicit SshReader(const std::span<const std::byte> bytes) noexcept
        : mBytes(bytes)
    {
    }

    [[nodiscard]] std::optional<std::uint32_t> ReadUint32() noexcept
    {
      if (mBytes.size() - mOffset < 4U)
      {
        return std::nullopt;
      }

      std::uint32_t value{};

      for (std::size_t index = 0; index < 4U; ++index)
      {
        value = static_cast<std::uint32_t>(
            (value << 8U) | std::to_integer<std::uint8_t>(mBytes[mOffset + index]));
      }

      mOffset += 4U;

      return value;
    }

    [[nodiscard]] std::optional<std::span<const std::byte>> ReadString() noexcept
    {
      const auto length = ReadUint32();

      if (!length || *length > mBytes.size() - mOffset)
      {
        return std::nullopt;
      }

      const auto value = mBytes.subspan(mOffset, *length);

      mOffset += *length;

      return value;
    }

    [[nodiscard]] std::size_t Remaining() const noexcept
    {
      return mBytes.size() - mOffset;
    }

  private:
    std::span<const std::byte> mBytes;
    std::size_t mOffset{};
  };

  [[nodiscard]] bool ByteStringEqual(const std::span<const std::byte> value,
                                     const std::string_view expected) noexcept
  {
    return value.size() == expected.size() &&
           std::memcmp(value.data(), expected.data(), value.size()) == 0;
  }

  [[nodiscard]] std::size_t AuthenticationTagSize(
      const std::span<const std::byte> cipher) noexcept
  {
    if (ByteStringEqual(cipher, "aes128-gcm@openssh.com") ||
        ByteStringEqual(cipher, "aes256-gcm@openssh.com") ||
        ByteStringEqual(cipher, "chacha20-poly1305@openssh.com"))
    {
      return 16U;
    }

    return 0U;
  }

  [[nodiscard]] havremote::sftp::PrivateKeyEncryption ClassifyOpenSsh(
      const std::string_view contents) noexcept
  {
    constexpr std::string_view Begin{"-----BEGIN OPENSSH PRIVATE KEY-----"};
    constexpr std::string_view End{"-----END OPENSSH PRIVATE KEY-----"};
    constexpr std::string_view Magic{"openssh-key-v1\0", 15U};

    const auto armor = ExtractArmor(contents, Begin, End);
    if (!armor)
    {
      return havremote::sftp::PrivateKeyEncryption::Unknown;
    }

    const auto decoded = DecodeBase64(armor->body);
    if (!decoded || decoded->Size() < Magic.size() ||
        std::memcmp(decoded->Data(), Magic.data(), Magic.size()) != 0)
    {
      return havremote::sftp::PrivateKeyEncryption::Unknown;
    }

    SshReader reader{decoded->Span().subspan(Magic.size())};

    const auto cipher = reader.ReadString();
    const auto kdf = reader.ReadString();
    const auto kdfOptions = reader.ReadString();
    const auto keyCount = reader.ReadUint32();

    // libssh2's OpenSSH private-key reader supports one key per container
    if (!cipher || !kdf || !kdfOptions || !keyCount || *keyCount != 1U)
    {
      return havremote::sftp::PrivateKeyEncryption::Unknown;
    }

    for (std::uint32_t index = 0; index < *keyCount; ++index)
    {
      const auto publicKey = reader.ReadString();

      if (!publicKey || publicKey->empty())
      {
        return havremote::sftp::PrivateKeyEncryption::Unknown;
      }
    }

    const auto privateSection = reader.ReadString();

    if (!privateSection || privateSection->empty() ||
        reader.Remaining() != AuthenticationTagSize(*cipher))
    {
      return havremote::sftp::PrivateKeyEncryption::Unknown;
    }

    const bool noCipher = ByteStringEqual(*cipher, "none");
    const bool noKdf = ByteStringEqual(*kdf, "none");

    if (noCipher && noKdf && kdfOptions->empty())
    {
      return havremote::sftp::PrivateKeyEncryption::Unencrypted;
    }

    if (!cipher->empty() && !kdf->empty() && !noCipher && !noKdf &&
        !kdfOptions->empty())
    {
      return havremote::sftp::PrivateKeyEncryption::Encrypted;
    }

    return havremote::sftp::PrivateKeyEncryption::Unknown;
  }

  struct LegacyPemBody final
  {
    std::string_view base64;
    bool encrypted{};
  };

  [[nodiscard]] std::optional<LegacyPemBody> ParseLegacyPemBody(
      const std::string_view body) noexcept
  {
    const auto firstLineEnd = body.find_first_of("\r\n");
    const auto firstLine = TrimAsciiWhitespace(body.substr(0U, firstLineEnd));

    if (firstLine.find(':') == std::string_view::npos)
    {
      return LegacyPemBody{.base64 = body, .encrypted = false};
    }

    bool procType{};
    bool dekInfo{};
    std::size_t cursor{};
    bool foundSeparator{};

    while (cursor < body.size())
    {
      const auto lineEnd = body.find('\n', cursor);
      const auto next = lineEnd == std::string_view::npos ? body.size() : lineEnd + 1U;

      auto line = body.substr(cursor, (lineEnd == std::string_view::npos
                                           ? body.size()
                                           : lineEnd) -
                                          cursor);

      if (!line.empty() && line.back() == '\r')
      {
        line.remove_suffix(1U);
      }

      line = TrimAsciiWhitespace(line);

      cursor = next;

      if (line.empty())
      {
        foundSeparator = true;

        break;
      }

      const auto colon = line.find(':');

      if (colon == std::string_view::npos)
      {
        return std::nullopt;
      }

      const auto name = TrimAsciiWhitespace(line.substr(0U, colon));
      const auto value = TrimAsciiWhitespace(line.substr(colon + 1U));

      if (AsciiEqualIgnoreCase(name, "Proc-Type"))
      {
        if (procType || !AsciiEqualIgnoreCase(value, "4,ENCRYPTED"))
        {
          return std::nullopt;
        }

        procType = true;
      }
      else if (AsciiEqualIgnoreCase(name, "DEK-Info"))
      {
        if (dekInfo || value.empty())
        {
          return std::nullopt;
        }

        dekInfo = true;
      }
      else
      {
        return std::nullopt;
      }
    }

    if (!foundSeparator || !procType || !dekInfo || cursor >= body.size())
    {
      return std::nullopt;
    }

    return LegacyPemBody{.base64 = body.substr(cursor), .encrypted = true};
  }

  [[nodiscard]] havremote::sftp::PrivateKeyEncryption ClassifyPem(
      const std::string_view contents,
      const std::string_view beginMarker,
      const std::string_view endMarker,
      const bool inherentlyEncrypted) noexcept
  {
    const auto armor = ExtractArmor(contents, beginMarker, endMarker);
    if (!armor)
    {
      return havremote::sftp::PrivateKeyEncryption::Unknown;
    }

    const auto body = ParseLegacyPemBody(armor->body);
    if (!body || (inherentlyEncrypted && body->encrypted))
    {
      return havremote::sftp::PrivateKeyEncryption::Unknown;
    }

    const auto decoded = DecodeBase64(body->base64);
    if (!decoded)
    {
      return havremote::sftp::PrivateKeyEncryption::Unknown;
    }

    if (inherentlyEncrypted || body->encrypted)
    {
      return havremote::sftp::PrivateKeyEncryption::Encrypted;
    }

    return havremote::sftp::PrivateKeyEncryption::Unencrypted;
  }
} // namespace

namespace havremote::sftp
{
  PrivateKeyEncryption DetectPrivateKeyEncryption(
      const std::string_view contents) noexcept
  {
    constexpr std::string_view OpenSshBegin{"-----BEGIN OPENSSH PRIVATE KEY-----"};
    constexpr std::string_view EncryptedPkcs8Begin{"-----BEGIN ENCRYPTED PRIVATE KEY-----"};

    struct PemMarkers final
    {
      std::string_view begin;
      std::string_view end;
    };

    constexpr std::array KnownUnencryptedPemMarkers{
        PemMarkers{"-----BEGIN PRIVATE KEY-----", "-----END PRIVATE KEY-----"},
        PemMarkers{"-----BEGIN RSA PRIVATE KEY-----",
                   "-----END RSA PRIVATE KEY-----"},
        PemMarkers{"-----BEGIN EC PRIVATE KEY-----",
                   "-----END EC PRIVATE KEY-----"},
        PemMarkers{"-----BEGIN DSA PRIVATE KEY-----",
                   "-----END DSA PRIVATE KEY-----"}};

    const auto trimmed = TrimAsciiWhitespace(contents);
    if (trimmed.starts_with(OpenSshBegin))
    {
      return ClassifyOpenSsh(trimmed);
    }
    if (trimmed.starts_with(EncryptedPkcs8Begin))
    {
      return ClassifyPem(trimmed, EncryptedPkcs8Begin,
                         "-----END ENCRYPTED PRIVATE KEY-----", true);
    }

    for (const auto &markers : KnownUnencryptedPemMarkers)
    {
      if (!trimmed.starts_with(markers.begin))
      {
        continue;
      }

      return ClassifyPem(trimmed, markers.begin, markers.end, false);
    }

    return PrivateKeyEncryption::Unknown;
  }
} // namespace havremote::sftp
