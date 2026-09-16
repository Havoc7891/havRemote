// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_CORE_TYPES_HPP
#define HAVREMOTE_INCLUDE_CORE_TYPES_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <stop_token>
#include <vector>

namespace havremote
{
  enum class ProtocolKind
  {
    Ftp,
    FtpsExplicit,
    FtpsImplicit,
    Sftp,
  };

  [[nodiscard]] std::string_view ToString(ProtocolKind protocol) noexcept;
  [[nodiscard]] std::optional<ProtocolKind> ProtocolKindFromString(std::string_view value) noexcept;
  [[nodiscard]] std::uint16_t DefaultPort(ProtocolKind protocol) noexcept;
  // Accepts an unbracketed IPv4/IPv6 literal or an ASCII DNS name. URL syntax,
  // userinfo, ports, zone identifiers, whitespace, and Unicode are rejected.
  // Callers should supply IDNs in punycode form.
  [[nodiscard]] bool IsValidEndpointHost(std::string_view host) noexcept;
  // Accepts only an unbracketed IPv4 or IPv6 literal. This is used where DNS
  // resolution or address decoration would make an advertised address ambiguous.
  [[nodiscard]] bool IsValidIpAddress(std::string_view address) noexcept;

  enum class FtpDataConnectionMode
  {
    Passive,
    Active,
  };

  [[nodiscard]] std::string_view ToString(FtpDataConnectionMode mode) noexcept;
  [[nodiscard]] std::optional<FtpDataConnectionMode> FtpDataConnectionModeFromString(
      std::string_view value) noexcept;

  enum class AuthenticationKind
  {
    Password,
    PasswordKeyboardInteractive,
    PrivateKey,
    Agent,
    KeyboardInteractive,
  };

  [[nodiscard]] std::string_view ToString(AuthenticationKind kind) noexcept;
  [[nodiscard]] std::optional<AuthenticationKind> AuthenticationKindFromString(
      std::string_view value) noexcept;

  // RemotePath intentionally keeps the server's original bytes separate from the
  // UTF-8 text used by the UI. Protocol implementations must use Bytes().
  class RemotePath final
  {
  public:
    RemotePath();
    explicit RemotePath(std::string bytes);
    RemotePath(std::string bytes, std::string displayUtf8);

    [[nodiscard]] static RemotePath Root();
    [[nodiscard]] const std::string &Bytes() const noexcept;
    [[nodiscard]] const std::string &DisplayUtf8() const noexcept;
    [[nodiscard]] bool Empty() const noexcept;
    [[nodiscard]] bool IsAbsolute() const noexcept;
    [[nodiscard]] bool IsRoot() const noexcept;
    [[nodiscard]] RemotePath Parent() const;
    [[nodiscard]] RemotePath Filename() const;
    [[nodiscard]] RemotePath Joined(const RemotePath &child) const;

    friend bool operator==(const RemotePath &, const RemotePath &) = default;

  private:
    std::string mBytes;
    std::string mDisplayUtf8;
  };

  struct Authentication final
  {
    AuthenticationKind kind{AuthenticationKind::Password};
    // Opaque identifiers for the platform credential store, never secret values
    std::string credentialId;
    std::filesystem::path privateKeyFile;
    std::filesystem::path publicKeyFile;
    std::string passphraseCredentialId;
  };

  struct SiteProfile final
  {
    std::string id;
    std::string name;
    ProtocolKind protocol{ProtocolKind::Sftp};
    std::string host;
    std::uint16_t port{22};
    std::string username;
    Authentication authentication;
    RemotePath initialRemoteDirectory{RemotePath::Root()};
    std::filesystem::path initialLocalDirectory;
    std::string ftpEncoding{"UTF-8"};
    FtpDataConnectionMode ftpDataConnectionMode{FtpDataConnectionMode::Passive};
    // Empty selects the local endpoint of the established FTP control socket.
    // A non-empty value must name an assigned local IPv4/IPv6 interface
    // address. libcurl binds the active listener to it and also sends it in
    // EPRT/PORT. This is not an independent advertised-address override.
    std::string ftpActiveAddress;
  };

  // Immutable connection snapshot attached to queued work. Stable site UUIDs
  // survive Site Manager edits, so UUID alone is not an endpoint boundary.
  struct SiteEndpointIdentity final
  {
    ProtocolKind protocol{ProtocolKind::Sftp};
    std::string host;
    std::uint16_t port{22};
    std::string username;

