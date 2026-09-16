// SPDX-License-Identifier: MIT

#include "protocol/ftpSession.hpp"
#include "protocol/sftpSession.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

using namespace havremote;

namespace
{
  constexpr std::string_view TestPassword = "havremote-test-password";

  class TemporaryDirectory final
  {
  public:
    TemporaryDirectory()
        : mPath(std::filesystem::temp_directory_path() /
                ("havremote-integration-" + GenerateId()))
    {
      std::filesystem::create_directories(mPath);
    }

    ~TemporaryDirectory()
    {
      std::error_code ignored;
      std::filesystem::remove_all(mPath, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const noexcept { return mPath; }

  private:
    std::filesystem::path mPath;
  };

  [[nodiscard]] std::string Environment(std::string_view name,
                                        std::string_view fallback)
  {
    if (const auto *value = std::getenv(std::string{name}.c_str()))
    {
      return value;
    }

    return std::string{fallback};
  }

  [[nodiscard]] std::uint16_t EnvironmentPort(std::string_view name,
                                              std::uint16_t fallback)
  {
    const auto text = Environment(name, std::to_string(fallback));

    unsigned int value{};

    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);

    if (error != std::errc{} || end != text.data() + text.size() || value == 0 ||
        value > 65535)
    {
      FAIL("Invalid port in environment variable " << name << ": " << text);
    }

    return static_cast<std::uint16_t>(value);
  }

  [[nodiscard]] std::string ReadFile(const std::filesystem::path &path)
  {
    std::ifstream input{path, std::ios::binary};

    REQUIRE(input);

    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
  }

  void WriteFile(const std::filesystem::path &path, std::string_view contents)
  {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};

