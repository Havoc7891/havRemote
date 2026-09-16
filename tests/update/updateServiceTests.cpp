// SPDX-License-Identifier: MIT

#include "update/updateService.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

using namespace havremote;

namespace
{
  constexpr std::string_view Owner = "Havoc7891";
  constexpr std::string_view Repository = "havRemote";
  constexpr std::string_view Tag = "v0.2.0";
  constexpr std::string_view CanonicalVersion = "0.2.0";
  constexpr std::string_view RuntimeAssetName =
      "havRemote-0.2.0-windows-x86_64.zip";
  constexpr std::string_view ReleaseUrl =
      "https://github.com/Havoc7891/havRemote/releases/tag/v0.2.0";
  constexpr std::string_view DownloadUrl =
      "https://github.com/Havoc7891/havRemote/releases/download/"
      "v0.2.0/havRemote-0.2.0-windows-x86_64.zip";

  struct TargetAssetCase final
  {
    updates::ReleaseTarget target;
    std::string_view name;
  };

  constexpr std::array TargetAssetCases{
      TargetAssetCase{{updates::ReleasePlatform::Windows,
                       updates::ReleaseArchitecture::X86_64},
                      "havRemote-0.2.0-windows-x86_64.zip"},
      TargetAssetCase{{updates::ReleasePlatform::Windows,
                       updates::ReleaseArchitecture::Arm64},
                      "havRemote-0.2.0-windows-arm64.zip"},
      TargetAssetCase{{updates::ReleasePlatform::Linux,
                       updates::ReleaseArchitecture::X86_64},
                      "havRemote-0.2.0-linux-x86_64.tar.gz"},
      TargetAssetCase{{updates::ReleasePlatform::Linux,
                       updates::ReleaseArchitecture::Arm64},
                      "havRemote-0.2.0-linux-arm64.tar.gz"},
      TargetAssetCase{{updates::ReleasePlatform::MacOS,
                       updates::ReleaseArchitecture::X86_64},
                      "havRemote-0.2.0-macos-x86_64.zip"},
      TargetAssetCase{{updates::ReleasePlatform::MacOS,
                       updates::ReleaseArchitecture::Arm64},
                      "havRemote-0.2.0-macos-arm64.zip"},
  };

  updates::GitHubUpdateSource Source(
      const updates::Version current = {.major = 0, .minor = 1, .patch = 0})
  {
    return updates::GitHubUpdateSource{
        .owner = std::string{Owner},
        .repository = std::string{Repository},
        .currentVersion = current,
        .target = {updates::ReleasePlatform::Windows,
                   updates::ReleaseArchitecture::X86_64},
    };
  }

  std::string AssetDownloadUrl(const std::string_view name)
  {
    return "https://github.com/Havoc7891/havRemote/releases/download/" +
           std::string{Tag} + '/' + std::string{name};
  }

  std::string AssetJson(
      const std::string_view name = RuntimeAssetName,
      const std::string_view downloadUrl = DownloadUrl,
      const std::string_view sizeJson = "4096",
      const std::string_view digestJson = "null")
  {
    return "{\"name\":\"" + std::string{name} +
           "\",\"browser_download_url\":\"" +
           std::string{downloadUrl} + "\",\"size\":" +
           std::string{sizeJson} + ",\"digest\":" +
           std::string{digestJson} + '}';
  }

  std::string ReleaseJson(
      const std::string_view tag = Tag,
      const std::string_view htmlUrl = ReleaseUrl,
      const std::string_view assetsJson = {},
      const std::string_view titleJson = "\"Second release\"",
      const std::string_view bodyJson = "\"Release notes\"")
  {
    const auto assets =
        assetsJson.empty() ? AssetJson() : std::string{assetsJson};

    return "{\"tag_name\":\"" + std::string{tag} +
           "\",\"html_url\":\"" + std::string{htmlUrl} +
           "\",\"name\":" + std::string{titleJson} +
           ",\"body\":" + std::string{bodyJson} +
           ",\"assets\":[" + assets + "]}";
  }

