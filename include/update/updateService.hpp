// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UPDATE_UPDATE_SERVICE_HPP
#define HAVREMOTE_INCLUDE_UPDATE_UPDATE_SERVICE_HPP

#include <compare>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace havremote::updates
{
  struct Version final
  {
    std::uint32_t major{};
    std::uint32_t minor{};
    std::uint32_t patch{};

    auto operator<=>(const Version &) const = default;
  };

  struct ReleaseAsset final
  {
    std::string name;
    std::string downloadUrl;
    std::uint64_t size{};
    std::optional<std::string> sha256;
  };

  enum class ReleasePlatform
  {
    Unknown,
    Windows,
    Linux,
    MacOS,
  };

  enum class ReleaseArchitecture
  {
    Unknown,
    X86_64,
    Arm64,
  };

  struct ReleaseTarget final
  {
    ReleasePlatform platform{ReleasePlatform::Unknown};
    ReleaseArchitecture architecture{ReleaseArchitecture::Unknown};

    auto operator<=>(const ReleaseTarget &) const = default;
  };

  [[nodiscard]] ReleaseTarget NativeReleaseTarget() noexcept;
  [[nodiscard]] std::optional<std::string> RuntimeReleaseAssetName(
      const Version &version, ReleaseTarget target);

  struct Release final
  {
    Version version;
    std::string canonicalVersion;
    std::string tagName;
    std::string title;
    std::string notes;
    std::string releasePageUrl;
    std::optional<ReleaseAsset> runtimeAsset;
  };

  enum class UpdateDisposition
  {
    UpToDate,
    Available,
  };

  struct UpdateCheck final
  {
    UpdateDisposition disposition{UpdateDisposition::UpToDate};
    std::optional<Release> release;
  };

  enum class UpdateErrorKind
  {
    Cancelled,
    Initialization,
    Network,
    HttpStatus,
    ResponseTooLarge,
    Parse,
    InvalidResponse,
  };

  struct UpdateError final
  {
    UpdateErrorKind kind{UpdateErrorKind::Network};
    std::string message;
    std::optional<long> httpStatus;
    std::optional<int> curlCode;
  };

  struct UpdateHttpResponse final
  {
    long status{};
    std::string body;
  };

  using UpdateTransport = std::function<
      std::expected<UpdateHttpResponse, UpdateError>(
          std::string_view url,
          std::string_view userAgent,
          std::stop_token stopToken)>;

  struct GitHubUpdateSource final
  {
    std::string owner;
    std::string repository;
    Version currentVersion;
    ReleaseTarget target{NativeReleaseTarget()};
  };

  class IUpdateService
  {
  public:
    virtual ~IUpdateService() = default;

    [[nodiscard]] virtual std::expected<UpdateCheck, UpdateError>
    CheckLatest(std::stop_token stopToken) = 0;
  };

  [[nodiscard]] std::expected<Version, UpdateError> ParseReleaseVersion(
      std::string_view value);
  [[nodiscard]] std::string ReleaseVersionText(const Version &version);

  // Parses GitHub's JSON response with havCSON's semantic parser. Only
  // an exact runtime archive for the reported version and target is accepted.
  // Releases without a matching package still expose their validated release page.
  [[nodiscard]] std::expected<UpdateCheck, UpdateError>
  EvaluateGitHubLatestRelease(std::string_view response,
                              const GitHubUpdateSource &source);

  // Empty transport selects the production HTTPS/libcurl implementation.
  // Tests can inject a deterministic transport without contacting GitHub.
  [[nodiscard]] std::unique_ptr<IUpdateService> MakeGitHubUpdateService(
      GitHubUpdateSource source,
      UpdateTransport transport = {});
} // namespace havremote::updates

#endif // HAVREMOTE_INCLUDE_UPDATE_UPDATE_SERVICE_HPP
