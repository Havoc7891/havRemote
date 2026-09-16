// SPDX-License-Identifier: MIT

#include "update/updateService.hpp"

#include "network/curlRuntime.hpp"

#include <curl/curl.h>
#include <havCSON.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <memory>
#include <utility>

namespace havremote::updates
{
  namespace
  {
    constexpr std::size_t MaximumResponseBytes = 2U * 1024U * 1024U;
    constexpr std::size_t MaximumReleaseNotesBytes = 64U * 1024U;
    constexpr std::size_t MaximumShortStringBytes = 1024U;
    constexpr std::string_view GitHubApiVersion = "2026-03-10";

    UpdateError MakeUpdateError(UpdateErrorKind kind, std::string message)
    {
      return UpdateError{
          .kind = kind,
          .message = std::move(message),
          .httpStatus = std::nullopt,
          .curlCode = std::nullopt,
      };
    }

    bool ValidRepositoryComponent(const std::string_view value) noexcept
    {
      return !value.empty() && value.size() <= 100U &&
             std::ranges::all_of(
                 value,
                 [](const unsigned char character)
                 {
                   return (character >= 'a' && character <= 'z') ||
                          (character >= 'A' && character <= 'Z') ||
                          (character >= '0' && character <= '9') ||
                          character == '-' || character == '_' ||
                          character == '.';
                 });
    }

    std::expected<std::uint32_t, UpdateError> ParseVersionComponent(
        const std::string_view value)
    {
      if (value.empty() || (value.size() > 1U && value.front() == '0') ||
          !std::ranges::all_of(value, [](const unsigned char character)
                               { return character >= '0' && character <= '9'; }))
      {
        return std::unexpected(MakeUpdateError(
            UpdateErrorKind::InvalidResponse,
            "Release versions must use canonical MAJOR.MINOR.PATCH numbers"));
      }

      std::uint32_t result{};

      const auto [end, errorCode] =
          std::from_chars(value.data(), value.data() + value.size(), result);

      if (errorCode != std::errc{} || end != value.data() + value.size())
      {
        return std::unexpected(MakeUpdateError(
            UpdateErrorKind::InvalidResponse,
            "A release version component is outside the supported range"));
      }

      return result;
    }

    const havCSON::Value *ObjectMember(const havCSON::Value &value,
                                       const std::string_view key)
    {
      const auto member = havCSON::ValueView(value).Member(key).Get();
      return member ? &member->get() : nullptr;
    }

    std::expected<std::string, UpdateError> RequiredString(
        const havCSON::Value &value,
        const std::string_view key,
        const std::size_t maximumLength = MaximumShortStringBytes)
    {
      const auto member = havCSON::ValueView(value).Member(key).AsString();
      if (!member)
      {
        return std::unexpected(MakeUpdateError(
            UpdateErrorKind::InvalidResponse,
            "GitHub's release response is missing the string field '" +
                std::string{key} + "'"));
      }

      const auto &text = member->get();
      if (text.empty() || text.size() > maximumLength)
      {
        return std::unexpected(MakeUpdateError(
            UpdateErrorKind::InvalidResponse,
            "GitHub's release field '" + std::string{key} +
                "' has an unsupported length"));
      }

      return text;
    }

    std::expected<std::string, UpdateError> NullableString(
        const havCSON::Value &value,
        const std::string_view key,
        const std::size_t maximumLength)
    {
      const auto *member = ObjectMember(value, key);
      if (member == nullptr || member->isNull())
      {
        return std::string{};
      }

      const auto string = havCSON::ValueView(*member).AsString();
      if (!string)
      {
        return std::unexpected(MakeUpdateError(
            UpdateErrorKind::InvalidResponse,
            "GitHub's release field '" + std::string{key} +
                "' must be a string or null"));
      }

      auto text = string->get();
      if (text.size() > maximumLength)
      {
        std::size_t boundary = maximumLength;

        while (boundary > 0U && boundary < text.size() &&
               (static_cast<unsigned char>(text[boundary]) & 0xc0U) == 0x80U)
        {
          --boundary;
        }

        text.resize(boundary);
      }

      return text;
    }