    REQUIRE(output);

    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));

    REQUIRE(output);
  }

  [[nodiscard]] std::string MakePayload()
  {
    std::string result(2U * 1024U * 1024U, '\0');

    for (std::size_t index = 0; index < result.size(); ++index)
    {
      result[index] = static_cast<char>((index * 31U + 17U) % 251U);
    }

    return result;
  }

  [[nodiscard]] bool HasEntry(const std::vector<RemoteEntry> &entries,
                              std::string_view name,
                              RemoteEntryKind kind)
  {
    return std::ranges::any_of(entries, [&](const RemoteEntry &entry)
                               { return entry.name.Bytes() == name && entry.kind == kind; });
  }

  [[nodiscard]] bool HasDiagnosticContaining(
      const std::vector<std::pair<DiagnosticLevel, std::string>> &diagnostics,
      const std::string_view text)
  {
    return std::ranges::any_of(
        diagnostics,
        [text](const auto &diagnostic)
        {
          return diagnostic.second.find(text) != std::string::npos;
        });
  }

  [[nodiscard]] SessionCallbacks PasswordCallbacks(
      std::vector<std::pair<DiagnosticLevel, std::string>> *diagnostics = nullptr)
  {
    SessionCallbacks callbacks;
    callbacks.requestCredential = [](const CredentialRequest &, std::stop_token)
    {
      return Result<std::string>{std::string{TestPassword}};
    };

    if (diagnostics)
    {
      callbacks.diagnostic = [diagnostics](DiagnosticLevel level, std::string_view message)
      {
        diagnostics->emplace_back(level, std::string{message});
      };
    }

    return callbacks;
  }

  [[nodiscard]] SiteProfile Profile(ProtocolKind protocol,
                                    std::uint16_t port,
                                    RemotePath initialDirectory)
  {
    SiteProfile site;
    site.id = "integration-" + std::string{ToString(protocol)};
    site.name = "Integration " + std::string{ToString(protocol)};
    site.protocol = protocol;
    site.host = Environment("HAVREMOTE_INTEGRATION_HOST", "127.0.0.1");
    site.port = port;
    site.username = "havremote";
    site.authentication.kind = AuthenticationKind::Password;
    site.authentication.credentialId = "integration-only";
    site.initialRemoteDirectory = std::move(initialDirectory);

    return site;
  }

  void ExerciseProtocol(IRemoteSession &session,
                        const SiteProfile &site,
                        const SessionCallbacks &callbacks)
  {
    INFO("protocol=" << ToString(site.protocol) << " port=" << site.port);

    const auto connected = session.Connect(site, callbacks, {});

    INFO((connected ? "Connected" : connected.error().message));
    REQUIRE(connected);

    auto fixture = session.List(site.initialRemoteDirectory, {});

    REQUIRE(fixture);
    CHECK(HasEntry(*fixture, "readme.txt", RemoteEntryKind::File));
    CHECK(HasEntry(*fixture, "nested", RemoteEntryKind::Directory));
    CHECK(HasEntry(*fixture, std::string{"Gr\xc3\xbc\xc3\x9f"
                                         "e.txt"},
                   RemoteEntryKind::File));

    if (site.protocol == ProtocolKind::Sftp)
    {
      const auto readme = std::ranges::find(*fixture, "readme.txt",
                                            [](const RemoteEntry &entry)
                                            {
                                              return entry.name.Bytes();
                                            });

      REQUIRE(readme != fixture->end());
      REQUIRE(readme->permissions);
      CHECK((*readme->permissions & ~07777U) == 0U);
      REQUIRE(readme->owner);
      CHECK_FALSE(readme->owner->empty());
      REQUIRE(readme->group);
      CHECK_FALSE(readme->group->empty());

      const auto danglingLink = site.initialRemoteDirectory.Joined(
          RemotePath{"dangling-link.txt"});

      const auto missingTarget = site.initialRemoteDirectory.Joined(
          RemotePath{"missing-link-target.txt"});

      const auto linkBeforeCreate = session.Stat(danglingLink, {});

      REQUIRE(linkBeforeCreate);
      CHECK(linkBeforeCreate->kind == RemoteEntryKind::Symlink);

      const auto targetBeforeCreate = session.Stat(missingTarget, {});

      REQUIRE_FALSE(targetBeforeCreate);
      CHECK(targetBeforeCreate.error().code == RemoteErrorCode::NotFound);

      const auto createOverLink = session.CreateRemoteFile(danglingLink, false, {});

      REQUIRE_FALSE(createOverLink);
      CHECK(createOverLink.error().code == RemoteErrorCode::AlreadyExists);
      CHECK_FALSE(createOverLink.error().operationMayHaveSucceeded);

      const auto preservedLink = session.Stat(danglingLink, {});

      REQUIRE(preservedLink);
      CHECK(preservedLink->kind == RemoteEntryKind::Symlink);

      const auto targetAfterCreate = session.Stat(missingTarget, {});

      REQUIRE_FALSE(targetAfterCreate);
      CHECK(targetAfterCreate.error().code == RemoteErrorCode::NotFound);
    }

    const RemotePath remoteRoot = site.initialRemoteDirectory.Joined(
        RemotePath{"smoke-" + GenerateId()});

    REQUIRE(session.Mkdir(remoteRoot.Joined(RemotePath{"deep/child"}), true, {}));

    const auto createdEmpty = remoteRoot.Joined(RemotePath{"created-empty.txt"});

    REQUIRE(session.CreateRemoteFile(createdEmpty, false, {}));

    const auto createdEmptyInfo = session.Stat(createdEmpty, {});

    REQUIRE(createdEmptyInfo);
    CHECK(createdEmptyInfo->kind == RemoteEntryKind::File);
    CHECK(createdEmptyInfo->size == 0U);

    const auto duplicateCreate = session.CreateRemoteFile(createdEmpty, false, {});

    REQUIRE_FALSE(duplicateCreate);
    CHECK(duplicateCreate.error().code == RemoteErrorCode::AlreadyExists);
    CHECK_FALSE(duplicateCreate.error().retryable);
    CHECK_FALSE(duplicateCreate.error().operationMayHaveSucceeded);

    if (site.protocol == ProtocolKind::Sftp)
    {
      const auto existingDirectory = remoteRoot.Joined(RemotePath{"deep/child"});

      const auto createOverDirectory = session.CreateRemoteFile(existingDirectory, false, {});

      REQUIRE_FALSE(createOverDirectory);
      CHECK(createOverDirectory.error().code == RemoteErrorCode::AlreadyExists);
      CHECK_FALSE(createOverDirectory.error().operationMayHaveSucceeded);

      const auto preservedDirectory = session.Stat(existingDirectory, {});

      REQUIRE(preservedDirectory);
      CHECK(preservedDirectory->kind == RemoteEntryKind::Directory);

      const auto deniedDirectory = remoteRoot.Joined(RemotePath{"denied"});

      const auto deniedFile = deniedDirectory.Joined(RemotePath{"new-file.txt"});

      REQUIRE(session.Mkdir(deniedDirectory, false, {}));
      REQUIRE(session.SetPermissions(deniedDirectory, 0000U, {}));

      const auto deniedCreate = session.CreateRemoteFile(deniedFile, false, {});

      REQUIRE(session.SetPermissions(deniedDirectory, 0755U, {}));
      REQUIRE_FALSE(deniedCreate);
      CHECK(deniedCreate.error().code == RemoteErrorCode::PermissionDenied);
      CHECK_FALSE(deniedCreate.error().operationMayHaveSucceeded);

      const auto notCreated = session.Stat(deniedFile, {});

      REQUIRE_FALSE(notCreated);
      CHECK(notCreated.error().code == RemoteErrorCode::NotFound);

      const auto cancelledFile = remoteRoot.Joined(RemotePath{"cancelled-create.txt"});

      std::stop_source createCancellation;
      createCancellation.request_stop();

      const auto cancelledCreate = session.CreateRemoteFile(
          cancelledFile, false, createCancellation.get_token());

      REQUIRE_FALSE(cancelledCreate);
      CHECK(cancelledCreate.error().code == RemoteErrorCode::Cancelled);
      CHECK_FALSE(cancelledCreate.error().operationMayHaveSucceeded);

      const auto cancelledFileInfo = session.Stat(cancelledFile, {});

      REQUIRE_FALSE(cancelledFileInfo);
      CHECK(cancelledFileInfo.error().code == RemoteErrorCode::NotFound);
    }

    TemporaryDirectory local;

    const auto payload = MakePayload();

    const auto uploadPath = local.Path() / "upload.bin";

    WriteFile(uploadPath, payload);

    const auto remoteUpload = remoteRoot.Joined(RemotePath{"upload.bin"});
    const auto remoteTemporary = remoteRoot.Joined(
        RemotePath{"upload.part-" + GenerateId()});

    std::uint64_t lastUploadProgress{};

    REQUIRE(session.Upload(
        uploadPath, remoteUpload,
        TransferOptions{.jobId = "upload",
                        .overwrite = false,
                        .useTemporaryName = true,
                        .temporaryRemotePath = remoteTemporary,
                        .beforeFinalize = {}},
        [&](const TransferProgress &progress)
        {
          lastUploadProgress = progress.bytesTransferred;

          return TransferControl::Continue;
        },
        {}));

    CHECK(lastUploadProgress == payload.size());

    auto uploaded = session.Stat(remoteUpload, {});

    REQUIRE(uploaded);
    CHECK(uploaded->kind == RemoteEntryKind::File);
    CHECK(uploaded->size == payload.size());

    const auto createOverExisting = session.CreateRemoteFile(remoteUpload, false, {});

    REQUIRE_FALSE(createOverExisting);
    CHECK(createOverExisting.error().code == RemoteErrorCode::AlreadyExists);
    CHECK_FALSE(createOverExisting.error().retryable);
    CHECK_FALSE(createOverExisting.error().operationMayHaveSucceeded);

    const auto preservedUpload = session.Stat(remoteUpload, {});

    REQUIRE(preservedUpload);
    CHECK(preservedUpload->size == payload.size());

    // Seed the recorded temporary name with a deterministic prefix so upload
    // resume cannot accidentally pass by replacing the file with its suffix.
    const auto prefixSize = payload.size() / 3U + 17U;

    const auto prefixPath = local.Path() / "upload-prefix.bin";

    WriteFile(prefixPath, std::string_view{payload}.substr(0, prefixSize));

    const auto resumedUpload = remoteRoot.Joined(RemotePath{"resumed-upload.bin"});

    const auto resumedTemporary = remoteRoot.Joined(
        RemotePath{"resumed-upload.part-" + GenerateId()});

    REQUIRE(session.Upload(
        prefixPath, resumedTemporary,
        TransferOptions{.jobId = "upload-resume-prefix",
                        .overwrite = false,
                        .useTemporaryName = false,
                        .temporaryRemotePath = std::nullopt,
                        .beforeFinalize = {}},
        {}, {}));

    const auto partialUpload = session.Stat(resumedTemporary, {});

    REQUIRE(partialUpload);
    REQUIRE(partialUpload->size == prefixSize);
    REQUIRE(partialUpload->size < payload.size());

    const auto beforeResumedUpload = session.Stat(resumedUpload, {});

    REQUIRE_FALSE(beforeResumedUpload);
    CHECK(beforeResumedUpload.error().code == RemoteErrorCode::NotFound);

    std::uint64_t lastResumedUploadProgress{};

    bool resumedUploadFinalizationChecked{};

    const auto resumedUploadResult = session.Upload(
        uploadPath, resumedUpload,
        TransferOptions{
            .jobId = "upload-resume",
            .resumeOffset = partialUpload->size,
            .overwrite = false,
            .useTemporaryName = true,
            .temporaryRemotePath = resumedTemporary,
            .beforeFinalize = [&resumedUploadFinalizationChecked](
                                  const bool plannedOverwrite) -> Result<bool>
            {
              resumedUploadFinalizationChecked = true;

              CHECK_FALSE(plannedOverwrite);

              return plannedOverwrite;
            },
        },
        [&](const TransferProgress &progress)
        {
          CHECK(progress.bytesTransferred >= prefixSize);
          CHECK(progress.bytesTransferred <= payload.size());

          lastResumedUploadProgress = progress.bytesTransferred;

          return TransferControl::Continue;
        },
        {});

    INFO((resumedUploadResult ? "Upload resumed" : resumedUploadResult.error().message));
    REQUIRE(resumedUploadResult);
    CHECK(resumedUploadFinalizationChecked);
    CHECK(lastResumedUploadProgress == payload.size());

    const auto completedResumedUpload = session.Stat(resumedUpload, {});

    REQUIRE(completedResumedUpload);
    CHECK(completedResumedUpload->size == payload.size());

    const auto finalizedResumedTemporary = session.Stat(resumedTemporary, {});

    REQUIRE_FALSE(finalizedResumedTemporary);
    CHECK(finalizedResumedTemporary.error().code == RemoteErrorCode::NotFound);

    const auto resumedDownload = local.Path() / "resumed-upload-download.bin";

    REQUIRE(session.Download(
        resumedUpload, resumedDownload,
        TransferOptions{.jobId = "download-resumed-upload",
                        .overwrite = false,
                        .useTemporaryName = true,
                        .temporaryRemotePath = std::nullopt,
                        .beforeFinalize = {}},
        {}, {}));
    CHECK(ReadFile(resumedDownload) == payload);

    const auto overwriteTarget = remoteRoot.Joined(RemotePath{"overwrite.bin"});

    const auto overwriteTemporary = remoteRoot.Joined(
        RemotePath{"overwrite.part-" + GenerateId()});

    REQUIRE(session.Upload(
        uploadPath, overwriteTarget,
        TransferOptions{.jobId = "upload-overwrite-target",
                        .overwrite = false,
                        .useTemporaryName = true,
                        .temporaryRemotePath = overwriteTemporary,
                        .beforeFinalize = {}},
        {}, {}));
    REQUIRE(session.CreateRemoteFile(overwriteTarget, true, {}));

    const auto overwritten = session.Stat(overwriteTarget, {});

    REQUIRE(overwritten);
    CHECK(overwritten->kind == RemoteEntryKind::File);
    CHECK(overwritten->size == 0U);

    const auto guardedUploadTemporary = remoteRoot.Joined(
        RemotePath{"guarded-upload.part-" + GenerateId()});

    bool uploadFinalizationChecked{};

    const auto rejectedUpload = session.Upload(
        uploadPath,
        overwriteTarget,
        TransferOptions{
            .jobId = "upload-finalization-rejected",
            .overwrite = true,
            .useTemporaryName = true,
            .temporaryRemotePath = guardedUploadTemporary,
            .beforeFinalize = [&uploadFinalizationChecked](const bool) -> Result<bool>
            {
              uploadFinalizationChecked = true;

              return std::unexpected(RemoteError{
                  .code = RemoteErrorCode::Conflict,
                  .message = "Rejected by integration-test finalization guard",
              });
            },
        },
        {},
        {});

    REQUIRE_FALSE(rejectedUpload);
    CHECK(rejectedUpload.error().code == RemoteErrorCode::Conflict);
    CHECK(uploadFinalizationChecked);

    const auto preservedGuardedUpload = session.Stat(overwriteTarget, {});

    REQUIRE(preservedGuardedUpload);
    CHECK(preservedGuardedUpload->size == 0U);

    const auto retainedGuardedUpload = session.Stat(guardedUploadTemporary, {});

    REQUIRE(retainedGuardedUpload);
    CHECK(retainedGuardedUpload->size == payload.size());

    const auto authorizedUploadTemporary = remoteRoot.Joined(
        RemotePath{"authorized-upload.part-" + GenerateId()});

    bool uploadFinalizationAuthorized{};

    bool uploadPlannedOverwrite{};

    REQUIRE(session.Upload(
        uploadPath,
        overwriteTarget,
        TransferOptions{
            .jobId = "upload-finalization-authorized",
            .overwrite = false,
            .useTemporaryName = true,
            .temporaryRemotePath = authorizedUploadTemporary,
            .beforeFinalize = [&uploadFinalizationAuthorized, &uploadPlannedOverwrite](
                                  const bool plannedOverwrite) -> Result<bool>
            {
              uploadFinalizationAuthorized = true;

              uploadPlannedOverwrite = plannedOverwrite;

              return true;
            },
        },
        {},
        {}));

    CHECK(uploadFinalizationAuthorized);
    CHECK_FALSE(uploadPlannedOverwrite);

    const auto authorizedGuardedUpload = session.Stat(overwriteTarget, {});

    REQUIRE(authorizedGuardedUpload);
    CHECK(authorizedGuardedUpload->size == payload.size());

    const auto finalizedAuthorizedTemporary = session.Stat(authorizedUploadTemporary, {});

    REQUIRE_FALSE(finalizedAuthorizedTemporary);
    CHECK(finalizedAuthorizedTemporary.error().code == RemoteErrorCode::NotFound);

    const auto overwrittenDownload = local.Path() / "overwrite-download.bin";

    REQUIRE(session.Download(
        overwriteTarget,
        overwrittenDownload,
        TransferOptions{.jobId = "download-overwritten-upload",
                        .overwrite = true,
                        .useTemporaryName = true,
                        .temporaryRemotePath = std::nullopt,
                        .beforeFinalize = {}},
        {}, {}));
    CHECK(ReadFile(overwrittenDownload) == payload);

    if (site.protocol == ProtocolKind::Sftp)
    {
      REQUIRE(uploaded->permissions);
      CHECK((*uploaded->permissions & ~07777U) == 0U);
      REQUIRE(uploaded->owner);
      CHECK_FALSE(uploaded->owner->empty());
      REQUIRE(uploaded->group);
      CHECK_FALSE(uploaded->group->empty());
    }

    const auto invalidPermissions = session.SetPermissions(remoteUpload, 010000U, {});

    REQUIRE_FALSE(invalidPermissions);
    CHECK(invalidPermissions.error().code == RemoteErrorCode::InvalidArgument);

    const auto rootPermissions = session.SetPermissions(RemotePath::Root(), 0755U, {});

    REQUIRE_FALSE(rootPermissions);
    CHECK(rootPermissions.error().code == RemoteErrorCode::InvalidArgument);
    REQUIRE(session.SetPermissions(remoteUpload, 0640U, {}));

    const auto changedPermissions = session.Stat(remoteUpload, {});

    REQUIRE(changedPermissions);
    REQUIRE(changedPermissions->permissions);
    CHECK((*changedPermissions->permissions & 07777U) == 0640U);

    const auto finalizedTemporary = session.Stat(remoteTemporary, {});

    REQUIRE_FALSE(finalizedTemporary);
    CHECK(finalizedTemporary.error().code == RemoteErrorCode::NotFound);

    const auto downloadPath = local.Path() / "download.bin";

    bool requestedPause{};

    auto paused = session.Download(
        remoteUpload, downloadPath,
        TransferOptions{.jobId = "download-pause",
                        .overwrite = true,
                        .useTemporaryName = true,
                        .temporaryRemotePath = std::nullopt,
                        .beforeFinalize = {}},
        [&](const TransferProgress &progress)
        {
          if (!requestedPause && progress.bytesTransferred > 0)
          {
            requestedPause = true;

            return TransferControl::Pause;
          }

          return TransferControl::Continue;
        },
        {});

    REQUIRE_FALSE(paused);
    CHECK(paused.error().code == RemoteErrorCode::Paused);

    auto partialPath = downloadPath;
    partialPath += ".havremote.part";

    REQUIRE(std::filesystem::is_regular_file(partialPath));

    const auto resumeOffset = std::filesystem::file_size(partialPath);

    REQUIRE(resumeOffset > 0);
    REQUIRE(resumeOffset <= payload.size());
    REQUIRE(session.Download(
        remoteUpload, downloadPath,
        TransferOptions{.jobId = "download-resume",
                        .resumeOffset = resumeOffset,
                        .overwrite = true,
                        .useTemporaryName = true,
                        .temporaryRemotePath = std::nullopt,
                        .beforeFinalize = {}},
        {}, {}));
    CHECK(ReadFile(downloadPath) == payload);
    CHECK_FALSE(std::filesystem::exists(partialPath));

    const auto replacePath = local.Path() / "replace-existing.bin";

    WriteFile(replacePath, "existing-good-content");

    REQUIRE(session.Download(
        remoteUpload, replacePath,
        TransferOptions{.jobId = "download-replace",
                        .overwrite = true,
                        .useTemporaryName = true,
                        .temporaryRemotePath = std::nullopt,
                        .beforeFinalize = {}},
        {}, {}));
    CHECK(ReadFile(replacePath) == payload);

    const auto guardedDownloadPath = local.Path() / "guarded-download.bin";

    constexpr std::string_view guardedDownloadOriginal = "keep-before-finalization";

    WriteFile(guardedDownloadPath, guardedDownloadOriginal);

    bool downloadFinalizationChecked{};

    const auto rejectedDownload = session.Download(
        remoteUpload,
        guardedDownloadPath,
        TransferOptions{
            .jobId = "download-finalization-rejected",
            .overwrite = true,
            .useTemporaryName = true,
            .temporaryRemotePath = std::nullopt,
            .beforeFinalize = [&downloadFinalizationChecked](const bool) -> Result<bool>
            {
              downloadFinalizationChecked = true;

              return std::unexpected(RemoteError{
                  .code = RemoteErrorCode::Conflict,
                  .message = "Rejected by integration-test finalization guard",
              });
            },
        },
        {},
        {});

    REQUIRE_FALSE(rejectedDownload);
    CHECK(rejectedDownload.error().code == RemoteErrorCode::Conflict);
    CHECK(downloadFinalizationChecked);
    CHECK(ReadFile(guardedDownloadPath) == guardedDownloadOriginal);

    auto guardedDownloadPart = guardedDownloadPath;
    guardedDownloadPart += ".havremote.part";

    REQUIRE(std::filesystem::is_regular_file(guardedDownloadPart));
    CHECK(ReadFile(guardedDownloadPart) == payload);

    const auto authorizedDownloadPath = local.Path() / "authorized-download.bin";

    WriteFile(authorizedDownloadPath, guardedDownloadOriginal);

    bool downloadFinalizationAuthorized{};

    bool downloadPlannedOverwrite{};

    REQUIRE(session.Download(
        remoteUpload,
        authorizedDownloadPath,
        TransferOptions{
            .jobId = "download-finalization-authorized",
            .overwrite = false,
            .useTemporaryName = true,
            .temporaryRemotePath = std::nullopt,
            .beforeFinalize = [&downloadFinalizationAuthorized, &downloadPlannedOverwrite](
                                  const bool plannedOverwrite) -> Result<bool>
            {
              downloadFinalizationAuthorized = true;

              downloadPlannedOverwrite = plannedOverwrite;

              return true;
            },
        },
        {},
        {}));

    CHECK(downloadFinalizationAuthorized);
    CHECK_FALSE(downloadPlannedOverwrite);
    CHECK(ReadFile(authorizedDownloadPath) == payload);

    const auto pauseBeforeReplacePath = local.Path() / "pause-before-replace.bin";

    constexpr std::string_view originalDestination = "keep-this-destination";

    WriteFile(pauseBeforeReplacePath, originalDestination);

    bool pauseFinalizationChecked{};

    const auto pausedBeforeReplace = session.Download(
        remoteUpload, pauseBeforeReplacePath,
        TransferOptions{.jobId = "download-pause-before-replace",
                        .overwrite = true,
                        .useTemporaryName = true,
                        .temporaryRemotePath = std::nullopt,
                        .beforeFinalize = [&pauseFinalizationChecked](const bool) -> Result<bool>
                        {
                          pauseFinalizationChecked = true;
                          return true;
                        }},
        [&](const TransferProgress &transfer)
        {
          return transfer.bytesTransferred >= payload.size()
                     ? TransferControl::Pause
                     : TransferControl::Continue;
        },
        {});

    REQUIRE_FALSE(pausedBeforeReplace);
    CHECK(pausedBeforeReplace.error().code == RemoteErrorCode::Paused);
    CHECK_FALSE(pauseFinalizationChecked);
    CHECK(ReadFile(pauseBeforeReplacePath) == originalDestination);

    const auto cancelPath = local.Path() / "cancel.bin";

    auto cancelled = session.Download(
        remoteUpload, cancelPath,
        TransferOptions{.jobId = "download-cancel",
                        .overwrite = true,
                        .useTemporaryName = true,
                        .temporaryRemotePath = std::nullopt,
                        .beforeFinalize = {}},
        [](const TransferProgress &progress)
        {
          return progress.bytesTransferred > 0 ? TransferControl::Cancel
                                               : TransferControl::Continue;
        },
        {});

    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == RemoteErrorCode::Cancelled);

    const auto renamed = remoteRoot.Joined(
        RemotePath{std::string{"renamed-Gr\xc3\xbc\xc3\x9f"
                               "e.bin"}});

    REQUIRE(session.Rename(remoteUpload, renamed, false, {}));

    auto renamedInfo = session.Stat(renamed, {});

    REQUIRE(renamedInfo);
    CHECK(renamedInfo->size == payload.size());

    REQUIRE(session.Remove(remoteRoot, true, {}));

    const auto removed = session.Stat(remoteRoot, {});

    REQUIRE_FALSE(removed);
    CHECK(removed.error().code == RemoteErrorCode::NotFound);

    session.Disconnect();
  }
} // namespace