  void CheckInvalidRelease(const std::string_view document,
                           const updates::UpdateErrorKind expectedKind =
                               updates::UpdateErrorKind::InvalidResponse)
  {
    const auto result = updates::EvaluateGitHubLatestRelease(document,
                                                             Source());

    REQUIRE_FALSE(result);
    CHECK(result.error().kind == expectedKind);
    CHECK_FALSE(result.error().message.empty());
  }
} // namespace

TEST_CASE("Release metadata parsing bounds nesting in havCSON",
          "[update][parse]")
{
  const auto document = std::string(257, '[') + '0' + std::string(257, ']');

  CheckInvalidRelease(document, updates::UpdateErrorKind::Parse);
}

TEST_CASE("Release versions use strict numeric semantic ordering",
          "[update][version]")
{
  const auto plain = updates::ParseReleaseVersion("0.10.3");

  REQUIRE(plain);
  CHECK(plain->major == 0);
  CHECK(plain->minor == 10);
  CHECK(plain->patch == 3);
  CHECK(updates::ReleaseVersionText(*plain) == "0.10.3");

  const auto tagged = updates::ParseReleaseVersion("v12.0.45");

  REQUIRE(tagged);
  CHECK((*tagged ==
         updates::Version{.major = 12, .minor = 0, .patch = 45}));
  CHECK((updates::Version{.major = 0, .minor = 10, .patch = 0} >
         updates::Version{.major = 0, .minor = 9, .patch = 99}));
}

TEST_CASE("Release versions reject noncanonical or unsupported forms",
          "[update][version]")
{
  constexpr std::array invalid{
      "",
      "v",
      "1",
      "1.2",
      "1.2.3.4",
      ".1.2",
      "1..2",
      "1.2.",
      "01.2.3",
      "1.02.3",
      "1.2.03",
      "V1.2.3",
      "v1.2.3-beta.1",
      "1.2.3+build",
      " 1.2.3",
      "1.2.3 ",
      "4294967296.0.0",
  };

  for (const std::string_view value : invalid)
  {
    CAPTURE(value);

    const auto parsed = updates::ParseReleaseVersion(value);

    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().kind == updates::UpdateErrorKind::InvalidResponse);
  }
}