    bool ValidSha256Digest(const std::string_view value) noexcept
    {
      constexpr std::string_view Prefix = "sha256:";
      return value.size() == Prefix.size() + 64U &&
             value.starts_with(Prefix) &&
             std::ranges::all_of(
                 value.substr(Prefix.size()),
                 [](const unsigned char character)
                 {
                   return (character >= '0' && character <= '9') ||
                          (character >= 'a' && character <= 'f') ||
                          (character >= 'A' && character <= 'F');
                 });
    }

    std::expected<std::uint64_t, UpdateError> AssetSize(
        const havCSON::Value &asset)
    {
      const auto *member = ObjectMember(asset, "size");
      if (member == nullptr || !member->isNumber())
      {
        return std::unexpected(MakeUpdateError(
            UpdateErrorKind::InvalidResponse,
            "GitHub's runtime release asset has no numeric size"));
      }

      // GitHub sends numeric sizes. Reject values outside double's exact range.
      constexpr std::uint64_t LargestExactInteger = 9'007'199'254'740'991ULL;

      const auto size = havCSON::ValueView(*member).AsInteger<std::uint64_t>(
          1, LargestExactInteger);

      if (!size)
      {
        return std::unexpected(MakeUpdateError(
            UpdateErrorKind::InvalidResponse,
            "GitHub's runtime release asset has an invalid size"));
      }

      return *size;
    }

    bool ExactGitHubUrl(const std::string_view actual,
                        const std::string_view expected) noexcept
    {
      return actual == expected && actual.starts_with("https://github.com/");
    }

    struct CurlDeleter final
    {
      void operator()(CURL *handle) const noexcept
      {
        if (handle != nullptr)
        {
          curl_easy_cleanup(handle);
        }
      }
    };

    struct CurlListDeleter final
    {
      void operator()(curl_slist *list) const noexcept
      {
        curl_slist_free_all(list);
      }
    };

    struct WriteContext final
    {
      std::string body;
      bool tooLarge{};
      bool writeFailed{};
    };

    std::size_t WriteResponse(char *data,
                              const std::size_t size,
                              const std::size_t count,
                              void *userData)
    {
      auto &context = *static_cast<WriteContext *>(userData);

      if (size != 0U && count > std::numeric_limits<std::size_t>::max() / size)
      {
        context.tooLarge = true;

        return 0U;
      }

      const auto bytes = size * count;

      if (bytes > MaximumResponseBytes - context.body.size())
      {
        context.tooLarge = true;

        return 0U;
      }

      try
      {
        context.body.append(data, bytes);
      }
      catch (...)
      {
        // Exceptions must never cross libcurl's C callback boundary
        context.writeFailed = true;

        return 0U;
      }

      return bytes;
    }

    int OnUpdateTransferProgress(void *userData,
                                 curl_off_t,
                                 curl_off_t,
                                 curl_off_t,
                                 curl_off_t)
    {
      return static_cast<const std::stop_token *>(userData)->stop_requested()
                 ? 1
                 : 0;
    }