TEST_CASE("FTP Docker fixture supports browse, file creation, mutation, transfer, pause, resume, and cancel",
          "[integration][docker][ftp]")
{
  std::vector<std::pair<DiagnosticLevel, std::string>> diagnostics;

  auto callbacks = PasswordCallbacks(&diagnostics);

  auto site = Profile(ProtocolKind::Ftp,
                      EnvironmentPort("HAVREMOTE_FTP_PORT", 2121),
                      RemotePath::Root());

  auto session = MakeFtpSession();

  ExerciseProtocol(*session, site, callbacks);

  CHECK(std::ranges::any_of(diagnostics, [](const auto &diagnostic)
                            { return diagnostic.first == DiagnosticLevel::Warning &&
                                     diagnostic.second.find("insecure") != std::string::npos; }));
}

TEST_CASE("explicit FTPS Docker fixture rejects an unpinned certificate and accepts its PIN",
          "[integration][docker][ftps]")
{
  auto site = Profile(ProtocolKind::FtpsExplicit,
                      EnvironmentPort("HAVREMOTE_FTPS_PORT", 2122),
                      RemotePath::Root());

  int rejectedCredentialRequests{};

  auto rejectedCallbacks = PasswordCallbacks();

  rejectedCallbacks.requestCredential = [&](const CredentialRequest &, std::stop_token)
  {
    ++rejectedCredentialRequests;

    return Result<std::string>{std::string{TestPassword}};
  };

  auto untrusted = MakeFtpSession();

  const auto rejected = untrusted->Connect(site, rejectedCallbacks, {});

  REQUIRE_FALSE(rejected);
  CHECK(rejectedCredentialRequests == 0);

  untrusted->Disconnect();

  int trustChallenges{};

  int acceptedCredentialRequests{};

  auto acceptedCallbacks = PasswordCallbacks();
  acceptedCallbacks.requestCredential = [&](const CredentialRequest &, std::stop_token)
  {
    ++acceptedCredentialRequests;

    return Result<std::string>{std::string{TestPassword}};
  };
  acceptedCallbacks.verifyTrust = [&](const TrustChallenge &challenge, std::stop_token)
  {
    ++trustChallenges;

    CHECK(acceptedCredentialRequests == 0);
    CHECK(challenge.kind == TrustKind::TlsCertificate);
    CHECK(challenge.status == TrustStatus::Invalid);
    CHECK(challenge.host == site.host);
    CHECK(challenge.port == site.port);
    CHECK(challenge.sha256Fingerprint ==
          "SHA256:L4IHQpsCHemCVp2BuMv+FQSwULTsaGuvyedqYug1Bf4");

    return Result<TrustDecision>{TrustDecision::AcceptOnce};
  };

  auto accepted = MakeFtpSession();

  const auto acceptedResult = accepted->Connect(site, acceptedCallbacks, {});

  INFO((acceptedResult ? "Self-signed certificate accepted" : acceptedResult.error().message));
  REQUIRE(acceptedResult);

  accepted->Disconnect();

  CHECK(trustChallenges == 1);
  CHECK(acceptedCredentialRequests == 1);

  constexpr std::string_view wrongPin =
      "sha256//AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";

  int changedCredentialRequests{};

  auto changedCallbacks = PasswordCallbacks();
  changedCallbacks.tlsPinnedPublicKey = wrongPin;
  changedCallbacks.requestCredential = [&](const CredentialRequest &, std::stop_token)
  {
    ++changedCredentialRequests;

    return Result<std::string>{std::string{TestPassword}};
  };
  changedCallbacks.verifyTrust = [&](const TrustChallenge &challenge, std::stop_token)
  {
    CHECK(changedCredentialRequests == 0);
    CHECK(challenge.status == TrustStatus::Changed);

    return Result<TrustDecision>{TrustDecision::AcceptOnce};
  };

  auto changed = MakeFtpSession();

  const auto notExplicitlyReplaced = changed->Connect(site, changedCallbacks, {});

  REQUIRE_FALSE(notExplicitlyReplaced);
  CHECK(notExplicitlyReplaced.error().code == RemoteErrorCode::HostKeyChanged);
  CHECK(changedCredentialRequests == 0);

  int replacementChallenges{};

  int replacementCredentialRequests{};

  auto replacementCallbacks = PasswordCallbacks();
  replacementCallbacks.tlsPinnedPublicKey = wrongPin;
  replacementCallbacks.requestCredential = [&](const CredentialRequest &, std::stop_token)
  {
    ++replacementCredentialRequests;

    return Result<std::string>{std::string{TestPassword}};
  };
  replacementCallbacks.verifyTrust = [&](const TrustChallenge &challenge, std::stop_token)
  {
    ++replacementChallenges;

    CHECK(replacementCredentialRequests == 0);
    CHECK(challenge.status == TrustStatus::Changed);

    return Result<TrustDecision>{TrustDecision::ReplaceStored};
  };

  auto replacement = MakeFtpSession();

  REQUIRE(replacement->Connect(site, replacementCallbacks, {}));

  replacement->Disconnect();

  CHECK(replacementChallenges == 1);
  CHECK(replacementCredentialRequests == 1);

  auto callbacks = PasswordCallbacks();
  callbacks.tlsPinnedPublicKey = Environment(
      "HAVREMOTE_FTPS_PIN",
      "sha256//L4IHQpsCHemCVp2BuMv+FQSwULTsaGuvyedqYug1Bf4=");

  auto trusted = MakeFtpSession();

  ExerciseProtocol(*trusted, site, callbacks);
}

