// SPDX-License-Identifier: MIT

#include "core/logSanitizer.hpp"
#include "config/configPathProvider.hpp"
#include "config/configRepository.hpp"
#include "config/csonConfigRepository.hpp"
#include "platform/credentialStore.hpp"
#include "protocol/ftpSession.hpp"

#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace havremote;

namespace
{
  [[nodiscard]] std::string IdleEnvironment(std::string_view name,
                                            std::string_view fallback = {})
  {
    if (const auto *value = std::getenv(std::string{name}.c_str()))
    {
      return value;
    }

    return std::string{fallback};
  }

  [[nodiscard]] unsigned int IdleNumber(std::string_view name,
                                        unsigned int fallback,
                                        unsigned int minimum,
                                        unsigned int maximum)
  {
    const auto text = IdleEnvironment(name, std::to_string(fallback));

    unsigned int value{};

    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);

    INFO("Invalid numeric test setting: " << name);
    REQUIRE(error == std::errc{});
    REQUIRE(end == text.data() + text.size());
    REQUIRE(value >= minimum);
    REQUIRE(value <= maximum);

    return value;
  }

  [[nodiscard]] std::string_view IdleLevel(DiagnosticLevel level)
  {
    switch (level)
    {
    case DiagnosticLevel::Debug:
      return "DEBUG";
    case DiagnosticLevel::Information:
      return "INFO";
    case DiagnosticLevel::Warning:
      return "WARNING";
    case DiagnosticLevel::Error:
      return "ERROR";
    }

    return "UNKNOWN";
  }

  class IdleDownloadDirectory final
  {
  public:
    IdleDownloadDirectory()
        : mPath(std::filesystem::temp_directory_path() /
                ("havremote-ftp-idle-" + GenerateId()))
    {
      std::filesystem::create_directories(mPath);
    }

    ~IdleDownloadDirectory()
    {
      std::error_code ignored;
      std::filesystem::remove_all(mPath, ignored);
    }

    [[nodiscard]] const std::filesystem::path &Path() const noexcept { return mPath; }

  private:
    std::filesystem::path mPath;
  };

  class IdlePassword final
  {
  public:
    ~IdlePassword()
    {
      if (!value.empty())
      {
        volatile char *bytes = value.data();

        for (std::size_t index = 0; index < value.size(); ++index)
        {
          bytes[index] = 0;
        }
      }
    }

    std::string value;
  };
} // namespace