    std::expected<UpdateHttpResponse, UpdateError> PerformHttpsGet(
        const std::string_view url,
        const std::string_view userAgent,
        const std::stop_token stopToken)
    {
      if (stopToken.stop_requested())
      {
        return std::unexpected(MakeUpdateError(UpdateErrorKind::Cancelled,
                                               "The update check was cancelled"));
      }

      const CURLcode runtime = network::CurlRuntimeResult();
      if (runtime != CURLE_OK)
      {
        auto result = MakeUpdateError(
            UpdateErrorKind::Initialization,
            "libcurl initialization failed: " +
                std::string{curl_easy_strerror(runtime)});

        result.curlCode = static_cast<int>(runtime);

        return std::unexpected(std::move(result));
      }

      std::unique_ptr<CURL, CurlDeleter> handle{curl_easy_init()};

      if (!handle)
      {
        return std::unexpected(MakeUpdateError(UpdateErrorKind::Initialization,
                                               "Could not create an HTTPS request"));
      }

      std::array<char, CURL_ERROR_SIZE> errorBuffer{};
      WriteContext response;
      std::string requestUrl{url};
      std::string requestUserAgent{userAgent};
      curl_slist *rawHeaders{};

      const auto appendHeader = [&rawHeaders](const char *header)
      {
        auto *const appended = curl_slist_append(rawHeaders, header);
        if (appended == nullptr)
        {
          return false;
        }

        rawHeaders = appended;

        return true;
      };

      const std::string apiHeader =
          "X-GitHub-Api-Version: " + std::string{GitHubApiVersion};

      if (!appendHeader("Accept: application/vnd.github+json") ||
          !appendHeader(apiHeader.c_str()))
      {
        curl_slist_free_all(rawHeaders);

        return std::unexpected(MakeUpdateError(UpdateErrorKind::Initialization,
                                               "Could not create GitHub request headers"));
      }

      std::unique_ptr<curl_slist, CurlListDeleter> headers{rawHeaders};

      const auto set = [&](const CURLoption option, const auto value)
      {
        return curl_easy_setopt(handle.get(), option, value) == CURLE_OK;
      };

      if (!set(CURLOPT_URL, requestUrl.c_str()) ||
          !set(CURLOPT_USERAGENT, requestUserAgent.c_str()) ||
          !set(CURLOPT_HTTPHEADER, headers.get()) ||
          !set(CURLOPT_ERRORBUFFER, errorBuffer.data()) ||
          !set(CURLOPT_WRITEFUNCTION, WriteResponse) ||
          !set(CURLOPT_WRITEDATA, &response) ||
          !set(CURLOPT_CONNECTTIMEOUT_MS, 10'000L) ||
          !set(CURLOPT_TIMEOUT_MS, 20'000L) ||
          !set(CURLOPT_NOSIGNAL, 1L) ||
          !set(CURLOPT_NOPROGRESS, 0L) ||
          !set(CURLOPT_XFERINFOFUNCTION, OnUpdateTransferProgress) ||
          !set(CURLOPT_XFERINFODATA, &stopToken) ||
          !set(CURLOPT_PROTOCOLS_STR, "https") ||
          !set(CURLOPT_REDIR_PROTOCOLS_STR, "https") ||
          !set(CURLOPT_FOLLOWLOCATION, 0L) ||
          !set(CURLOPT_SSL_VERIFYPEER, 1L) ||
          !set(CURLOPT_SSL_VERIFYHOST, 2L))
      {
        return std::unexpected(MakeUpdateError(UpdateErrorKind::Initialization,
                                               "Could not configure the HTTPS request"));
      }

      const CURLcode performed = curl_easy_perform(handle.get());
      if (performed != CURLE_OK)
      {
        if (stopToken.stop_requested() || performed == CURLE_ABORTED_BY_CALLBACK)
        {
          return std::unexpected(MakeUpdateError(UpdateErrorKind::Cancelled,
                                                 "The update check was cancelled"));
        }

        if (response.tooLarge)
        {
          return std::unexpected(MakeUpdateError(
              UpdateErrorKind::ResponseTooLarge,
              "GitHub's release response exceeded 2 MiB"));
        }

        if (response.writeFailed)
        {
          return std::unexpected(MakeUpdateError(
              UpdateErrorKind::Initialization,
              "The GitHub release response could not be stored"));
        }

        std::string detail = errorBuffer.front() == '\0'
                                 ? std::string{curl_easy_strerror(performed)}
                                 : std::string{errorBuffer.data()};

        auto result = MakeUpdateError(UpdateErrorKind::Network,
                                      "GitHub request failed: " + detail);

        result.curlCode = static_cast<int>(performed);

        return std::unexpected(std::move(result));
      }

      long status{};

      if (curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status) !=
          CURLE_OK)
      {
        return std::unexpected(MakeUpdateError(UpdateErrorKind::Network,
                                               "Could not read GitHub's HTTP status"));
      }

      return UpdateHttpResponse{.status = status,
                                .body = std::move(response.body)};
    }

    class GitHubUpdateService final : public IUpdateService
    {
    public:
      GitHubUpdateService(GitHubUpdateSource source, UpdateTransport transport)
          : mSource(std::move(source)), mTransport(std::move(transport))
      {
      }

      std::expected<UpdateCheck, UpdateError> CheckLatest(
          const std::stop_token stopToken) override
      {
        if (!ValidRepositoryComponent(mSource.owner) ||
            !ValidRepositoryComponent(mSource.repository))
        {
          return std::unexpected(MakeUpdateError(
              UpdateErrorKind::Initialization,
              "The configured GitHub repository identifier is invalid"));
        }

        const auto current = ReleaseVersionText(mSource.currentVersion);
        const std::string repositoryUrl =
            "https://github.com/" + mSource.owner + '/' + mSource.repository;
        const std::string apiUrl =
            "https://api.github.com/repos/" + mSource.owner + '/' +
            mSource.repository + "/releases/latest";
        const std::string userAgent =
            "havRemote/" + current + " (+" + repositoryUrl + ')';
        auto response = mTransport
                            ? mTransport(apiUrl, userAgent, stopToken)
                            : PerformHttpsGet(apiUrl, userAgent, stopToken);
        if (!response)
        {
          return std::unexpected(std::move(response.error()));
        }
        if (response->body.size() > MaximumResponseBytes)
        {
          return std::unexpected(MakeUpdateError(
              UpdateErrorKind::ResponseTooLarge,
              "GitHub's release response exceeded 2 MiB"));
        }
        if (response->status != 200)
        {
          std::string message;

          if (response->status == 404)
          {
            message = "GitHub has no published stable havRemote release";
          }
          else if (response->status == 403 || response->status == 429)
          {
            message = "GitHub temporarily refused the update check because "
                      "its request limit was reached";
          }
          else
          {
            message = "GitHub returned HTTP status " +
                      std::to_string(response->status);
          }

          auto result = MakeUpdateError(UpdateErrorKind::HttpStatus, std::move(message));
          result.httpStatus = response->status;

          return std::unexpected(std::move(result));
        }

        return EvaluateGitHubLatestRelease(response->body, mSource);
      }

    private:
      GitHubUpdateSource mSource;
      UpdateTransport mTransport;
    };
  } // namespace

