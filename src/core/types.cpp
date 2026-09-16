// SPDX-License-Identifier: MIT

#include "core/types.hpp"

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace havremote
{
  namespace
  {
    [[nodiscard]] std::string TrimTrailingSlashes(std::string value)
    {
      while (value.size() > 1 && value.back() == '/')
      {
        value.pop_back();
      }

      return value;
    }

    [[nodiscard]] bool ValidIpv4(const std::string_view value) noexcept
    {
      std::size_t position = 0;
      unsigned int components = 0;

      while (position < value.size())
      {
        const auto dot = value.find('.', position);
        const auto end = dot == std::string_view::npos ? value.size() : dot;
        const auto piece = value.substr(position, end - position);

        if (piece.empty() || piece.size() > 3 ||
            (piece.size() > 1 && piece.front() == '0'))
        {
          return false;
        }

        unsigned int number{};

        const auto parsed = std::from_chars(piece.data(), piece.data() + piece.size(), number);

        if (parsed.ec != std::errc{} || parsed.ptr != piece.data() + piece.size() || number > 255)
        {
          return false;
        }

        ++components;

        if (dot == std::string_view::npos)
        {
          break;
        }

        position = dot + 1;
      }
      return components == 4;
    }

    [[nodiscard]] bool ValidIpv6Side(const std::string_view side,
                                     const bool isFinalSide,
                                     unsigned int &groups) noexcept
    {
      if (side.empty())
      {
        return true;
      }

      // An empty side is valid only when it came from the surrounding "::"
      // compression split. A non-empty side ending in ':' represents a second,
      // unpaired empty group and must not be accepted.
      if (side.back() == ':')
      {
        return false;
      }

      std::size_t position = 0;

      while (position < side.size())
      {
        const auto colon = side.find(':', position);
        const auto end = colon == std::string_view::npos ? side.size() : colon;
        const auto piece = side.substr(position, end - position);

        if (piece.empty())
        {
          return false;
        }

        if (piece.find('.') != std::string_view::npos)
        {
          if (colon != std::string_view::npos || !isFinalSide || !ValidIpv4(piece))
          {
            return false;
          }

          groups += 2;
        }
        else
        {
          if (piece.size() > 4 || !std::ranges::all_of(piece, [](const unsigned char c)
                                                       { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                                                                (c >= 'A' && c <= 'F'); }))
          {
            return false;
          }

          ++groups;
        }

        if (colon == std::string_view::npos)
        {
          break;
        }

        position = colon + 1;
      }

      return true;
    }

    [[nodiscard]] bool ValidIpv6(const std::string_view value) noexcept
    {
      if (value.empty())
      {
        return false;
      }

      const auto compression = value.find("::");
      if (compression != std::string_view::npos &&
          value.find("::", compression + 2) != std::string_view::npos)
      {
        return false;
      }

      unsigned int groups = 0;

      if (compression == std::string_view::npos)
      {
        return ValidIpv6Side(value, true, groups) && groups == 8;
      }

      const auto left = value.substr(0, compression);
      const auto right = value.substr(compression + 2);

      if (!ValidIpv6Side(left, false, groups) || !ValidIpv6Side(right, true, groups))
      {
        return false;
      }

      // A double colon must compress at least one 16-bit group
      return groups < 8;
    }

    [[nodiscard]] std::array<std::byte, SHA256_DIGEST_LENGTH> Sha256(
        const std::byte *data, std::size_t size)
    {
      std::array<std::byte, SHA256_DIGEST_LENGTH> result{};

      unsigned int digestSize = 0;

      if (EVP_Digest(data, size, reinterpret_cast<unsigned char *>(result.data()),
                     &digestSize, EVP_sha256(), nullptr) != 1 ||
          digestSize != result.size())
      {
        throw std::runtime_error("Could not compute the SHA-256 fingerprint");
      }

      return result;
    }

    [[nodiscard]] std::string Base64(const std::byte *data, std::size_t size)
    {
      static constexpr std::string_view alphabet =
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

      std::string output;
      output.reserve(((size + 2U) / 3U) * 4U);

      for (std::size_t index = 0; index < size; index += 3U)
      {
        const auto a = std::to_integer<unsigned>(data[index]);
        const auto b = index + 1U < size ? std::to_integer<unsigned>(data[index + 1U]) : 0U;
        const auto c = index + 2U < size ? std::to_integer<unsigned>(data[index + 2U]) : 0U;
        const auto value = (a << 16U) | (b << 8U) | c;

        output.push_back(alphabet[(value >> 18U) & 0x3fU]);
        output.push_back(alphabet[(value >> 12U) & 0x3fU]);
        output.push_back(index + 1U < size ? alphabet[(value >> 6U) & 0x3fU] : '=');
        output.push_back(index + 2U < size ? alphabet[value & 0x3fU] : '=');
      }

      return output;
    }
  } // namespace

  std::string_view ToString(ProtocolKind protocol) noexcept
  {
    switch (protocol)
    {
    case ProtocolKind::Ftp:
      return "ftp";

    case ProtocolKind::FtpsExplicit:
      return "ftps-explicit";

    case ProtocolKind::FtpsImplicit:
      return "ftps-implicit";

    case ProtocolKind::Sftp:
      return "sftp";
    }

    return "unknown";
  }

  std::optional<ProtocolKind> ProtocolKindFromString(std::string_view value) noexcept
  {
    if (value == "ftp")
    {
      return ProtocolKind::Ftp;
    }

    if (value == "ftps-explicit")
    {
      return ProtocolKind::FtpsExplicit;
    }

    if (value == "ftps-implicit")
    {
      return ProtocolKind::FtpsImplicit;
    }

    if (value == "sftp")
    {
      return ProtocolKind::Sftp;
    }

    return std::nullopt;
  }

  std::uint16_t DefaultPort(ProtocolKind protocol) noexcept
  {
    switch (protocol)
    {
    case ProtocolKind::Ftp:
    case ProtocolKind::FtpsExplicit:
      return 21;

    case ProtocolKind::FtpsImplicit:
      return 990;

    case ProtocolKind::Sftp:
      return 22;
    }

    return 0;
  }

  std::string_view ToString(const FtpDataConnectionMode mode) noexcept
  {
    switch (mode)
    {
    case FtpDataConnectionMode::Passive:
      return "passive";

    case FtpDataConnectionMode::Active:
      return "active";
    }

    return "unknown";
  }

  std::optional<FtpDataConnectionMode> FtpDataConnectionModeFromString(
      const std::string_view value) noexcept
  {
    if (value == "passive")
    {
      return FtpDataConnectionMode::Passive;
    }

    if (value == "active")
    {
      return FtpDataConnectionMode::Active;
    }

    return std::nullopt;
  }

  bool IsValidEndpointHost(const std::string_view host) noexcept
  {
    if (host.empty())
    {
      return false;
    }

    if (host.find(':') != std::string_view::npos)
    {
      return ValidIpv6(host);
    }

    if (ValidIpv4(host))
    {
      return true;
    }

    if (std::ranges::all_of(host, [](const unsigned char character)
                            { return (character >= '0' && character <= '9') || character == '.'; }))
    {
      return false;
    }

    auto dns = host;
    if (dns.ends_with('.'))
    {
      dns.remove_suffix(1);
    }
    if (dns.empty() || dns.size() > 253)
    {
      return false;
    }

    std::size_t position = 0;

    while (position < dns.size())
    {
      const auto dot = dns.find('.', position);
      const auto end = dot == std::string_view::npos ? dns.size() : dot;
      const auto label = dns.substr(position, end - position);
      const auto alphaNumeric = [](const unsigned char character)
      {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9');
      };

      if (label.empty() || label.size() > 63 || !alphaNumeric(label.front()) ||
          !alphaNumeric(label.back()) ||
          !std::ranges::all_of(label, [alphaNumeric](const unsigned char character)
                               { return alphaNumeric(character) || character == '-'; }))
      {
        return false;
      }

      if (dot == std::string_view::npos)
      {
        break;
      }

      position = dot + 1;
    }

    return true;
  }

  bool IsValidIpAddress(const std::string_view address) noexcept
  {
    if (address.empty())
    {
      return false;
    }

    return address.find(':') != std::string_view::npos
               ? ValidIpv6(address)
               : ValidIpv4(address);
  }

  SiteEndpointIdentity EndpointIdentity(const SiteProfile &site)
  {
    return SiteEndpointIdentity{site.protocol, site.host, site.port, site.username};
  }

  bool operator==(const SiteEndpointIdentity &left,
                  const SiteEndpointIdentity &right) noexcept
  {
    return left.protocol == right.protocol && left.port == right.port &&
           left.username == right.username &&
           std::ranges::equal(
               left.host, right.host,
               [](const unsigned char lhs, const unsigned char rhs)
               {
                 const auto lower = [](const unsigned char value)
                 {
                   return value >= 'A' && value <= 'Z'
                              ? static_cast<unsigned char>(value - 'A' + 'a')
                              : value;
                 };
                 return lower(lhs) == lower(rhs);
               });
  }

  std::string_view ToString(AuthenticationKind kind) noexcept
  {
    switch (kind)
    {
    case AuthenticationKind::Password:
      return "password";

    case AuthenticationKind::PasswordKeyboardInteractive:
      return "password-keyboard-interactive";

    case AuthenticationKind::PrivateKey:
      return "private-key";

    case AuthenticationKind::Agent:
      return "agent";

    case AuthenticationKind::KeyboardInteractive:
      return "keyboard-interactive";
    }

    return "unknown";
  }

  std::optional<AuthenticationKind> AuthenticationKindFromString(std::string_view value) noexcept
  {
    if (value == "password")
    {
      return AuthenticationKind::Password;
    }

    if (value == "password-keyboard-interactive")
    {
      return AuthenticationKind::PasswordKeyboardInteractive;
    }

    if (value == "private-key")
    {
      return AuthenticationKind::PrivateKey;
    }

    if (value == "agent")
    {
      return AuthenticationKind::Agent;
    }

    if (value == "keyboard-interactive")
    {
      return AuthenticationKind::KeyboardInteractive;
    }

    return std::nullopt;
  }

  RemotePath::RemotePath() = default;

  RemotePath::RemotePath(std::string bytes)
      : mBytes(std::move(bytes)), mDisplayUtf8(mBytes) {}

  RemotePath::RemotePath(std::string bytes, std::string displayUtf8)
      : mBytes(std::move(bytes)),
        mDisplayUtf8(displayUtf8.empty() ? mBytes : std::move(displayUtf8)) {}

  RemotePath RemotePath::Root() { return RemotePath{"/"}; }
  const std::string &RemotePath::Bytes() const noexcept { return mBytes; }
  const std::string &RemotePath::DisplayUtf8() const noexcept { return mDisplayUtf8; }
  bool RemotePath::Empty() const noexcept { return mBytes.empty(); }
  bool RemotePath::IsAbsolute() const noexcept { return !mBytes.empty() && mBytes.front() == '/'; }
  bool RemotePath::IsRoot() const noexcept { return TrimTrailingSlashes(mBytes) == "/"; }

  RemotePath RemotePath::Parent() const
  {
    if (Empty())
    {
      return {};
    }

    auto bytes = TrimTrailingSlashes(mBytes);
    auto display = TrimTrailingSlashes(mDisplayUtf8);

    const auto bytesSlash = bytes.find_last_of('/');
    const auto displaySlash = display.find_last_of('/');

    bytes = bytesSlash == std::string::npos ? "/" : bytes.substr(0, std::max<std::size_t>(1, bytesSlash));
    display = displaySlash == std::string::npos
                  ? "/"
                  : display.substr(0, std::max<std::size_t>(1, displaySlash));

    return RemotePath{std::move(bytes), std::move(display)};
  }

  RemotePath RemotePath::Filename() const
  {
    if (Empty())
    {
      return {};
    }

    const auto bytes = TrimTrailingSlashes(mBytes);
    const auto display = TrimTrailingSlashes(mDisplayUtf8);
    const auto bytesSlash = bytes.find_last_of('/');
    const auto displaySlash = display.find_last_of('/');

    return RemotePath{bytes.substr(bytesSlash == std::string::npos ? 0 : bytesSlash + 1U),
                      display.substr(displaySlash == std::string::npos ? 0 : displaySlash + 1U)};
  }

  RemotePath RemotePath::Joined(const RemotePath &child) const
  {
    auto join = [](const std::string &parent, const std::string &name)
    {
      if (name.empty())
      {
        return parent;
      }

      if (name.front() == '/')
      {
        return name;
      }

      if (parent.empty() || parent == "/")
      {
        return std::string{"/"} + name;
      }

      return TrimTrailingSlashes(parent) + "/" + name;
    };

    return RemotePath{join(mBytes, child.mBytes), join(mDisplayUtf8, child.mDisplayUtf8)};
  }

  RemoteFileRevision MakeRemoteFileRevision(const RemoteEntry &entry) noexcept
  {
    return RemoteFileRevision{
        .kind = entry.kind,
        .size = entry.size,
        .modifiedAt = entry.modifiedAt,
    };
  }

  RemoteFileRevisionComparison CompareRemoteFileRevision(
      const RemoteFileRevision &expected,
      const std::optional<RemoteFileRevision> &current) noexcept
  {
    if (!current)
    {
      return RemoteFileRevisionComparison::Missing;
    }

    if (expected.kind != RemoteEntryKind::File ||
        current->kind != RemoteEntryKind::File)
    {
      return RemoteFileRevisionComparison::WrongKind;
    }

    if (!expected.modifiedAt || !current->modifiedAt)
    {
      return RemoteFileRevisionComparison::Unverifiable;
    }

    if (expected.size != current->size ||
        expected.modifiedAt != current->modifiedAt)
    {
      return RemoteFileRevisionComparison::Changed;
    }

    return RemoteFileRevisionComparison::Matches;
  }

  std::string GenerateId()
  {
    static std::atomic<std::uint64_t> sequence{};
    static thread_local std::mt19937_64 random{std::random_device{}()};

    std::array<std::uint8_t, 16> bytes{};

    const auto first = random();
    const auto second = random() ^ sequence.fetch_add(1, std::memory_order_relaxed);

    std::copy_n(reinterpret_cast<const std::uint8_t *>(&first), 8, bytes.begin());
    std::copy_n(reinterpret_cast<const std::uint8_t *>(&second), 8, bytes.begin() + 8);

    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);

    std::ostringstream output;
    output << std::hex << std::setfill('0');

    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
      output << std::setw(2) << static_cast<unsigned>(bytes[index]);

      if (index == 3 || index == 5 || index == 7 || index == 9)
      {
        output << '-';
      }
    }

    return output.str();
  }

  std::string Sha256Fingerprint(const std::byte *data, std::size_t size)
  {
    const auto digest = Sha256(data, size);

    auto encoded = Base64(digest.data(), digest.size());

    while (!encoded.empty() && encoded.back() == '=')
    {
      encoded.pop_back();
    }

    return "SHA256:" + encoded;
  }

  bool IsValidRemoteChildName(std::string_view bytes) noexcept
  {
    return !bytes.empty() && bytes != "." && bytes != ".." &&
           bytes.find('/') == std::string_view::npos &&
           bytes.find('\0') == std::string_view::npos;
  }
} // namespace havremote