TEST_CASE("active explicit FTPS validates LIST STOR upload resume and RETR independently of rename",
          "[integration][manual][active-ftps][.]")
{
  const auto *host = std::getenv("HAVREMOTE_ACTIVE_FTPS_HOST");

  if (host == nullptr || std::string_view{host}.empty())
  {
    SKIP("Set HAVREMOTE_ACTIVE_FTPS_HOST to run the reachable-server active FTPS test");
  }

  auto site = Profile(
      ProtocolKind::FtpsExplicit,
      EnvironmentPort("HAVREMOTE_ACTIVE_FTPS_PORT", 21),
      RemotePath{Environment("HAVREMOTE_ACTIVE_FTPS_DIRECTORY", "/")});
  site.host = host;
  site.username = Environment("HAVREMOTE_ACTIVE_FTPS_USERNAME", "havremote");
  site.ftpDataConnectionMode = FtpDataConnectionMode::Active;
  site.ftpActiveAddress.clear();

  const auto password = Environment("HAVREMOTE_ACTIVE_FTPS_PASSWORD",
                                    TestPassword);

  std::vector<std::pair<DiagnosticLevel, std::string>> diagnostics;

  auto callbacks = PasswordCallbacks(&diagnostics);
  callbacks.requestCredential = [password](const CredentialRequest &,
                                           std::stop_token)
  {
    return Result<std::string>{password};
  };
  callbacks.verifyTrust = [](const TrustChallenge &, std::stop_token)
  {
    return Result<TrustDecision>{TrustDecision::AcceptOnce};
  };
  callbacks.tlsPinnedPublicKey = Environment("HAVREMOTE_ACTIVE_FTPS_PIN", "");

  auto session = MakeFtpSession();

  INFO("active explicit FTPS host=" << site.host << " port=" << site.port);
  REQUIRE(session->Connect(site, callbacks, {}));
  CHECK(HasDiagnosticContaining(
      diagnostics, "Active FTP NLST data listener created at "));

  const auto requireDataListenerDiagnostic = [&diagnostics](
                                                 const std::string_view operation)
  {
    CHECK(HasDiagnosticContaining(
        diagnostics,
        "Active FTP " + std::string{operation} + " data listener created at "));
    CHECK_FALSE(HasDiagnosticContaining(diagnostics,
                                        "Active FTP libcurl trace"));
  };

  TemporaryDirectory local;

  constexpr std::string_view payload =
      "havRemote active explicit FTPS data-channel test\n";

  const auto uniqueName = "active-ftps-" + GenerateId() + ".txt";

  const auto remoteFile = site.initialRemoteDirectory.Joined(RemotePath{uniqueName});

  const auto localUpload = local.Path() / "upload.txt";

  WriteFile(localUpload, payload);

  const auto localPrefix = local.Path() / "upload-prefix.txt";

  constexpr auto prefixSize = payload.size() / 3U;

  WriteFile(localPrefix, payload.substr(0, prefixSize));

  // Deliberately upload directly to the unique destination: temporary-name
  // finalization would add RNFR/RNTO after STOR and could obscure which phase
  // failed. This assertion validates only the active FTPS STOR data path.
  diagnostics.clear();

  REQUIRE(session->Upload(
      localPrefix, remoteFile,
      TransferOptions{.jobId = "active-ftps-stor",
                      .overwrite = false,
                      .useTemporaryName = false,
                      .temporaryRemotePath = std::nullopt,
                      .beforeFinalize = {}},
      {}, {}));
  requireDataListenerDiagnostic("STOR");

  const auto partialUpload = session->Stat(remoteFile, {});

  REQUIRE(partialUpload);
  REQUIRE(partialUpload->size == prefixSize);

  diagnostics.clear();

  REQUIRE(session->Upload(
      localUpload, remoteFile,
      TransferOptions{.jobId = "active-ftps-stor-resume",
                      .resumeOffset = partialUpload->size,
                      .overwrite = true,
                      .useTemporaryName = false,
                      .temporaryRemotePath = std::nullopt,
                      .beforeFinalize = {}},
      {}, {}));

  requireDataListenerDiagnostic("STOR");

  // Directory listing and file retrieval each require their own active data
  // listener and therefore validate separate LIST/MLSD and RETR operations.
  diagnostics.clear();

  const auto entries = session->List(site.initialRemoteDirectory, {});

  REQUIRE(entries);
  CHECK(HasEntry(*entries, uniqueName, RemoteEntryKind::File));

  requireDataListenerDiagnostic("MLSD");

  diagnostics.clear();

  const auto localDownload = local.Path() / "download.txt";

  REQUIRE(session->Download(
      remoteFile, localDownload,
      TransferOptions{.jobId = "active-ftps-retr",
                      .overwrite = true,
                      .useTemporaryName = false,
                      .temporaryRemotePath = std::nullopt,
                      .beforeFinalize = {}},
      {}, {}));
  CHECK(ReadFile(localDownload) == payload);

  requireDataListenerDiagnostic("RETR");

  REQUIRE(session->Remove(remoteFile, false, {}));

  // RNFR/RNTO are control-channel commands. A rejected source path must remain
  // a normal FTP file-operation error and must not emit active-listener setup
  // diagnostics or an active-mode transport diagnosis.
  diagnostics.clear();

  const auto missing = site.initialRemoteDirectory.Joined(
      RemotePath{"missing-" + GenerateId() + ".txt"});

  const auto renamed = site.initialRemoteDirectory.Joined(
      RemotePath{"renamed-" + GenerateId() + ".txt"});

  const auto renameResult = session->Rename(missing, renamed, true, {});

  REQUIRE_FALSE(renameResult);
  CHECK(renameResult.error().code == RemoteErrorCode::PermissionDenied);
  CHECK(renameResult.error().message.find(
            "rejected the rename source or destination path (RNFR/RNTO)") !=
        std::string::npos);
  CHECK_FALSE(HasDiagnosticContaining(diagnostics, "Active FTP"));

  session->Disconnect();
}