  std::expected<Version, UpdateError> ParseReleaseVersion(
      std::string_view value)
  {
    if (value.starts_with('v'))
    {
      value.remove_prefix(1);
    }

    const auto first = value.find('.');
    const auto second = first == std::string_view::npos
                            ? std::string_view::npos
                            : value.find('.', first + 1U);

    if (first == std::string_view::npos || second == std::string_view::npos ||
        value.find('.', second + 1U) != std::string_view::npos)
    {
      return std::unexpected(MakeUpdateError(
          UpdateErrorKind::InvalidResponse,
          "Release versions must use MAJOR.MINOR.PATCH format"));
    }

    auto major = ParseVersionComponent(value.substr(0, first));
    auto minor = ParseVersionComponent(value.substr(first + 1U, second - first - 1U));
    auto patch = ParseVersionComponent(value.substr(second + 1U));

    if (!major)
    {
      return std::unexpected(major.error());
    }

    if (!minor)
    {
      return std::unexpected(minor.error());
    }

    if (!patch)
    {
      return std::unexpected(patch.error());
    }

    return Version{.major = *major, .minor = *minor, .patch = *patch};
  }

  std::string ReleaseVersionText(const Version &version)
  {
    return std::to_string(version.major) + '.' +
           std::to_string(version.minor) + '.' +
           std::to_string(version.patch);
  }