TEST_CASE("FTP reconnects on demand after a dedicated server idle timeout",
          "[integration][manual][ftp-idle][.]")
{
  const auto host = IdleEnvironment("HAVREMOTE_FTP_IDLE_HOST");

  if (host.empty())
  {
    SKIP("Set HAVREMOTE_FTP_IDLE_HOST to run the dedicated idle-timeout endpoint test");
  }

  INFO("Set HAVREMOTE_FTP_IDLE_USE_SAVED=1 only after approving read-only testing with this endpoint's saved credentials");
  REQUIRE(IdleEnvironment("HAVREMOTE_FTP_IDLE_USE_SAVED") == "1");

  const bool traceEnabled = IdleEnvironment("HAVREMOTE_FTP_CONTROL_TRACE") == "1";

  const auto port = static_cast<std::uint16_t>(
      IdleNumber("HAVREMOTE_FTP_IDLE_PORT", 2122, 1, 65535));

  const auto siteId = IdleEnvironment("HAVREMOTE_FTP_IDLE_SITE_ID");

  const auto protocolText = IdleEnvironment("HAVREMOTE_FTP_IDLE_PROTOCOL");

  SiteProfile site;
  IdlePassword secret;

  const auto &password = secret.value;

  std::string storedPin;

#if defined(_WIN32)
  {
    const auto *appData = _wgetenv(L"APPDATA");

    REQUIRE(appData != nullptr);

    auto provider = std::make_shared<config::AppDataConfigPathProvider>(
        std::filesystem::path{appData});

    std::unique_ptr<config::IConfigRepository> repository =
        std::make_unique<config::CsonConfigRepository>(provider);

    const auto configPath = repository->ConfigPath();

    REQUIRE(configPath);

    // OPEN_EXISTING plus a read-only sharing lock prevents Load() from ever
    // creating defaults or modifying the real configuration during this test.
    const auto rawLock = CreateFileW(configPath->c_str(), GENERIC_READ,
                                     FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);

    REQUIRE(rawLock != INVALID_HANDLE_VALUE);

    std::unique_ptr<void, decltype(&CloseHandle)> configLock{rawLock, CloseHandle};

    const auto loaded = repository->Load();

    INFO((loaded ? "Loaded existing configuration" : loaded.error().message));
    REQUIRE(loaded);
    REQUIRE_FALSE(loaded->createdDefaults);

    std::vector<const SiteProfile *> candidates;
    std::vector<const SiteProfile *> selected;

    for (const auto &candidate : loaded->data.sites)
    {
      if (candidate.host != host || candidate.port != port ||
          candidate.protocol == ProtocolKind::Sftp)
      {
        continue;
      }

      candidates.push_back(&candidate);

      if ((!siteId.empty() && candidate.id != siteId) ||
          (!protocolText.empty() && ToString(candidate.protocol) != protocolText))
      {
        continue;
      }

      selected.push_back(&candidate);
    }

    if (selected.size() != 1U)
    {
      for (const auto *candidate : candidates)
      {
        std::cout << "Matching saved endpoint: id="
                  << SanitizeDiagnosticText(candidate->id) << " name="
                  << SanitizeDiagnosticText(candidate->name) << " protocol="
                  << ToString(candidate->protocol) << std::endl;
      }
    }

    INFO("Exactly one saved site must match the requested endpoint. Use HAVREMOTE_FTP_IDLE_SITE_ID if ambiguous");
    REQUIRE(selected.size() == 1U);

    site = *selected.front();

    INFO("Selected saved site: " << SanitizeDiagnosticText(site.name)
                                 << " (" << SanitizeDiagnosticText(site.id) << ")");
    REQUIRE(site.authentication.kind == AuthenticationKind::Password);
    INFO("Save this dedicated site's password in Site Manager before running the saved-credential idle test. Another site's credentials will not be used");
    REQUIRE_FALSE(site.authentication.credentialId.empty());

    auto credentials = platform::MakeCredentialStore();

    auto payload = credentials->Load(site.authentication.credentialId);

    INFO((payload ? "Loaded selected endpoint credential in process" : payload.error().message));
    REQUIRE(payload);
    REQUIRE_FALSE(payload->Secret().empty());

    secret.value.assign(reinterpret_cast<const char *>(payload->Secret().data()),
                        payload->Secret().size());

    for (const auto &trust : loaded->data.tlsTrust)
    {
      if (trust.host == host && trust.port == port)
      {
        storedPin = trust.publicKeyPin;

        if (storedPin.starts_with("SHA256:"))
        {
          storedPin = "sha256//" + storedPin.substr(7);
        }

        if (storedPin.starts_with("sha256//"))
        {
          const auto separator = storedPin.find(';');
          const auto firstLength =
              (separator == std::string::npos ? storedPin.size() : separator) - 8U;

          if (firstLength % 4U != 0U)
          {
            storedPin.insert(8U + firstLength, 4U - firstLength % 4U, '=');
          }
        }

        break;
      }
    }
  }
#else
  FAIL("This saved-credential endpoint test requires Windows Credential Manager");
#endif

  if (const auto directory = IdleEnvironment("HAVREMOTE_FTP_IDLE_DIRECTORY"); !directory.empty())
  {
    site.initialRemoteDirectory = RemotePath{directory};
  }

  const auto idleTime = std::chrono::seconds{
      IdleNumber("HAVREMOTE_FTP_IDLE_SECONDS", 12, 11, 30)};

  const auto started = std::chrono::steady_clock::now();

  std::vector<std::pair<DiagnosticLevel, std::string>> diagnostics;

  auto safe = [&password](std::string_view text)
  {
    auto result = SanitizeDiagnosticText(text);

    for (std::size_t at = 0; (at = result.find(password, at)) != std::string::npos;)
    {
      result.replace(at, password.size(), "<redacted>");

      at += std::string_view{"<redacted>"}.size();
    }

    return result;
  };

  auto report = [&](std::string_view stage, std::string_view text)
  {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    std::cout << "[ftp-idle +" << elapsed << "ms " << stage << "] "
              << safe(text) << std::endl;
  };

  SessionCallbacks callbacks;
  callbacks.requestCredential = [&password, &site](const CredentialRequest &request,
                                                   std::stop_token)

  {
    if (request.siteId != site.id || request.kind != CredentialKind::Password)
    {
      return Result<std::string>{std::unexpected(RemoteError{
          .code = RemoteErrorCode::CredentialUnavailable,
          .message = "The idle test cannot provide credentials for another request",
      })};
    }

    return Result<std::string>{password};
  };

  callbacks.diagnostic = [&](DiagnosticLevel level, std::string_view message)
  {
    auto sanitized = safe(message);
    diagnostics.emplace_back(level, sanitized);
    report(IdleLevel(level), sanitized);
  };

  callbacks.tlsPinnedPublicKey = storedPin;
  callbacks.verifyTrust = [](const TrustChallenge &, std::stop_token)
  {
    // Do not silently grant an exception for an arbitrary supplied endpoint.
    // Self-signed test FTPS endpoints require an existing endpoint-scoped PIN.
    return Result<TrustDecision>{TrustDecision::Reject};
  };

  auto reconnectionCount = [&]
  {
    return std::ranges::count_if(diagnostics, [](const auto &diagnostic)
                                 { return diagnostic.first == DiagnosticLevel::Information &&
                                          diagnostic.second == "Reconnected to the FTP server"; });
  };

  auto describe = [&](std::string_view operation, const auto &result)
  {
    if (result)
    {
      report(operation, "success");
    }
    else
    {
      const auto &error = result.error();

      report(operation,
             "failed: nativeCode=" + std::to_string(error.nativeCode) +
                 " retryable=" + (error.retryable ? "true" : "false") +
                 " uncertain=" + (error.operationMayHaveSucceeded ? "true" : "false") +
                 " message=" + error.message);
    }
  };

  auto session = MakeFtpSession();

  report("CONNECT", std::string{ToString(site.protocol)} + " " + site.host + ":" +
                        std::to_string(site.port));

  const auto connected = session->Connect(site, callbacks, {});

  describe("CONNECT", connected);

  REQUIRE(connected);
  REQUIRE(session->Connected());
  CHECK(reconnectionCount() == 0);

  const auto initial = session->List(site.initialRemoteDirectory, {});

  describe("INITIAL LIST", initial);

  REQUIRE(initial);

  report("INITIAL LIST", "entry count=" + std::to_string(initial->size()));

  CHECK(reconnectionCount() == 0);
  CHECK(std::ranges::any_of(diagnostics, [](const auto &diagnostic)
                            { return diagnostic.second.find("FTP control trace [session=") != std::string::npos; }) == traceEnabled);

  auto idle = [&]
  {
    report("IDLE", "begin " + std::to_string(idleTime.count()) +
                       " seconds without calling any protocol operation");

    const auto before = diagnostics.size();

    std::this_thread::sleep_for(idleTime);

    const auto cachedConnected = session->Connected();

    report("IDLE", "end: cached connected()=" +
                       std::string{cachedConnected ? "true" : "false"} +
                       ", diagnostic delta=" + std::to_string(diagnostics.size() - before) +
                       ". Cached state is not a socket-liveness probe");

    CHECK(cachedConnected);
    CHECK(diagnostics.size() == before);
  };

  idle();

  const auto firstRefresh = session->List(site.initialRemoteDirectory, {});

  describe("FIRST POST-IDLE LIST", firstRefresh);

  CHECK(firstRefresh);

  if (firstRefresh)
  {
    report("FIRST POST-IDLE LIST", "entry count=" + std::to_string(firstRefresh->size()));
  }

  // MLSD plus its owner/group LIST fallback share the replacement control
  // connection. The whole refresh must report exactly one reconnection.
  CHECK(reconnectionCount() == 1);

  report("FIRST POST-IDLE LIST", "normal reconnection Info count=" +
                                     std::to_string(reconnectionCount()));

  const auto firstWarmRefresh = session->List(site.initialRemoteDirectory, {});

  describe("FIRST WARM LIST", firstWarmRefresh);

  CHECK(firstWarmRefresh);
  CHECK(reconnectionCount() == 1);

  idle();

  const auto secondRefresh = session->List(site.initialRemoteDirectory, {});

  describe("SECOND POST-IDLE LIST", secondRefresh);

  CHECK(secondRefresh);

  if (secondRefresh)
  {
    report("SECOND POST-IDLE LIST", "entry count=" + std::to_string(secondRefresh->size()));
  }

  CHECK(reconnectionCount() == 2);

  report("SECOND POST-IDLE LIST", "normal reconnection Info count=" +
                                      std::to_string(reconnectionCount()));

  const auto secondWarmRefresh = session->List(site.initialRemoteDirectory, {});

  describe("SECOND WARM LIST", secondWarmRefresh);

  CHECK(secondWarmRefresh);
  CHECK(reconnectionCount() == 2);

  // Never choose or read an arbitrary user file. RETR is opt-in for an exact
  // known-safe file. No operation in this test writes to the remote endpoint.
  const auto readFile = IdleEnvironment("HAVREMOTE_FTP_IDLE_READ_FILE");

  if (!readFile.empty())
  {
    const auto remote = RemotePath{readFile};

    const auto info = session->Stat(remote, {});

    describe("READ-ONLY STAT", info);

    REQUIRE(info);
    REQUIRE(info->kind == RemoteEntryKind::File);
    REQUIRE(info->size <= 16U * 1024U * 1024U);

    IdleDownloadDirectory temporary;

    const auto local = temporary.Path() / "read-only-download.bin";

    const auto downloaded = session->Download(
        remote, local,
        TransferOptions{.jobId = "idle-read-only-retr",
                        .temporaryRemotePath = std::nullopt,
                        .beforeFinalize = {}},
        {}, {});

    describe("READ-ONLY RETR", downloaded);

    REQUIRE(downloaded);
    CHECK(std::filesystem::file_size(local) == info->size);

    report("READ-ONLY RETR", "downloaded byte count=" + std::to_string(info->size));
  }
  else
  {
    report("READ-ONLY RETR", "not requested. Set HAVREMOTE_FTP_IDLE_READ_FILE for an exact known-safe file");
  }

  session->Disconnect();

  CHECK_FALSE(session->Connected());

  report("DISCONNECT", "complete");
}
