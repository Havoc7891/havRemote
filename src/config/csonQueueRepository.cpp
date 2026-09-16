// SPDX-License-Identifier: MIT

#include "config/csonQueueRepository.hpp"

#include "core/fileTime.hpp"
#include "core/logSanitizer.hpp"
#include "core/transferQueue.hpp"

#include <havCSON.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace havremote::config
{
  namespace
  {
    constexpr std::size_t MaximumQueueItems = 10'000;
    constexpr std::size_t MaximumResumeRecordsPerItem = 10'000;
    constexpr std::size_t MaximumDestinationOverridesPerItem = 10'000;
    constexpr std::uintmax_t MaximumQueueFileSize = 64U * 1024U * 1024U;
    constexpr std::size_t MaximumIdentifierLength = 256;
    constexpr std::size_t MaximumTextLength = 1024U * 1024U;
    constexpr std::size_t MaximumErrorMessageLength = 16U * 1024U;

    ConfigError Error(const ConfigErrorKind kind,
                      const std::filesystem::path &path,
                      std::string message,
                      const std::optional<std::size_t> line = std::nullopt,
                      const std::optional<std::size_t> column = std::nullopt)
    {
      return ConfigError{kind, path, std::move(message), line, column};
    }

    std::string PathToUtf8(const std::filesystem::path &path)
    {
      const auto value = path.generic_u8string();
      return {value.begin(), value.end()};
    }

    std::filesystem::path PathFromUtf8(const std::string_view value)
    {
      std::u8string result;
      result.reserve(value.size());

      for (const unsigned char character : value)
      {
        result.push_back(static_cast<char8_t>(character));
      }

      return std::filesystem::path{result};
    }

    std::expected<bool, ConfigError> FileExists(
        const std::filesystem::path &path)
    {
      std::error_code filesystemError;

      const auto status = std::filesystem::status(path, filesystemError);

      if (filesystemError &&
          filesystemError != std::errc::no_such_file_or_directory)
      {
        return std::unexpected(Error(
            ConfigErrorKind::Io,
            path,
            "Failed to inspect the persistent queue file: " +
                filesystemError.message()));
      }

      return !filesystemError && std::filesystem::exists(status);
    }

    std::expected<void, ConfigError> EnsureParentDirectory(
        const std::filesystem::path &path)
    {
      const auto parent = path.parent_path();

      if (parent.empty())
      {
        return {};
      }

      std::error_code filesystemError;
      std::filesystem::create_directories(parent, filesystemError);
      if (filesystemError)
      {
        return std::unexpected(Error(
            ConfigErrorKind::Io,
            path,
            "Failed to create the persistent queue directory: " +
                filesystemError.message()));
      }

      return {};
    }

    ConfigError WithSource(ConfigError error, const havCSON::Value &value)
    {
      if (const auto *source = value.Source(); source && !error.line)
      {
        error.line = source->valueSpan.begin.line;
        error.column = source->valueSpan.begin.column;
      }

      return error;
    }

    ConfigError AccessFailure(const havCSON::AccessError &failure,
                              const std::string_view logicalPath,
                              const std::filesystem::path &file)
    {
      auto error = Error(ConfigErrorKind::Validation, file,
                         std::string{logicalPath} + ": " + failure.message);

      if (failure.source)
      {
        error.line = failure.source->valueSpan.begin.line;
        error.column = failure.source->valueSpan.begin.column;
      }

      return error;
    }

    ConfigError WithMemberSource(ConfigError error, const havCSON::Value &object,
                                 const std::string_view key)
    {
      const auto member = havCSON::ValueView{object}.Member(key).Get();
      return WithSource(std::move(error), member ? member->get() : object);
    }

    std::expected<const havCSON::Value *, ConfigError> RequireObject(
        const havCSON::Value &value,
        const std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      const auto object = havCSON::ValueView{value}.AsObject();

      if (!object)
      {
        return std::unexpected(AccessFailure(object.error(), logicalPath, file));
      }

      return &value;
    }

    std::expected<const havCSON::Array *, ConfigError> RequireArray(
        const havCSON::Value &value,
        const std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      const auto array = havCSON::ValueView{value}.AsArray();

      if (!array)
      {
        return std::unexpected(AccessFailure(array.error(), logicalPath, file));
      }

      return &array->get();
    }

    std::expected<void, ConfigError> RequireExactMembers(
        const havCSON::Value &object,
        const std::initializer_list<std::string_view> expected,
        const std::string_view logicalPath,
        const std::filesystem::path &file,
        const std::initializer_list<std::string_view> optional = {})
    {
      for (const auto &[key, value] : object.asObject())
      {
        if (std::ranges::find(expected, key) == expected.end() &&
            std::ranges::find(optional, key) == optional.end())
        {
          return std::unexpected(WithSource(Error(
                                                ConfigErrorKind::Validation,
                                                file,
                                                std::string{logicalPath} + " contains unknown member '" + key +
                                                    "'"),
                                            value));
        }
      }

      for (const auto member : expected)
      {
        const auto required = havCSON::ValueView{object}.Member(member).Get();
        if (!required)
        {
          return std::unexpected(AccessFailure(required.error(), logicalPath, file));
        }
      }

      return {};
    }

    std::expected<const havCSON::Value *, ConfigError> RequireMember(
        const havCSON::Value &object,
        const std::string_view key,
        const std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      const auto member = havCSON::ValueView{object}.Member(key).Get();

      if (!member)
      {
        return std::unexpected(AccessFailure(member.error(), logicalPath, file));
      }

      return &member->get();
    }

    std::expected<std::string, ConfigError> RequireString(
        const havCSON::Value &object,
        const std::string_view key,
        const std::string_view logicalPath,
        const std::filesystem::path &file,
        const bool allowEmpty = true,
        const std::size_t maximumLength = MaximumTextLength)
    {
      const auto member = havCSON::ValueView{object}.Member(key);
      const auto value = member.AsString();
      if (!value)
      {
        return std::unexpected(AccessFailure(value.error(), logicalPath, file));
      }

      const auto &text = value->get();

      if ((!allowEmpty && text.empty()) || text.size() > maximumLength)
      {
        return std::unexpected(WithSource(Error(
                                              ConfigErrorKind::Validation,
                                              file,
                                              std::string{logicalPath} + "." + std::string{key} +
                                                  " has an invalid length"),
                                          member.Get()->get()));
      }

      return text;
    }

    std::expected<bool, ConfigError> RequireBoolean(
        const havCSON::Value &object,
        const std::string_view key,
        const std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      const auto value = havCSON::ValueView{object}.Member(key).AsBool();

      if (!value)
      {
        return std::unexpected(AccessFailure(value.error(), logicalPath, file));
      }

      return *value;
    }

    template <typename Integer>
    std::expected<Integer, ConfigError> RequireNumber(
        const havCSON::Value &object,
        const std::string_view key,
        const std::string_view logicalPath,
        const std::filesystem::path &file,
        const Integer minimum,
        const Integer maximum)
    {
      const auto value = havCSON::ValueView{object}.Member(key).AsInteger<Integer>(minimum, maximum);

      if (!value)
      {
        return std::unexpected(AccessFailure(value.error(), logicalPath, file));
      }

      return *value;
    }

    template <typename Integer>
    std::expected<Integer, ConfigError> ParseDecimal(
        const std::string_view text,
        const std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      Integer result{};

      const auto [end, conversionError] =
          std::from_chars(text.data(), text.data() + text.size(), result, 10);

      if (conversionError != std::errc{} || end != text.data() + text.size() ||
          text.empty() || std::to_string(result) != text)
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation,
            file,
            std::string{logicalPath} +
                " must be a canonical decimal integer string"));
      }

      return result;
    }

    template <typename Integer>
    std::expected<Integer, ConfigError> RequireDecimal(
        const havCSON::Value &object,
        const std::string_view key,
        const std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      const auto text = RequireString(object, key, logicalPath, file, false, 32);
      if (!text)
      {
        return std::unexpected(text.error());
      }

      auto parsed = ParseDecimal<Integer>(
          *text,
          std::string{logicalPath} + "." + std::string{key},
          file);
      if (!parsed)
      {
        return std::unexpected(WithSource(parsed.error(),
                                          havCSON::ValueView{object}.Member(key).Get()->get()));
      }

      return parsed;
    }

    std::expected<std::optional<std::int64_t>, ConfigError>
    RequireNullableSignedDecimal(const havCSON::Value &object,
                                 const std::string_view key,
                                 const std::string_view logicalPath,
                                 const std::filesystem::path &file)
    {
      const auto value = RequireMember(object, key, logicalPath, file);
      if (!value)
      {
        return std::unexpected(value.error());
      }
      if ((**value).isNull())
      {
        return std::optional<std::int64_t>{};
      }

      const auto text = havCSON::ValueView{**value}.AsString();
      if (!text)
      {
        return std::unexpected(WithSource(Error(
                                              ConfigErrorKind::Validation,
                                              file,
                                              std::string{logicalPath} + "." + std::string{key} +
                                                  " must be null or a decimal integer string"),
                                          **value));
      }

      auto parsed = ParseDecimal<std::int64_t>(
          text->get(),
          std::string{logicalPath} + "." + std::string{key},
          file);
      if (!parsed)
      {
        return std::unexpected(WithSource(parsed.error(), **value));
      }

      return std::optional<std::int64_t>{*parsed};
    }

    bool ContainsUnsafeControl(const std::string_view value) noexcept
    {
      return std::ranges::any_of(value, [](const unsigned char character)
                                 { return character < 0x20U || character == 0x7fU; });
    }

    std::string NormalizedKey(const std::string_view key)
    {
      std::string result;
      result.reserve(key.size());

      for (const unsigned char character : key)
      {
        if (std::isalnum(character) != 0)
        {
          result.push_back(static_cast<char>(std::tolower(character)));
        }
      }

      return result;
    }

    std::optional<std::pair<std::string, const havCSON::Value *>>
    ForbiddenSecretKey(const havCSON::Value &value)
    {
      static const std::unordered_set<std::string> forbidden{
          "password", "passphrase", "secret", "token", "accesstoken",
          "refreshtoken", "credentialvalue", "privatekeydata",
          "privatekeycontents", "certificateprivatekey"};

      if (value.isObject())
      {
        for (const auto &[key, child] : value.asObject())
        {
          if (forbidden.contains(NormalizedKey(key)))
          {
            return std::pair{key, &child};
          }

          if (auto nested = ForbiddenSecretKey(child))
          {
            return nested;
          }
        }
      }
      else if (value.isArray())
      {
        for (const auto &child : value.asArray())
        {
          if (auto nested = ForbiddenSecretKey(child))
          {
            return nested;
          }
        }
      }

      return std::nullopt;
    }

    std::string BytesToHex(const std::string_view bytes)
    {
      constexpr std::string_view digits{"0123456789ABCDEF"};

      std::string result;
      result.reserve(bytes.size() * 2U);

      for (const unsigned char byte : bytes)
      {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
      }

      return result;
    }

    std::expected<std::string, ConfigError> HexToBytes(
        const std::string_view hex,
        const std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      const auto nibble = [](const unsigned char character) -> int
      {
        if (character >= '0' && character <= '9')
        {
          return character - '0';
        }

        if (character >= 'a' && character <= 'f')
        {
          return character - 'a' + 10;
        }

        if (character >= 'A' && character <= 'F')
        {
          return character - 'A' + 10;
        }

        return -1;
      };

      if (hex.empty() || hex.size() % 2U != 0U ||
          hex.size() > MaximumTextLength * 2U)
      {
        return std::unexpected(Error(ConfigErrorKind::Validation,
                                     file,
                                     std::string{logicalPath} +
                                         " must be non-empty hexadecimal bytes"));
      }

      std::string result;
      result.reserve(hex.size() / 2U);

      for (std::size_t index = 0; index < hex.size(); index += 2U)
      {
        const int high = nibble(static_cast<unsigned char>(hex[index]));
        const int low = nibble(static_cast<unsigned char>(hex[index + 1U]));

        if (high < 0 || low < 0)
        {
          return std::unexpected(Error(ConfigErrorKind::Validation,
                                       file,
                                       std::string{logicalPath} +
                                           " contains a non-hexadecimal character"));
        }

        result.push_back(static_cast<char>((high << 4U) | low));
      }

      if (result.find('\0') != std::string::npos)
      {
        return std::unexpected(Error(ConfigErrorKind::Validation,
                                     file,
                                     std::string{logicalPath} +
                                         " decodes to a path containing NUL"));
      }

      return result;
    }

    std::string TimePointText(const std::chrono::system_clock::time_point value)
    {
      return std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                value.time_since_epoch())
                                .count());
    }

    std::string FileTimeText(const std::filesystem::file_time_type value)
    {
      return std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                FileTimeToSystemTime(value).time_since_epoch())
                                .count());
    }

    std::chrono::system_clock::time_point SystemTimeFromNanoseconds(
        const std::int64_t value)
    {
      return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
          std::chrono::sys_time<std::chrono::nanoseconds>{
              std::chrono::nanoseconds{value}});
    }

    std::filesystem::file_time_type FileTimeFromNanoseconds(
        const std::int64_t value)
    {
      return std::chrono::time_point_cast<std::filesystem::file_time_type::duration>(
          SystemTimeToFileTime(std::chrono::sys_time<std::chrono::nanoseconds>{
              std::chrono::nanoseconds{value}}));
    }

    havCSON::Value NullableTime(
        const std::optional<std::chrono::system_clock::time_point> &value)
    {
      return value ? havCSON::Value{TimePointText(*value)}
                   : havCSON::Value{nullptr};
    }

    havCSON::Value NullableFileTime(
        const std::optional<std::filesystem::file_time_type> &value)
    {
      return value ? havCSON::Value{FileTimeText(*value)}
                   : havCSON::Value{nullptr};
    }

    std::string_view DirectionName(const TransferDirection direction) noexcept
    {
      switch (direction)
      {
      case TransferDirection::Upload:
        return "upload";

      case TransferDirection::Download:
        return "download";
      }

      return {};
    }

    std::optional<TransferDirection> ParseDirection(
        const std::string_view value) noexcept
    {
      if (value == "upload")
      {
        return TransferDirection::Upload;
      }

      if (value == "download")
      {
        return TransferDirection::Download;
      }

      return std::nullopt;
    }

    std::string_view ConflictPolicyName(const ConflictPolicy policy) noexcept
    {
      switch (policy)
      {
      case ConflictPolicy::Ask:
        return "ask";

      case ConflictPolicy::Overwrite:
        return "overwrite";

      case ConflictPolicy::Skip:
        return "skip";

      case ConflictPolicy::Rename:
        return "rename";

      case ConflictPolicy::Resume:
        return "resume";
      }

      return {};
    }

    std::optional<ConflictPolicy> ParseConflictPolicy(
        const std::string_view value) noexcept
    {
      if (value == "ask")
      {
        return ConflictPolicy::Ask;
      }

      if (value == "overwrite")
      {
        return ConflictPolicy::Overwrite;
      }

      if (value == "skip")
      {
        return ConflictPolicy::Skip;
      }

      if (value == "rename")
      {
        return ConflictPolicy::Rename;
      }

      if (value == "resume")
      {
        return ConflictPolicy::Resume;
      }

      return std::nullopt;
    }

    std::string_view TransferStateName(const TransferState state) noexcept
    {
      switch (state)
      {
      case TransferState::Queued:
        return "queued";

      case TransferState::Enumerating:
        return "enumerating";

      case TransferState::Running:
        return "running";

      case TransferState::Paused:
        return "paused";

      case TransferState::Failed:
        return "failed";

      case TransferState::Completed:
        return "completed";

      case TransferState::Cancelled:
        return "canceled";
      }

      return {};
    }

    std::optional<TransferState> ParsePersistentTransferState(
        const std::string_view value) noexcept
    {
      if (value == "queued")
      {
        return TransferState::Queued;
      }

      if (value == "enumerating")
      {
        return TransferState::Enumerating;
      }

      if (value == "running")
      {
        return TransferState::Running;
      }

      if (value == "paused")
      {
        return TransferState::Paused;
      }

      if (value == "failed")
      {
        return TransferState::Failed;
      }

      return std::nullopt;
    }

    std::string_view EntryKindName(const RemoteEntryKind kind) noexcept
    {
      switch (kind)
      {
      case RemoteEntryKind::File:
        return "file";

      case RemoteEntryKind::Directory:
        return "directory";

      case RemoteEntryKind::Symlink:
        return "symlink";

      case RemoteEntryKind::Other:
        return "other";
      }

      return {};
    }

    std::optional<RemoteEntryKind> ParseEntryKind(
        const std::string_view value) noexcept
    {
      if (value == "file")
      {
        return RemoteEntryKind::File;
      }

      if (value == "directory")
      {
        return RemoteEntryKind::Directory;
      }

      if (value == "symlink")
      {
        return RemoteEntryKind::Symlink;
      }

      if (value == "other")
      {
        return RemoteEntryKind::Other;
      }

      return std::nullopt;
    }

    std::string_view ErrorCodeName(const RemoteErrorCode code) noexcept
    {
      switch (code)
      {
      case RemoteErrorCode::InvalidArgument:
        return "invalidArgument";

      case RemoteErrorCode::Unsupported:
        return "unsupported";

      case RemoteErrorCode::NotConnected:
        return "notConnected";

      case RemoteErrorCode::AlreadyConnected:
        return "alreadyConnected";

      case RemoteErrorCode::NameResolutionFailed:
        return "nameResolutionFailed";

      case RemoteErrorCode::ConnectionFailed:
        return "connectionFailed";

      case RemoteErrorCode::ConnectionLost:
        return "connectionLost";

      case RemoteErrorCode::TimedOut:
        return "timedOut";

      case RemoteErrorCode::AuthenticationFailed:
        return "authenticationFailed";

      case RemoteErrorCode::CredentialUnavailable:
        return "credentialUnavailable";

      case RemoteErrorCode::TrustRejected:
        return "trustRejected";

      case RemoteErrorCode::HostKeyChanged:
        return "hostKeyChanged";

      case RemoteErrorCode::CertificateInvalid:
        return "certificateInvalid";

      case RemoteErrorCode::NotFound:
        return "notFound";

      case RemoteErrorCode::AlreadyExists:
        return "alreadyExists";

      case RemoteErrorCode::PermissionDenied:
        return "permissionDenied";

      case RemoteErrorCode::NotDirectory:
        return "notDirectory";

      case RemoteErrorCode::IsDirectory:
        return "isDirectory";

      case RemoteErrorCode::DirectoryNotEmpty:
        return "directoryNotEmpty";

      case RemoteErrorCode::LocalIo:
        return "localIo";

      case RemoteErrorCode::RemoteIo:
        return "remoteIo";

      case RemoteErrorCode::ProtocolError:
        return "protocolError";

      case RemoteErrorCode::ParseError:
        return "parseError";

      case RemoteErrorCode::Conflict:
        return "conflict";

      case RemoteErrorCode::Paused:
        return "paused";

      case RemoteErrorCode::Cancelled:
        return "canceled";

      case RemoteErrorCode::Unknown:
        return "unknown";
      }

      return {};
    }

    std::optional<RemoteErrorCode> ParseErrorCode(
        const std::string_view value) noexcept
    {
      constexpr std::array values{
          RemoteErrorCode::InvalidArgument, RemoteErrorCode::Unsupported,
          RemoteErrorCode::NotConnected, RemoteErrorCode::AlreadyConnected,
          RemoteErrorCode::NameResolutionFailed, RemoteErrorCode::ConnectionFailed,
          RemoteErrorCode::ConnectionLost, RemoteErrorCode::TimedOut,
          RemoteErrorCode::AuthenticationFailed, RemoteErrorCode::CredentialUnavailable,
          RemoteErrorCode::TrustRejected, RemoteErrorCode::HostKeyChanged,
          RemoteErrorCode::CertificateInvalid, RemoteErrorCode::NotFound,
          RemoteErrorCode::AlreadyExists, RemoteErrorCode::PermissionDenied,
          RemoteErrorCode::NotDirectory, RemoteErrorCode::IsDirectory,
          RemoteErrorCode::DirectoryNotEmpty, RemoteErrorCode::LocalIo,
          RemoteErrorCode::RemoteIo, RemoteErrorCode::ProtocolError,
          RemoteErrorCode::ParseError, RemoteErrorCode::Conflict,
          RemoteErrorCode::Paused, RemoteErrorCode::Cancelled,
          RemoteErrorCode::Unknown};

      const auto found = std::ranges::find_if(
          values, [value](const RemoteErrorCode code)
          { return ErrorCodeName(code) == value; });

      return found == values.end() ? std::nullopt
                                   : std::optional<RemoteErrorCode>{*found};
    }

    bool ValidIdentifier(const std::string_view value) noexcept
    {
      return !value.empty() && value.size() <= MaximumIdentifierLength &&
             !ContainsUnsafeControl(value);
    }

    bool ValidGeneratedId(const std::string_view value) noexcept
    {
      if (value.size() != 36U || value[8] != '-' || value[13] != '-' ||
          value[18] != '-' || value[23] != '-' || value[14] != '4' ||
          (value[19] != '8' && value[19] != '9' && value[19] != 'a' &&
           value[19] != 'b'))
      {
        return false;
      }

      for (std::size_t index = 0; index < value.size(); ++index)
      {
        if (index == 8U || index == 13U || index == 18U || index == 23U)
        {
          continue;
        }

        const auto character = static_cast<unsigned char>(value[index]);
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f')))
        {
          return false;
        }
      }

      return true;
    }

    bool ValidTemporaryPathText(const std::string_view destination,
                                const std::string_view temporary) noexcept
    {
      constexpr std::string_view marker{".havremote."};
      constexpr std::string_view suffix{".part"};

      if (!temporary.starts_with(destination) ||
          temporary.size() != destination.size() + marker.size() + 36U +
                                  suffix.size())
      {
        return false;
      }

      const auto remainder = temporary.substr(destination.size());

      return remainder.starts_with(marker) && remainder.ends_with(suffix) &&
             ValidGeneratedId(remainder.substr(
                 marker.size(), remainder.size() - marker.size() - suffix.size()));
    }

    bool ValidUploadTemporaryPath(const RemotePath &destination,
                                  const RemotePath &temporary) noexcept
    {
      return temporary != destination &&
             temporary.Parent() == destination.Parent() &&
             ValidTemporaryPathText(destination.Bytes(), temporary.Bytes()) &&
             ValidTemporaryPathText(destination.DisplayUtf8(),
                                    temporary.DisplayUtf8());
    }

    std::expected<void, ConfigError> ValidateRemotePath(
        const RemotePath &path,
        const std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      if (path.Empty() || path.Bytes().size() > MaximumTextLength ||
          path.DisplayUtf8().size() > MaximumTextLength ||
          path.Bytes().find('\0') != std::string::npos ||
          ContainsUnsafeControl(path.DisplayUtf8()))
      {
        return std::unexpected(Error(ConfigErrorKind::Validation,
                                     file,
                                     std::string{logicalPath} +
                                         " is not a safe non-empty remote path"));
      }

      return {};
    }

    std::expected<void, ConfigError> ValidateItem(
        const PersistentQueueItem &item,
        const std::filesystem::path &file,
        const std::size_t index)
    {
      const auto logicalPath = "items[" + std::to_string(index) + "]";

      if (!ValidIdentifier(item.connectionId))
      {
        return std::unexpected(Error(ConfigErrorKind::Validation,
                                     file,
                                     logicalPath +
                                         ".connectionId is invalid"));
      }

      const auto &transfer = item.transfer;
      const auto &job = transfer.job;
      const auto localPathText = PathToUtf8(job.localPath);

      if (!ValidIdentifier(job.id) || !ValidIdentifier(job.siteId) ||
          job.localPath.empty() || localPathText.size() > MaximumTextLength ||
          ContainsUnsafeControl(localPathText) ||
          (job.direction != TransferDirection::Upload &&
           job.direction != TransferDirection::Download))
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation,
            file,
            logicalPath + ".job contains an invalid id or local path"));
      }

      if (!job.siteEndpoint ||
          !ProtocolKindFromString(ToString(job.siteEndpoint->protocol)) ||
          !IsValidEndpointHost(job.siteEndpoint->host) ||
          job.siteEndpoint->port == 0 ||
          job.siteEndpoint->username.size() > MaximumTextLength ||
          ContainsUnsafeControl(job.siteEndpoint->username))
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation,
            file,
            logicalPath + ".job.endpoint is invalid"));
      }

      if (auto valid = ValidateRemotePath(job.remotePath,
                                          logicalPath + ".job.remotePath",
                                          file);
          !valid)
      {
        return valid;
      }

      if (ConflictPolicyName(job.conflictPolicy).empty() ||
          (job.expectedRemoteRevision &&
           (job.recursive ||
            job.expectedRemoteRevision->kind != RemoteEntryKind::File)))
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation,
            file,
            logicalPath + ".job contains invalid conflict or revision metadata"));
      }

      if (job.destinationOverrides.size() > MaximumDestinationOverridesPerItem)
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation,
            file,
            logicalPath + ".job.destinationOverrides contains too many entries"));
      }

      for (std::size_t destinationIndex = 0; destinationIndex < job.destinationOverrides.size(); ++destinationIndex)
      {
        const auto &destination = job.destinationOverrides[destinationIndex];
        const auto destinationPath = logicalPath + ".job.destinationOverrides[" +
                                     std::to_string(destinationIndex) + "]";

        for (const auto *local : {&destination.originalLocalPath,
                                  &destination.localPath})
        {
          const auto text = PathToUtf8(*local);

          if (local->empty() || text.size() > MaximumTextLength ||
              ContainsUnsafeControl(text))
          {
            return std::unexpected(Error(
                ConfigErrorKind::Validation,
                file,
                destinationPath + " contains an invalid local path"));
          }
        }

        if (auto valid = ValidateRemotePath(destination.originalRemotePath,
                                            destinationPath + ".originalRemotePath",
                                            file);
            !valid)
        {
          return valid;
        }

        if (auto valid = ValidateRemotePath(destination.remotePath,
                                            destinationPath + ".remotePath",
                                            file);
            !valid)
        {
          return valid;
        }
      }

      if (const auto valid = ValidateTransferDestinations(job); !valid)
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation,
            file,
            logicalPath + ".job.destinationOverrides: " + valid.error().message));
      }

      if (transfer.progress.jobId != job.id ||
          (transfer.progress.totalBytes &&
           transfer.progress.bytesTransferred > *transfer.progress.totalBytes))
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation,
            file,
            logicalPath + ".progress does not match its transfer job"));
      }

      if (transfer.state == TransferState::Completed ||
          transfer.state == TransferState::Cancelled ||
          TransferStateName(transfer.state).empty())
      {
        return std::unexpected(Error(ConfigErrorKind::Validation,
                                     file,
                                     logicalPath +
                                         ".state is not persistable"));
      }

      if ((transfer.state == TransferState::Failed) != transfer.error.has_value())
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation,
            file,
            logicalPath +
                ".error must be present exactly when state is failed"));
      }

      if (transfer.error &&
          (transfer.error->message.size() > MaximumErrorMessageLength ||
           ErrorCodeName(transfer.error->code).empty()))
      {
        return std::unexpected(Error(ConfigErrorKind::Validation,
                                     file,
                                     logicalPath +
                                         ".error contains invalid data"));
      }

      if (item.resumeRecords.size() > MaximumResumeRecordsPerItem)
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation,
            file,
            logicalPath + ".resumeRecords contains too many entries"));
      }

      std::unordered_set<std::string> resumeIdentities;
      std::unordered_map<std::string, const TransferDestinationOverride *> destinationsBySource;

      for (const auto &destination : job.destinationOverrides)
      {
        destinationsBySource.emplace(
            job.direction == TransferDirection::Upload
                ? PathToUtf8(destination.originalLocalPath)
                : destination.originalRemotePath.Bytes(),
            &destination);
      }

      // Mappings were validated above. Use the same core boundary checks for
      // unmapped checkpoints without revalidating every mapping for every file.
      const TransferJob originalRoots{
          .id = {},
          .siteId = {},
          .siteEndpoint = std::nullopt,
          .direction = job.direction,
          .localPath = job.localPath,
          .remotePath = job.remotePath,
          .recursive = job.recursive,
          .expectedRemoteRevision = std::nullopt};

      for (std::size_t resumeIndex = 0; resumeIndex < item.resumeRecords.size(); ++resumeIndex)
      {
        const auto &resume = item.resumeRecords[resumeIndex];
        const auto resumePath = logicalPath + ".resumeRecords[" +
                                std::to_string(resumeIndex) + "]";
        const auto resumeLocalPath = PathToUtf8(resume.localPath);

        if (resume.localPath.empty() ||
            resumeLocalPath.size() > MaximumTextLength ||
            ContainsUnsafeControl(resumeLocalPath) ||
            (resume.kind != QueueResumeKind::Download &&
             resume.kind != QueueResumeKind::Upload))
        {
          return std::unexpected(Error(ConfigErrorKind::Validation,
                                       file,
                                       resumePath + ".localPath is empty"));
        }

        if (auto valid = ValidateRemotePath(
                resume.remotePath, resumePath + ".remotePath", file);
            !valid)
        {
          return valid;
        }

        if ((resume.kind == QueueResumeKind::Download &&
             job.direction != TransferDirection::Download) ||
            (resume.kind == QueueResumeKind::Upload &&
             job.direction != TransferDirection::Upload))
        {
          return std::unexpected(Error(
              ConfigErrorKind::Validation,
              file,
              resumePath + " has the wrong kind for its transfer direction"));
        }

        if (!job.recursive &&
            (resume.localPath != job.localPath ||
             resume.remotePath != job.remotePath))
        {
          return std::unexpected(Error(
              ConfigErrorKind::Validation,
              file,
              resumePath + " does not match its single-file transfer job"));
        }

        if (job.recursive)
        {
          const auto destination = destinationsBySource.find(
              job.direction == TransferDirection::Upload
                  ? resumeLocalPath
                  : resume.remotePath.Bytes());
          const auto matches = destination != destinationsBySource.end()
                                   ? destination->second->localPath == resume.localPath &&
                                         destination->second->remotePath == resume.remotePath
                                   : ResolveTransferPaths(originalRoots,
                                                          resume.localPath,
                                                          resume.remotePath)
                                         .has_value();
          if (!matches)
          {
            return std::unexpected(Error(
                ConfigErrorKind::Validation,
                file,
                resumePath + " does not match its recursive transfer destination"));
          }
        }

        if (resume.kind == QueueResumeKind::Download)
        {
          if (!resume.remoteModifiedAt || !resume.partModifiedAt ||
              resume.partSize > resume.remoteSize || resume.localModifiedAt ||
              resume.temporaryRemotePath || resume.localSize != 0)
          {
            return std::unexpected(Error(
                ConfigErrorKind::Validation,
                file,
                resumePath + " contains inconsistent download metadata"));
          }
        }
        else
        {
          if (!resume.localModifiedAt || !resume.temporaryRemotePath ||
              resume.remoteModifiedAt || resume.remoteSize != 0 ||
              resume.partSize != 0 || resume.partModifiedAt)
          {
            return std::unexpected(Error(
                ConfigErrorKind::Validation,
                file,
                resumePath + " contains inconsistent upload metadata"));
          }

          if (auto valid = ValidateRemotePath(*resume.temporaryRemotePath,
                                              resumePath +
                                                  ".temporaryRemotePath",
                                              file);
              !valid)
          {
            return valid;
          }

          if (!ValidUploadTemporaryPath(resume.remotePath,
                                        *resume.temporaryRemotePath))
          {
            return std::unexpected(Error(
                ConfigErrorKind::Validation,
                file,
                resumePath +
                    ".temporaryRemotePath is not the expected sibling temporary path"));
          }
        }

        auto identity = std::to_string(static_cast<int>(resume.kind));
        identity += '\n';
        identity += PathToUtf8(resume.localPath);
        identity += '\n';
        identity += resume.remotePath.Bytes();

        if (!resumeIdentities.insert(std::move(identity)).second)
        {
          return std::unexpected(Error(ConfigErrorKind::Validation,
                                       file,
                                       resumePath + " is duplicated"));
        }
      }

      return {};
    }

    havCSON::Value MakeRemotePath(const RemotePath &path)
    {
      havCSON::Object result;
      result.emplace("bytesHex", BytesToHex(path.Bytes()));
      result.emplace("display", path.DisplayUtf8());
      return result;
    }

    havCSON::Value MakeEndpoint(const SiteEndpointIdentity &endpoint)
    {
      havCSON::Object result;
      result.emplace("protocol", std::string{ToString(endpoint.protocol)});
      result.emplace("host", endpoint.host);
      result.emplace("port", static_cast<double>(endpoint.port));
      result.emplace("username", endpoint.username);
      return result;
    }

    havCSON::Value MakeRevision(const RemoteFileRevision &revision)
    {
      havCSON::Object result;
      result.emplace("kind", std::string{EntryKindName(revision.kind)});
      result.emplace("size", std::to_string(revision.size));
      result.emplace("modifiedAtUnixNanoseconds", NullableTime(revision.modifiedAt));
      return result;
    }

    havCSON::Value MakeJob(const TransferJob &job)
    {
      havCSON::Object result;
      result.emplace("id", job.id);
      result.emplace("siteId", job.siteId);
      result.emplace("endpoint", MakeEndpoint(*job.siteEndpoint));
      result.emplace("direction", std::string{DirectionName(job.direction)});
      result.emplace("localPath", PathToUtf8(job.localPath));
      result.emplace("remotePath", MakeRemotePath(job.remotePath));
      result.emplace("recursive", job.recursive);
      result.emplace("conflictPolicy", std::string{ConflictPolicyName(job.conflictPolicy)});
      result.emplace("expectedRemoteRevision",
                     job.expectedRemoteRevision
                         ? MakeRevision(*job.expectedRemoteRevision)
                         : havCSON::Value{nullptr});

      if (!job.destinationOverrides.empty())
      {
        havCSON::Array destinations;
        destinations.reserve(job.destinationOverrides.size());

        for (const auto &destination : job.destinationOverrides)
        {
          havCSON::Object item;
          item.emplace("originalLocalPath", PathToUtf8(destination.originalLocalPath));
          item.emplace("originalRemotePath", MakeRemotePath(destination.originalRemotePath));
          item.emplace("localPath", PathToUtf8(destination.localPath));
          item.emplace("remotePath", MakeRemotePath(destination.remotePath));
          destinations.emplace_back(std::move(item));
        }

        result.emplace("destinationOverrides", std::move(destinations));
      }

      return result;
    }

    havCSON::Value MakeProgress(const TransferProgress &progress)
    {
      havCSON::Object result;
      result.emplace("bytesTransferred",
                     std::to_string(progress.bytesTransferred));
      result.emplace("totalBytes",
                     progress.totalBytes
                         ? havCSON::Value{std::to_string(*progress.totalBytes)}
                         : havCSON::Value{nullptr});
      return result;
    }

    havCSON::Value MakeError(const std::optional<RemoteError> &remoteError)
    {
      if (!remoteError)
      {
        return nullptr;
      }

      havCSON::Object result;
      result.emplace("code", std::string{ErrorCodeName(remoteError->code)});
      result.emplace("message", SanitizeDiagnosticText(remoteError->message));
      result.emplace("nativeCode", static_cast<double>(remoteError->nativeCode));
      result.emplace("retryable", remoteError->retryable);
      result.emplace("operationMayHaveSucceeded", remoteError->operationMayHaveSucceeded);

      return result;
    }

    havCSON::Value MakeResumeRecord(const QueueResumeRecord &resume)
    {
      havCSON::Object result;
      result.emplace("kind",
                     resume.kind == QueueResumeKind::Upload ? "upload"
                                                            : "download");
      result.emplace("localPath", PathToUtf8(resume.localPath));
      result.emplace("remotePath", MakeRemotePath(resume.remotePath));

      if (resume.kind == QueueResumeKind::Download)
      {
        result.emplace("remoteSize", std::to_string(resume.remoteSize));
        result.emplace("remoteModifiedAtUnixNanoseconds",
                       NullableTime(resume.remoteModifiedAt));
        result.emplace("partialSize", std::to_string(resume.partSize));
        result.emplace("partialModifiedAtUnixNanoseconds",
                       NullableFileTime(resume.partModifiedAt));
      }
      else
      {
        result.emplace("localSize", std::to_string(resume.localSize));
        result.emplace("localModifiedAtUnixNanoseconds",
                       NullableFileTime(resume.localModifiedAt));
        result.emplace("temporaryRemotePath",
                       MakeRemotePath(*resume.temporaryRemotePath));
      }

      return result;
    }

    havCSON::Value MakeItem(const PersistentQueueItem &item)
    {
      havCSON::Array resumeRecords;
      resumeRecords.reserve(item.resumeRecords.size());

      for (const auto &resume : item.resumeRecords)
      {
        resumeRecords.push_back(MakeResumeRecord(resume));
      }

      havCSON::Object result;
      result.emplace("connectionId", item.connectionId);
      result.emplace("job", MakeJob(item.transfer.job));
      result.emplace("state", std::string{TransferStateName(item.transfer.state)});
      result.emplace("progress", MakeProgress(item.transfer.progress));
      result.emplace("attempt", static_cast<double>(item.transfer.attempt));
      result.emplace("error", MakeError(item.transfer.error));
      result.emplace("resumeRecords", std::move(resumeRecords));

      return result;
    }

    std::expected<havCSON::Value, ConfigError> MakeDocument(
        const std::vector<PersistentQueueItem> &items,
        const std::filesystem::path &file)
    {
      havCSON::Array encodedItems;
      encodedItems.reserve(items.size());

      std::unordered_set<std::string> jobIds;

      for (std::size_t index = 0; index < items.size(); ++index)
      {
        const auto &item = items[index];

        if (item.transfer.state == TransferState::Completed ||
            item.transfer.state == TransferState::Cancelled)
        {
          continue;
        }

        if (encodedItems.size() >= MaximumQueueItems)
        {
          return std::unexpected(Error(ConfigErrorKind::Validation,
                                       file,
                                       "The persistent queue contains too many items"));
        }

        if (auto valid = ValidateItem(item, file, index); !valid)
        {
          return std::unexpected(valid.error());
        }

        if (!jobIds.insert(item.transfer.job.id).second)
        {
          return std::unexpected(Error(
              ConfigErrorKind::Validation,
              file,
              "The persistent queue contains duplicate transfer id '" +
                  item.transfer.job.id + "'"));
        }

        encodedItems.push_back(MakeItem(item));
      }

      havCSON::Object root;
      root.emplace("formatVersion", static_cast<double>(CurrentQueueFormatVersion));
      root.emplace("items", std::move(encodedItems));

      return havCSON::Value{std::move(root)};
    }

    std::expected<std::optional<std::uint64_t>, ConfigError>
    RequireNullableUnsignedDecimal(const havCSON::Value &object,
                                   const std::string_view key,
                                   const std::string_view logicalPath,
                                   const std::filesystem::path &file)
    {
      const auto value = RequireMember(object, key, logicalPath, file);
      if (!value)
      {
        return std::unexpected(value.error());
      }
      if ((**value).isNull())
      {
        return std::optional<std::uint64_t>{};
      }

      const auto text = havCSON::ValueView{**value}.AsString();
      if (!text)
      {
        return std::unexpected(WithSource(Error(
                                              ConfigErrorKind::Validation,
                                              file,
                                              std::string{logicalPath} + "." + std::string{key} +
                                                  " must be null or a decimal integer string"),
                                          **value));
      }

      auto parsed = ParseDecimal<std::uint64_t>(
          text->get(),
          std::string{logicalPath} + "." + std::string{key},
          file);
      if (!parsed)
      {
        return std::unexpected(WithSource(parsed.error(), **value));
      }

      return std::optional<std::uint64_t>{*parsed};
    }

    std::expected<RemotePath, ConfigError> DecodeRemotePath(
        const havCSON::Value &value,
        const std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      auto object = RequireObject(value, logicalPath, file);
      if (!object)
      {
        return std::unexpected(object.error());
      }

      if (auto exact = RequireExactMembers(
              **object, {"bytesHex", "display"}, logicalPath, file);
          !exact)
      {
        return std::unexpected(exact.error());
      }

      auto hex = RequireString(**object,
                               "bytesHex",
                               logicalPath,
                               file,
                               false,
                               MaximumTextLength * 2U);
      auto display = RequireString(**object,
                                   "display",
                                   logicalPath,
                                   file,
                                   true,
                                   MaximumTextLength);

      if (!hex)
      {
        return std::unexpected(hex.error());
      }

      if (!display)
      {
        return std::unexpected(display.error());
      }

      auto bytes = HexToBytes(
          *hex, std::string{logicalPath} + ".bytesHex", file);
      if (!bytes)
      {
        return std::unexpected(WithMemberSource(bytes.error(), value, "bytesHex"));
      }

      RemotePath result{std::move(*bytes), std::move(*display)};

      if (auto valid = ValidateRemotePath(result, logicalPath, file); !valid)
      {
        return std::unexpected(WithSource(valid.error(), value));
      }

      return result;
    }

    std::expected<SiteEndpointIdentity, ConfigError> DecodeEndpoint(
        const havCSON::Value &value,
        const std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      auto object = RequireObject(value, logicalPath, file);
      if (!object)
      {
        return std::unexpected(object.error());
      }

      if (auto exact = RequireExactMembers(
              **object,
              {"protocol", "host", "port", "username"},
              logicalPath,
              file);
          !exact)
      {
        return std::unexpected(exact.error());
      }

      auto protocolText =
          RequireString(**object, "protocol", logicalPath, file, false, 32);
      auto host = RequireString(
          **object, "host", logicalPath, file, false, MaximumTextLength);
      auto port = RequireNumber<std::uint16_t>(
          **object, "port", logicalPath, file, 1, 65535);
      auto username = RequireString(
          **object, "username", logicalPath, file, true, MaximumTextLength);

      if (!protocolText)
      {
        return std::unexpected(protocolText.error());
      }

      if (!host)
      {
        return std::unexpected(host.error());
      }

      if (!port)
      {
        return std::unexpected(port.error());
      }

      if (!username)
      {
        return std::unexpected(username.error());
      }

      const auto protocol = ProtocolKindFromString(*protocolText);
      if (!protocol || !IsValidEndpointHost(*host) ||
          ContainsUnsafeControl(*username))
      {
        const auto invalidMember = !protocol                     ? "protocol"
                                   : !IsValidEndpointHost(*host) ? "host"
                                                                 : "username";

        return std::unexpected(WithMemberSource(Error(ConfigErrorKind::Validation,
                                                      file,
                                                      std::string{logicalPath} +
                                                          " contains an invalid endpoint"),
                                                value, invalidMember));
      }

      return SiteEndpointIdentity{*protocol,
                                  std::move(*host),
                                  *port,
                                  std::move(*username)};
    }

    std::expected<RemoteFileRevision, ConfigError> DecodeRevision(
        const havCSON::Value &value,
        const std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      auto object = RequireObject(value, logicalPath, file);
      if (!object)
      {
        return std::unexpected(object.error());
      }

      if (auto exact = RequireExactMembers(
              **object,
              {"kind", "size", "modifiedAtUnixNanoseconds"},
              logicalPath,
              file);
          !exact)
      {
        return std::unexpected(exact.error());
      }

      auto kindText =
          RequireString(**object, "kind", logicalPath, file, false, 32);
      auto size = RequireDecimal<std::uint64_t>(
          **object, "size", logicalPath, file);
      auto modified = RequireNullableSignedDecimal(
          **object,
          "modifiedAtUnixNanoseconds",
          logicalPath,
          file);

      if (!kindText)
      {
        return std::unexpected(kindText.error());
      }

      if (!size)
      {
        return std::unexpected(size.error());
      }

      if (!modified)
      {
        return std::unexpected(modified.error());
      }

      const auto kind = ParseEntryKind(*kindText);
      if (!kind || *kind != RemoteEntryKind::File)
      {
        return std::unexpected(WithMemberSource(Error(ConfigErrorKind::Validation,
                                                      file,
                                                      std::string{logicalPath} +
                                                          ".kind must be file"),
                                                value, "kind"));
      }

      return RemoteFileRevision{
          .kind = *kind,
          .size = *size,
          .modifiedAt = *modified
                            ? std::optional<std::chrono::system_clock::time_point>{
                                  SystemTimeFromNanoseconds(**modified)}
                            : std::nullopt};
    }

    std::expected<TransferDestinationOverride, ConfigError> DecodeDestinationOverride(
        const havCSON::Value &value,
        const std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      auto object = RequireObject(value, logicalPath, file);
      if (!object)
      {
        return std::unexpected(object.error());
      }

      if (auto exact = RequireExactMembers(
              **object,
              {"originalLocalPath", "originalRemotePath", "localPath", "remotePath"},
              logicalPath,
              file);
          !exact)
      {
        return std::unexpected(exact.error());
      }

      auto originalLocal = RequireString(
          **object, "originalLocalPath", logicalPath, file, false, MaximumTextLength);
      auto local = RequireString(
          **object, "localPath", logicalPath, file, false, MaximumTextLength);
      auto originalRemoteMember = RequireMember(
          **object, "originalRemotePath", logicalPath, file);
      auto remoteMember = RequireMember(**object, "remotePath", logicalPath, file);

      if (!originalLocal)
      {
        return std::unexpected(originalLocal.error());
      }

      if (!local)
      {
        return std::unexpected(local.error());
      }

      if (!originalRemoteMember)
      {
        return std::unexpected(originalRemoteMember.error());
      }

      if (!remoteMember)
      {
        return std::unexpected(remoteMember.error());
      }

      auto originalRemote = DecodeRemotePath(
          **originalRemoteMember, std::string{logicalPath} + ".originalRemotePath", file);
      auto remote = DecodeRemotePath(
          **remoteMember, std::string{logicalPath} + ".remotePath", file);

      if (!originalRemote)
      {
        return std::unexpected(originalRemote.error());
      }

      if (!remote)
      {
        return std::unexpected(remote.error());
      }

      return TransferDestinationOverride{
          .originalLocalPath = PathFromUtf8(*originalLocal),
          .originalRemotePath = std::move(*originalRemote),
          .localPath = PathFromUtf8(*local),
          .remotePath = std::move(*remote)};
    }

    std::expected<TransferJob, ConfigError> DecodeJob(
        const havCSON::Value &value,
        const std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      auto object = RequireObject(value, logicalPath, file);
      if (!object)
      {
        return std::unexpected(object.error());
      }

      if (auto exact = RequireExactMembers(
              **object,
              {"id", "siteId", "endpoint", "direction", "localPath",
               "remotePath", "recursive", "conflictPolicy",
               "expectedRemoteRevision"},
              logicalPath,
              file,
              {"destinationOverrides"});
          !exact)
      {
        return std::unexpected(exact.error());
      }

      auto id = RequireString(**object,
                              "id",
                              logicalPath,
                              file,
                              false,
                              MaximumIdentifierLength);
      auto siteId = RequireString(**object,
                                  "siteId",
                                  logicalPath,
                                  file,
                                  false,
                                  MaximumIdentifierLength);
      auto directionText =
          RequireString(**object, "direction", logicalPath, file, false, 32);
      auto localPath = RequireString(
          **object, "localPath", logicalPath, file, false, MaximumTextLength);
      auto recursive = RequireBoolean(**object, "recursive", logicalPath, file);
      auto conflictText = RequireString(
          **object, "conflictPolicy", logicalPath, file, false, 32);

      if (!id)
      {
        return std::unexpected(id.error());
      }

      if (!siteId)
      {
        return std::unexpected(siteId.error());
      }

      if (!directionText)
      {
        return std::unexpected(directionText.error());
      }

      if (!localPath)
      {
        return std::unexpected(localPath.error());
      }

      if (!recursive)
      {
        return std::unexpected(recursive.error());
      }

      if (!conflictText)
      {
        return std::unexpected(conflictText.error());
      }

      if (!ValidIdentifier(*id) || !ValidIdentifier(*siteId) ||
          ContainsUnsafeControl(*localPath))
      {
        const auto invalidMember = !ValidIdentifier(*id)       ? "id"
                                   : !ValidIdentifier(*siteId) ? "siteId"
                                                               : "localPath";

        return std::unexpected(WithMemberSource(Error(ConfigErrorKind::Validation,
                                                      file,
                                                      std::string{logicalPath} +
                                                          " contains an unsafe id or local path"),
                                                value, invalidMember));
      }

      const auto direction = ParseDirection(*directionText);
      const auto conflictPolicy = ParseConflictPolicy(*conflictText);
      if (!direction || !conflictPolicy)
      {
        return std::unexpected(WithMemberSource(Error(ConfigErrorKind::Validation,
                                                      file,
                                                      std::string{logicalPath} +
                                                          " contains an unknown enum value"),
                                                value,
                                                !direction ? "direction" : "conflictPolicy"));
      }

      auto endpointMember = RequireMember(**object, "endpoint", logicalPath, file);
      auto remotePathMember =
          RequireMember(**object, "remotePath", logicalPath, file);
      auto revisionMember = RequireMember(
          **object, "expectedRemoteRevision", logicalPath, file);

      if (!endpointMember)
      {
        return std::unexpected(endpointMember.error());
      }

      if (!remotePathMember)
      {
        return std::unexpected(remotePathMember.error());
      }

      if (!revisionMember)
      {
        return std::unexpected(revisionMember.error());
      }

      auto endpoint = DecodeEndpoint(
          **endpointMember, std::string{logicalPath} + ".endpoint", file);
      auto remotePath = DecodeRemotePath(
          **remotePathMember,
          std::string{logicalPath} + ".remotePath",
          file);

      if (!endpoint)
      {
        return std::unexpected(endpoint.error());
      }

      if (!remotePath)
      {
        return std::unexpected(remotePath.error());
      }

      std::optional<RemoteFileRevision> revision;

      if (!(**revisionMember).isNull())
      {
        auto decoded = DecodeRevision(
            **revisionMember,
            std::string{logicalPath} + ".expectedRemoteRevision",
            file);
        if (!decoded)
        {
          return std::unexpected(decoded.error());
        }

        revision = std::move(*decoded);
      }

      if (revision && *recursive)
      {
        return std::unexpected(WithSource(Error(
                                              ConfigErrorKind::Validation,
                                              file,
                                              std::string{logicalPath} +
                                                  ".expectedRemoteRevision is invalid for a recursive job"),
                                          **revisionMember));
      }

      std::vector<TransferDestinationOverride> destinations;

      if (const auto found = (*object)->asObject().find("destinationOverrides");
          found != (*object)->asObject().end())
      {
        const auto destinationPath = std::string{logicalPath} + ".destinationOverrides";
        auto array = RequireArray(found->second, destinationPath, file);

        if (!array)
        {
          return std::unexpected(array.error());
        }

        if ((*array)->size() > MaximumDestinationOverridesPerItem)
        {
          return std::unexpected(WithSource(Error(
                                                ConfigErrorKind::Validation,
                                                file,
                                                destinationPath + " contains too many entries"),
                                            found->second));
        }

        destinations.reserve((*array)->size());

        for (std::size_t index = 0; index < (*array)->size(); ++index)
        {
          auto destination = DecodeDestinationOverride(
              (**array)[index], destinationPath + "[" + std::to_string(index) + "]", file);

          if (!destination)
          {
            return std::unexpected(destination.error());
          }

          destinations.push_back(std::move(*destination));
        }
      }

      return TransferJob{.id = std::move(*id),
                         .siteId = std::move(*siteId),
                         .siteEndpoint = std::move(*endpoint),
                         .direction = *direction,
                         .localPath = PathFromUtf8(*localPath),
                         .remotePath = std::move(*remotePath),
                         .recursive = *recursive,
                         .conflictPolicy = *conflictPolicy,
                         .expectedRemoteRevision = std::move(revision),
                         .destinationOverrides = std::move(destinations)};
    }

    std::expected<TransferProgress, ConfigError> DecodeProgress(
        const havCSON::Value &value,
        const std::string_view jobId,
        const std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      auto object = RequireObject(value, logicalPath, file);
      if (!object)
      {
        return std::unexpected(object.error());
      }

      if (auto exact = RequireExactMembers(
              **object,
              {"bytesTransferred", "totalBytes"},
              logicalPath,
              file);
          !exact)
      {
        return std::unexpected(exact.error());
      }

      auto transferred = RequireDecimal<std::uint64_t>(
          **object, "bytesTransferred", logicalPath, file);
      auto total = RequireNullableUnsignedDecimal(
          **object, "totalBytes", logicalPath, file);

      if (!transferred)
      {
        return std::unexpected(transferred.error());
      }

      if (!total)
      {
        return std::unexpected(total.error());
      }

      if (*total && *transferred > **total)
      {
        return std::unexpected(WithMemberSource(Error(ConfigErrorKind::Validation,
                                                      file,
                                                      std::string{logicalPath} +
                                                          " exceeds its total byte count"),
                                                value, "bytesTransferred"));
      }

      return TransferProgress{.jobId = std::string{jobId},
                              .bytesTransferred = *transferred,
                              .totalBytes = *total,
                              .elapsed = {}};
    }

    std::expected<std::optional<RemoteError>, ConfigError> DecodeError(
        const havCSON::Value &value,
        const std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      if (value.isNull())
      {
        return std::optional<RemoteError>{};
      }

      auto object = RequireObject(value, logicalPath, file);
      if (!object)
      {
        return std::unexpected(object.error());
      }

      if (auto exact = RequireExactMembers(
              **object,
              {"code", "message", "nativeCode", "retryable",
               "operationMayHaveSucceeded"},
              logicalPath,
              file);
          !exact)
      {
        return std::unexpected(exact.error());
      }

      auto codeText =
          RequireString(**object, "code", logicalPath, file, false, 64);
      auto message = RequireString(**object,
                                   "message",
                                   logicalPath,
                                   file,
                                   true,
                                   MaximumErrorMessageLength);
      auto nativeCode = RequireNumber<int>(
          **object,
          "nativeCode",
          logicalPath,
          file,
          std::numeric_limits<int>::min(),
          std::numeric_limits<int>::max());
      auto retryable = RequireBoolean(
          **object, "retryable", logicalPath, file);
      auto uncertain = RequireBoolean(
          **object, "operationMayHaveSucceeded", logicalPath, file);

      if (!codeText)
      {
        return std::unexpected(codeText.error());
      }

      if (!message)
      {
        return std::unexpected(message.error());
      }

      if (!nativeCode)
      {
        return std::unexpected(nativeCode.error());
      }

      if (!retryable)
      {
        return std::unexpected(retryable.error());
      }

      if (!uncertain)
      {
        return std::unexpected(uncertain.error());
      }

      const auto code = ParseErrorCode(*codeText);
      if (!code)
      {
        return std::unexpected(WithMemberSource(Error(ConfigErrorKind::Validation,
                                                      file,
                                                      std::string{logicalPath} +
                                                          ".code has an unknown value"),
                                                value, "code"));
      }

      if (SanitizeDiagnosticText(*message) != *message)
      {
        return std::unexpected(WithMemberSource(Error(
                                                    ConfigErrorKind::Validation,
                                                    file,
                                                    std::string{logicalPath} +
                                                        ".message contains unsafe or secret-bearing diagnostic text"),
                                                value, "message"));
      }

      return std::optional<RemoteError>{RemoteError{
          .code = *code,
          .message = std::move(*message),
          .nativeCode = *nativeCode,
          .retryable = *retryable,
          .operationMayHaveSucceeded = *uncertain}};
    }

    std::expected<QueueResumeRecord, ConfigError> DecodeResumeRecord(
        const havCSON::Value &value,
        const std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      auto object = RequireObject(value, logicalPath, file);
      if (!object)
      {
        return std::unexpected(object.error());
      }

      auto kindText =
          RequireString(**object, "kind", logicalPath, file, false, 32);
      if (!kindText)
      {
        return std::unexpected(kindText.error());
      }

      QueueResumeKind kind;
      if (*kindText == "download")
      {
        kind = QueueResumeKind::Download;

        if (auto exact = RequireExactMembers(
                **object,
                {"kind", "localPath", "remotePath", "remoteSize",
                 "remoteModifiedAtUnixNanoseconds", "partialSize",
                 "partialModifiedAtUnixNanoseconds"},
                logicalPath,
                file);
            !exact)
        {
          return std::unexpected(exact.error());
        }
      }
      else if (*kindText == "upload")
      {
        kind = QueueResumeKind::Upload;

        if (auto exact = RequireExactMembers(
                **object,
                {"kind", "localPath", "remotePath", "localSize",
                 "localModifiedAtUnixNanoseconds", "temporaryRemotePath"},
                logicalPath,
                file);
            !exact)
        {
          return std::unexpected(exact.error());
        }
      }
      else
      {
        return std::unexpected(WithMemberSource(Error(ConfigErrorKind::Validation,
                                                      file,
                                                      std::string{logicalPath} +
                                                          ".kind has an unknown value"),
                                                value, "kind"));
      }

      auto localPath = RequireString(
          **object, "localPath", logicalPath, file, false, MaximumTextLength);
      auto remotePathMember =
          RequireMember(**object, "remotePath", logicalPath, file);

      if (!localPath)
      {
        return std::unexpected(localPath.error());
      }

      if (!remotePathMember)
      {
        return std::unexpected(remotePathMember.error());
      }

      if (ContainsUnsafeControl(*localPath))
      {
        return std::unexpected(WithMemberSource(Error(ConfigErrorKind::Validation,
                                                      file,
                                                      std::string{logicalPath} +
                                                          ".localPath contains control characters"),
                                                value, "localPath"));
      }

      auto remotePath = DecodeRemotePath(
          **remotePathMember,
          std::string{logicalPath} + ".remotePath",
          file);
      if (!remotePath)
      {
        return std::unexpected(remotePath.error());
      }

      QueueResumeRecord result;
      result.kind = kind;
      result.localPath = PathFromUtf8(*localPath);
      result.remotePath = std::move(*remotePath);

      if (kind == QueueResumeKind::Download)
      {
        auto remoteSize = RequireDecimal<std::uint64_t>(
            **object, "remoteSize", logicalPath, file);
        auto remoteModified = RequireNullableSignedDecimal(
            **object,
            "remoteModifiedAtUnixNanoseconds",
            logicalPath,
            file);
        auto partialSize = RequireDecimal<std::uint64_t>(
            **object, "partialSize", logicalPath, file);
        auto partialModified = RequireNullableSignedDecimal(
            **object,
            "partialModifiedAtUnixNanoseconds",
            logicalPath,
            file);

        if (!remoteSize)
        {
          return std::unexpected(remoteSize.error());
        }

        if (!remoteModified)
        {
          return std::unexpected(remoteModified.error());
        }

        if (!partialSize)
        {
          return std::unexpected(partialSize.error());
        }

        if (!partialModified)
        {
          return std::unexpected(partialModified.error());
        }

        if (!*partialModified || *partialSize > *remoteSize)
        {
          return std::unexpected(WithMemberSource(Error(
                                                      ConfigErrorKind::Validation,
                                                      file,
                                                      std::string{logicalPath} +
                                                          " contains inconsistent partial download metadata"),
                                                  value,
                                                  !*partialModified ? "partialModifiedAtUnixNanoseconds" : "partialSize"));
        }

        result.remoteSize = *remoteSize;
        result.remoteModifiedAt =
            *remoteModified
                ? std::optional<std::chrono::system_clock::time_point>{
                      SystemTimeFromNanoseconds(**remoteModified)}
                : std::nullopt;
        result.partSize = *partialSize;
        result.partModifiedAt = FileTimeFromNanoseconds(**partialModified);
      }
      else
      {
        auto localSize = RequireDecimal<std::uint64_t>(
            **object, "localSize", logicalPath, file);
        auto localModified = RequireNullableSignedDecimal(
            **object,
            "localModifiedAtUnixNanoseconds",
            logicalPath,
            file);
        auto temporaryMember = RequireMember(
            **object, "temporaryRemotePath", logicalPath, file);

        if (!localSize)
        {
          return std::unexpected(localSize.error());
        }

        if (!localModified)
        {
          return std::unexpected(localModified.error());
        }

        if (!temporaryMember)
        {
          return std::unexpected(temporaryMember.error());
        }

        if (!*localModified)
        {
          return std::unexpected(WithMemberSource(Error(
                                                      ConfigErrorKind::Validation,
                                                      file,
                                                      std::string{logicalPath} +
                                                          ".localModifiedAtUnixNanoseconds must not be null"),
                                                  value,
                                                  "localModifiedAtUnixNanoseconds"));
        }

        auto temporaryPath = DecodeRemotePath(
            **temporaryMember,
            std::string{logicalPath} + ".temporaryRemotePath",
            file);
        if (!temporaryPath)
        {
          return std::unexpected(temporaryPath.error());
        }

        result.localSize = *localSize;
        result.localModifiedAt = FileTimeFromNanoseconds(**localModified);
        result.temporaryRemotePath = std::move(*temporaryPath);
      }

      return result;
    }

    std::expected<PersistentQueueItem, ConfigError> DecodeItem(
        const havCSON::Value &value,
        const std::size_t index,
        const std::filesystem::path &file)
    {
      const auto logicalPath = "items[" + std::to_string(index) + "]";

      auto object = RequireObject(value, logicalPath, file);
      if (!object)
      {
        return std::unexpected(object.error());
      }

      if (auto exact = RequireExactMembers(
              **object,
              {"connectionId", "job", "state", "progress", "attempt",
               "error", "resumeRecords"},
              logicalPath,
              file);
          !exact)
      {
        return std::unexpected(exact.error());
      }

      auto connectionId = RequireString(**object,
                                        "connectionId",
                                        logicalPath,
                                        file,
                                        false,
                                        MaximumIdentifierLength);
      auto stateText =
          RequireString(**object, "state", logicalPath, file, false, 32);
      auto attempt = RequireNumber<std::uint32_t>(
          **object,
          "attempt",
          logicalPath,
          file,
          0,
          std::numeric_limits<std::uint32_t>::max());

      if (!connectionId)
      {
        return std::unexpected(connectionId.error());
      }

      if (!stateText)
      {
        return std::unexpected(stateText.error());
      }

      if (!attempt)
      {
        return std::unexpected(attempt.error());
      }

      if (!ValidIdentifier(*connectionId))
      {
        return std::unexpected(WithMemberSource(Error(ConfigErrorKind::Validation,
                                                      file,
                                                      logicalPath +
                                                          ".connectionId is invalid"),
                                                value, "connectionId"));
      }

      const auto state = ParsePersistentTransferState(*stateText);
      if (!state)
      {
        return std::unexpected(WithMemberSource(Error(ConfigErrorKind::Validation,
                                                      file,
                                                      logicalPath +
                                                          ".state is not persistable"),
                                                value, "state"));
      }

      auto jobMember = RequireMember(**object, "job", logicalPath, file);
      auto progressMember =
          RequireMember(**object, "progress", logicalPath, file);
      auto errorMember = RequireMember(**object, "error", logicalPath, file);
      auto resumeMember =
          RequireMember(**object, "resumeRecords", logicalPath, file);

      if (!jobMember)
      {
        return std::unexpected(jobMember.error());
      }

      if (!progressMember)
      {
        return std::unexpected(progressMember.error());
      }

      if (!errorMember)
      {
        return std::unexpected(errorMember.error());
      }

      if (!resumeMember)
      {
        return std::unexpected(resumeMember.error());
      }

      auto job = DecodeJob(
          **jobMember, logicalPath + ".job", file);
      if (!job)
      {
        return std::unexpected(job.error());
      }

      auto progress = DecodeProgress(
          **progressMember,
          job->id,
          logicalPath + ".progress",
          file);
      auto remoteError = DecodeError(
          **errorMember, logicalPath + ".error", file);
      auto resumeArray = RequireArray(
          **resumeMember, logicalPath + ".resumeRecords", file);

      if (!progress)
      {
        return std::unexpected(progress.error());
      }

      if (!remoteError)
      {
        return std::unexpected(remoteError.error());
      }

      if (!resumeArray)
      {
        return std::unexpected(resumeArray.error());
      }

      if ((*resumeArray)->size() > MaximumResumeRecordsPerItem)
      {
        return std::unexpected(WithSource(Error(
                                              ConfigErrorKind::Validation,
                                              file,
                                              logicalPath + ".resumeRecords contains too many entries"),
                                          **resumeMember));
      }

      PersistentQueueItem result;
      result.connectionId = std::move(*connectionId);
      result.transfer = QueuedTransfer{.job = std::move(*job),
                                       .state = *state,
                                       .progress = std::move(*progress),
                                       .error = std::move(*remoteError),
                                       .attempt = *attempt};
      result.resumeRecords.reserve((*resumeArray)->size());

      for (std::size_t resumeIndex = 0; resumeIndex < (*resumeArray)->size(); ++resumeIndex)
      {
        auto resume = DecodeResumeRecord(
            (**resumeArray)[resumeIndex],
            logicalPath + ".resumeRecords[" +
                std::to_string(resumeIndex) + "]",
            file);

        if (!resume)
        {
          return std::unexpected(resume.error());
        }

        result.resumeRecords.push_back(std::move(*resume));
      }

      if (auto valid = ValidateItem(result, file, index); !valid)
      {
        // Cross-field consistency failures refer to the actual containing item,
        // rather than guessing a member from the diagnostic message.
        return std::unexpected(WithSource(valid.error(), value));
      }

      // Work that may have been in progress when the process ended is always
      // recovered inertly. The user must reconnect and explicitly retry it.
      if (result.transfer.state == TransferState::Queued ||
          result.transfer.state == TransferState::Enumerating ||
          result.transfer.state == TransferState::Running)
      {
        result.transfer.state = TransferState::Paused;
      }

      return result;
    }

    std::expected<QueueLoadResult, ConfigError> DecodeDocument(
        const havCSON::Value &document,
        const std::filesystem::path &file)
    {
      if (const auto forbidden = ForbiddenSecretKey(document))
      {
        return std::unexpected(WithSource(Error(
                                              ConfigErrorKind::Validation,
                                              file,
                                              "Persistent queue contains forbidden secret-bearing field '" +
                                                  forbidden->first + "'"),
                                          *forbidden->second));
      }

      auto root = RequireObject(document, "root", file);
      if (!root)
      {
        return std::unexpected(root.error());
      }

      auto version = RequireNumber<std::uint32_t>(
          **root,
          "formatVersion",
          "root",
          file,
          0,
          std::numeric_limits<std::uint32_t>::max());
      if (!version)
      {
        return std::unexpected(version.error());
      }
      if (*version != CurrentQueueFormatVersion)
      {
        return std::unexpected(WithSource(Error(
                                              ConfigErrorKind::UnsupportedVersion,
                                              file,
                                              "Persistent queue format version " + std::to_string(*version) +
                                                  " is not supported. Expected version " +
                                                  std::to_string(CurrentQueueFormatVersion)),
                                          havCSON::ValueView{document}.Member("formatVersion").Get()->get()));
      }

      if (auto exact = RequireExactMembers(
              **root, {"formatVersion", "items"}, "root", file);
          !exact)
      {
        return std::unexpected(exact.error());
      }

      auto itemsMember = RequireMember(**root, "items", "root", file);
      if (!itemsMember)
      {
        return std::unexpected(itemsMember.error());
      }

      auto items = RequireArray(**itemsMember, "items", file);
      if (!items)
      {
        return std::unexpected(items.error());
      }

      if ((*items)->size() > MaximumQueueItems)
      {
        return std::unexpected(WithSource(Error(ConfigErrorKind::Validation,
                                                file,
                                                "Persistent queue contains too many items"),
                                          **itemsMember));
      }

      QueueLoadResult result;
      result.items.reserve((*items)->size());

      std::unordered_set<std::string> jobIds;

      for (std::size_t index = 0; index < (*items)->size(); ++index)
      {
        auto item = DecodeItem((**items)[index], index, file);
        if (!item)
        {
          return std::unexpected(item.error());
        }

        if (!jobIds.insert(item->transfer.job.id).second)
        {
          return std::unexpected(WithSource(Error(
                                                ConfigErrorKind::Validation,
                                                file,
                                                "Persistent queue contains duplicate transfer id '" +
                                                    item->transfer.job.id + "'"),
                                            havCSON::ValueView{(**items)[index]}.Member("job").Member("id").Get()->get()));
        }

        result.items.push_back(std::move(*item));
      }

      return result;
    }

    std::expected<void, ConfigError> WriteDocument(
        const std::filesystem::path &path,
        const havCSON::Value &document)
    {
      if (auto directory = EnsureParentDirectory(path); !directory)
      {
        return std::unexpected(directory.error());
      }

      havCSON::WriteOptions options;
      options.indentWidth = 2;
      options.sortObjectKeys = true;

      havCSON::Error writeError;

      if (!havCSON::WriteFileAtomic(
              PathToUtf8(path), document, options, &writeError))
      {
        return std::unexpected(Error(
            ConfigErrorKind::AtomicWrite,
            path,
            writeError.message.empty()
                ? "Failed to atomically save the persistent queue"
                : writeError.message));
      }

      return {};
    }
  } // namespace

  struct CsonQueueRepository::Impl final
  {
    enum class LoadState
    {
      Unloaded,
      Ready,
      Blocked,
    };

    explicit Impl(std::filesystem::path selectedPath)
        : path(std::move(selectedPath)) {}

    std::filesystem::path path;
    mutable std::mutex mutex;
    LoadState loadState{LoadState::Unloaded};

    std::expected<QueueLoadResult, ConfigError> LoadUnlocked()
    {
      if (path.empty())
      {
        loadState = LoadState::Blocked;

        return std::unexpected(Error(ConfigErrorKind::PathUnavailable,
                                     path,
                                     "The persistent queue path is empty"));
      }

      auto exists = FileExists(path);
      if (!exists)
      {
        loadState = LoadState::Blocked;

        return std::unexpected(exists.error());
      }

      if (!*exists)
      {
        loadState = LoadState::Ready;

        return QueueLoadResult{};
      }

      havCSON::Value parsed;
      havCSON::Error parseFailure;

      const havCSON::ParseOptions parseOptions{
          .trackSourceLocations = true,
          .maxDepth = 256,
          .maxInputBytes = MaximumQueueFileSize};

      if (havCSON::ParseFile(PathToUtf8(path), parsed, &parseFailure, parseOptions) !=
          havCSON::ErrorCode::OK)
      {
        loadState = LoadState::Blocked;

        const bool fileFailure =
            parseFailure.code == havCSON::ErrorCode::IoError;
        const auto kind = fileFailure ? ConfigErrorKind::Io
                          : parseFailure.code == havCSON::ErrorCode::ResourceLimit
                              ? ConfigErrorKind::Validation
                              : ConfigErrorKind::Parse;

        return std::unexpected(Error(
            kind,
            path,
            parseFailure.message.empty()
                ? "Failed to parse the persistent queue"
                : parseFailure.message,
            fileFailure
                ? std::nullopt
                : std::optional<std::size_t>{parseFailure.where.line},
            fileFailure
                ? std::nullopt
                : std::optional<std::size_t>{parseFailure.where.column}));
      }

      auto decoded = DecodeDocument(parsed, path);
      if (!decoded)
      {
        loadState = LoadState::Blocked;

        return std::unexpected(decoded.error());
      }

      loadState = LoadState::Ready;

      return decoded;
    }
  };

  CsonQueueRepository::CsonQueueRepository(std::filesystem::path path)
      : mImpl(std::make_unique<Impl>(std::move(path))) {}

  CsonQueueRepository::~CsonQueueRepository() = default;
  CsonQueueRepository::CsonQueueRepository(CsonQueueRepository &&) noexcept = default;
  CsonQueueRepository &CsonQueueRepository::operator=(
      CsonQueueRepository &&) noexcept = default;

  std::expected<QueueLoadResult, ConfigError> CsonQueueRepository::Load()
  {
    std::scoped_lock lock{mImpl->mutex};

    return mImpl->LoadUnlocked();
  }

  std::expected<void, ConfigError> CsonQueueRepository::Save(
      const std::vector<PersistentQueueItem> &items)
  {
    std::scoped_lock lock{mImpl->mutex};

    if (mImpl->loadState == Impl::LoadState::Blocked)
    {
      return std::unexpected(Error(
          ConfigErrorKind::Validation,
          mImpl->path,
          "Persistent queue writes are disabled because the existing file did not load successfully"));
    }

    if (mImpl->loadState == Impl::LoadState::Unloaded)
    {
      auto loaded = mImpl->LoadUnlocked();
      if (!loaded)
      {
        return std::unexpected(loaded.error());
      }
    }

    auto document = MakeDocument(items, mImpl->path);
    if (!document)
    {
      return std::unexpected(document.error());
    }

    return WriteDocument(mImpl->path, *document);
  }

  std::expected<std::filesystem::path, ConfigError>
  CsonQueueRepository::QueuePath() const
  {
    std::scoped_lock lock{mImpl->mutex};

    if (mImpl->path.empty())
    {
      return std::unexpected(Error(ConfigErrorKind::PathUnavailable,
                                   mImpl->path,
                                   "The persistent queue path is empty"));
    }

    return mImpl->path;
  }
} // namespace havremote::config