  ReleaseTarget NativeReleaseTarget() noexcept
  {
    ReleaseTarget target;

#if defined(_WIN32)
    target.platform = ReleasePlatform::Windows;
#elif defined(__APPLE__) && defined(__MACH__)
    target.platform = ReleasePlatform::MacOS;
#elif defined(__linux__)
    target.platform = ReleasePlatform::Linux;
#endif

#if defined(__aarch64__) || defined(_M_ARM64) || defined(_M_ARM64EC)
    target.architecture = ReleaseArchitecture::Arm64;
#elif defined(__x86_64__) || defined(_M_X64)
    target.architecture = ReleaseArchitecture::X86_64;
#endif

    return target;
  }

  std::optional<std::string> RuntimeReleaseAssetName(
      const Version &version, const ReleaseTarget target)
  {
    std::string_view platform;
    std::string_view extension;

    switch (target.platform)
    {
    case ReleasePlatform::Windows:
      platform = "windows";
      extension = ".zip";
      break;

    case ReleasePlatform::Linux:
      platform = "linux";
      extension = ".tar.gz";
      break;

    case ReleasePlatform::MacOS:
      platform = "macos";
      extension = ".zip";
      break;

    default:
      return std::nullopt;
    }

    std::string_view architecture;

    switch (target.architecture)
    {
    case ReleaseArchitecture::X86_64:
      architecture = "x86_64";
      break;

    case ReleaseArchitecture::Arm64:
      architecture = "arm64";
      break;

    default:
      return std::nullopt;
    }

    return "havRemote-" + ReleaseVersionText(version) + '-' +
           std::string{platform} + '-' + std::string{architecture} +
           std::string{extension};
  }