    // DNS names and textual IP addresses are compared case-insensitively
    friend bool operator==(const SiteEndpointIdentity &left,
                           const SiteEndpointIdentity &right) noexcept;
  };

  [[nodiscard]] SiteEndpointIdentity EndpointIdentity(const SiteProfile &site);

  enum class RemoteEntryKind
  {
    File,
    Directory,
    Symlink,
    Other,
  };

  struct RemoteEntry final
  {
    RemotePath path;
    RemotePath name;
    RemoteEntryKind kind{RemoteEntryKind::Other};
    std::uint64_t size{};
    std::optional<std::chrono::system_clock::time_point> modifiedAt;
    std::optional<std::uint32_t> permissions;
    std::optional<std::string> owner;
    std::optional<std::string> group;
    bool hidden{};
  };

  // Metadata captured when a remote file is opened for external editing. It
  // deliberately excludes path text: TransferJob::remotePath binds the value
  // snapshot to the destination being guarded.
  struct RemoteFileRevision final
  {
    RemoteEntryKind kind{RemoteEntryKind::Other};
    std::uint64_t size{};
    std::optional<std::chrono::system_clock::time_point> modifiedAt;

    friend bool operator==(const RemoteFileRevision &,
                           const RemoteFileRevision &) = default;
  };

  enum class RemoteFileRevisionComparison
  {
    Matches,
    Missing,
    WrongKind,
    Changed,
    Unverifiable,
  };

  [[nodiscard]] RemoteFileRevision MakeRemoteFileRevision(
      const RemoteEntry &entry) noexcept;
  [[nodiscard]] RemoteFileRevisionComparison CompareRemoteFileRevision(
      const RemoteFileRevision &expected,
      const std::optional<RemoteFileRevision> &current) noexcept;

  enum class RemoteErrorCode
  {
    InvalidArgument,
    Unsupported,
    NotConnected,
    AlreadyConnected,
    NameResolutionFailed,
    ConnectionFailed,
    ConnectionLost,
    TimedOut,
    AuthenticationFailed,
    CredentialUnavailable,
    TrustRejected,
    HostKeyChanged,
    CertificateInvalid,
    NotFound,
    AlreadyExists,
    PermissionDenied,
    NotDirectory,
    IsDirectory,
    DirectoryNotEmpty,
    LocalIo,
    RemoteIo,
    ProtocolError,
    ParseError,
    Conflict,
    Paused,
    Cancelled,
    Unknown,
  };

  struct RemoteError final
  {
    RemoteErrorCode code{RemoteErrorCode::Unknown};
    std::string message;
    int nativeCode{};
    bool retryable{};
    // True only when a failed mutating command may have reached the server
    bool operationMayHaveSucceeded{};
  };

  template <typename T>
  using Result = std::expected<T, RemoteError>;

  enum class TransferDirection
  {
    Upload,
    Download,
  };

  enum class ConflictPolicy
  {
    Ask,
    Overwrite,
    Skip,
    Rename,
    Resume,
  };

  enum class TransferState
  {
    Queued,
    Enumerating,
    Running,
    Paused,
    Completed,
    Failed,
    Cancelled,
  };

  // A conflict rename changes only the destination. Recursive jobs retain
  // their original roots and map individual source/destination pairs so a
  // retry can enumerate the same tree without choosing another new name.
  struct TransferDestinationOverride final
  {
    std::filesystem::path originalLocalPath;
    RemotePath originalRemotePath;
    std::filesystem::path localPath;
    RemotePath remotePath;

    friend bool operator==(const TransferDestinationOverride &,
                           const TransferDestinationOverride &) = default;
  };

  struct TransferJob final
  {
    std::string id;
    // Stable site identity. A queued job may only be claimed by workers for
    // this site, so reconnecting to another endpoint cannot retarget it.
    std::string siteId;
    std::optional<SiteEndpointIdentity> siteEndpoint;
    TransferDirection direction{TransferDirection::Download};
    std::filesystem::path localPath;
    RemotePath remotePath;
    bool recursive{};
    ConflictPolicy conflictPolicy{ConflictPolicy::Ask};
    // Present for a single-file external-edit transfer. The controller guards
    // both the initial download and subsequent replacement against this value.
    std::optional<RemoteFileRevision> expectedRemoteRevision;
    std::vector<TransferDestinationOverride> destinationOverrides{};
  };