TEST_CASE("SFTP Docker fixture persists and reuses an accepted OpenSSH host key",
          "[integration][docker][sftp]")
{
  TemporaryDirectory local;

  auto site = Profile(ProtocolKind::Sftp,
                      EnvironmentPort("HAVREMOTE_SFTP_PORT", 2222),
                      RemotePath{"/home/havremote/fixture"});

  int initialChallenges{};

  auto firstCallbacks = PasswordCallbacks();

  firstCallbacks.knownHostsFile = local.Path() / "known_hosts";
  firstCallbacks.verifyTrust = [&](const TrustChallenge &challenge, std::stop_token)
  {
    ++initialChallenges;

    CHECK(challenge.kind == TrustKind::SshHostKey);
    CHECK(challenge.status == TrustStatus::Unknown);
    CHECK(challenge.host == site.host);
    CHECK(challenge.port == site.port);
    CHECK(challenge.algorithm == "ssh-ed25519");
    CHECK_FALSE(challenge.sha256Fingerprint.empty());

    return Result<TrustDecision>{TrustDecision::AcceptPermanently};
  };

  auto first = MakeSftpSession();

  REQUIRE(first->Connect(site, firstCallbacks, {}));

  first->Disconnect();

  CHECK(initialChallenges == 1);
  CHECK(std::filesystem::file_size(firstCallbacks.knownHostsFile) > 0);

  for (const auto &entry : std::filesystem::directory_iterator{local.Path()})
  {
    CHECK(entry.path().filename().string().find("known_hosts.tmp-") ==
          std::string::npos);
  }

  int repeatedChallenges{};

  auto knownCallbacks = PasswordCallbacks();
  knownCallbacks.knownHostsFile = firstCallbacks.knownHostsFile;
  knownCallbacks.verifyTrust = [&](const TrustChallenge &, std::stop_token)
  {
    ++repeatedChallenges;

    return Result<TrustDecision>{TrustDecision::Reject};
  };

  auto known = MakeSftpSession();

  ExerciseProtocol(*known, site, knownCallbacks);

  CHECK(repeatedChallenges == 0);
}
