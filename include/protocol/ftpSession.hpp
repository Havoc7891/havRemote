// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_PROTOCOL_FTP_SESSION_HPP
#define HAVREMOTE_INCLUDE_PROTOCOL_FTP_SESSION_HPP

#include "core/session.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace havremote
{
  [[nodiscard]] RemoteSessionPtr MakeFtpSession();

  namespace ftp
  {
    // Validates a user-supplied endpoint as either an ASCII DNS name or an
    // unadorned IPv4/IPv6 literal and returns the representation safe to place in
    // a URL authority. In particular, callers must not pass URL/userinfo/path
    // syntax here. IPv6 brackets are added by this function.
    [[nodiscard]] Result<std::string> FormatHostForUrl(std::string_view host);

    // Returns the value passed to CURLOPT_FTPPORT for active mode. An empty
    // address deliberately becomes "-": curl then reads the established
    // control socket's local endpoint with getsockname(), binds the listener
    // to that address on port 0, reads back the assigned port, and advertises
    // it with EPRT (falling back to PORT for IPv4). An explicit address is both
    // curl's listener-bind address and its EPRT/PORT address. It must be a
    // numeric address assigned to a local interface, never a hostname or URL.
    // libcurl exposes no separate public bind-address/advertised-address pair.
    [[nodiscard]] Result<std::string> MakeActiveModePortSpecification(
        std::string_view localAddress);

    // Checks a numeric address against the currently active local interfaces
    [[nodiscard]] Result<bool> IsAssignedLocalAddress(std::string_view address);

    // Converts the path representation used after an FTP URL authority into
    // the equivalent operand for a raw FTP command. A single leading slash is
    // the URL separator and therefore denotes the user's login directory, not
    // the server filesystem root. Removing exactly that separator keeps raw
    // commands such as RNFR, DELE, and MKD in the same namespace as transfers.
    // A second leading slash remains significant: libcurl uses it to request a
    // genuinely server-absolute FTP URL path.
    [[nodiscard]] Result<std::string> MakeLoginRelativeCommandPath(
        std::string_view encodedPath);

    // Only these FTP operations open a data connection. Control-only commands
    // such as RNFR/RNTO must never initialize passive or active data-channel
    // state merely because the saved site uses active mode.
    enum class OperationKind
    {
      ControlCommand,
      Nlst,
      Mlsd,
      List,
      Retr,
      Stor,
      Rename,
    };

    [[nodiscard]] bool UsesDataConnection(OperationKind operation) noexcept;

    // FTP servers that have not negotiated UTF-8 expose path and listing names in
    // an endpoint-specific byte encoding. Explicit aliases identify code pages
    // supported by the platform's converter.
    // UTF-16/32, UTF-7, symbol, and stateful ISO-2022 encodings are rejected.
    struct TextEncoding final
    {
      std::uint32_t windowsCodePage{65001};
      std::string canonicalName{"UTF-8"};
      bool utf8{true};

      friend bool operator==(const TextEncoding &, const TextEncoding &) = default;
    };

    [[nodiscard]] Result<TextEncoding> ResolveTextEncoding(std::string_view name);
    // Converts UI/config UTF-8 to exact server bytes. A conversion that would use
    // a default character or a best-fit substitution fails.
    [[nodiscard]] Result<std::string> EncodeServerText(
        std::string_view utf8, const TextEncoding &encoding);
    // Converts server bytes to display UTF-8. Invalid byte sequences fail instead
    // of being silently replaced. Callers retain the original bytes separately.
    [[nodiscard]] Result<std::string> DecodeServerText(
        std::string_view bytes, const TextEncoding &encoding);

    struct TlsPublicKeyIdentity final
    {
      std::string curlPin;
      std::string sha256Fingerprint;
      std::vector<std::byte> subjectPublicKeyInfo;
      // Set only after the certificate's subject/issuer names match and its
      // signature verifies with its own public key.
      bool genuinelySelfSigned{};
      bool currentlyTimeValid{};
      // True only when the certificate verifier can build the leaf's chain for
      // server authentication with no errors beyond an absent trust anchor
      // (plus unavailable/offline revocation information for that anchor).
      // A locally trusted copy may therefore have no remaining chain errors
      // and is also true.
      // This prevents a higher-priority "untrusted root" diagnostic from
      // masking another certificate defect.
      bool serverAuthenticationValidExceptTrustAnchor{};
    };

    enum class TlsVerificationFailure
    {
      UntrustedIssuer,
      ExpiredOrNotYetValid,
      Revoked,
      HostnameMismatch,
      Other,
    };

    [[nodiscard]] TlsVerificationFailure ClassifyTlsVerificationFailure(
        std::string_view diagnostic);

    // Extracts the exact DER SubjectPublicKeyInfo used by libcurl's public-key
    // pinning implementation.
    [[nodiscard]] Result<TlsPublicKeyIdentity> InspectCertificatePem(
        std::string_view certificatePem);

    // Only a currently valid, cryptographically self-signed leaf rejected
    // solely for its issuer may be pinned.
    [[nodiscard]] bool IsTlsPinExceptionEligible(
        const TlsPublicKeyIdentity &identity,
        TlsVerificationFailure failure) noexcept;

    // A changed endpoint PIN requires ReplaceStored, not first-use acceptance.
    [[nodiscard]] bool IsTlsTrustDecisionAllowed(TrustStatus status,
                                                 TrustDecision decision) noexcept;

    [[nodiscard]] Result<std::vector<RemoteEntry>> ParseMlsdListing(
        std::string_view listing, const RemotePath &directory);
    [[nodiscard]] Result<std::vector<RemoteEntry>> ParseMlsdListing(
        std::string_view listing,
        const RemotePath &directory,
        const TextEncoding &encoding);
    [[nodiscard]] Result<std::vector<RemoteEntry>> ParseListListing(
        std::string_view listing, const RemotePath &directory);
    [[nodiscard]] Result<std::vector<RemoteEntry>> ParseListListing(
        std::string_view listing,
        const RemotePath &directory,
        const TextEncoding &encoding);

    // MLSD's portable facts do not include ownership, and many Unix FTP servers
    // expose owner/group only through their traditional LIST output. Supplement a
    // successful MLSD result without letting the server-specific LIST parser
    // replace authoritative MLSD fields. A value is copied only when both
    // listings contain exactly one entry with the same raw name and entry kind.
    void SupplementMlsdOwnerGroup(
        std::vector<RemoteEntry> &mlsdEntries,
        const std::vector<RemoteEntry> &listEntries);
  } // namespace ftp
} // namespace havremote

#endif // HAVREMOTE_INCLUDE_PROTOCOL_FTP_SESSION_HPP