  struct TransferProgress final
  {
    std::string jobId;
    // Logical progress includes bytes that were already present when a safe
    // resume began and items intentionally skipped by a conflict policy.
    std::uint64_t bytesTransferred{};
    std::optional<std::uint64_t> totalBytes;
    // Bytes physically transferred during the current attempt. Protocol
    // sessions exclude the resume offset. Controllers accumulate this value
    // across the files in a recursive job. Together with elapsed, this is the
    // throughput sample presented by the UI and is never persisted.
    std::uint64_t activeBytesTransferred{};
    // Active protocol-transfer time for activeBytesTransferred. Controllers
    // accumulate per-file durations without counting conflict-dialog time.
    std::chrono::steady_clock::duration elapsed{};
  };

  enum class TransferControl
  {
    Continue,
    Pause,
    Cancel,
  };

  using ProgressCallback = std::function<TransferControl(const TransferProgress &)>;

  // Runs after all bytes have reached the temporary destination but immediately
  // before it is committed to the caller-visible path. The argument is the
  // planned overwrite flag. The returned flag controls finalization.
  using PreFinalizationCallback = std::function<Result<bool>(bool overwrite)>;

  struct TransferOptions final
  {
    std::string jobId;
    std::uint64_t resumeOffset{};
    bool overwrite{};
    bool useTemporaryName{true};
    // Controllers use a unique, recorded path for safe concurrent uploads and
    // retries. When omitted, the temporary path uses the default suffix.
    std::optional<RemotePath> temporaryRemotePath;
    PreFinalizationCallback beforeFinalize;
  };

  enum class CredentialKind
  {
    Password,
    PrivateKeyPassphrase,
    KeyboardInteractive,
  };

  struct CredentialRequest final
  {
    CredentialKind kind{CredentialKind::Password};
    std::string siteId;
    std::string username;
    std::string prompt;
    bool echo{};
    // Bypass cached and persisted credentials when an authentication backend
    // knows the previously supplied value was rejected.
    bool forcePrompt{};
  };

  enum class TrustKind
  {
    TlsCertificate,
    SshHostKey,
  };

  enum class TrustStatus
  {
    Unknown,
    Changed,
    Invalid,
  };

  enum class TrustDecision
  {
    Reject,
    AcceptOnce,
    AcceptPermanently,
    ReplaceStored,
  };

  struct TrustChallenge final
  {
    TrustKind kind{TrustKind::SshHostKey};
    TrustStatus status{TrustStatus::Unknown};
    std::string siteId;
    std::string host;
    std::uint16_t port{};
    std::string algorithm;
    std::string sha256Fingerprint;
    std::vector<std::byte> publicKey;
    std::string diagnostic;
  };

  enum class DiagnosticLevel
  {
    Debug,
    Information,
    Warning,
    Error,
  };

  using CredentialCallback =
      std::function<Result<std::string>(const CredentialRequest &, std::stop_token)>;
  using TrustCallback =
      std::function<Result<TrustDecision>(const TrustChallenge &, std::stop_token)>;
  using DiagnosticCallback = std::function<void(DiagnosticLevel, std::string_view)>;

  struct SessionCallbacks final
  {
    CredentialCallback requestCredential;
    TrustCallback verifyTrust;
    DiagnosticCallback diagnostic;
    std::filesystem::path knownHostsFile;
    std::chrono::seconds connectionTimeout{20};
    std::chrono::seconds commandIdleTimeout{60};
    // libcurl PIN syntax (normally "sha256//<base64>"). When set for FTPS,
    // chain validation may be bypassed but hostname and this endpoint-scoped
    // public-key PIN are still required.
    std::string tlsPinnedPublicKey;
  };

  [[nodiscard]] std::string GenerateId();
  [[nodiscard]] std::string Sha256Fingerprint(const std::byte *data, std::size_t size);
  [[nodiscard]] bool IsValidRemoteChildName(std::string_view bytes) noexcept;
} // namespace havremote

#endif // HAVREMOTE_INCLUDE_CORE_TYPES_HPP