TEST_CASE("GitHub release metadata selects only the exact runtime archive",
          "[update][github]")
{
  const std::string digest(64U, 'a');

  const auto runtime = AssetJson(RuntimeAssetName,
                                 DownloadUrl,
                                 "9007199254740991",
                                 "\"sha256:" + digest + "\"");

  const auto debug = AssetJson(
      "havRemote-0.2.0-windows-x86_64-debug.zip",
      "https://github.com/Havoc7891/havRemote/releases/download/v0.2.0/"
      "havRemote-0.2.0-windows-x86_64-debug.zip");

  const auto document = ReleaseJson(Tag,
                                    ReleaseUrl,
                                    debug + ',' + runtime,
                                    "null",
                                    "null");

  const auto result =
      updates::EvaluateGitHubLatestRelease(document, Source());

  REQUIRE(result);
  CHECK(result->disposition == updates::UpdateDisposition::Available);
  REQUIRE(result->release);

  const auto &release = *result->release;

  CHECK((release.version ==
         updates::Version{.major = 0, .minor = 2, .patch = 0}));
  CHECK(release.canonicalVersion == CanonicalVersion);
  CHECK(release.tagName == Tag);
  CHECK(release.title == "havRemote 0.2.0");
  CHECK(release.notes.empty());
  CHECK(release.releasePageUrl == ReleaseUrl);
  REQUIRE(release.runtimeAsset);
  CHECK(release.runtimeAsset->name == RuntimeAssetName);
  CHECK(release.runtimeAsset->downloadUrl == DownloadUrl);
  CHECK(release.runtimeAsset->size == 9'007'199'254'740'991ULL);
  REQUIRE(release.runtimeAsset->sha256);
  CHECK(*release.runtimeAsset->sha256 == digest);
}

TEST_CASE("Runtime archive names match the release platform and architecture",
          "[update][platform]")
{
  for (const auto &[target, name] : TargetAssetCases)
  {
    CAPTURE(name);

    const auto asset = updates::RuntimeReleaseAssetName({0, 2, 0}, target);

    REQUIRE(asset);
    CHECK(*asset == name);
  }

  for (const auto target : {
           updates::ReleaseTarget{},
           updates::ReleaseTarget{updates::ReleasePlatform::Linux,
                                  updates::ReleaseArchitecture::Unknown},
           updates::ReleaseTarget{updates::ReleasePlatform::Unknown,
                                  updates::ReleaseArchitecture::Arm64}})
  {
    CHECK_FALSE(updates::RuntimeReleaseAssetName({0, 2, 0}, target));
  }
}

TEST_CASE("The default update target follows the compiler target",
          "[update][platform]")
{
  const auto native = updates::NativeReleaseTarget();

  CHECK(updates::GitHubUpdateSource{}.target == native);
#if defined(_WIN32)
  CHECK(native.platform == updates::ReleasePlatform::Windows);
#elif defined(__APPLE__) && defined(__MACH__)
  CHECK(native.platform == updates::ReleasePlatform::MacOS);
#elif defined(__linux__)
  CHECK(native.platform == updates::ReleasePlatform::Linux);
#else
  CHECK(native.platform == updates::ReleasePlatform::Unknown);
#endif
#if defined(__aarch64__) || defined(_M_ARM64) || defined(_M_ARM64EC)
  CHECK(native.architecture == updates::ReleaseArchitecture::Arm64);
#elif defined(__x86_64__) || defined(_M_X64)
  CHECK(native.architecture == updates::ReleaseArchitecture::X86_64);
#else
  CHECK(native.architecture == updates::ReleaseArchitecture::Unknown);
#endif
}

TEST_CASE("GitHub releases select the matching asset from every supported target",
          "[update][github][platform]")
{
  std::string assets;

  for (const auto &[target, name] : TargetAssetCases)
  {
    if (!assets.empty())
    {
      assets += ',';
    }

    assets += AssetJson(name, AssetDownloadUrl(name));
  }

  const auto document = ReleaseJson(Tag, ReleaseUrl, assets);

  for (const auto &[target, name] : TargetAssetCases)
  {
    CAPTURE(name);

    auto source = Source();
    source.target = target;

    const auto result = updates::EvaluateGitHubLatestRelease(document, source);

    REQUIRE(result);
    REQUIRE(result->release);
    REQUIRE(result->release->runtimeAsset);
    CHECK(result->release->runtimeAsset->name == name);
    CHECK(result->release->runtimeAsset->downloadUrl == AssetDownloadUrl(name));
  }
}

TEST_CASE("GitHub releases never offer a different platform or architecture",
          "[update][github][platform][security]")
{
  for (const auto &[target, name] : TargetAssetCases)
  {
    for (const auto &[otherTarget, otherName] : TargetAssetCases)
    {
      if (otherTarget == target)
      {
        continue;
      }

      CAPTURE(name, otherName);

      auto source = Source();
      source.target = target;

      const auto result = updates::EvaluateGitHubLatestRelease(
          ReleaseJson(Tag, ReleaseUrl,
                      AssetJson(otherName, AssetDownloadUrl(otherName))),
          source);

      REQUIRE(result);
      CHECK(result->disposition == updates::UpdateDisposition::Available);
      REQUIRE(result->release);
      CHECK_FALSE(result->release->runtimeAsset);
      CHECK(result->release->releasePageUrl == ReleaseUrl);
    }
  }
}

TEST_CASE("A release without a native runtime keeps its validated release page",
          "[update][github][platform]")
{
  SECTION("only debug symbols exist")
  {
    const auto result = updates::EvaluateGitHubLatestRelease(
        ReleaseJson(Tag, ReleaseUrl,
                    AssetJson("havRemote-0.2.0-windows-x86_64-debug.zip")),
        Source());

    REQUIRE(result);
    REQUIRE(result->release);
    CHECK_FALSE(result->release->runtimeAsset);
    CHECK(result->release->releasePageUrl == ReleaseUrl);
  }
  SECTION("no assets are published")
  {
    const auto document =
        "{\"tag_name\":\"v0.2.0\",\"html_url\":\"" +
        std::string{ReleaseUrl} + "\",\"assets\":[]}";

    const auto result = updates::EvaluateGitHubLatestRelease(document, Source());

    REQUIRE(result);
    REQUIRE(result->release);
    CHECK_FALSE(result->release->runtimeAsset);
    CHECK(result->release->releasePageUrl == ReleaseUrl);
  }
  SECTION("the target is unsupported")
  {
    auto source = Source();
    source.target = {};

    const auto result = updates::EvaluateGitHubLatestRelease(ReleaseJson(), source);

    REQUIRE(result);
    REQUIRE(result->release);
    CHECK_FALSE(result->release->runtimeAsset);
    CHECK(result->release->releasePageUrl == ReleaseUrl);
  }
}

TEST_CASE("Native runtime assets retain exact GitHub URL validation",
          "[update][github][platform][security]")
{
  for (const auto &[target, name] : TargetAssetCases)
  {
    auto source = Source();
    source.target = target;

    for (const auto &url : {
             "https://example.test/" + std::string{name},
             AssetDownloadUrl(name) + "?redirect=other",
             AssetDownloadUrl(name) + "/../another-asset.zip"})
    {
      CAPTURE(name, url);

      const auto result = updates::EvaluateGitHubLatestRelease(
          ReleaseJson(Tag, ReleaseUrl, AssetJson(name, url)), source);

      REQUIRE_FALSE(result);
      CHECK(result.error().kind == updates::UpdateErrorKind::InvalidResponse);
    }

    const auto result = updates::EvaluateGitHubLatestRelease(
        ReleaseJson(Tag, "https://example.test/release",
                    AssetJson("unrelated-asset.zip")),
        source);

    REQUIRE_FALSE(result);
    CHECK(result.error().kind == updates::UpdateErrorKind::InvalidResponse);
  }
}

TEST_CASE("An equal or older GitHub release is up to date without an asset",
          "[update][github]")
{
  for (const std::string_view tag : {"v0.1.0", "v0.0.99"})
  {
    CAPTURE(tag);

    const auto document =
        "{\"tag_name\":\"" + std::string{tag} + "\"}";

    const auto result =
        updates::EvaluateGitHubLatestRelease(document, Source());

    REQUIRE(result);
    CHECK(result->disposition == updates::UpdateDisposition::UpToDate);
    CHECK_FALSE(result->release);
  }
}

TEST_CASE("GitHub release metadata rejects malformed document structure",
          "[update][github]")
{
  SECTION("malformed syntax")
  {
    CheckInvalidRelease("{", updates::UpdateErrorKind::Parse);
  }
  SECTION("non-object root")
  {
    CheckInvalidRelease("[]");
  }
  SECTION("missing tag")
  {
    CheckInvalidRelease("{}");
  }
  SECTION("invalid tag")
  {
    CheckInvalidRelease(ReleaseJson("v0.2.0-beta.1"));
  }
  SECTION("missing assets")
  {
    CheckInvalidRelease(
        "{\"tag_name\":\"v0.2.0\",\"html_url\":\"" +
        std::string{ReleaseUrl} + "\"}");
  }
  SECTION("non-object asset")
  {
    CheckInvalidRelease(ReleaseJson(Tag, ReleaseUrl, "42"));
  }
}

TEST_CASE("GitHub release metadata enforces trusted release and asset URLs",
          "[update][github][security]")
{
  SECTION("release URL")
  {
    CheckInvalidRelease(ReleaseJson(
        Tag, "https://example.test/Havoc7891/havRemote/releases/tag/v0.2.0"));
  }
  SECTION("download URL")
  {
    CheckInvalidRelease(ReleaseJson(
        Tag,
        ReleaseUrl,
        AssetJson(RuntimeAssetName,
                  "https://example.test/havRemote-0.2.0-windows-x86_64.zip")));
  }
  SECTION("tag and URL must agree")
  {
    CheckInvalidRelease(ReleaseJson(
        "0.2.0",
        ReleaseUrl,
        AssetJson(RuntimeAssetName, DownloadUrl)));
  }
}

TEST_CASE("GitHub release metadata rejects ambiguous or invalid runtime assets",
          "[update][github][security]")
{
  SECTION("duplicate runtime archive")
  {
    const auto runtime = AssetJson();

    CheckInvalidRelease(ReleaseJson(Tag,
                                    ReleaseUrl,
                                    runtime + ',' + runtime));
  }
  SECTION("zero size")
  {
    CheckInvalidRelease(
        ReleaseJson(Tag, ReleaseUrl, AssetJson(RuntimeAssetName, DownloadUrl, "0")));
  }
  SECTION("fractional size")
  {
    CheckInvalidRelease(
        ReleaseJson(Tag, ReleaseUrl, AssetJson(RuntimeAssetName, DownloadUrl, "1.5")));
  }
  SECTION("size is beyond the exact JSON integer range")
  {
    CheckInvalidRelease(
        ReleaseJson(Tag, ReleaseUrl, AssetJson(RuntimeAssetName, DownloadUrl, "9007199254740992")));
  }
  SECTION("invalid digest")
  {
    CheckInvalidRelease(ReleaseJson(
        Tag,
        ReleaseUrl,
        AssetJson(RuntimeAssetName, DownloadUrl, "4096", "\"sha256:1234\"")));
  }
  SECTION("title has the wrong type")
  {
    CheckInvalidRelease(ReleaseJson(Tag, ReleaseUrl, {}, "false"));
  }
  SECTION("notes have the wrong type")
  {
    CheckInvalidRelease(ReleaseJson(Tag, ReleaseUrl, {}, "null", "[]"));
  }
}

TEST_CASE("Long GitHub release notes are truncated on a UTF-8 boundary",
          "[update][github]")
{
  std::string notes(65'535U, 'x');
  notes += "\xE2\x82\xAC";

  const auto document = ReleaseJson(
      Tag,
      ReleaseUrl,
      {},
      "\"Second release\"",
      "\"" + notes + "\"");

  const auto result =
      updates::EvaluateGitHubLatestRelease(document, Source());

  REQUIRE(result);
  REQUIRE(result->release);
  CHECK(result->release->notes == std::string(65'535U, 'x'));
}

TEST_CASE("GitHub release responses have a hard two MiB limit",
          "[update][github][security]")
{
  const std::string oversized(2U * 1024U * 1024U + 1U, 'x');

  const auto result =
      updates::EvaluateGitHubLatestRelease(oversized, Source());

  REQUIRE_FALSE(result);
  CHECK(result.error().kind == updates::UpdateErrorKind::ResponseTooLarge);
}

TEST_CASE("GitHub update service constructs the public latest-release request",
          "[update][github][transport]")
{
  bool called{};

  updates::UpdateTransport transport =
      [&](const std::string_view url,
          const std::string_view userAgent,
          const std::stop_token stopToken)
      -> std::expected<updates::UpdateHttpResponse, updates::UpdateError>
  {
    called = true;

    CHECK(url ==
          "https://api.github.com/repos/Havoc7891/havRemote/releases/latest");
    CHECK(userAgent ==
          "havRemote/0.1.0 (+https://github.com/Havoc7891/havRemote)");
    CHECK_FALSE(stopToken.stop_requested());

    return updates::UpdateHttpResponse{.status = 200,
                                       .body = ReleaseJson()};
  };

  auto service = updates::MakeGitHubUpdateService(Source(),
                                                  std::move(transport));

  const auto result = service->CheckLatest({});

  REQUIRE(called);
  REQUIRE(result);
  CHECK(result->disposition == updates::UpdateDisposition::Available);
}

TEST_CASE("GitHub update service rejects invalid repository identifiers locally",
          "[update][github][security]")
{
  for (auto invalidSource : {
           updates::GitHubUpdateSource{"", "havRemote", {0, 1, 0}},
           updates::GitHubUpdateSource{"Havoc7891/other", "havRemote", {0, 1, 0}},
           updates::GitHubUpdateSource{"Havoc7891", "havRemote@evil", {0, 1, 0}},
       })
  {
    CAPTURE(invalidSource.owner, invalidSource.repository);

    bool called{};

    auto service = updates::MakeGitHubUpdateService(
        std::move(invalidSource),
        [&](std::string_view, std::string_view, std::stop_token)
            -> std::expected<updates::UpdateHttpResponse,
                             updates::UpdateError>
        {
          called = true;

          return updates::UpdateHttpResponse{};
        });

    const auto result = service->CheckLatest({});

    REQUIRE_FALSE(result);
    CHECK(result.error().kind == updates::UpdateErrorKind::Initialization);
    CHECK_FALSE(called);
  }
}

TEST_CASE("GitHub update service classifies HTTP failures",
          "[update][github][transport]")
{
  constexpr std::array statuses{404L, 403L, 429L, 500L};

  for (const long status : statuses)
  {
    CAPTURE(status);

    auto service = updates::MakeGitHubUpdateService(
        Source(),
        [status](std::string_view, std::string_view, std::stop_token)
            -> std::expected<updates::UpdateHttpResponse,
                             updates::UpdateError>
        {
          return updates::UpdateHttpResponse{.status = status, .body = {}};
        });

    const auto result = service->CheckLatest({});

    REQUIRE_FALSE(result);
    CHECK(result.error().kind == updates::UpdateErrorKind::HttpStatus);
    CHECK(result.error().httpStatus == status);
    CHECK_FALSE(result.error().message.empty());
  }
}

TEST_CASE("GitHub update service preserves transport cancellation details",
          "[update][github][transport]")
{
  std::stop_source stop;
  stop.request_stop();

  auto service = updates::MakeGitHubUpdateService(
      Source(),
      [](std::string_view, std::string_view, const std::stop_token token)
          -> std::expected<updates::UpdateHttpResponse, updates::UpdateError>
      {
        CHECK(token.stop_requested());

        return std::unexpected(updates::UpdateError{
            .kind = updates::UpdateErrorKind::Cancelled,
            .message = "cancelled by test",
            .httpStatus = std::nullopt,
            .curlCode = 42,
        });
      });

  const auto result = service->CheckLatest(stop.get_token());

  REQUIRE_FALSE(result);
  CHECK(result.error().kind == updates::UpdateErrorKind::Cancelled);
  CHECK(result.error().message == "cancelled by test");
  CHECK(result.error().curlCode == 42);
}

TEST_CASE("GitHub update service also limits injected transport responses",
          "[update][github][transport][security]")
{
  auto service = updates::MakeGitHubUpdateService(
      Source(),
      [](std::string_view, std::string_view, std::stop_token)
          -> std::expected<updates::UpdateHttpResponse, updates::UpdateError>
      {
        return updates::UpdateHttpResponse{
            .status = 200,
            .body = std::string(2U * 1024U * 1024U + 1U, 'x'),
        };
      });

  const auto result = service->CheckLatest({});

  REQUIRE_FALSE(result);
  CHECK(result.error().kind == updates::UpdateErrorKind::ResponseTooLarge);
}