  std::expected<UpdateCheck, UpdateError> EvaluateGitHubLatestRelease(
      const std::string_view response,
      const GitHubUpdateSource &source)
  {
    if (!ValidRepositoryComponent(source.owner) ||
        !ValidRepositoryComponent(source.repository))
    {
      return std::unexpected(MakeUpdateError(
          UpdateErrorKind::Initialization,
          "The configured GitHub repository identifier is invalid"));
    }

    if (response.size() > MaximumResponseBytes)
    {
      return std::unexpected(MakeUpdateError(UpdateErrorKind::ResponseTooLarge,
                                             "GitHub's release response exceeded 2 MiB"));
    }

    havCSON::Value root;
    havCSON::Error parseError;
    const havCSON::ParseOptions options{
        .maxDepth = 256,
        .maxInputBytes = MaximumResponseBytes,
    };

    if (havCSON::Parse(response, root, &parseError, options) != havCSON::ErrorCode::OK)
    {
      return std::unexpected(MakeUpdateError(
          UpdateErrorKind::Parse,
          "GitHub returned malformed release metadata at " +
              std::to_string(parseError.where.line) + ':' +
              std::to_string(parseError.where.column)));
    }

    if (!root.isObject())
    {
      return std::unexpected(MakeUpdateError(UpdateErrorKind::InvalidResponse,
                                             "GitHub's release response is not an object"));
    }

    auto tagName = RequiredString(root, "tag_name");
    if (!tagName)
    {
      return std::unexpected(tagName.error());
    }

    auto version = ParseReleaseVersion(*tagName);
    if (!version)
    {
      return std::unexpected(version.error());
    }
    if (*version <= source.currentVersion)
    {
      return UpdateCheck{
          .disposition = UpdateDisposition::UpToDate,
          .release = std::nullopt,
      };
    }

    const auto canonicalVersion = ReleaseVersionText(*version);
    const auto expectedAssetName = RuntimeReleaseAssetName(*version, source.target);
    const auto expectedRepositoryUrl =
        "https://github.com/" + source.owner + '/' + source.repository;
    const auto expectedReleaseUrl =
        expectedRepositoryUrl + "/releases/tag/" + *tagName;
    auto releasePageUrl = RequiredString(root, "html_url", 4096U);

    if (!releasePageUrl ||
        !ExactGitHubUrl(releasePageUrl.value_or(std::string{}),
                        expectedReleaseUrl))
    {
      return std::unexpected(MakeUpdateError(
          UpdateErrorKind::InvalidResponse,
          "GitHub returned an unexpected release page URL"));
    }

    const auto assets = havCSON::ValueView(root).Member("assets").AsArray();
    if (!assets)
    {
      return std::unexpected(MakeUpdateError(
          UpdateErrorKind::InvalidResponse,
          "GitHub's release response has no asset list"));
    }

    const havCSON::Value *runtimeAsset{};

    for (const auto &asset : assets->get())
    {
      if (!asset.isObject())
      {
        return std::unexpected(MakeUpdateError(
            UpdateErrorKind::InvalidResponse,
            "GitHub's release asset list contains a non-object value"));
      }

      const auto *name = ObjectMember(asset, "name");
      if (expectedAssetName && name != nullptr && name->isString() &&
          std::get<std::string>(*name) == *expectedAssetName)
      {
        if (runtimeAsset != nullptr)
        {
          return std::unexpected(MakeUpdateError(
              UpdateErrorKind::InvalidResponse,
              "GitHub's release contains duplicate runtime assets"));
        }
        runtimeAsset = &asset;
      }
    }

    std::optional<ReleaseAsset> selectedAsset;

    if (runtimeAsset != nullptr)
    {
      auto downloadUrl = RequiredString(*runtimeAsset,
                                        "browser_download_url", 4096U);

      const auto expectedDownloadUrl =
          expectedRepositoryUrl + "/releases/download/" + *tagName + '/' +
          *expectedAssetName;

      if (!downloadUrl ||
          !ExactGitHubUrl(downloadUrl.value_or(std::string{}),
                          expectedDownloadUrl))
      {
        return std::unexpected(MakeUpdateError(
            UpdateErrorKind::InvalidResponse,
            "GitHub returned an unexpected runtime download URL"));
      }

      auto size = AssetSize(*runtimeAsset);
      if (!size)
      {
        return std::unexpected(size.error());
      }

      std::optional<std::string> sha256;
      if (const auto *digest = ObjectMember(*runtimeAsset, "digest");
          digest != nullptr && !digest->isNull())
      {
        if (!digest->isString() ||
            !ValidSha256Digest(std::get<std::string>(*digest)))
        {
          return std::unexpected(MakeUpdateError(
              UpdateErrorKind::InvalidResponse,
              "GitHub returned an invalid SHA-256 runtime asset digest"));
        }

        sha256 = std::get<std::string>(*digest).substr(7U);
      }

      selectedAsset = ReleaseAsset{
          .name = *expectedAssetName,
          .downloadUrl = std::move(*downloadUrl),
          .size = *size,
          .sha256 = std::move(sha256),
      };
    }

    auto title = NullableString(root, "name", MaximumShortStringBytes);
    auto notes = NullableString(root, "body", MaximumReleaseNotesBytes);

    if (!title)
    {
      return std::unexpected(title.error());
    }

    if (!notes)
    {
      return std::unexpected(notes.error());
    }

    if (title->empty())
    {
      *title = "havRemote " + canonicalVersion;
    }

    return UpdateCheck{
        .disposition = UpdateDisposition::Available,
        .release = Release{
            .version = *version,
            .canonicalVersion = canonicalVersion,
            .tagName = std::move(*tagName),
            .title = std::move(*title),
            .notes = std::move(*notes),
            .releasePageUrl = std::move(*releasePageUrl),
            .runtimeAsset = std::move(selectedAsset),
        },
    };
  }

  std::unique_ptr<IUpdateService> MakeGitHubUpdateService(
      GitHubUpdateSource source,
      UpdateTransport transport)
  {
    return std::make_unique<GitHubUpdateService>(std::move(source),
                                                 std::move(transport));
  }
} // namespace havremote::updates
