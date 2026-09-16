// SPDX-License-Identifier: MIT

#include "config/csonConfigRepository.hpp"

#include "config/csonDocumentEditor.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace havremote::config
{
  namespace
  {
    using LosslessValue = havCSON::LosslessValue;

    constexpr std::uint32_t MinTransferConcurrency = 1;
    constexpr std::uint32_t MaxTransferConcurrency = 16;
    constexpr std::uint32_t MinConnectionTimeoutSeconds = 1;
    constexpr std::uint32_t MaxConnectionTimeoutSeconds = 600;
    constexpr std::uint32_t MinCommandIdleTimeoutSeconds = 1;
    constexpr std::uint32_t MaxCommandIdleTimeoutSeconds = 3600;
    constexpr std::size_t MaxLanguageCodeLength = 35;
    constexpr std::int32_t MinWindowCoordinate = -1'000'000;
    constexpr std::int32_t MaxWindowCoordinate = 1'000'000;
    constexpr std::uint32_t MinWindowWidth = 320;
    constexpr std::uint32_t MaxWindowWidth = 100'000;
    constexpr std::uint32_t MinWindowHeight = 240;
    constexpr std::uint32_t MaxWindowHeight = 100'000;
    constexpr std::uint32_t MinFileListColumnWidth = 32;
    constexpr std::uint32_t MaxFileListColumnWidth = 4096;
    constexpr std::size_t LocalFileListColumnCount = 4;
    constexpr std::size_t RemoteFileListColumnCount = 6;
    constexpr std::size_t MaxOpenConnectionTabs = 64;
    constexpr std::size_t MaxConfigInputBytes = 64U * 1024U * 1024U;

    struct DiagnosticLocation final
    {
      std::size_t line{1};
      std::size_t column{1};
    };

    LosslessValue Scalar(havCSON::Value value)
    {
      return havCSON::MakeLossless(value);
    }

    LosslessValue Object()
    {
      LosslessValue result;
      result.value = havCSON::Object{};

      return result;
    }

    LosslessValue Array()
    {
      LosslessValue result;
      result.value = havCSON::Array{};

      return result;
    }

    std::string PathToUtf8(const std::filesystem::path &path)
    {
      const auto utf8 = path.generic_u8string();

      return {utf8.begin(), utf8.end()};
    }

    std::filesystem::path PathFromUtf8(std::string_view value)
    {
      std::u8string utf8;
      utf8.reserve(value.size());

      for (const char character : value)
      {
        utf8.push_back(static_cast<char8_t>(static_cast<unsigned char>(character)));
      }

      return std::filesystem::path(utf8);
    }

    ConfigError Error(ConfigErrorKind kind,
                      const std::filesystem::path &path,
                      std::string message,
                      std::optional<DiagnosticLocation> location = std::nullopt)
    {
      return ConfigError{
          .kind = kind,
          .path = path,
          .message = std::move(message),
          .line = location ? std::optional<std::size_t>{location->line} : std::nullopt,
          .column = location ? std::optional<std::size_t>{location->column} : std::nullopt,
      };
    }

    std::optional<DiagnosticLocation> SourceLocation(const havCSON::SourceInfo *source)
    {
      if (source == nullptr)
      {
        return std::nullopt;
      }

      const auto &position = source->keySpan ? source->keySpan->begin
                                             : source->valueSpan.begin;

      return DiagnosticLocation{position.line, position.column};
    }

    ConfigError WithSource(ConfigError error, const LosslessValue &value)
    {
      if (!error.line)
      {
        if (const auto location = SourceLocation(value.Source()))
        {
          error.line = location->line;
          error.column = location->column;
        }
      }

      return error;
    }

    ConfigError ErrorAt(const LosslessValue &value,
                        ConfigErrorKind kind,
                        const std::filesystem::path &file,
                        std::string message)
    {
      return WithSource(Error(kind, file, std::move(message)), value);
    }

    std::string DescribeCsonError(const havCSON::Error &error,
                                  std::string_view fallback)
    {
      std::string message = error.message.empty() ? std::string(fallback)
                                                  : error.message;

      if (!error.operation.empty())
      {
        message += " (" + error.operation + ")";
      }

      if (error.systemError)
      {
        message += ": " + error.systemError.message();
      }

      return message;
    }

    ConfigError ParseError(const std::filesystem::path &path,
                           const havCSON::Error &source)
    {
      const bool fileFailure = source.code == havCSON::ErrorCode::IoError;

      ConfigError result{
          .kind = fileFailure ? ConfigErrorKind::Io : ConfigErrorKind::Parse,
          .path = path,
          .message = DescribeCsonError(source, "Failed to parse CSON configuration"),
          .line = fileFailure ? std::nullopt
                              : std::optional<std::size_t>{source.where.line},
          .column = fileFailure ? std::nullopt
                                : std::optional<std::size_t>{source.where.column},
      };

      return result;
    }

    const LosslessValue *Member(const LosslessValue &objectValue,
                                std::string_view key)
    {
      const auto found = std::find_if(
          objectValue.objectItems.begin(), objectValue.objectItems.end(),
          [key](const auto &item)
          { return item.first == key; });

      return found == objectValue.objectItems.end() ? nullptr : &found->second;
    }

    std::expected<const LosslessValue *, ConfigError> RequiredMember(
        const LosslessValue &objectValue,
        std::string_view key,
        std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      const auto member = havCSON::ValueView(objectValue.value).Member(key).Get();

      if (!member)
      {
        const auto &failure = member.error();

        return std::unexpected(Error(
            ConfigErrorKind::Validation, file,
            failure.code == havCSON::ErrorCode::TypeMismatch
                ? std::string(logicalPath) + " must be an object"
                : std::string(logicalPath) + "." + std::string(key) + " is required",
            SourceLocation(failure.source ? &*failure.source : nullptr)));
      }

      return Member(objectValue, key);
    }

    std::expected<std::string, ConfigError> StringValue(
        const LosslessValue &value,
        std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      const auto result = havCSON::ValueView(value.value).AsString();

      if (!result)
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation, file,
            std::string(logicalPath) + " must be a string",
            SourceLocation(value.Source())));
      }

      return result->get();
    }

    std::expected<std::string, ConfigError> RequiredString(
        const LosslessValue &objectValue,
        std::string_view key,
        std::string_view logicalPath,
        const std::filesystem::path &file,
        bool allowEmpty = true)
    {
      const auto found = RequiredMember(objectValue, key, logicalPath, file);

      if (!found)
      {
        return std::unexpected(found.error());
      }

      auto result = StringValue(**found,
                                std::string(logicalPath) + "." + std::string(key),
                                file);

      if (result && !allowEmpty && result->empty())
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation, file,
            std::string(logicalPath) + "." + std::string(key) +
                " must not be empty",
            SourceLocation((**found).Source())));
      }

      return result;
    }

    std::expected<bool, ConfigError> BooleanValue(
        const LosslessValue &value,
        std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      const auto result = havCSON::ValueView(value.value).AsBool();

      if (!result)
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation, file,
            std::string(logicalPath) + " must be a boolean",
            SourceLocation(value.Source())));
      }

      return *result;
    }

    std::expected<bool, ConfigError> RequiredBoolean(
        const LosslessValue &objectValue,
        std::string_view key,
        std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      const auto found = RequiredMember(objectValue, key, logicalPath, file);

      if (!found)
      {
        return std::unexpected(found.error());
      }

      return BooleanValue(**found,
                          std::string(logicalPath) + "." + std::string(key),
                          file);
    }

    std::expected<std::uint32_t, ConfigError> UnsignedIntegerValue(
        const LosslessValue &value,
        std::string_view logicalPath,
        std::uint32_t minimum,
        std::uint32_t maximum,
        const std::filesystem::path &file)
    {
      const auto result = havCSON::ValueView(value.value).AsInteger<std::uint32_t>(minimum, maximum);

      if (!result)
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation, file,
            std::string(logicalPath) +
                (result.error().code == havCSON::ErrorCode::TypeMismatch
                     ? " must be a number"
                     : " must be an integer from " + std::to_string(minimum) +
                           " through " + std::to_string(maximum)),
            SourceLocation(value.Source())));
      }

      return *result;
    }
    std::expected<std::uint32_t, ConfigError> RequiredUnsignedInteger(
        const LosslessValue &objectValue,
        std::string_view key,
        std::string_view logicalPath,
        std::uint32_t minimum,
        std::uint32_t maximum,
        const std::filesystem::path &file)
    {
      const auto found = RequiredMember(objectValue, key, logicalPath, file);

      if (!found)
      {
        return std::unexpected(found.error());
      }

      return UnsignedIntegerValue(**found,
                                  std::string(logicalPath) + "." + std::string(key),
                                  minimum, maximum, file);
    }

    std::expected<std::int32_t, ConfigError> SignedIntegerValue(
        const LosslessValue &value,
        std::string_view logicalPath,
        std::int32_t minimum,
        std::int32_t maximum,
        const std::filesystem::path &file)
    {
      const auto result = havCSON::ValueView(value.value).AsInteger<std::int32_t>(minimum, maximum);

      if (!result)
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation, file,
            std::string(logicalPath) +
                (result.error().code == havCSON::ErrorCode::TypeMismatch
                     ? " must be a number"
                     : " must be an integer from " + std::to_string(minimum) +
                           " through " + std::to_string(maximum)),
            SourceLocation(value.Source())));
      }

      return *result;
    }

    std::expected<std::optional<std::int32_t>, ConfigError>
    RequiredNullableSignedInteger(
        const LosslessValue &objectValue,
        std::string_view key,
        std::string_view logicalPath,
        std::int32_t minimum,
        std::int32_t maximum,
        const std::filesystem::path &file)
    {
      const auto found = RequiredMember(objectValue, key, logicalPath, file);

      if (!found)
      {
        return std::unexpected(found.error());
      }

      if ((**found).value.isNull())
      {
        return std::optional<std::int32_t>{};
      }

      auto decoded = SignedIntegerValue(
          **found, std::string(logicalPath) + "." + std::string(key), minimum,
          maximum, file);

      if (!decoded)
      {
        return std::unexpected(decoded.error());
      }

      return std::optional<std::int32_t>{*decoded};
    }

    std::string NormalizedKey(std::string_view key)
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

    std::optional<std::string> ForbiddenSecretKey(const LosslessValue &value)
    {
      static const std::unordered_set<std::string> forbidden{
          "password",
          "passphrase",
          "secret",
          "token",
          "accesstoken",
          "refreshtoken",
          "credentialvalue",
          "privatekeydata",
          "privatekeycontents",
          "certificateprivatekey",
      };

      for (const auto &[key, child] : value.objectItems)
      {
        if (forbidden.contains(NormalizedKey(key)))
        {
          return key;
        }

        if (auto nested = ForbiddenSecretKey(child))
        {
          return nested;
        }
      }

      for (const auto &child : value.arrayItems)
      {
        if (auto nested = ForbiddenSecretKey(child))
        {
          return nested;
        }
      }

      return std::nullopt;
    }

    std::string ProtocolName(ProtocolKind protocol)
    {
      switch (protocol)
      {
      case ProtocolKind::Ftp:
        return "ftp";

      case ProtocolKind::FtpsExplicit:
        return "ftpsExplicit";

      case ProtocolKind::FtpsImplicit:
        return "ftpsImplicit";

      case ProtocolKind::Sftp:
        return "sftp";
      }

      return {};
    }

    std::optional<ProtocolKind> ParseProtocol(std::string_view value)
    {
      if (value == "ftp")
      {
        return ProtocolKind::Ftp;
      }

      if (value == "ftpsExplicit")
      {
        return ProtocolKind::FtpsExplicit;
      }

      if (value == "ftpsImplicit")
      {
        return ProtocolKind::FtpsImplicit;
      }

      if (value == "sftp")
      {
        return ProtocolKind::Sftp;
      }

      return std::nullopt;
    }

    std::string FtpDataConnectionModeName(const FtpDataConnectionMode mode)
    {
      switch (mode)
      {
      case FtpDataConnectionMode::Passive:
        return "passive";

      case FtpDataConnectionMode::Active:
        return "active";
      }

      return {};
    }

    std::optional<FtpDataConnectionMode> ParseFtpDataConnectionMode(
        const std::string_view value)
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

    std::string AuthenticationName(AuthenticationKind kind)
    {
      switch (kind)
      {
      case AuthenticationKind::Password:
        return "password";

      case AuthenticationKind::PasswordKeyboardInteractive:
        return "passwordKeyboardInteractive";

      case AuthenticationKind::PrivateKey:
        return "privateKey";

      case AuthenticationKind::Agent:
        return "agent";

      case AuthenticationKind::KeyboardInteractive:
        return "keyboardInteractive";
      }

      return {};
    }

    std::optional<AuthenticationKind> ParseAuthentication(std::string_view value)
    {
      if (value == "password")
      {
        return AuthenticationKind::Password;
      }

      if (value == "passwordKeyboardInteractive")
      {
        return AuthenticationKind::PasswordKeyboardInteractive;
      }

      if (value == "privateKey")
      {
        return AuthenticationKind::PrivateKey;
      }

      if (value == "agent")
      {
        return AuthenticationKind::Agent;
      }

      if (value == "keyboardInteractive")
      {
        return AuthenticationKind::KeyboardInteractive;
      }

      return std::nullopt;
    }

    std::string ConflictPolicyName(ConflictPolicy policy)
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

    std::optional<ConflictPolicy> ParseConflictPolicy(std::string_view value)
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

    std::string ThemeName(AppearanceTheme theme)
    {
      switch (theme)
      {
      case AppearanceTheme::Light:
        return "light";

      case AppearanceTheme::Dark:
        return "dark";
      }

      return {};
    }

    std::optional<AppearanceTheme> ParseTheme(std::string_view value)
    {
      if (value == "light")
      {
        return AppearanceTheme::Light;
      }

      if (value == "dark")
      {
        return AppearanceTheme::Dark;
      }

      return std::nullopt;
    }

    std::string ExternalEditorModeName(const ExternalEditorMode mode)
    {
      switch (mode)
      {
      case ExternalEditorMode::SystemDefault:
        return "system";

      case ExternalEditorMode::Custom:
        return "custom";
      }

      return {};
    }

    std::optional<ExternalEditorMode> ParseExternalEditorMode(
        const std::string_view value)
    {
      if (value == "system")
      {
        return ExternalEditorMode::SystemDefault;
      }

      if (value == "custom")
      {
        return ExternalEditorMode::Custom;
      }

      return std::nullopt;
    }

    std::string FileListSortColumnName(const FileListSortColumn column)
    {
      switch (column)
      {
      case FileListSortColumn::Name:
        return "name";

      case FileListSortColumn::Size:
        return "size";

      case FileListSortColumn::Type:
        return "type";

      case FileListSortColumn::Modified:
        return "modified";

      case FileListSortColumn::Permissions:
        return "permissions";

      case FileListSortColumn::Owner:
        return "owner";
      }

      return {};
    }

    std::optional<FileListSortColumn> ParseFileListSortColumn(
        const std::string_view value)
    {
      if (value == "name")
      {
        return FileListSortColumn::Name;
      }

      if (value == "size")
      {
        return FileListSortColumn::Size;
      }

      if (value == "type")
      {
        return FileListSortColumn::Type;
      }

      if (value == "modified")
      {
        return FileListSortColumn::Modified;
      }

      if (value == "permissions")
      {
        return FileListSortColumn::Permissions;
      }

      if (value == "owner")
      {
        return FileListSortColumn::Owner;
      }

      return std::nullopt;
    }

    bool IsRemoteOnlyFileListSortColumn(
        const FileListSortColumn column) noexcept
    {
      return column == FileListSortColumn::Permissions ||
             column == FileListSortColumn::Owner;
    }

    bool IsAsciiAlphaNumeric(const char character) noexcept
    {
      return (character >= 'a' && character <= 'z') ||
             (character >= 'A' && character <= 'Z') ||
             (character >= '0' && character <= '9');
    }

    bool IsValidLanguageCode(const std::string_view value) noexcept
    {
      if (value.empty() || value.size() > MaxLanguageCodeLength ||
          !IsAsciiAlphaNumeric(value.front()) ||
          !IsAsciiAlphaNumeric(value.back()))
      {
        return false;
      }

      bool previousWasHyphen = false;

      for (const char character : value)
      {
        if (IsAsciiAlphaNumeric(character))
        {
          previousWasHyphen = false;

          continue;
        }

        if (character != '-' || previousWasHyphen)
        {
          return false;
        }

        previousWasHyphen = true;
      }

      return true;
    }

    bool IsDecimalUnsignedIntegerString(const std::string_view value) noexcept
    {
      if (value.empty() || value.size() > 20 ||
          (value.size() > 1 && value.front() == '0'))
      {
        return false;
      }

      std::uint64_t parsed{};
      const auto [end, errorCode] =
          std::from_chars(value.data(), value.data() + value.size(), parsed);

      return errorCode == std::errc{} && end == value.data() + value.size();
    }

    bool IsCanonicalReleaseVersion(const std::string_view value) noexcept
    {
      if (value.empty())
      {
        return true;
      }

      std::size_t componentBegin{};

      for (int component = 0; component < 3; ++component)
      {
        const auto separator = value.find('.', componentBegin);
        const auto componentEnd =
            separator == std::string_view::npos ? value.size() : separator;
        const auto text = value.substr(componentBegin,
                                       componentEnd - componentBegin);

        if (text.empty() || (text.size() > 1 && text.front() == '0') ||
            !std::ranges::all_of(text, [](const unsigned char character)
                                 { return character >= '0' && character <= '9'; }))
        {
          return false;
        }

        std::uint32_t parsed{};
        const auto [end, errorCode] =
            std::from_chars(text.data(), text.data() + text.size(), parsed);

        if (errorCode != std::errc{} || end != text.data() + text.size())
        {
          return false;
        }

        if (component < 2 && separator == std::string_view::npos)
        {
          return false;
        }

        if (component == 2 && separator != std::string_view::npos)
        {
          return false;
        }

        componentBegin = componentEnd + 1;
      }

      return true;
    }

    bool ContainsControlCharacter(const std::string_view value) noexcept
    {
      return std::ranges::any_of(value, [](const unsigned char character)
                                 { return character < 0x20U || character == 0x7fU; });
    }

    bool IsValidCredentialIdentifier(const std::string_view value)
    {
      if (value.empty() || value.find_first_of(std::string_view{"\0\r\n", 3}) !=
                               std::string_view::npos)
      {
        return false;
      }

      // Identifiers must round-trip through CSON without changing the store key
      std::string serialized;
      if (!havCSON::ToString(havCSON::Value{std::string{value}}, serialized))
      {
        return false;
      }

      havCSON::Value decoded;
      havCSON::ParseOptions options;
      options.maxInputBytes = 0;

      return havCSON::Parse(serialized, decoded, nullptr, options) ==
                 havCSON::ErrorCode::OK &&
             decoded.isString() && std::get<std::string>(decoded) == value;
    }

    std::optional<std::string> CredentialDeletionValidationError(
        const std::string_view id,
        const std::size_t index,
        std::unordered_set<std::string_view> &seenIds)
    {
      const auto logicalPath =
          "pendingCredentialDeletions[" + std::to_string(index) + "]";

      if (!IsValidCredentialIdentifier(id))
      {
        return logicalPath +
               " must be a nonempty UTF-8 credential identifier without NUL or line breaks";
      }

      if (!seenIds.insert(id).second)
      {
        return logicalPath + " contains a duplicate credential identifier";
      }

      return std::nullopt;
    }

    bool IsSafeWorkspacePath(const std::string_view value,
                             const bool allowEmpty) noexcept
    {
      return (allowEmpty || !value.empty()) && !ContainsControlCharacter(value);
    }

    bool IsSafeExternalEditorExecutable(const std::string_view value,
                                        const bool allowEmpty) noexcept
    {
      return (allowEmpty || !value.empty()) && !ContainsControlCharacter(value);
    }

    bool IsSafeExternalEditorArguments(const std::string_view value) noexcept
    {
      constexpr std::string_view Placeholder = "{file}";

      if (ContainsControlCharacter(value))
      {
        return false;
      }

      const auto first = value.find(Placeholder);

      if (first == std::string_view::npos ||
          value.find(Placeholder, first + Placeholder.size()) !=
              std::string_view::npos)
      {
        return false;
      }

      // Keep configuration validation aligned with the shell-free launcher:
      // quotes group arguments and backslashes may escape a quote or another
      // backslash. Reject malformed templates while the filename is still only
      // a placeholder, rather than waiting until a user tries to open a file.
      char quote{};

      for (std::size_t index = 0; index < value.size(); ++index)
      {
        const char character = value[index];

        if (character == '\\' && index + 1U < value.size())
        {
          const char next = value[index + 1U];

          if (next == '\\' || next == '\'' || next == '"' ||
              next == ' ' || next == '\t')
          {
            ++index;

            continue;
          }
        }

        if (character != '\'' && character != '"')
        {
          continue;
        }

        if (quote == 0)
        {
          quote = character;
        }
        else if (quote == character)
        {
          quote = 0;
        }
      }

      return quote == 0;
    }

    void SetMember(CsonDocumentEditor &editor,
                   LosslessValue &parent,
                   std::string key,
                   LosslessValue value)
    {
      if (const auto *existing = editor.FindMember(parent, key))
      {
        // These replacements contain schema values, not new comments. Leave an
        // unchanged value's complete lossless node alone. The generic editor
        // must still support comment-only replacements by other callers.
        if (existing->value == value.value)
        {
          return;
        }

        (void)editor.ReplaceMember(parent, key, std::move(value));
      }
      else
      {
        (void)editor.InsertMember(parent, std::move(key), std::move(value));
      }
    }

    LosslessValue MakeFileListSortSettings(
        const FileListSortSettings &settings,
        CsonDocumentEditor &editor)
    {
      auto result = Object();

      (void)editor.InsertMember(
          result, "sortColumn",
          Scalar(FileListSortColumnName(settings.sortColumn)));
      (void)editor.InsertMember(result, "sortAscending",
                                Scalar(settings.sortAscending));

      auto widths = Array();

      for (const auto width : settings.columnWidths)
      {
        (void)editor.AppendArrayItem(
            widths, Scalar(static_cast<double>(width)));
      }

      (void)editor.InsertMember(result, "columnWidths", std::move(widths));

      return result;
    }

    LosslessValue MakeFileListSettings(const FileListSettings &settings,
                                       CsonDocumentEditor &editor)
    {
      auto result = Object();

      (void)editor.InsertMember(
          result, "local", MakeFileListSortSettings(settings.local, editor));
      (void)editor.InsertMember(
          result, "remote", MakeFileListSortSettings(settings.remote, editor));

      return result;
    }

    LosslessValue MakeExternalEditorSettings(
        const ExternalEditorSettings &settings,
        CsonDocumentEditor &editor)
    {
      auto result = Object();

      (void)editor.InsertMember(result, "mode",
                                Scalar(ExternalEditorModeName(settings.mode)));
      (void)editor.InsertMember(result, "executable",
                                Scalar(PathToUtf8(settings.executable)));
      (void)editor.InsertMember(result, "arguments", Scalar(settings.arguments));

      return result;
    }

    LosslessValue MakeUpdateSettings(const UpdateSettings &settings,
                                     CsonDocumentEditor &editor)
    {
      auto result = Object();

      (void)editor.InsertMember(result, "checkAutomatically",
                                Scalar(settings.checkAutomatically));
      (void)editor.InsertMember(result, "lastCheckUnixSeconds",
                                Scalar(settings.lastCheckUnixSeconds));
      (void)editor.InsertMember(result, "skippedVersion",
                                Scalar(settings.skippedVersion));

      return result;
    }

    LosslessValue MakeMainWindowState(const MainWindowState &state,
                                      CsonDocumentEditor &editor)
    {
      auto result = Object();

      (void)editor.InsertMember(
          result, "x",
          state.position ? Scalar(static_cast<double>(state.position->x))
                         : Scalar(havCSON::Value{nullptr}));
      (void)editor.InsertMember(
          result, "y",
          state.position ? Scalar(static_cast<double>(state.position->y))
                         : Scalar(havCSON::Value{nullptr}));
      (void)editor.InsertMember(result, "width",
                                Scalar(static_cast<double>(state.width)));
      (void)editor.InsertMember(result, "height",
                                Scalar(static_cast<double>(state.height)));
      (void)editor.InsertMember(result, "maximized", Scalar(state.maximized));

      return result;
    }

    LosslessValue MakeSettings(const AppSettings &settings,
                               CsonDocumentEditor &editor)
    {
      auto result = Object();

      (void)editor.InsertMember(result, "transferConcurrency",
                                Scalar(static_cast<double>(settings.transferConcurrency)));
      (void)editor.InsertMember(
          result, "connectionTimeoutSeconds",
          Scalar(static_cast<double>(settings.connectionTimeoutSeconds)));
      (void)editor.InsertMember(
          result, "commandIdleTimeoutSeconds",
          Scalar(static_cast<double>(settings.commandIdleTimeoutSeconds)));
      (void)editor.InsertMember(
          result, "defaultConflictPolicy",
          Scalar(ConflictPolicyName(settings.defaultConflictPolicy)));
      (void)editor.InsertMember(result, "theme", Scalar(ThemeName(settings.theme)));
      (void)editor.InsertMember(result, "language", Scalar(settings.language));
      (void)editor.InsertMember(result, "fileLists",
                                MakeFileListSettings(settings.fileLists, editor));
      (void)editor.InsertMember(result, "updates",
                                MakeUpdateSettings(settings.updates, editor));
      (void)editor.InsertMember(
          result, "externalEditor",
          MakeExternalEditorSettings(settings.externalEditor, editor));

      return result;
    }

    LosslessValue MakeRememberedConnectionDirectories(
        const RememberedConnectionDirectories &remembered,
        CsonDocumentEditor &editor)
    {
      auto result = Object();

      std::visit(
          [&](const auto &identity)
          {
            using Identity = std::decay_t<decltype(identity)>;
            if constexpr (std::is_same_v<Identity,
                                         SavedSiteWorkspaceIdentity>)
            {
              (void)editor.InsertMember(result, "siteId",
                                        Scalar(identity.siteId));
            }
            else
            {
              (void)editor.InsertMember(
                  result, "protocol",
                  Scalar(ProtocolName(identity.endpoint.protocol)));
              (void)editor.InsertMember(result, "host",
                                        Scalar(identity.endpoint.host));
              (void)editor.InsertMember(
                  result, "port",
                  Scalar(static_cast<double>(identity.endpoint.port)));
              (void)editor.InsertMember(
                  result, "username",
                  Scalar(identity.endpoint.username));
            }
          },
          remembered.connection);
      (void)editor.InsertMember(
          result, "localDirectory",
          Scalar(PathToUtf8(remembered.localDirectory)));
      (void)editor.InsertMember(result, "remoteDirectory",
                                Scalar(remembered.remoteDirectory.DisplayUtf8()));

      return result;
    }

    LosslessValue MakeOpenConnectionTab(
        const OpenConnectionTabState &tab,
        CsonDocumentEditor &editor)
    {
      auto result = Object();

      (void)editor.InsertMember(result, "connectionId",
                                Scalar(tab.connectionId));

      if (tab.connection)
      {
        std::visit(
            [&](const auto &identity)
            {
              using Identity = std::decay_t<decltype(identity)>;
              if constexpr (std::is_same_v<Identity,
                                           SavedSiteWorkspaceIdentity>)
              {
                (void)editor.InsertMember(result, "siteId",
                                          Scalar(identity.siteId));
              }
              else
              {
                (void)editor.InsertMember(
                    result, "protocol",
                    Scalar(ProtocolName(identity.endpoint.protocol)));
                (void)editor.InsertMember(result, "host",
                                          Scalar(identity.endpoint.host));
                (void)editor.InsertMember(
                    result, "port",
                    Scalar(static_cast<double>(identity.endpoint.port)));
                (void)editor.InsertMember(result, "username",
                                          Scalar(identity.endpoint.username));
              }
            },
            *tab.connection);
      }

      (void)editor.InsertMember(result, "localDirectory",
                                Scalar(PathToUtf8(tab.localDirectory)));
      (void)editor.InsertMember(result, "remoteDirectory",
                                Scalar(tab.remoteDirectory.DisplayUtf8()));

      return result;
    }

    LosslessValue MakeWorkspace(const WorkspaceState &workspace,
                                CsonDocumentEditor &editor)
    {
      auto result = Object();

      (void)editor.InsertMember(result, "localDirectory",
                                Scalar(PathToUtf8(workspace.lastLocalDirectory)));

      auto connectionDirectories = Array();

      for (const auto &remembered : workspace.connectionDirectories)
      {
        (void)editor.AppendArrayItem(
            connectionDirectories,
            MakeRememberedConnectionDirectories(remembered, editor));
      }

      (void)editor.InsertMember(result, "connectionDirectories",
                                std::move(connectionDirectories));

      auto openTabs = Array();

      for (const auto &tab : workspace.openTabs)
      {
        (void)editor.AppendArrayItem(
            openTabs, MakeOpenConnectionTab(tab, editor));
      }

      (void)editor.InsertMember(result, "openTabs", std::move(openTabs));
      (void)editor.InsertMember(result, "selectedConnectionId",
                                Scalar(workspace.selectedConnectionId));
      (void)editor.InsertMember(result, "mainWindow",
                                MakeMainWindowState(workspace.mainWindow, editor));

      return result;
    }

    LosslessValue MakeAuthentication(const Authentication &authentication,
                                     CsonDocumentEditor &editor)
    {
      auto result = Object();

      (void)editor.InsertMember(result, "kind",
                                Scalar(AuthenticationName(authentication.kind)));
      (void)editor.InsertMember(result, "credentialId",
                                Scalar(authentication.credentialId));
      (void)editor.InsertMember(result, "privateKeyFile",
                                Scalar(PathToUtf8(authentication.privateKeyFile)));
      (void)editor.InsertMember(result, "publicKeyFile",
                                Scalar(PathToUtf8(authentication.publicKeyFile)));
      (void)editor.InsertMember(result, "passphraseCredentialId",
                                Scalar(authentication.passphraseCredentialId));

      return result;
    }

    LosslessValue MakeSite(const SiteProfile &site, CsonDocumentEditor &editor)
    {
      auto result = Object();

      (void)editor.InsertMember(result, "id", Scalar(site.id));
      (void)editor.InsertMember(result, "name", Scalar(site.name));
      (void)editor.InsertMember(result, "protocol", Scalar(ProtocolName(site.protocol)));
      (void)editor.InsertMember(result, "host", Scalar(site.host));
      (void)editor.InsertMember(result, "port", Scalar(static_cast<double>(site.port)));
      (void)editor.InsertMember(result, "username", Scalar(site.username));
      (void)editor.InsertMember(result, "authentication",
                                MakeAuthentication(site.authentication, editor));
      (void)editor.InsertMember(result, "initialRemoteDirectory",
                                Scalar(site.initialRemoteDirectory.Bytes()));
      (void)editor.InsertMember(result, "initialLocalDirectory",
                                Scalar(PathToUtf8(site.initialLocalDirectory)));
      (void)editor.InsertMember(result, "ftpEncoding", Scalar(site.ftpEncoding));
      (void)editor.InsertMember(
          result, "ftpDataConnectionMode",
          Scalar(FtpDataConnectionModeName(site.ftpDataConnectionMode)));
      (void)editor.InsertMember(result, "ftpActiveAddress",
                                Scalar(site.ftpActiveAddress));

      return result;
    }

    LosslessValue MakeSiteFolder(const SiteFolder &folder,
                                 CsonDocumentEditor &editor)
    {
      auto result = Object();

      (void)editor.InsertMember(result, "id", Scalar(folder.id));
      (void)editor.InsertMember(result, "name", Scalar(folder.name));
      (void)editor.InsertMember(result, "parentId", Scalar(folder.parentId));

      auto siteIds = Array();

      for (const auto &siteId : folder.siteIds)
      {
        (void)editor.AppendArrayItem(siteIds, Scalar(siteId));
      }

      (void)editor.InsertMember(result, "siteIds", std::move(siteIds));

      return result;
    }

    LosslessValue MakeQuickConnectHistoryEntry(
        const QuickConnectHistoryEntry &entry,
        CsonDocumentEditor &editor)
    {
      auto result = Object();

      (void)editor.InsertMember(result, "protocol",
                                Scalar(ProtocolName(entry.protocol)));
      (void)editor.InsertMember(result, "host", Scalar(entry.host));
      (void)editor.InsertMember(result, "port",
                                Scalar(static_cast<double>(entry.port)));
      (void)editor.InsertMember(result, "username", Scalar(entry.username));

      return result;
    }

    LosslessValue MakeTlsTrust(const TlsTrustRecord &record,
                               CsonDocumentEditor &editor)
    {
      auto result = Object();

      (void)editor.InsertMember(result, "host", Scalar(record.host));
      (void)editor.InsertMember(result, "port", Scalar(static_cast<double>(record.port)));
      (void)editor.InsertMember(result, "publicKeyPin", Scalar(record.publicKeyPin));

      return result;
    }

    LosslessValue MakeDocument(const ConfigData &data)
    {
      auto root = Object();

      CsonDocumentEditor editor(root);

      (void)editor.InsertMember(root, "formatVersion",
                                Scalar(static_cast<double>(CurrentFormatVersion)));
      (void)editor.InsertMember(root, "settings", MakeSettings(data.settings, editor));
      (void)editor.InsertMember(root, "workspace",
                                MakeWorkspace(data.workspace, editor));

      auto history = Array();

      for (const auto &entry : data.quickConnectHistory)
      {
        (void)editor.AppendArrayItem(
            history, MakeQuickConnectHistoryEntry(entry, editor));
      }

      (void)editor.InsertMember(root, "quickConnectHistory", std::move(history));

      auto sites = Array();

      for (const auto &site : data.sites)
      {
        (void)editor.AppendArrayItem(sites, MakeSite(site, editor));
      }

      (void)editor.InsertMember(root, "sites", std::move(sites));

      auto siteFolders = Array();

      for (const auto &folder : data.siteFolders)
      {
        (void)editor.AppendArrayItem(siteFolders,
                                     MakeSiteFolder(folder, editor));
      }

      (void)editor.InsertMember(root, "siteFolders", std::move(siteFolders));

      auto siteManagerOrder = Array();

      for (const auto &id : data.siteManagerOrder)
      {
        (void)editor.AppendArrayItem(siteManagerOrder, Scalar(id));
      }

      (void)editor.InsertMember(root, "siteManagerOrder",
                                std::move(siteManagerOrder));

      auto pendingCredentialDeletions = Array();

      for (const auto &id : data.pendingCredentialDeletions)
      {
        (void)editor.AppendArrayItem(pendingCredentialDeletions, Scalar(id));
      }

      (void)editor.InsertMember(root, "pendingCredentialDeletions",
                                std::move(pendingCredentialDeletions));

      auto trust = Array();

      for (const auto &record : data.tlsTrust)
      {
        (void)editor.AppendArrayItem(trust, MakeTlsTrust(record, editor));
      }

      (void)editor.InsertMember(root, "tlsTrust", std::move(trust));

      editor.Synchronize(root);

      return root;
    }

    std::expected<FileListSortSettings, ConfigError> DecodeFileListSortSettings(
        const LosslessValue &value,
        const std::string_view logicalPath,
        const std::filesystem::path &file,
        const bool allowRemoteMetadata)
    {
      if (!value.value.isObject())
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       std::string(logicalPath) + " must be an object"));
      }

      auto columnText = RequiredString(value, "sortColumn", logicalPath, file,
                                       false);
      if (!columnText)
      {
        return std::unexpected(columnText.error());
      }

      const auto column = ParseFileListSortColumn(*columnText);
      if (!column)
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       std::string(logicalPath) + ".sortColumn has an unknown value"));
      }

      if (!allowRemoteMetadata && IsRemoteOnlyFileListSortColumn(*column))
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       std::string(logicalPath) +
                                           ".sortColumn is only available for remote file lists"));
      }

      auto ascending = RequiredBoolean(value, "sortAscending", logicalPath, file);
      if (!ascending)
      {
        return std::unexpected(ascending.error());
      }

      std::vector<std::uint32_t> columnWidths;

      if (const auto *widths = Member(value, "columnWidths"))
      {
        if (!widths->value.isArray())
        {
          return std::unexpected(ErrorAt(value,
                                         ConfigErrorKind::Validation, file,
                                         std::string(logicalPath) + ".columnWidths must be an array"));
        }

        const auto expectedCount = allowRemoteMetadata
                                       ? RemoteFileListColumnCount
                                       : LocalFileListColumnCount;

        if (!widths->arrayItems.empty() &&
            widths->arrayItems.size() != expectedCount)
        {
          return std::unexpected(ErrorAt(value,
                                         ConfigErrorKind::Validation, file,
                                         std::string(logicalPath) + ".columnWidths must be empty or contain " +
                                             std::to_string(expectedCount) + " widths"));
        }

        columnWidths.reserve(widths->arrayItems.size());

        for (std::size_t index = 0; index < widths->arrayItems.size(); ++index)
        {
          auto width = UnsignedIntegerValue(
              widths->arrayItems[index],
              std::string(logicalPath) + ".columnWidths[" +
                  std::to_string(index) + "]",
              MinFileListColumnWidth, MaxFileListColumnWidth, file);

          if (!width)
          {
            return std::unexpected(width.error());
          }

          columnWidths.push_back(*width);
        }
      }

      return FileListSortSettings{
          .sortColumn = *column,
          .sortAscending = *ascending,
          .columnWidths = std::move(columnWidths),
      };
    }

    std::expected<FileListSettings, ConfigError> DecodeFileListSettings(
        const LosslessValue &value,
        const std::filesystem::path &file)
    {
      constexpr std::string_view logicalPath = "settings.fileLists";

      if (!value.value.isObject())
      {
        return std::unexpected(ErrorAt(value, ConfigErrorKind::Validation, file,
                                       "settings.fileLists must be an object"));
      }

      const auto localNode = RequiredMember(value, "local", logicalPath, file);
      if (!localNode)
      {
        return std::unexpected(localNode.error());
      }

      const auto remoteNode = RequiredMember(value, "remote", logicalPath, file);
      if (!remoteNode)
      {
        return std::unexpected(remoteNode.error());
      }

      auto local = DecodeFileListSortSettings(
          **localNode, "settings.fileLists.local", file, false);
      if (!local)
      {
        return std::unexpected(local.error());
      }

      auto remote = DecodeFileListSortSettings(
          **remoteNode, "settings.fileLists.remote", file, true);
      if (!remote)
      {
        return std::unexpected(remote.error());
      }

      return FileListSettings{
          .local = *local,
          .remote = *remote,
      };
    }

    std::expected<ExternalEditorSettings, ConfigError>
    DecodeExternalEditorSettings(const LosslessValue &value,
                                 const std::filesystem::path &file)
    {
      constexpr std::string_view logicalPath = "settings.externalEditor";

      if (!value.value.isObject())
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       "settings.externalEditor must be an object"));
      }

      auto modeText = RequiredString(value, "mode", logicalPath, file, false);
      if (!modeText)
      {
        return std::unexpected(modeText.error());
      }

      const auto mode = ParseExternalEditorMode(*modeText);
      if (!mode)
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       "settings.externalEditor.mode has an unknown value"));
      }

      auto executable = RequiredString(value, "executable", logicalPath, file);
      if (!executable)
      {
        return std::unexpected(executable.error());
      }

      const bool allowEmptyExecutable =
          *mode == ExternalEditorMode::SystemDefault;
      if (!IsSafeExternalEditorExecutable(*executable,
                                          allowEmptyExecutable))
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       "settings.externalEditor.executable must be a safe path" +
                                           std::string(allowEmptyExecutable
                                                           ? ""
                                                           : " and must not be empty")));
      }

      auto arguments = RequiredString(value, "arguments", logicalPath, file,
                                      false);
      if (!arguments)
      {
        return std::unexpected(arguments.error());
      }

      if (!IsSafeExternalEditorArguments(*arguments))
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       "settings.externalEditor.arguments must be safe and contain "
                                       "exactly one {file} placeholder"));
      }

      return ExternalEditorSettings{
          .mode = *mode,
          .executable = PathFromUtf8(*executable),
          .arguments = std::move(*arguments),
      };
    }

    std::expected<AppSettings, ConfigError> DecodeSettings(
        const LosslessValue &value,
        const std::filesystem::path &file)
    {
      if (!value.value.isObject())
      {
        return std::unexpected(ErrorAt(value, ConfigErrorKind::Validation, file,
                                       "settings must be an object"));
      }

      AppSettings settings;

      auto concurrency = RequiredUnsignedInteger(
          value, "transferConcurrency", "settings", MinTransferConcurrency,
          MaxTransferConcurrency, file);
      if (!concurrency)
      {
        return std::unexpected(concurrency.error());
      }
      settings.transferConcurrency = *concurrency;

      auto connectionTimeout = RequiredUnsignedInteger(
          value, "connectionTimeoutSeconds", "settings",
          MinConnectionTimeoutSeconds, MaxConnectionTimeoutSeconds, file);
      if (!connectionTimeout)
      {
        return std::unexpected(connectionTimeout.error());
      }
      settings.connectionTimeoutSeconds = *connectionTimeout;

      auto idleTimeout = RequiredUnsignedInteger(
          value, "commandIdleTimeoutSeconds", "settings",
          MinCommandIdleTimeoutSeconds, MaxCommandIdleTimeoutSeconds, file);
      if (!idleTimeout)
      {
        return std::unexpected(idleTimeout.error());
      }
      settings.commandIdleTimeoutSeconds = *idleTimeout;

      auto policyName = RequiredString(value, "defaultConflictPolicy", "settings",
                                       file, false);
      if (!policyName)
      {
        return std::unexpected(policyName.error());
      }

      const auto policy = ParseConflictPolicy(*policyName);
      if (!policy)
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       "settings.defaultConflictPolicy has an unknown value"));
      }
      settings.defaultConflictPolicy = *policy;

      auto themeText = RequiredString(value, "theme", "settings", file, false);
      if (!themeText)
      {
        return std::unexpected(themeText.error());
      }

      const auto theme = ParseTheme(*themeText);
      if (!theme)
      {
        return std::unexpected(ErrorAt(value, ConfigErrorKind::Validation, file,
                                       "settings.theme has an unknown value"));
      }
      settings.theme = *theme;

      auto language = RequiredString(value, "language", "settings", file, false);
      if (!language)
      {
        return std::unexpected(language.error());
      }

      if (!IsValidLanguageCode(*language))
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       "settings.language must be a safe language code containing only "
                                       "ASCII letters, digits, and single hyphens"));
      }
      settings.language = std::move(*language);

      if (const auto *fileLists = Member(value, "fileLists"))
      {
        auto decodedFileLists = DecodeFileListSettings(*fileLists, file);

        if (!decodedFileLists)
        {
          return std::unexpected(decodedFileLists.error());
        }

        settings.fileLists = std::move(*decodedFileLists);
      }

      const auto updatesNode =
          RequiredMember(value, "updates", "settings", file);
      if (!updatesNode)
      {
        return std::unexpected(updatesNode.error());
      }

      {
        const auto *updates = *updatesNode;
        constexpr std::string_view logicalPath = "settings.updates";

        if (!updates->value.isObject())
        {
          return std::unexpected(ErrorAt(value,
                                         ConfigErrorKind::Validation, file,
                                         "settings.updates must be an object"));
        }

        auto checkAutomatically = RequiredBoolean(
            *updates, "checkAutomatically", logicalPath, file);
        auto lastCheck = RequiredString(
            *updates, "lastCheckUnixSeconds", logicalPath, file);
        auto skippedVersion = RequiredString(
            *updates, "skippedVersion", logicalPath, file);

        if (!checkAutomatically)
        {
          return std::unexpected(checkAutomatically.error());
        }

        if (!lastCheck)
        {
          return std::unexpected(lastCheck.error());
        }

        if (!skippedVersion)
        {
          return std::unexpected(skippedVersion.error());
        }

        if (!lastCheck->empty() &&
            !IsDecimalUnsignedIntegerString(*lastCheck))
        {
          return std::unexpected(ErrorAt(value,
                                         ConfigErrorKind::Validation, file,
                                         "settings.updates.lastCheckUnixSeconds must be an empty or "
                                         "canonical unsigned decimal string"));
        }

        if (!IsCanonicalReleaseVersion(*skippedVersion))
        {
          return std::unexpected(ErrorAt(value,
                                         ConfigErrorKind::Validation, file,
                                         "settings.updates.skippedVersion must be empty or a canonical "
                                         "MAJOR.MINOR.PATCH version"));
        }

        settings.updates = UpdateSettings{
            .checkAutomatically = *checkAutomatically,
            .lastCheckUnixSeconds = std::move(*lastCheck),
            .skippedVersion = std::move(*skippedVersion),
        };
      }

      if (const auto *externalEditor = Member(value, "externalEditor"))
      {
        auto decodedExternalEditor =
            DecodeExternalEditorSettings(*externalEditor, file);
        if (!decodedExternalEditor)
        {
          return std::unexpected(decodedExternalEditor.error());
        }

        settings.externalEditor = std::move(*decodedExternalEditor);
      }

      return settings;
    }

    std::expected<SiteEndpointIdentity, ConfigError> DecodeEndpointIdentity(
        const LosslessValue &value,
        const std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      if (!value.value.isObject())
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       std::string(logicalPath) + " must be an object"));
      }

      auto protocolText =
          RequiredString(value, "protocol", logicalPath, file, false);
      auto host = RequiredString(value, "host", logicalPath, file, false);
      auto port =
          RequiredUnsignedInteger(value, "port", logicalPath, 1, 65535, file);
      auto username = RequiredString(value, "username", logicalPath, file);

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

      const auto protocol = ParseProtocol(*protocolText);
      if (!protocol)
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       std::string(logicalPath) + ".protocol has an unknown value"));
      }

      if (!IsValidEndpointHost(*host))
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       std::string(logicalPath) +
                                           ".host must be a bare DNS name or IP address without a scheme, userinfo, or port"));
      }

      return SiteEndpointIdentity{
          .protocol = *protocol,
          .host = std::move(*host),
          .port = static_cast<std::uint16_t>(*port),
          .username = std::move(*username),
      };
    }

    std::expected<QuickConnectHistoryEntry, ConfigError>
    DecodeQuickConnectHistoryEntry(const LosslessValue &value,
                                   const std::size_t index,
                                   const std::filesystem::path &file)
    {
      return DecodeEndpointIdentity(
          value,
          "quickConnectHistory[" + std::to_string(index) + "]",
          file);
    }

    std::expected<Authentication, ConfigError> DecodeAuthentication(
        const LosslessValue &value,
        std::string_view logicalPath,
        const std::filesystem::path &file)
    {
      if (!value.value.isObject())
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       std::string(logicalPath) + " must be an object"));
      }

      Authentication authentication;

      auto kindName = RequiredString(value, "kind", logicalPath, file, false);
      if (!kindName)
      {
        return std::unexpected(kindName.error());
      }

      const auto kind = ParseAuthentication(*kindName);
      if (!kind)
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       std::string(logicalPath) + ".kind has an unknown value"));
      }

      authentication.kind = *kind;

      auto credentialId = RequiredString(value, "credentialId", logicalPath, file);
      auto privateKey = RequiredString(value, "privateKeyFile", logicalPath, file);
      auto publicKey = RequiredString(value, "publicKeyFile", logicalPath, file);
      auto passphraseId =
          RequiredString(value, "passphraseCredentialId", logicalPath, file);

      if (!credentialId)
      {
        return std::unexpected(credentialId.error());
      }

      if (!privateKey)
      {
        return std::unexpected(privateKey.error());
      }

      if (!publicKey)
      {
        return std::unexpected(publicKey.error());
      }

      if (!passphraseId)
      {
        return std::unexpected(passphraseId.error());
      }

      if (authentication.kind == AuthenticationKind::PrivateKey &&
          privateKey->empty())
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       std::string(logicalPath) +
                                           ".privateKeyFile is required for private-key authentication"));
      }

      authentication.credentialId = std::move(*credentialId);
      authentication.privateKeyFile = PathFromUtf8(*privateKey);
      authentication.publicKeyFile = PathFromUtf8(*publicKey);
      authentication.passphraseCredentialId = std::move(*passphraseId);

      return authentication;
    }

    std::expected<SiteProfile, ConfigError> DecodeSite(
        const LosslessValue &value,
        std::size_t index,
        const std::filesystem::path &file)
    {
      const std::string logicalPath = "sites[" + std::to_string(index) + "]";

      if (!value.value.isObject())
      {
        return std::unexpected(ErrorAt(value, ConfigErrorKind::Validation, file,
                                       logicalPath + " must be an object"));
      }

      SiteProfile site;

      auto id = RequiredString(value, "id", logicalPath, file, false);
      auto name = RequiredString(value, "name", logicalPath, file, false);
      auto protocolText = RequiredString(value, "protocol", logicalPath, file, false);
      auto host = RequiredString(value, "host", logicalPath, file, false);
      auto port = RequiredUnsignedInteger(value, "port", logicalPath, 1, 65535, file);
      auto username = RequiredString(value, "username", logicalPath, file);
      const auto authenticationNode =
          RequiredMember(value, "authentication", logicalPath, file);
      auto remoteDirectory =
          RequiredString(value, "initialRemoteDirectory", logicalPath, file);
      auto localDirectory =
          RequiredString(value, "initialLocalDirectory", logicalPath, file);
      auto ftpEncoding =
          RequiredString(value, "ftpEncoding", logicalPath, file, false);

      if (!id)
      {
        return std::unexpected(id.error());
      }

      if (!name)
      {
        return std::unexpected(name.error());
      }

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

      if (!authenticationNode)
      {
        return std::unexpected(authenticationNode.error());
      }

      if (!remoteDirectory)
      {
        return std::unexpected(remoteDirectory.error());
      }

      if (!localDirectory)
      {
        return std::unexpected(localDirectory.error());
      }

      if (!ftpEncoding)
      {
        return std::unexpected(ftpEncoding.error());
      }

      if (!IsValidEndpointHost(*host))
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       logicalPath +
                                           ".host must be a bare DNS name or IP address without a scheme, userinfo, or port"));
      }

      const auto protocol = ParseProtocol(*protocolText);
      if (!protocol)
      {
        return std::unexpected(ErrorAt(value, ConfigErrorKind::Validation, file,
                                       logicalPath + ".protocol has an unknown value"));
      }

      auto authentication = DecodeAuthentication(
          **authenticationNode, logicalPath + ".authentication", file);
      if (!authentication)
      {
        return std::unexpected(authentication.error());
      }

      if (*protocol != ProtocolKind::Sftp &&
          authentication->kind != AuthenticationKind::Password)
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       logicalPath +
                                           ".authentication.kind must be password for FTP and FTPS sites"));
      }

      auto ftpDataConnectionMode = FtpDataConnectionMode::Passive;

      if (Member(value, "ftpDataConnectionMode") != nullptr)
      {
        auto modeText = RequiredString(
            value, "ftpDataConnectionMode", logicalPath, file, false);
        if (!modeText)
        {
          return std::unexpected(modeText.error());
        }

        const auto mode = ParseFtpDataConnectionMode(*modeText);
        if (!mode)
        {
          return std::unexpected(ErrorAt(value,
                                         ConfigErrorKind::Validation, file,
                                         logicalPath +
                                             ".ftpDataConnectionMode has an unknown value"));
        }

        ftpDataConnectionMode = *mode;
      }

      if (*protocol == ProtocolKind::Sftp &&
          ftpDataConnectionMode != FtpDataConnectionMode::Passive)
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       logicalPath +
                                           ".ftpDataConnectionMode must be passive for SFTP sites"));
      }

      std::string ftpActiveAddress;
      if (Member(value, "ftpActiveAddress") != nullptr)
      {
        auto address = RequiredString(
            value, "ftpActiveAddress", logicalPath, file);
        if (!address)
        {
          return std::unexpected(address.error());
        }

        ftpActiveAddress = std::move(*address);
      }

      if (!ftpActiveAddress.empty() &&
          !IsValidIpAddress(ftpActiveAddress))
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       logicalPath +
                                           ".ftpActiveAddress must be an unbracketed IPv4 or IPv6 address"));
      }

      if (*protocol == ProtocolKind::Sftp &&
          !ftpActiveAddress.empty())
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       logicalPath +
                                           ".ftpActiveAddress must be empty for SFTP sites"));
      }

      site.id = std::move(*id);
      site.name = std::move(*name);
      site.protocol = *protocol;
      site.host = std::move(*host);
      site.port = static_cast<std::uint16_t>(*port);
      site.username = std::move(*username);
      site.authentication = std::move(*authentication);
      site.initialRemoteDirectory = RemotePath(std::move(*remoteDirectory));
      site.initialLocalDirectory = PathFromUtf8(*localDirectory);
      site.ftpEncoding = std::move(*ftpEncoding);
      site.ftpDataConnectionMode = ftpDataConnectionMode;
      site.ftpActiveAddress = std::move(ftpActiveAddress);

      return site;
    }

    std::expected<SiteFolder, ConfigError> DecodeSiteFolder(
        const LosslessValue &value,
        const std::size_t index,
        const std::filesystem::path &file)
    {
      const std::string logicalPath =
          "siteFolders[" + std::to_string(index) + "]";

      if (!value.value.isObject())
      {
        return std::unexpected(ErrorAt(value, ConfigErrorKind::Validation, file,
                                       logicalPath + " must be an object"));
      }

      auto id = RequiredString(value, "id", logicalPath, file, false);
      auto name = RequiredString(value, "name", logicalPath, file, false);
      auto parentId = RequiredString(value, "parentId", logicalPath, file);
      const auto siteIdsNode =
          RequiredMember(value, "siteIds", logicalPath, file);

      if (!id)
      {
        return std::unexpected(id.error());
      }

      if (!name)
      {
        return std::unexpected(name.error());
      }

      if (!parentId)
      {
        return std::unexpected(parentId.error());
      }

      if (!siteIdsNode)
      {
        return std::unexpected(siteIdsNode.error());
      }

      if (!(**siteIdsNode).value.isArray())
      {
        return std::unexpected(ErrorAt(value, ConfigErrorKind::Validation, file,
                                       logicalPath + ".siteIds must be an array"));
      }

      SiteFolder folder{
          .id = std::move(*id),
          .name = std::move(*name),
          .parentId = std::move(*parentId),
          .siteIds = {},
      };
      folder.siteIds.reserve((**siteIdsNode).arrayItems.size());

      for (std::size_t siteIndex = 0; siteIndex < (**siteIdsNode).arrayItems.size(); ++siteIndex)
      {
        auto siteId = StringValue(
            (**siteIdsNode).arrayItems[siteIndex],
            logicalPath + ".siteIds[" + std::to_string(siteIndex) + "]",
            file);
        if (!siteId)
        {
          return std::unexpected(siteId.error());
        }

        if (siteId->empty() || ContainsControlCharacter(*siteId))
        {
          return std::unexpected(ErrorAt(value,
                                         ConfigErrorKind::Validation, file,
                                         logicalPath + ".siteIds[" + std::to_string(siteIndex) +
                                             "] must be a non-empty safe identifier"));
        }

        folder.siteIds.push_back(std::move(*siteId));
      }

      return folder;
    }

    std::optional<std::string> SiteFolderValidationError(
        const std::vector<SiteFolder> &folders,
        const std::vector<SiteProfile> &sites)
    {
      std::unordered_set<std::string> siteIds;
      siteIds.reserve(sites.size());

      for (const auto &site : sites)
      {
        siteIds.insert(site.id);
      }

      std::unordered_map<std::string, std::size_t> folderIndices;
      folderIndices.reserve(folders.size());

      for (std::size_t index = 0; index < folders.size(); ++index)
      {
        const auto &folder = folders[index];

        if (folder.id.empty() || ContainsControlCharacter(folder.id))
        {
          return "A site folder has an invalid stable id";
        }

        if (folder.name.empty() || ContainsControlCharacter(folder.name))
        {
          return "A site folder has an invalid name";
        }

        if (ContainsControlCharacter(folder.parentId))
        {
          return "A site folder has an invalid parent id";
        }

        if (siteIds.contains(folder.id))
        {
          return "Site and folder stable ids must not overlap";
        }

        if (!folderIndices.emplace(folder.id, index).second)
        {
          return "Site folder ids must be unique";
        }
      }

      std::unordered_set<std::string> assignedSiteIds;
      assignedSiteIds.reserve(sites.size());

      for (const auto &folder : folders)
      {
        if (!folder.parentId.empty())
        {
          if (folder.parentId == folder.id)
          {
            return "A site folder cannot be its own parent";
          }

          if (!folderIndices.contains(folder.parentId))
          {
            return "A site folder references a missing parent folder";
          }
        }

        for (const auto &siteId : folder.siteIds)
        {
          if (siteId.empty() || ContainsControlCharacter(siteId) ||
              !siteIds.contains(siteId))
          {
            return "A site folder references a missing or invalid saved site";
          }

          if (!assignedSiteIds.insert(siteId).second)
          {
            return "A saved site cannot belong to more than one folder";
          }
        }
      }

      // Follow parent links rather than recursing through children so even a
      // maliciously deep hierarchy is validated without consuming stack space.
      std::vector<unsigned char> states(folders.size());

      for (std::size_t start = 0; start < folders.size(); ++start)
      {
        if (states[start] == 2U)
        {
          continue;
        }

        std::vector<std::size_t> chain;

        auto current = start;

        while (states[current] == 0U)
        {
          states[current] = 1U;

          chain.push_back(current);

          const auto &parentId = folders[current].parentId;
          if (parentId.empty())
          {
            break;
          }

          current = folderIndices.at(parentId);
        }

        if (states[current] == 1U &&
            std::ranges::find(chain, current) != chain.end() &&
            !folders[current].parentId.empty())
        {
          return "Site folders must not contain a parent cycle";
        }

        for (const auto index : chain)
        {
          states[index] = 2U;
        }
      }

      return std::nullopt;
    }

    std::vector<std::string> LegacySiteManagerOrder(
        const std::vector<SiteFolder> &folders,
        const std::vector<SiteProfile> &sites)
    {
      std::vector<std::string> result;
      result.reserve(folders.size() + sites.size());

      std::unordered_set<std::string_view> assignedSiteIds;
      assignedSiteIds.reserve(sites.size());

      for (const auto &folder : folders)
      {
        assignedSiteIds.insert(folder.siteIds.begin(), folder.siteIds.end());
      }

      const auto appendFolder =
          [&](auto &&self, const SiteFolder &folder) -> void
      {
        result.push_back(folder.id);

        for (const auto &child : folders)
        {
          if (child.parentId == folder.id)
          {
            self(self, child);
          }
        }

        result.insert(result.end(), folder.siteIds.begin(),
                      folder.siteIds.end());
      };

      for (const auto &folder : folders)
      {
        if (folder.parentId.empty())
        {
          appendFolder(appendFolder, folder);
        }
      }

      for (const auto &site : sites)
      {
        if (!assignedSiteIds.contains(site.id))
        {
          result.push_back(site.id);
        }
      }

      return result;
    }

    std::optional<std::string> SiteManagerOrderValidationError(
        const std::vector<std::string> &order,
        const std::vector<SiteFolder> &folders,
        const std::vector<SiteProfile> &sites)
    {
      std::unordered_set<std::string_view> knownIds;
      knownIds.reserve(folders.size() + sites.size());

      for (const auto &folder : folders)
      {
        knownIds.insert(folder.id);
      }

      for (const auto &site : sites)
      {
        knownIds.insert(site.id);
      }

      std::unordered_set<std::string_view> orderedIds;
      orderedIds.reserve(order.size());

      for (const auto &id : order)
      {
        if (id.empty() || ContainsControlCharacter(id))
        {
          return "siteManagerOrder contains an invalid stable id";
        }

        if (!knownIds.contains(id))
        {
          return "siteManagerOrder references an unknown site or folder id '" +
                 id + "'";
        }

        if (!orderedIds.insert(id).second)
        {
          return "siteManagerOrder contains duplicate id '" + id + "'";
        }
      }

      if (orderedIds.size() != knownIds.size())
      {
        return "siteManagerOrder must contain every site and folder id exactly once";
      }

      return std::nullopt;
    }

    std::expected<TlsTrustRecord, ConfigError> DecodeTlsTrust(
        const LosslessValue &value,
        std::size_t index,
        const std::filesystem::path &file)
    {
      const std::string logicalPath = "tlsTrust[" + std::to_string(index) + "]";

      if (!value.value.isObject())
      {
        return std::unexpected(ErrorAt(value, ConfigErrorKind::Validation, file,
                                       logicalPath + " must be an object"));
      }

      auto host = RequiredString(value, "host", logicalPath, file, false);
      auto port = RequiredUnsignedInteger(value, "port", logicalPath, 1, 65535, file);
      auto pin = RequiredString(value, "publicKeyPin", logicalPath, file, false);

      if (!host)
      {
        return std::unexpected(host.error());
      }

      if (!port)
      {
        return std::unexpected(port.error());
      }

      if (!pin)
      {
        return std::unexpected(pin.error());
      }

      if (!IsValidEndpointHost(*host))
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       logicalPath +
                                           ".host must be a bare DNS name or IP address without a scheme, userinfo, or port"));
      }

      return TlsTrustRecord{
          .host = std::move(*host),
          .port = static_cast<std::uint16_t>(*port),
          .publicKeyPin = std::move(*pin),
      };
    }

    std::string EndpointKey(std::string_view host, std::uint16_t port)
    {
      std::string key;
      key.reserve(host.size() + 8);

      for (const unsigned char character : host)
      {
        key.push_back(static_cast<char>(std::tolower(character)));
      }

      key.push_back(':');

      key += std::to_string(port);

      return key;
    }

    std::string QuickConnectHistoryKey(
        const QuickConnectHistoryEntry &entry)
    {
      std::string key = ProtocolName(entry.protocol);

      key.push_back('\0');
      key += EndpointKey(entry.host, entry.port);

      key.push_back('\0');
      key += entry.username;

      return key;
    }

    std::string WorkspaceConnectionKey(
        const WorkspaceConnectionIdentity &connection)
    {
      return std::visit(
          [](const auto &identity)
          {
            using Identity = std::decay_t<decltype(identity)>;
            std::string key;
            if constexpr (std::is_same_v<Identity,
                                         SavedSiteWorkspaceIdentity>)
            {
              key.assign("site", 4U);
              key.push_back('\0');
              key += identity.siteId;
            }
            else
            {
              key.assign("quick", 5U);
              key.push_back('\0');
              key += QuickConnectHistoryKey(identity.endpoint);
            }
            return key;
          },
          connection);
    }

    std::expected<RememberedConnectionDirectories, ConfigError>
    DecodeRememberedConnectionDirectories(
        const LosslessValue &value,
        const std::size_t index,
        const std::filesystem::path &file,
        const bool legacyRemoteDirectories,
        const std::filesystem::path &legacyLocalDirectory)
    {
      const std::string logicalPath =
          std::string{"workspace."} +
          (legacyRemoteDirectories ? "remoteDirectories["
                                   : "connectionDirectories[") +
          std::to_string(index) + "]";

      if (!value.value.isObject())
      {
        return std::unexpected(ErrorAt(value, ConfigErrorKind::Validation, file,
                                       logicalPath + " must be an object"));
      }

      const auto *siteIdNode = Member(value, "siteId");
      const bool hasQuickIdentityMember =
          Member(value, "protocol") != nullptr || Member(value, "host") != nullptr ||
          Member(value, "port") != nullptr || Member(value, "username") != nullptr;

      if (siteIdNode != nullptr && hasQuickIdentityMember)
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       logicalPath +
                                           " must use either siteId or Quick Connect endpoint fields, not both"));
      }

      if (siteIdNode == nullptr && !hasQuickIdentityMember)
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       logicalPath +
                                           " must identify either a saved site or a Quick Connect endpoint"));
      }

      std::filesystem::path localDirectory = legacyLocalDirectory;

      if (!legacyRemoteDirectories)
      {
        auto local = RequiredString(value, "localDirectory", logicalPath,
                                    file, false);

        if (!local)
        {
          return std::unexpected(local.error());
        }

        if (!IsSafeWorkspacePath(*local, false))
        {
          return std::unexpected(ErrorAt(value,
                                         ConfigErrorKind::Validation, file,
                                         logicalPath +
                                             ".localDirectory must not contain control characters"));
        }

        localDirectory = PathFromUtf8(*local);
      }

      const std::string_view remoteDirectoryKey =
          legacyRemoteDirectories ? "directory" : "remoteDirectory";

      auto remoteDirectory = RequiredString(value, remoteDirectoryKey,
                                            logicalPath, file, false);

      if (!remoteDirectory)
      {
        return std::unexpected(remoteDirectory.error());
      }

      if (!IsSafeWorkspacePath(*remoteDirectory, false))
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       logicalPath + "." + std::string{remoteDirectoryKey} +
                                           " must not contain control characters"));
      }

      WorkspaceConnectionIdentity identity;

      if (siteIdNode != nullptr)
      {
        auto siteId = StringValue(*siteIdNode, logicalPath + ".siteId", file);
        if (!siteId)
        {
          return std::unexpected(siteId.error());
        }
        if (siteId->empty() || ContainsControlCharacter(*siteId))
        {
          return std::unexpected(ErrorAt(value,
                                         ConfigErrorKind::Validation, file,
                                         logicalPath + ".siteId must be a non-empty safe identifier"));
        }

        identity = SavedSiteWorkspaceIdentity{std::move(*siteId)};
      }
      else
      {
        auto endpoint = DecodeEndpointIdentity(value, logicalPath, file);
        if (!endpoint)
        {
          return std::unexpected(endpoint.error());
        }

        identity = QuickConnectWorkspaceIdentity{std::move(*endpoint)};
      }

      return RememberedConnectionDirectories{
          .connection = std::move(identity),
          .localDirectory = std::move(localDirectory),
          .remoteDirectory = RemotePath{std::move(*remoteDirectory)},
      };
    }

    std::expected<OpenConnectionTabState, ConfigError>
    DecodeOpenConnectionTab(const LosslessValue &value,
                            const std::size_t index,
                            const std::filesystem::path &file)
    {
      const std::string logicalPath =
          "workspace.openTabs[" + std::to_string(index) + "]";

      if (!value.value.isObject())
      {
        return std::unexpected(ErrorAt(value, ConfigErrorKind::Validation, file,
                                       logicalPath + " must be an object"));
      }

      auto connectionId = RequiredString(
          value, "connectionId", logicalPath, file, false);

      if (!connectionId)
      {
        return std::unexpected(connectionId.error());
      }

      if (connectionId->empty() || ContainsControlCharacter(*connectionId) ||
          connectionId->size() > 256U)
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       logicalPath + ".connectionId must be a safe identifier"));
      }

      const auto *siteIdNode = Member(value, "siteId");
      const bool hasQuickIdentityMember =
          Member(value, "protocol") != nullptr ||
          Member(value, "host") != nullptr ||
          Member(value, "port") != nullptr ||
          Member(value, "username") != nullptr;

      if (siteIdNode != nullptr && hasQuickIdentityMember)
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       logicalPath +
                                           " must use either siteId or Quick Connect endpoint fields, not both"));
      }

      std::optional<WorkspaceConnectionIdentity> identity;

      if (siteIdNode != nullptr)
      {
        auto siteId = StringValue(*siteIdNode, logicalPath + ".siteId", file);
        if (!siteId)
        {
          return std::unexpected(siteId.error());
        }
        if (siteId->empty() || ContainsControlCharacter(*siteId))
        {
          return std::unexpected(ErrorAt(value,
                                         ConfigErrorKind::Validation, file,
                                         logicalPath + ".siteId must be a non-empty safe identifier"));
        }

        identity = SavedSiteWorkspaceIdentity{std::move(*siteId)};
      }
      else if (hasQuickIdentityMember)
      {
        auto endpoint = DecodeEndpointIdentity(value, logicalPath, file);
        if (!endpoint)
        {
          return std::unexpected(endpoint.error());
        }

        identity = QuickConnectWorkspaceIdentity{std::move(*endpoint)};
      }

      auto localDirectory = RequiredString(
          value, "localDirectory", logicalPath, file);
      if (!localDirectory)
      {
        return std::unexpected(localDirectory.error());
      }

      auto remoteDirectory = RequiredString(
          value, "remoteDirectory", logicalPath, file, false);
      if (!remoteDirectory)
      {
        return std::unexpected(remoteDirectory.error());
      }

      if (!IsSafeWorkspacePath(*localDirectory, true) ||
          !IsSafeWorkspacePath(*remoteDirectory, false))
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       logicalPath + " contains an unsafe directory path"));
      }

      return OpenConnectionTabState{
          .connectionId = std::move(*connectionId),
          .connection = std::move(identity),
          .localDirectory = PathFromUtf8(*localDirectory),
          .remoteDirectory = RemotePath{std::move(*remoteDirectory)},
      };
    }

    std::expected<MainWindowState, ConfigError> DecodeMainWindowState(
        const LosslessValue &value,
        const std::filesystem::path &file)
    {
      constexpr std::string_view logicalPath = "workspace.mainWindow";

      if (!value.value.isObject())
      {
        return std::unexpected(ErrorAt(value, ConfigErrorKind::Validation, file,
                                       "workspace.mainWindow must be an object"));
      }

      auto x = RequiredNullableSignedInteger(
          value, "x", logicalPath, MinWindowCoordinate, MaxWindowCoordinate,
          file);
      if (!x)
      {
        return std::unexpected(x.error());
      }

      auto y = RequiredNullableSignedInteger(
          value, "y", logicalPath, MinWindowCoordinate, MaxWindowCoordinate,
          file);
      if (!y)
      {
        return std::unexpected(y.error());
      }

      if (x->has_value() != y->has_value())
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       "workspace.mainWindow.x and workspace.mainWindow.y must either "
                                       "both be integers or both be null"));
      }

      auto width = RequiredUnsignedInteger(
          value, "width", logicalPath, MinWindowWidth, MaxWindowWidth, file);
      if (!width)
      {
        return std::unexpected(width.error());
      }

      auto height = RequiredUnsignedInteger(
          value, "height", logicalPath, MinWindowHeight, MaxWindowHeight, file);
      if (!height)
      {
        return std::unexpected(height.error());
      }

      auto maximized = RequiredBoolean(value, "maximized", logicalPath, file);
      if (!maximized)
      {
        return std::unexpected(maximized.error());
      }

      MainWindowState result{
          .position = std::nullopt,
          .width = *width,
          .height = *height,
          .maximized = *maximized,
      };

      if (x->has_value())
      {
        result.position = WindowPosition{.x = **x, .y = **y};
      }

      return result;
    }

    std::expected<WorkspaceState, ConfigError> DecodeWorkspace(
        const LosslessValue &value,
        const std::filesystem::path &file)
    {
      constexpr std::string_view logicalPath = "workspace";

      if (!value.value.isObject())
      {
        return std::unexpected(ErrorAt(value, ConfigErrorKind::Validation, file,
                                       "workspace must be an object"));
      }

      auto localDirectory =
          RequiredString(value, "localDirectory", logicalPath, file);
      if (!localDirectory)
      {
        return std::unexpected(localDirectory.error());
      }

      if (!IsSafeWorkspacePath(*localDirectory, true))
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       "workspace.localDirectory must not contain control characters"));
      }

      const auto *connectionDirectories = Member(value, "connectionDirectories");
      const auto *legacyRemoteDirectories = Member(value, "remoteDirectories");
      if (connectionDirectories != nullptr && legacyRemoteDirectories != nullptr)
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       "workspace must not contain both connectionDirectories and remoteDirectories"));
      }

      const auto *directories = connectionDirectories != nullptr
                                    ? connectionDirectories
                                    : legacyRemoteDirectories;

      if (directories == nullptr)
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       "workspace.connectionDirectories is required"));
      }

      if (!directories->value.isArray())
      {
        return std::unexpected(ErrorAt(value,
                                       ConfigErrorKind::Validation, file,
                                       std::string{"workspace."} +
                                           (connectionDirectories != nullptr ? "connectionDirectories"
                                                                             : "remoteDirectories") +
                                           " must be an array"));
      }

      WorkspaceState workspace;
      workspace.lastLocalDirectory = PathFromUtf8(*localDirectory);

      if (const auto *mainWindow = Member(value, "mainWindow"))
      {
        auto decodedMainWindow = DecodeMainWindowState(*mainWindow, file);
        if (!decodedMainWindow)
        {
          return std::unexpected(decodedMainWindow.error());
        }

        workspace.mainWindow = std::move(*decodedMainWindow);
      }

      std::unordered_set<std::string> identities;

      for (std::size_t index = 0; index < directories->arrayItems.size(); ++index)
      {
        auto remembered = DecodeRememberedConnectionDirectories(
            directories->arrayItems[index], index, file,
            legacyRemoteDirectories != nullptr,
            workspace.lastLocalDirectory);
        if (!remembered)
        {
          return std::unexpected(remembered.error());
        }

        if (!identities.insert(
                           WorkspaceConnectionKey(remembered->connection))
                 .second)
        {
          return std::unexpected(ErrorAt(value,
                                         ConfigErrorKind::Validation, file,
                                         "workspace connectionDirectories contains a duplicate connection identity"));
        }

        workspace.connectionDirectories.push_back(std::move(*remembered));
      }

      if (const auto *openTabs = Member(value, "openTabs"))
      {
        if (!openTabs->value.isArray())
        {
          return std::unexpected(ErrorAt(value,
                                         ConfigErrorKind::Validation, file,
                                         "workspace.openTabs must be an array"));
        }

        if (openTabs->arrayItems.size() > MaxOpenConnectionTabs)
        {
          return std::unexpected(ErrorAt(value,
                                         ConfigErrorKind::Validation, file,
                                         "workspace.openTabs contains too many tabs"));
        }

        std::unordered_set<std::string> connectionIds;
        for (std::size_t index = 0; index < openTabs->arrayItems.size(); ++index)
        {
          auto tab = DecodeOpenConnectionTab(
              openTabs->arrayItems[index], index, file);
          if (!tab)
          {
            return std::unexpected(tab.error());
          }

          if (!connectionIds.insert(tab->connectionId).second)
          {
            return std::unexpected(ErrorAt(value,
                                           ConfigErrorKind::Validation, file,
                                           "workspace.openTabs contains duplicate connection ids"));
          }

          workspace.openTabs.push_back(std::move(*tab));
        }
      }

      if (const auto *selected = Member(value, "selectedConnectionId"))
      {
        auto decoded = StringValue(
            *selected, "workspace.selectedConnectionId", file);
        if (!decoded)
        {
          return std::unexpected(decoded.error());
        }

        if (ContainsControlCharacter(*decoded) || decoded->size() > 256U)
        {
          return std::unexpected(ErrorAt(value,
                                         ConfigErrorKind::Validation, file,
                                         "workspace.selectedConnectionId must be a safe identifier"));
        }

        workspace.selectedConnectionId = std::move(*decoded);
      }

      return workspace;
    }

    std::expected<ConfigData, ConfigError> DecodeDocument(
        const LosslessValue &document,
        const std::filesystem::path &file)
    {
      if (!document.value.isObject())
      {
        return std::unexpected(ErrorAt(document, ConfigErrorKind::Validation, file,
                                       "The configuration root must be an object"));
      }

      if (const auto forbidden = ForbiddenSecretKey(document))
      {
        return std::unexpected(ErrorAt(document,
                                       ConfigErrorKind::Validation, file,
                                       "Configuration contains forbidden secret-bearing field '" +
                                           *forbidden + "'"));
      }

      auto version = RequiredUnsignedInteger(document, "formatVersion", "root", 0,
                                             std::numeric_limits<std::uint32_t>::max(),
                                             file);
      if (!version)
      {
        return std::unexpected(version.error());
      }
      if (*version != CurrentFormatVersion)
      {
        return std::unexpected(ErrorAt(document,
                                       ConfigErrorKind::UnsupportedVersion, file,
                                       "Configuration format version " + std::to_string(*version) +
                                           " is not supported. Expected version " +
                                           std::to_string(CurrentFormatVersion)));
      }

      const auto settingsNode = RequiredMember(document, "settings", "root", file);
      const auto *workspaceNode = Member(document, "workspace");
      const auto *historyNode = Member(document, "quickConnectHistory");
      const auto sitesNode = RequiredMember(document, "sites", "root", file);
      const auto *siteFoldersNode = Member(document, "siteFolders");
      const auto *siteManagerOrderNode = Member(document, "siteManagerOrder");
      const auto *pendingCredentialDeletionsNode =
          Member(document, "pendingCredentialDeletions");
      const auto trustNode = RequiredMember(document, "tlsTrust", "root", file);

      if (!settingsNode)
      {
        return std::unexpected(settingsNode.error());
      }

      if (!sitesNode)
      {
        return std::unexpected(sitesNode.error());
      }

      if (!trustNode)
      {
        return std::unexpected(trustNode.error());
      }

      auto settings = DecodeSettings(**settingsNode, file);
      if (!settings)
      {
        return std::unexpected(settings.error());
      }

      if (!(**sitesNode).value.isArray())
      {
        return std::unexpected(ErrorAt(document, ConfigErrorKind::Validation, file,
                                       "sites must be an array"));
      }

      if (historyNode != nullptr && !historyNode->value.isArray())
      {
        return std::unexpected(ErrorAt(document, ConfigErrorKind::Validation, file,
                                       "quickConnectHistory must be an array"));
      }

      if (siteFoldersNode != nullptr && !siteFoldersNode->value.isArray())
      {
        return std::unexpected(ErrorAt(document, ConfigErrorKind::Validation, file,
                                       "siteFolders must be an array"));
      }

      if (siteManagerOrderNode != nullptr &&
          !siteManagerOrderNode->value.isArray())
      {
        return std::unexpected(ErrorAt(document, ConfigErrorKind::Validation, file,
                                       "siteManagerOrder must be an array"));
      }

      if (!(**trustNode).value.isArray())
      {
        return std::unexpected(ErrorAt(document, ConfigErrorKind::Validation, file,
                                       "tlsTrust must be an array"));
      }

      if (pendingCredentialDeletionsNode != nullptr &&
          !pendingCredentialDeletionsNode->value.isArray())
      {
        return std::unexpected(ErrorAt(*pendingCredentialDeletionsNode,
                                       ConfigErrorKind::Validation, file,
                                       "pendingCredentialDeletions must be an array"));
      }

      ConfigData result;
      result.formatVersion = *version;
      result.settings = std::move(*settings);

      if (workspaceNode != nullptr)
      {
        auto workspace = DecodeWorkspace(*workspaceNode, file);
        if (!workspace)
        {
          return std::unexpected(workspace.error());
        }

        result.workspace = std::move(*workspace);
      }

      if (historyNode != nullptr)
      {
        if (historyNode->arrayItems.size() > MaxQuickConnectHistoryEntries)
        {
          return std::unexpected(ErrorAt(document,
                                         ConfigErrorKind::Validation, file,
                                         "quickConnectHistory must contain no more than " +
                                             std::to_string(MaxQuickConnectHistoryEntries) + " entries"));
        }

        std::unordered_set<std::string> historyEntries;

        for (std::size_t index = 0; index < historyNode->arrayItems.size(); ++index)
        {
          auto entry = DecodeQuickConnectHistoryEntry(
              historyNode->arrayItems[index], index, file);
          if (!entry)
          {
            return std::unexpected(entry.error());
          }

          if (!historyEntries.insert(QuickConnectHistoryKey(*entry)).second)
          {
            return std::unexpected(ErrorAt(document,
                                           ConfigErrorKind::Validation, file,
                                           "quickConnectHistory contains a duplicate connection"));
          }

          result.quickConnectHistory.push_back(std::move(*entry));
        }
      }

      std::unordered_set<std::string> siteIds;
      for (std::size_t index = 0; index < (**sitesNode).arrayItems.size(); ++index)
      {
        auto site = DecodeSite((**sitesNode).arrayItems[index], index, file);
        if (!site)
        {
          return std::unexpected(site.error());
        }

        if (!siteIds.insert(site->id).second)
        {
          return std::unexpected(ErrorAt(document,
                                         ConfigErrorKind::Validation, file,
                                         "sites contains duplicate stable id '" + site->id + "'"));
        }

        result.sites.push_back(std::move(*site));
      }

      if (siteFoldersNode != nullptr)
      {
        result.siteFolders.reserve(siteFoldersNode->arrayItems.size());

        for (std::size_t index = 0; index < siteFoldersNode->arrayItems.size(); ++index)
        {
          auto folder = DecodeSiteFolder(siteFoldersNode->arrayItems[index],
                                         index, file);
          if (!folder)
          {
            return std::unexpected(folder.error());
          }

          result.siteFolders.push_back(std::move(*folder));
        }
      }

      if (const auto folderError = SiteFolderValidationError(result.siteFolders, result.sites))
      {
        return std::unexpected(ErrorAt(document, ConfigErrorKind::Validation, file,
                                       *folderError));
      }

      if (siteManagerOrderNode == nullptr)
      {
        result.siteManagerOrder = LegacySiteManagerOrder(result.siteFolders, result.sites);
      }
      else
      {
        result.siteManagerOrder.reserve(siteManagerOrderNode->arrayItems.size());

        for (std::size_t index = 0; index < siteManagerOrderNode->arrayItems.size(); ++index)
        {
          auto id = StringValue(
              siteManagerOrderNode->arrayItems[index],
              "siteManagerOrder[" + std::to_string(index) + "]", file);
          if (!id)
          {
            return std::unexpected(id.error());
          }
          result.siteManagerOrder.push_back(std::move(*id));
        }

        if (const auto orderError = SiteManagerOrderValidationError(
                result.siteManagerOrder, result.siteFolders, result.sites))
        {
          return std::unexpected(ErrorAt(document, ConfigErrorKind::Validation, file,
                                         *orderError));
        }
      }

      if (pendingCredentialDeletionsNode != nullptr)
      {
        result.pendingCredentialDeletions.reserve(pendingCredentialDeletionsNode->arrayItems.size());

        std::unordered_set<std::string_view> seenIds;

        for (std::size_t index = 0; index < pendingCredentialDeletionsNode->arrayItems.size(); ++index)
        {
          const auto &node = pendingCredentialDeletionsNode->arrayItems[index];

          auto id = StringValue(
              node, "pendingCredentialDeletions[" + std::to_string(index) + "]",
              file);
          if (!id)
          {
            return std::unexpected(id.error());
          }

          result.pendingCredentialDeletions.push_back(std::move(*id));

          if (const auto error = CredentialDeletionValidationError(
                  result.pendingCredentialDeletions.back(), index, seenIds))
          {
            return std::unexpected(ErrorAt(
                node, ConfigErrorKind::Validation, file, *error));
          }
        }
      }

      std::unordered_set<std::string> endpoints;

      for (std::size_t index = 0; index < (**trustNode).arrayItems.size(); ++index)
      {
        auto trust = DecodeTlsTrust((**trustNode).arrayItems[index], index, file);
        if (!trust)
        {
          return std::unexpected(trust.error());
        }

        if (!endpoints.insert(EndpointKey(trust->host, trust->port)).second)
        {
          return std::unexpected(ErrorAt(document,
                                         ConfigErrorKind::Validation, file,
                                         "tlsTrust contains a duplicate host/port endpoint"));
        }

        result.tlsTrust.push_back(std::move(*trust));
      }

      return result;
    }

    std::expected<void, ConfigError> ValidateTypedData(
        const ConfigData &data,
        const std::filesystem::path &file)
    {
      if (data.formatVersion != CurrentFormatVersion)
      {
        return std::unexpected(Error(
            ConfigErrorKind::UnsupportedVersion,
            file, "Only the current configuration format can be saved"));
      }

      if (data.settings.transferConcurrency < MinTransferConcurrency ||
          data.settings.transferConcurrency > MaxTransferConcurrency ||
          data.settings.connectionTimeoutSeconds < MinConnectionTimeoutSeconds ||
          data.settings.connectionTimeoutSeconds > MaxConnectionTimeoutSeconds ||
          data.settings.commandIdleTimeoutSeconds < MinCommandIdleTimeoutSeconds ||
          data.settings.commandIdleTimeoutSeconds > MaxCommandIdleTimeoutSeconds ||
          ConflictPolicyName(data.settings.defaultConflictPolicy).empty() ||
          ThemeName(data.settings.theme).empty() ||
          !IsValidLanguageCode(data.settings.language) ||
          IsRemoteOnlyFileListSortColumn(
              data.settings.fileLists.local.sortColumn) ||
          FileListSortColumnName(
              data.settings.fileLists.local.sortColumn)
              .empty() ||
          FileListSortColumnName(
              data.settings.fileLists.remote.sortColumn)
              .empty() ||
          (!data.settings.updates.lastCheckUnixSeconds.empty() &&
           !IsDecimalUnsignedIntegerString(
               data.settings.updates.lastCheckUnixSeconds)) ||
          !IsCanonicalReleaseVersion(data.settings.updates.skippedVersion))
      {
        return std::unexpected(Error(ConfigErrorKind::Validation, file,
                                     "Application settings are outside supported ranges"));
      }

      const auto validColumnWidths = [](const auto &widths,
                                        const std::size_t expectedCount)
      {
        return (widths.empty() || widths.size() == expectedCount) &&
               std::ranges::all_of(
                   widths,
                   [](const std::uint32_t width)
                   {
                     return width >= MinFileListColumnWidth &&
                            width <= MaxFileListColumnWidth;
                   });
      };

      if (!validColumnWidths(
              data.settings.fileLists.local.columnWidths,
              LocalFileListColumnCount) ||
          !validColumnWidths(
              data.settings.fileLists.remote.columnWidths,
              RemoteFileListColumnCount))
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation, file,
            "File-list column widths are outside supported ranges"));
      }

      const auto externalEditorExecutable =
          PathToUtf8(data.settings.externalEditor.executable);
      const bool allowEmptyExternalEditorExecutable =
          data.settings.externalEditor.mode ==
          ExternalEditorMode::SystemDefault;

      if (ExternalEditorModeName(data.settings.externalEditor.mode).empty() ||
          !IsSafeExternalEditorExecutable(
              externalEditorExecutable,
              allowEmptyExternalEditorExecutable) ||
          !IsSafeExternalEditorArguments(
              data.settings.externalEditor.arguments))
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation, file,
            "External editor settings are incomplete or unsafe"));
      }

      const auto localDirectory = PathToUtf8(data.workspace.lastLocalDirectory);
      if (!IsSafeWorkspacePath(localDirectory, true))
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation, file,
            "The remembered local directory contains control characters"));
      }

      const auto &mainWindow = data.workspace.mainWindow;
      const bool validWindowPosition =
          !mainWindow.position ||
          (mainWindow.position->x >= MinWindowCoordinate &&
           mainWindow.position->x <= MaxWindowCoordinate &&
           mainWindow.position->y >= MinWindowCoordinate &&
           mainWindow.position->y <= MaxWindowCoordinate);
      if (!validWindowPosition || mainWindow.width < MinWindowWidth ||
          mainWindow.width > MaxWindowWidth ||
          mainWindow.height < MinWindowHeight ||
          mainWindow.height > MaxWindowHeight)
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation, file,
            "The saved main-window placement is outside supported ranges"));
      }

      std::unordered_set<std::string> rememberedConnections;

      for (const auto &remembered : data.workspace.connectionDirectories)
      {
        if (remembered.connection.valueless_by_exception())
        {
          return std::unexpected(Error(
              ConfigErrorKind::Validation, file,
              "Remembered connection directories have no connection identity"));
        }

        const bool validIdentity = std::visit(
            [](const auto &identity)
            {
              using Identity = std::decay_t<decltype(identity)>;
              if constexpr (std::is_same_v<Identity,
                                           SavedSiteWorkspaceIdentity>)
              {
                return !identity.siteId.empty() &&
                       !ContainsControlCharacter(identity.siteId);
              }
              else
              {
                return !ProtocolName(identity.endpoint.protocol).empty() &&
                       IsValidEndpointHost(identity.endpoint.host) &&
                       identity.endpoint.port != 0;
              }
            },
            remembered.connection);

        if (!validIdentity ||
            !IsSafeWorkspacePath(PathToUtf8(remembered.localDirectory),
                                 false) ||
            !IsSafeWorkspacePath(remembered.remoteDirectory.DisplayUtf8(),
                                 false))
        {
          return std::unexpected(Error(
              ConfigErrorKind::Validation, file,
              "Remembered connection directories are incomplete or invalid"));
        }

        if (!rememberedConnections.insert(
                                      WorkspaceConnectionKey(remembered.connection))
                 .second)
        {
          return std::unexpected(Error(
              ConfigErrorKind::Validation, file,
              "Remembered connection directory identities must be unique"));
        }
      }

      if (data.workspace.openTabs.size() > MaxOpenConnectionTabs)
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation, file,
            "The workspace contains too many open connection tabs"));
      }

      std::unordered_set<std::string> openConnectionIds;

      for (const auto &tab : data.workspace.openTabs)
      {
        bool validIdentity = true;

        if (tab.connection)
        {
          if (tab.connection->valueless_by_exception())
          {
            validIdentity = false;
          }
          else
          {
            validIdentity = std::visit(
                [](const auto &identity)
                {
                  using Identity = std::decay_t<decltype(identity)>;
                  if constexpr (std::is_same_v<
                                    Identity,
                                    SavedSiteWorkspaceIdentity>)
                  {
                    return !identity.siteId.empty() &&
                           !ContainsControlCharacter(identity.siteId);
                  }
                  else
                  {
                    return !ProtocolName(identity.endpoint.protocol).empty() &&
                           IsValidEndpointHost(identity.endpoint.host) &&
                           identity.endpoint.port != 0;
                  }
                },
                *tab.connection);
          }
        }

        if (tab.connectionId.empty() || tab.connectionId.size() > 256U ||
            ContainsControlCharacter(tab.connectionId) || !validIdentity ||
            !IsSafeWorkspacePath(PathToUtf8(tab.localDirectory), true) ||
            !IsSafeWorkspacePath(tab.remoteDirectory.DisplayUtf8(), false) ||
            !openConnectionIds.insert(tab.connectionId).second)
        {
          return std::unexpected(Error(
              ConfigErrorKind::Validation, file,
              "An open connection tab is incomplete or invalid"));
        }
      }

      if (!data.workspace.selectedConnectionId.empty() &&
          !openConnectionIds.contains(
              data.workspace.selectedConnectionId))
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation, file,
            "The selected connection tab is not present in workspace.openTabs"));
      }

      if (data.quickConnectHistory.size() > MaxQuickConnectHistoryEntries)
      {
        return std::unexpected(Error(
            ConfigErrorKind::Validation, file,
            "Quick Connect history exceeds the supported entry limit"));
      }

      std::unordered_set<std::string> historyEntries;

      for (const auto &entry : data.quickConnectHistory)
      {
        if (ProtocolName(entry.protocol).empty() ||
            !IsValidEndpointHost(entry.host) || entry.port == 0)
        {
          return std::unexpected(Error(
              ConfigErrorKind::Validation, file,
              "A Quick Connect history entry is incomplete or invalid"));
        }

        if (!historyEntries.insert(QuickConnectHistoryKey(entry)).second)
        {
          return std::unexpected(Error(
              ConfigErrorKind::Validation, file,
              "Quick Connect history entries must be unique"));
        }
      }

      std::unordered_set<std::string> ids;

      for (const auto &site : data.sites)
      {
        if (site.id.empty() || site.name.empty() || !IsValidEndpointHost(site.host) ||
            site.port == 0 ||
            site.ftpEncoding.empty() || ProtocolName(site.protocol).empty() ||
            AuthenticationName(site.authentication.kind).empty() ||
            FtpDataConnectionModeName(site.ftpDataConnectionMode).empty() ||
            (!site.ftpActiveAddress.empty() &&
             !IsValidIpAddress(site.ftpActiveAddress)) ||
            (site.protocol == ProtocolKind::Sftp &&
             (site.ftpDataConnectionMode != FtpDataConnectionMode::Passive ||
              !site.ftpActiveAddress.empty())))
        {
          return std::unexpected(Error(ConfigErrorKind::Validation, file,
                                       "A saved site is incomplete or invalid"));
        }

        if (site.authentication.kind == AuthenticationKind::PrivateKey &&
            site.authentication.privateKeyFile.empty())
        {
          return std::unexpected(Error(
              ConfigErrorKind::Validation, file,
              "Private-key authentication requires a private key file path"));
        }

        if (site.protocol != ProtocolKind::Sftp &&
            site.authentication.kind != AuthenticationKind::Password)
        {
          return std::unexpected(Error(
              ConfigErrorKind::Validation, file,
              "FTP and FTPS sites support password authentication only"));
        }

        if (!ids.insert(site.id).second)
        {
          return std::unexpected(Error(ConfigErrorKind::Validation, file,
                                       "Saved site ids must be unique"));
        }
      }

      if (const auto folderError =
              SiteFolderValidationError(data.siteFolders, data.sites))
      {
        return std::unexpected(Error(ConfigErrorKind::Validation, file,
                                     *folderError));
      }

      if (const auto orderError = SiteManagerOrderValidationError(
              data.siteManagerOrder, data.siteFolders, data.sites))
      {
        return std::unexpected(Error(ConfigErrorKind::Validation, file,
                                     *orderError));
      }

      std::unordered_set<std::string_view> pendingCredentialIds;

      for (std::size_t index = 0; index < data.pendingCredentialDeletions.size(); ++index)
      {
        if (const auto error = CredentialDeletionValidationError(
                data.pendingCredentialDeletions[index], index,
                pendingCredentialIds))
        {
          return std::unexpected(Error(ConfigErrorKind::Validation, file, *error));
        }
      }

      std::unordered_set<std::string> endpoints;

      for (const auto &trust : data.tlsTrust)
      {
        if (!IsValidEndpointHost(trust.host) || trust.port == 0 ||
            trust.publicKeyPin.empty() ||
            !endpoints.insert(EndpointKey(trust.host, trust.port)).second)
        {
          return std::unexpected(Error(ConfigErrorKind::Validation, file,
                                       "TLS trust records must be complete and unique"));
        }
      }

      return {};
    }

    // Persistence compares spelling exactly, unlike connection identity, where
    // host names are case-insensitive. Do not hide a user's host spelling edit.
    bool SameStoredEndpoint(const SiteEndpointIdentity &left,
                            const SiteEndpointIdentity &right)
    {
      return std::tie(left.protocol, left.host, left.port, left.username) ==
             std::tie(right.protocol, right.host, right.port, right.username);
    }

    bool SameStoredIdentity(const WorkspaceConnectionIdentity &left,
                            const WorkspaceConnectionIdentity &right)
    {
      if (left.index() != right.index())
      {
        return false;
      }

      if (const auto *saved = std::get_if<SavedSiteWorkspaceIdentity>(&left))
      {
        return saved->siteId == std::get<SavedSiteWorkspaceIdentity>(right).siteId;
      }

      return SameStoredEndpoint(std::get<QuickConnectWorkspaceIdentity>(left).endpoint,
                                std::get<QuickConnectWorkspaceIdentity>(right).endpoint);
    }

    bool SameStoredDirectories(const RememberedConnectionDirectories &left,
                               const RememberedConnectionDirectories &right)
    {
      return SameStoredIdentity(left.connection, right.connection) &&
             left.localDirectory == right.localDirectory &&
             left.remoteDirectory == right.remoteDirectory;
    }

    bool SameStoredTab(const OpenConnectionTabState &left,
                       const OpenConnectionTabState &right)
    {
      return left.connectionId == right.connectionId &&
             left.connection.has_value() == right.connection.has_value() &&
             (!left.connection || SameStoredIdentity(*left.connection, *right.connection)) &&
             left.localDirectory == right.localDirectory &&
             left.remoteDirectory == right.remoteDirectory;
    }

    bool SameStoredWorkspace(const WorkspaceState &left, const WorkspaceState &right)
    {
      return left.lastLocalDirectory == right.lastLocalDirectory &&
             left.mainWindow == right.mainWindow &&
             left.selectedConnectionId == right.selectedConnectionId &&
             std::ranges::equal(left.connectionDirectories, right.connectionDirectories,
                                SameStoredDirectories) &&
             std::ranges::equal(left.openTabs, right.openTabs, SameStoredTab);
    }

    bool SameStoredSite(const SiteProfile &left, const SiteProfile &right)
    {
      const auto fields = [](const SiteProfile &site)
      {
        return std::tie(site.id, site.name, site.protocol, site.host, site.port,
                        site.username, site.authentication.kind,
                        site.authentication.credentialId, site.authentication.privateKeyFile,
                        site.authentication.publicKeyFile,
                        site.authentication.passphraseCredentialId,
                        site.initialRemoteDirectory, site.initialLocalDirectory,
                        site.ftpEncoding, site.ftpDataConnectionMode, site.ftpActiveAddress);
      };

      return fields(left) == fields(right);
    }

    void UpdateFileListSortSettings(CsonDocumentEditor &editor,
                                    LosslessValue &target,
                                    const FileListSortSettings &settings)
    {
      SetMember(editor, target, "sortColumn",
                Scalar(FileListSortColumnName(settings.sortColumn)));
      SetMember(editor, target, "sortAscending", Scalar(settings.sortAscending));

      auto *widths = editor.FindMember(target, "columnWidths");
      if (widths != nullptr && widths->value.isArray() &&
          widths->arrayItems.size() == settings.columnWidths.size())
      {
        for (std::size_t index = 0; index < settings.columnWidths.size(); ++index)
        {
          const auto width = static_cast<double>(settings.columnWidths[index]);

          if (widths->arrayItems[index].value == havCSON::Value{width})
          {
            continue;
          }

          (void)editor.ReplaceArrayItem(
              *widths,
              index,
              Scalar(width));
        }
      }
      else
      {
        auto replacement = Array();

        for (const auto width : settings.columnWidths)
        {
          (void)editor.AppendArrayItem(
              replacement, Scalar(static_cast<double>(width)));
        }

        SetMember(editor, target, "columnWidths", std::move(replacement));
      }
    }

    void UpdateFileListSettings(CsonDocumentEditor &editor,
                                LosslessValue &target,
                                const FileListSettings &settings)
    {
      auto *local = editor.FindMember(target, "local");
      if (local == nullptr || !local->value.isObject())
      {
        SetMember(editor, target, "local",
                  MakeFileListSortSettings(settings.local, editor));
      }
      else
      {
        UpdateFileListSortSettings(editor, *local, settings.local);
      }

      auto *remote = editor.FindMember(target, "remote");
      if (remote == nullptr || !remote->value.isObject())
      {
        SetMember(editor, target, "remote",
                  MakeFileListSortSettings(settings.remote, editor));
      }
      else
      {
        UpdateFileListSortSettings(editor, *remote, settings.remote);
      }
    }

    void UpdateExternalEditorSettings(
        CsonDocumentEditor &editor,
        LosslessValue &target,
        const ExternalEditorSettings &settings)
    {
      SetMember(editor, target, "mode",
                Scalar(ExternalEditorModeName(settings.mode)));
      SetMember(editor, target, "executable",
                Scalar(PathToUtf8(settings.executable)));
      SetMember(editor, target, "arguments", Scalar(settings.arguments));
    }

    void UpdateUpdateSettings(CsonDocumentEditor &editor,
                              LosslessValue &target,
                              const UpdateSettings &settings)
    {
      SetMember(editor, target, "checkAutomatically",
                Scalar(settings.checkAutomatically));
      SetMember(editor, target, "lastCheckUnixSeconds",
                Scalar(settings.lastCheckUnixSeconds));
      SetMember(editor, target, "skippedVersion",
                Scalar(settings.skippedVersion));
    }

    void UpdateSettings(CsonDocumentEditor &editor,
                        LosslessValue &target,
                        const AppSettings &settings)
    {
      SetMember(editor, target, "transferConcurrency",
                Scalar(static_cast<double>(settings.transferConcurrency)));
      SetMember(editor, target, "connectionTimeoutSeconds",
                Scalar(static_cast<double>(settings.connectionTimeoutSeconds)));
      SetMember(editor, target, "commandIdleTimeoutSeconds",
                Scalar(static_cast<double>(settings.commandIdleTimeoutSeconds)));
      SetMember(editor, target, "defaultConflictPolicy",
                Scalar(ConflictPolicyName(settings.defaultConflictPolicy)));
      SetMember(editor, target, "theme", Scalar(ThemeName(settings.theme)));
      SetMember(editor, target, "language", Scalar(settings.language));

      auto *fileLists = editor.FindMember(target, "fileLists");
      if (fileLists == nullptr || !fileLists->value.isObject())
      {
        SetMember(editor, target, "fileLists",
                  MakeFileListSettings(settings.fileLists, editor));
      }
      else
      {
        UpdateFileListSettings(editor, *fileLists, settings.fileLists);
      }

      auto *updates = editor.FindMember(target, "updates");
      if (updates == nullptr || !updates->value.isObject())
      {
        SetMember(editor, target, "updates",
                  MakeUpdateSettings(settings.updates, editor));
      }
      else
      {
        UpdateUpdateSettings(editor, *updates, settings.updates);
      }

      auto *externalEditor = editor.FindMember(target, "externalEditor");
      if (externalEditor == nullptr || !externalEditor->value.isObject())
      {
        SetMember(editor, target, "externalEditor",
                  MakeExternalEditorSettings(settings.externalEditor,
                                             editor));
      }
      else
      {
        UpdateExternalEditorSettings(editor, *externalEditor,
                                     settings.externalEditor);
      }
    }

    void UpdateQuickConnectHistoryEntry(
        CsonDocumentEditor &editor,
        LosslessValue &target,
        const QuickConnectHistoryEntry &entry)
    {
      SetMember(editor, target, "protocol", Scalar(ProtocolName(entry.protocol)));
      SetMember(editor, target, "host", Scalar(entry.host));
      SetMember(editor, target, "port", Scalar(static_cast<double>(entry.port)));
      SetMember(editor, target, "username", Scalar(entry.username));
    }

    void UpdateAuthentication(CsonDocumentEditor &editor,
                              LosslessValue &target,
                              const Authentication &authentication)
    {
      SetMember(editor, target, "kind", Scalar(AuthenticationName(authentication.kind)));
      SetMember(editor, target, "credentialId", Scalar(authentication.credentialId));
      SetMember(editor, target, "privateKeyFile",
                Scalar(PathToUtf8(authentication.privateKeyFile)));
      SetMember(editor, target, "publicKeyFile",
                Scalar(PathToUtf8(authentication.publicKeyFile)));
      SetMember(editor, target, "passphraseCredentialId",
                Scalar(authentication.passphraseCredentialId));
    }

    void UpdateSite(CsonDocumentEditor &editor,
                    LosslessValue &target,
                    const SiteProfile &site)
    {
      SetMember(editor, target, "id", Scalar(site.id));
      SetMember(editor, target, "name", Scalar(site.name));
      SetMember(editor, target, "protocol", Scalar(ProtocolName(site.protocol)));
      SetMember(editor, target, "host", Scalar(site.host));
      SetMember(editor, target, "port", Scalar(static_cast<double>(site.port)));
      SetMember(editor, target, "username", Scalar(site.username));

      if (auto *authentication = editor.FindMember(target, "authentication");
          authentication != nullptr && authentication->value.isObject())
      {
        UpdateAuthentication(editor, *authentication, site.authentication);
      }
      else
      {
        SetMember(editor, target, "authentication",
                  MakeAuthentication(site.authentication, editor));
      }

      SetMember(editor, target, "initialRemoteDirectory",
                Scalar(site.initialRemoteDirectory.Bytes()));
      SetMember(editor, target, "initialLocalDirectory",
                Scalar(PathToUtf8(site.initialLocalDirectory)));
      SetMember(editor, target, "ftpEncoding", Scalar(site.ftpEncoding));
      SetMember(editor, target, "ftpDataConnectionMode",
                Scalar(FtpDataConnectionModeName(
                    site.ftpDataConnectionMode)));
      SetMember(editor, target, "ftpActiveAddress",
                Scalar(site.ftpActiveAddress));
    }

    void UpdateSiteIdArray(
        CsonDocumentEditor &editor,
        LosslessValue &target,
        const std::vector<std::string> &siteIds,
        std::unordered_map<std::string, LosslessValue> &existingNodes)
    {
      std::vector<LosslessValue> reordered;
      reordered.reserve(siteIds.size());

      for (const auto &siteId : siteIds)
      {
        if (const auto found = existingNodes.find(siteId);
            found != existingNodes.end())
        {
          reordered.push_back(std::move(found->second));
          existingNodes.erase(found);
        }
        else
        {
          reordered.push_back(Scalar(siteId));
        }
      }

      (void)editor.SetArrayItems(target, std::move(reordered));
    }

    void UpdateSiteFolder(
        CsonDocumentEditor &editor,
        LosslessValue &target,
        const SiteFolder &folder,
        std::unordered_map<std::string, LosslessValue> &existingSiteIdNodes)
    {
      SetMember(editor, target, "id", Scalar(folder.id));
      SetMember(editor, target, "name", Scalar(folder.name));
      SetMember(editor, target, "parentId", Scalar(folder.parentId));

      if (auto *siteIds = editor.FindMember(target, "siteIds");
          siteIds != nullptr && siteIds->value.isArray())
      {
        UpdateSiteIdArray(editor, *siteIds, folder.siteIds,
                          existingSiteIdNodes);
      }
      else
      {
        auto replacement = Array();

        UpdateSiteIdArray(editor, replacement, folder.siteIds,
                          existingSiteIdNodes);

        SetMember(editor, target, "siteIds", std::move(replacement));
      }
    }

    void UpdateTrust(CsonDocumentEditor &editor,
                     LosslessValue &target,
                     const TlsTrustRecord &trust)
    {
      SetMember(editor, target, "host", Scalar(trust.host));
      SetMember(editor, target, "port", Scalar(static_cast<double>(trust.port)));
      SetMember(editor, target, "publicKeyPin", Scalar(trust.publicKeyPin));
    }

    std::optional<std::string> ObjectStringMember(const LosslessValue &value,
                                                  std::string_view key)
    {
      const auto *found = Member(value, key);
      if (found == nullptr || !found->value.isString())
      {
        return std::nullopt;
      }

      return std::get<std::string>(found->value);
    }

    std::optional<std::string> QuickConnectHistoryNodeKey(
        const LosslessValue &value)
    {
      const auto protocolText = ObjectStringMember(value, "protocol");
      const auto host = ObjectStringMember(value, "host");
      const auto username = ObjectStringMember(value, "username");
      const auto *portValue = Member(value, "port");

      if (!protocolText || !host || !username || portValue == nullptr)
      {
        return std::nullopt;
      }

      const auto protocol = ParseProtocol(*protocolText);
      const auto port = havCSON::ValueView(portValue->value)
                            .AsInteger<std::uint16_t>(1, 65535);

      if (!protocol || !port)
      {
        return std::nullopt;
      }

      return QuickConnectHistoryKey(QuickConnectHistoryEntry{
          .protocol = *protocol,
          .host = *host,
          .port = *port,
          .username = *username,
      });
    }

    std::optional<std::string> WorkspaceConnectionDirectoryNodeKey(
        const LosslessValue &value)
    {
      if (const auto siteId = ObjectStringMember(value, "siteId"))
      {
        if (siteId->empty() || ContainsControlCharacter(*siteId) ||
            Member(value, "protocol") != nullptr ||
            Member(value, "host") != nullptr ||
            Member(value, "port") != nullptr ||
            Member(value, "username") != nullptr)
        {
          return std::nullopt;
        }

        return WorkspaceConnectionKey(
            SavedSiteWorkspaceIdentity{*siteId});
      }

      const auto endpoint = QuickConnectHistoryNodeKey(value);
      if (!endpoint)
      {
        return std::nullopt;
      }

      std::string key{"quick", 5U};
      key.push_back('\0');
      key += *endpoint;

      return key;
    }

    void UpdateWorkspaceConnectionIdentity(
        CsonDocumentEditor &editor,
        LosslessValue &target,
        const WorkspaceConnectionIdentity &connection)
    {
      std::visit(
          [&](const auto &identity)
          {
            using Identity = std::decay_t<decltype(identity)>;
            if constexpr (std::is_same_v<Identity,
                                         SavedSiteWorkspaceIdentity>)
            {
              if (editor.FindMember(target, "siteId") == nullptr &&
                  editor.FindMember(target, "protocol") != nullptr)
              {
                (void)editor.RenameMember(target, "protocol", "siteId");
              }

              SetMember(editor, target, "siteId", Scalar(identity.siteId));

              for (const std::string_view obsolete : {"protocol", "host", "port", "username"})
              {
                (void)editor.RemoveMember(target, obsolete);
              }
            }
            else
            {
              if (editor.FindMember(target, "protocol") == nullptr &&
                  editor.FindMember(target, "siteId") != nullptr)
              {
                (void)editor.RenameMember(target, "siteId", "protocol");
              }
              else
              {
                (void)editor.RemoveMember(target, "siteId");
              }

              SetMember(editor, target, "protocol",
                        Scalar(ProtocolName(identity.endpoint.protocol)));
              SetMember(editor, target, "host",
                        Scalar(identity.endpoint.host));
              SetMember(editor, target, "port",
                        Scalar(static_cast<double>(identity.endpoint.port)));
              SetMember(editor, target, "username",
                        Scalar(identity.endpoint.username));
            }
          },
          connection);
    }

    void UpdateRememberedConnectionDirectories(
        CsonDocumentEditor &editor,
        LosslessValue &target,
        const RememberedConnectionDirectories &remembered)
    {
      UpdateWorkspaceConnectionIdentity(editor, target, remembered.connection);

      SetMember(editor, target, "localDirectory",
                Scalar(PathToUtf8(remembered.localDirectory)));
      SetMember(editor, target, "remoteDirectory",
                Scalar(remembered.remoteDirectory.DisplayUtf8()));
    }

    std::optional<SiteEndpointIdentity> UniqueSavedSiteEndpoint(
        const std::string_view siteId,
        const std::vector<SiteProfile> &sites)
    {
      const auto site = std::find_if(
          sites.begin(), sites.end(),
          [siteId](const SiteProfile &candidate)
          { return candidate.id == siteId; });

      if (site == sites.end())
      {
        return std::nullopt;
      }

      SiteEndpointIdentity endpoint{
          .protocol = site->protocol,
          .host = site->host,
          .port = site->port,
          .username = site->username,
      };

      const auto endpointIdentity = QuickConnectHistoryKey(endpoint);
      const auto matches = std::ranges::count_if(
          sites, [&](const SiteProfile &candidate)
          { return QuickConnectHistoryKey(SiteEndpointIdentity{
                       .protocol = candidate.protocol,
                       .host = candidate.host,
                       .port = candidate.port,
                       .username = candidate.username,
                   }) == endpointIdentity; });

      return matches == 1 ? std::optional{std::move(endpoint)} : std::nullopt;
    }

    void PromoteUnambiguousWorkspaceQuickConnections(
        CsonDocumentEditor &editor,
        LosslessValue &target,
        const std::vector<RememberedConnectionDirectories> &connectionDirectories,
        const std::vector<SiteProfile> &sites,
        const std::unordered_set<std::string> &wanted)
    {
      for (const auto &remembered : connectionDirectories)
      {
        const auto *saved =
            std::get_if<SavedSiteWorkspaceIdentity>(&remembered.connection);
        if (saved == nullptr)
        {
          continue;
        }

        const auto desiredKey = WorkspaceConnectionKey(remembered.connection);
        if (std::ranges::any_of(
                target.arrayItems, [&](const LosslessValue &value)
                { return WorkspaceConnectionDirectoryNodeKey(value) ==
                         desiredKey; }))
        {
          continue;
        }

        const auto endpoint = UniqueSavedSiteEndpoint(saved->siteId, sites);
        if (!endpoint)
        {
          continue;
        }

        const auto endpointKeyValue = QuickConnectHistoryKey(*endpoint);
        const auto quickConnectionKey = WorkspaceConnectionKey(
            QuickConnectWorkspaceIdentity{*endpoint});

        // When both identities remain desired, they represent two deliberate
        // records rather than an identity transition.
        if (wanted.contains(quickConnectionKey))
        {
          continue;
        }

        auto candidate = target.arrayItems.end();

        std::size_t candidateCount = 0;

        for (auto item = target.arrayItems.begin(); item != target.arrayItems.end(); ++item)
        {
          if (editor.FindMember(*item, "siteId") == nullptr &&
              QuickConnectHistoryNodeKey(*item) == endpointKeyValue)
          {
            candidate = item;
            ++candidateCount;
          }
        }

        if (candidateCount != 1)
        {
          continue;
        }

        UpdateWorkspaceConnectionIdentity(editor, *candidate,
                                          remembered.connection);
      }
    }

    void UpdateWorkspaceConnectionDirectories(
        CsonDocumentEditor &editor,
        LosslessValue &target,
        const std::vector<RememberedConnectionDirectories> &
            connectionDirectories,
        const std::vector<SiteProfile> &sites)
    {
      std::unordered_set<std::string> wanted;

      for (const auto &remembered : connectionDirectories)
      {
        wanted.insert(WorkspaceConnectionKey(remembered.connection));
      }

      PromoteUnambiguousWorkspaceQuickConnections(
          editor, target, connectionDirectories, sites, wanted);

      for (std::size_t index = 0; index < target.arrayItems.size();)
      {
        const auto key =
            WorkspaceConnectionDirectoryNodeKey(target.arrayItems[index]);
        if (!key || !wanted.contains(*key))
        {
          (void)editor.RemoveArrayItem(target, index);
        }
        else
        {
          ++index;
        }
      }

      std::vector<LosslessValue> reordered;

      reordered.reserve(connectionDirectories.size());

      for (const auto &remembered : connectionDirectories)
      {
        const auto connectionKey =
            WorkspaceConnectionKey(remembered.connection);
        const auto found = std::find_if(
            target.arrayItems.begin(), target.arrayItems.end(),
            [&](const LosslessValue &value)
            {
              return WorkspaceConnectionDirectoryNodeKey(value) ==
                     connectionKey;
            });

        if (found == target.arrayItems.end())
        {
          reordered.push_back(
              MakeRememberedConnectionDirectories(remembered, editor));
          continue;
        }

        LosslessValue existing = *found;
        CsonDocumentEditor existingEditor(existing);

        UpdateRememberedConnectionDirectories(existingEditor, existing, remembered);

        reordered.push_back(std::move(existing));
      }

      (void)editor.SetArrayItems(target, std::move(reordered));
    }

    std::optional<std::string> OpenConnectionTabNodeKey(
        const LosslessValue &value)
    {
      const auto connectionId = ObjectStringMember(value, "connectionId");
      return connectionId && !connectionId->empty() &&
                     !ContainsControlCharacter(*connectionId)
                 ? connectionId
                 : std::nullopt;
    }

    void UpdateOpenConnectionTab(CsonDocumentEditor &editor,
                                 LosslessValue &target,
                                 const OpenConnectionTabState &tab)
    {
      SetMember(editor, target, "connectionId", Scalar(tab.connectionId));

      if (tab.connection)
      {
        UpdateWorkspaceConnectionIdentity(editor, target, *tab.connection);
      }
      else
      {
        for (const std::string_view memberName : {"siteId", "protocol", "host", "port", "username"})
        {
          (void)editor.RemoveMember(target, memberName);
        }
      }

      SetMember(editor, target, "localDirectory",
                Scalar(PathToUtf8(tab.localDirectory)));
      SetMember(editor, target, "remoteDirectory",
                Scalar(tab.remoteDirectory.DisplayUtf8()));
    }

    void UpdateOpenConnectionTabs(
        CsonDocumentEditor &editor,
        LosslessValue &target,
        const std::vector<OpenConnectionTabState> &tabs)
    {
      std::unordered_set<std::string> wanted;

      for (const auto &tab : tabs)
      {
        wanted.insert(tab.connectionId);
      }

      for (std::size_t index = 0; index < target.arrayItems.size();)
      {
        const auto key = OpenConnectionTabNodeKey(target.arrayItems[index]);
        if (!key || !wanted.contains(*key))
        {
          (void)editor.RemoveArrayItem(target, index);
        }
        else
        {
          ++index;
        }
      }

      std::vector<LosslessValue> reordered;

      reordered.reserve(tabs.size());

      for (const auto &tab : tabs)
      {
        const auto found = std::ranges::find_if(
            target.arrayItems,
            [&](const LosslessValue &value)
            { return OpenConnectionTabNodeKey(value) == tab.connectionId; });

        if (found == target.arrayItems.end())
        {
          reordered.push_back(MakeOpenConnectionTab(tab, editor));
          continue;
        }

        // Clearing/changing a tab identity removes fields and promotes their
        // comments. Edit in the original document so havCSON knows the final
        // indentation before copying this complete node into the chosen order.
        UpdateOpenConnectionTab(editor, *found, tab);

        reordered.push_back(*found);
      }

      (void)editor.SetArrayItems(target, std::move(reordered));
    }

    void UpdateMainWindowState(CsonDocumentEditor &editor,
                               LosslessValue &target,
                               const MainWindowState &state)
    {
      SetMember(editor, target, "x",
                state.position
                    ? Scalar(static_cast<double>(state.position->x))
                    : Scalar(havCSON::Value{nullptr}));
      SetMember(editor, target, "y",
                state.position
                    ? Scalar(static_cast<double>(state.position->y))
                    : Scalar(havCSON::Value{nullptr}));
      SetMember(editor, target, "width",
                Scalar(static_cast<double>(state.width)));
      SetMember(editor, target, "height",
                Scalar(static_cast<double>(state.height)));
      SetMember(editor, target, "maximized", Scalar(state.maximized));
    }

    void UpdateWorkspace(CsonDocumentEditor &editor,
                         LosslessValue &target,
                         const WorkspaceState &workspace,
                         const std::vector<SiteProfile> &sites,
                         const WorkspaceState *previous = nullptr,
                         const bool updateSchema = true)
    {
      SetMember(editor, target, "localDirectory",
                Scalar(PathToUtf8(workspace.lastLocalDirectory)));

      auto *connectionDirectories =
          editor.FindMember(target, "connectionDirectories");
      if (connectionDirectories == nullptr)
      {
        auto *legacyRemoteDirectories =
            editor.FindMember(target, "remoteDirectories");

        if (legacyRemoteDirectories != nullptr &&
            legacyRemoteDirectories->value.isArray())
        {
          (void)editor.RenameMember(target, "remoteDirectories",
                                    "connectionDirectories");
          connectionDirectories =
              editor.FindMember(target, "connectionDirectories");

          for (auto &item : connectionDirectories->arrayItems)
          {
            if (item.value.isObject() &&
                editor.FindMember(item, "remoteDirectory") == nullptr)
            {
              (void)editor.RenameMember(item, "directory",
                                        "remoteDirectory");
            }
          }
        }
      }

      if (connectionDirectories == nullptr ||
          !connectionDirectories->value.isArray())
      {
        auto replacement = Array();

        for (const auto &remembered : workspace.connectionDirectories)
        {
          (void)editor.AppendArrayItem(
              replacement,
              MakeRememberedConnectionDirectories(remembered, editor));
        }

        SetMember(editor, target, "connectionDirectories",
                  std::move(replacement));
      }
      else if (updateSchema || previous == nullptr ||
               !std::ranges::equal(workspace.connectionDirectories,
                                   previous->connectionDirectories,
                                   SameStoredDirectories))
      {
        UpdateWorkspaceConnectionDirectories(
            editor, *connectionDirectories, workspace.connectionDirectories,
            sites);
      }

      auto *openTabs = editor.FindMember(target, "openTabs");
      if (openTabs == nullptr || !openTabs->value.isArray())
      {
        auto replacement = Array();

        for (const auto &tab : workspace.openTabs)
        {
          (void)editor.AppendArrayItem(
              replacement, MakeOpenConnectionTab(tab, editor));
        }

        SetMember(editor, target, "openTabs", std::move(replacement));
      }
      else if (updateSchema || previous == nullptr ||
               !std::ranges::equal(workspace.openTabs, previous->openTabs, SameStoredTab))
      {
        UpdateOpenConnectionTabs(editor, *openTabs, workspace.openTabs);
      }

      SetMember(editor, target, "selectedConnectionId",
                Scalar(workspace.selectedConnectionId));

      auto *mainWindow = editor.FindMember(target, "mainWindow");
      if (mainWindow == nullptr || !mainWindow->value.isObject())
      {
        SetMember(editor, target, "mainWindow",
                  MakeMainWindowState(workspace.mainWindow, editor));
      }
      else
      {
        UpdateMainWindowState(editor, *mainWindow, workspace.mainWindow);
      }
    }

    void UpdateQuickConnectHistoryArray(
        CsonDocumentEditor &editor,
        LosslessValue &target,
        const std::vector<QuickConnectHistoryEntry> &entries)
    {
      std::unordered_set<std::string> wanted;

      for (const auto &entry : entries)
      {
        wanted.insert(QuickConnectHistoryKey(entry));
      }

      for (std::size_t index = 0; index < target.arrayItems.size();)
      {
        const auto key = QuickConnectHistoryNodeKey(target.arrayItems[index]);
        if (!key || !wanted.contains(*key))
        {
          (void)editor.RemoveArrayItem(target, index);
        }
        else
        {
          ++index;
        }
      }

      std::vector<LosslessValue> reordered;

      reordered.reserve(entries.size());

      for (const auto &entry : entries)
      {
        const std::string entryKey = QuickConnectHistoryKey(entry);

        const auto found = std::find_if(
            target.arrayItems.begin(), target.arrayItems.end(),
            [&](const LosslessValue &value)
            {
              return QuickConnectHistoryNodeKey(value) == entryKey;
            });
        if (found == target.arrayItems.end())
        {
          reordered.push_back(MakeQuickConnectHistoryEntry(entry, editor));

          continue;
        }

        LosslessValue existing = *found;
        CsonDocumentEditor existingEditor(existing);

        UpdateQuickConnectHistoryEntry(existingEditor, existing, entry);

        reordered.push_back(std::move(existing));
      }

      (void)editor.SetArrayItems(target, std::move(reordered));
    }

    void UpdateSiteArray(CsonDocumentEditor &editor,
                         LosslessValue &target,
                         const std::vector<SiteProfile> &sites)
    {
      std::unordered_set<std::string> wanted;

      for (const auto &site : sites)
      {
        wanted.insert(site.id);
      }

      for (std::size_t index = 0; index < target.arrayItems.size();)
      {
        const auto id = ObjectStringMember(target.arrayItems[index], "id");
        if (!id || !wanted.contains(*id))
        {
          (void)editor.RemoveArrayItem(target, index);
        }
        else
        {
          ++index;
        }
      }

      std::vector<LosslessValue> reordered;

      reordered.reserve(sites.size());

      for (const auto &site : sites)
      {
        const auto found = std::find_if(
            target.arrayItems.begin(), target.arrayItems.end(),
            [&site](const LosslessValue &value)
            {
              return ObjectStringMember(value, "id") == site.id;
            });

        if (found == target.arrayItems.end())
        {
          reordered.push_back(MakeSite(site, editor));

          continue;
        }

        LosslessValue existing = *found;
        CsonDocumentEditor existingEditor(existing);

        UpdateSite(existingEditor, existing, site);

        reordered.push_back(std::move(existing));
      }

      (void)editor.SetArrayItems(target, std::move(reordered));
    }

    void UpdateSiteFolderArray(CsonDocumentEditor &editor,
                               LosslessValue &target,
                               const std::vector<SiteFolder> &folders)
    {
      std::unordered_set<std::string> wantedFolderIds;
      std::unordered_set<std::string> wantedSiteIds;

      for (const auto &folder : folders)
      {
        wantedFolderIds.insert(folder.id);
        wantedSiteIds.insert(folder.siteIds.begin(), folder.siteIds.end());
      }

      // Site references can move between folders. Detach every still-wanted
      // scalar node before restructuring the folder array so its comments and
      // formatting travel with the reference instead of being recreated in the
      // destination folder.
      std::unordered_map<std::string, LosslessValue> existingSiteIdNodes;
      existingSiteIdNodes.reserve(wantedSiteIds.size());

      for (auto &folderNode : target.arrayItems)
      {
        auto *siteIds = editor.FindMember(folderNode, "siteIds");
        if (siteIds == nullptr || !siteIds->value.isArray())
        {
          continue;
        }

        for (std::size_t index = 0; index < siteIds->arrayItems.size();)
        {
          const auto siteId = StringValue(
              siteIds->arrayItems[index], "siteFolders.siteIds", {});

          if (siteId && wantedSiteIds.contains(*siteId) &&
              !existingSiteIdNodes.contains(*siteId))
          {
            auto detached = editor.TakeArrayItem(*siteIds, index);
            if (!detached)
            {
              throw std::logic_error("Could not detach a validated site reference");
            }

            existingSiteIdNodes.emplace(*siteId, std::move(*detached));

            continue;
          }

          (void)editor.RemoveArrayItem(*siteIds, index);
        }

        editor.Synchronize(*siteIds);
      }

      for (std::size_t index = 0; index < target.arrayItems.size();)
      {
        const auto id = ObjectStringMember(target.arrayItems[index], "id");
        if (!id || !wantedFolderIds.contains(*id))
        {
          (void)editor.RemoveArrayItem(target, index);
        }
        else
        {
          ++index;
        }
      }

      std::vector<LosslessValue> reordered;

      reordered.reserve(folders.size());

      for (const auto &folder : folders)
      {
        const auto found = std::find_if(
            target.arrayItems.begin(), target.arrayItems.end(),
            [&folder](const LosslessValue &value)
            {
              return ObjectStringMember(value, "id") == folder.id;
            });
        if (found == target.arrayItems.end())
        {
          auto replacement = MakeSiteFolder(folder, editor);

          CsonDocumentEditor replacementEditor(replacement);

          if (auto *siteIds = editor.FindMember(replacement, "siteIds"))
          {
            UpdateSiteIdArray(replacementEditor, *siteIds, folder.siteIds,
                              existingSiteIdNodes);
          }

          reordered.push_back(std::move(replacement));

          continue;
        }

        LosslessValue existing = *found;
        CsonDocumentEditor existingEditor(existing);

        UpdateSiteFolder(existingEditor, existing, folder, existingSiteIdNodes);

        reordered.push_back(std::move(existing));
      }

      (void)editor.SetArrayItems(target, std::move(reordered));
    }

    void UpdateIdentifierArray(
        CsonDocumentEditor &editor,
        LosslessValue &target,
        const std::vector<std::string> &order)
    {
      std::unordered_set<std::string_view> wantedIds;
      wantedIds.reserve(order.size());

      for (const auto &id : order)
      {
        wantedIds.insert(id);
      }

      for (std::size_t index = 0; index < target.arrayItems.size();)
      {
        const auto &value = target.arrayItems[index].value;
        if (!value.isString() ||
            !wantedIds.contains(std::get<std::string>(value)))
        {
          (void)editor.RemoveArrayItem(target, index);
        }
        else
        {
          ++index;
        }
      }

      std::vector<LosslessValue> reordered;

      reordered.reserve(order.size());

      for (const auto &id : order)
      {
        const auto found = std::find_if(
            target.arrayItems.begin(), target.arrayItems.end(),
            [&id](const LosslessValue &value)
            {
              return value.value.isString() &&
                     std::get<std::string>(value.value) == id;
            });
        if (found == target.arrayItems.end())
        {
          reordered.push_back(Scalar(id));

          continue;
        }

        reordered.push_back(*found);
      }

      (void)editor.SetArrayItems(target, std::move(reordered));
    }

    void UpdateTrustArray(CsonDocumentEditor &editor,
                          LosslessValue &target,
                          const std::vector<TlsTrustRecord> &records)
    {
      std::unordered_set<std::string> wanted;

      for (const auto &record : records)
      {
        wanted.insert(EndpointKey(record.host, record.port));
      }

      auto keyForNode = [](const LosslessValue &value) -> std::optional<std::string>
      {
        const auto host = ObjectStringMember(value, "host");
        const auto *portValue = Member(value, "port");

        if (!host || portValue == nullptr)
        {
          return std::nullopt;
        }

        const auto port = havCSON::ValueView(portValue->value)
                              .AsInteger<std::uint16_t>(1, 65535);
        if (!port)
        {
          return std::nullopt;
        }

        return EndpointKey(*host, *port);
      };

      for (std::size_t index = 0; index < target.arrayItems.size();)
      {
        const auto key = keyForNode(target.arrayItems[index]);
        if (!key || !wanted.contains(*key))
        {
          (void)editor.RemoveArrayItem(target, index);
        }
        else
        {
          ++index;
        }
      }

      std::vector<LosslessValue> reordered;

      reordered.reserve(records.size());

      for (const auto &record : records)
      {
        const std::string recordKey = EndpointKey(record.host, record.port);
        const auto found = std::find_if(
            target.arrayItems.begin(), target.arrayItems.end(),
            [&](const LosslessValue &value)
            { return keyForNode(value) == recordKey; });

        if (found == target.arrayItems.end())
        {
          reordered.push_back(MakeTlsTrust(record, editor));

          continue;
        }

        LosslessValue existing = *found;
        CsonDocumentEditor existingEditor(existing);

        UpdateTrust(existingEditor, existing, record);

        reordered.push_back(std::move(existing));
      }
      (void)editor.SetArrayItems(target, std::move(reordered));
    }

    std::size_t QuickConnectHistoryInsertionPosition(
        const LosslessValue &document)
    {
      const auto following = std::find_if(
          document.objectItems.begin(), document.objectItems.end(),
          [](const auto &item)
          {
            return item.first == "sites" || item.first == "siteFolders" ||
                   item.first == "siteManagerOrder" ||
                   item.first == "tlsTrust";
          });

      return following == document.objectItems.end()
                 ? CsonDocumentEditor::Append
                 : static_cast<std::size_t>(
                       std::distance(document.objectItems.begin(), following));
    }

    std::size_t WorkspaceInsertionPosition(const LosslessValue &document)
    {
      const auto settings = std::find_if(
          document.objectItems.begin(), document.objectItems.end(),
          [](const auto &item)
          { return item.first == "settings"; });
      if (settings != document.objectItems.end())
      {
        return static_cast<std::size_t>(
            std::distance(document.objectItems.begin(), settings) + 1);
      }

      const auto following = std::find_if(
          document.objectItems.begin(), document.objectItems.end(),
          [](const auto &item)
          {
            return item.first == "quickConnectHistory" ||
                   item.first == "sites" || item.first == "siteFolders" ||
                   item.first == "siteManagerOrder" ||
                   item.first == "tlsTrust";
          });

      return following == document.objectItems.end()
                 ? CsonDocumentEditor::Append
                 : static_cast<std::size_t>(
                       std::distance(document.objectItems.begin(), following));
    }

    std::size_t SiteFoldersInsertionPosition(const LosslessValue &document)
    {
      const auto sites = std::find_if(
          document.objectItems.begin(), document.objectItems.end(),
          [](const auto &item)
          { return item.first == "sites"; });
      if (sites != document.objectItems.end())
      {
        return static_cast<std::size_t>(
            std::distance(document.objectItems.begin(), sites) + 1);
      }

      const auto following = std::find_if(
          document.objectItems.begin(), document.objectItems.end(),
          [](const auto &item)
          {
            return item.first == "siteManagerOrder" ||
                   item.first == "tlsTrust";
          });

      return following == document.objectItems.end()
                 ? CsonDocumentEditor::Append
                 : static_cast<std::size_t>(
                       std::distance(document.objectItems.begin(), following));
    }

    std::size_t SiteManagerOrderInsertionPosition(
        const LosslessValue &document)
    {
      const auto siteFolders = std::find_if(
          document.objectItems.begin(), document.objectItems.end(),
          [](const auto &item)
          { return item.first == "siteFolders"; });
      if (siteFolders != document.objectItems.end())
      {
        return static_cast<std::size_t>(
            std::distance(document.objectItems.begin(), siteFolders) + 1);
      }

      const auto sites = std::find_if(
          document.objectItems.begin(), document.objectItems.end(),
          [](const auto &item)
          { return item.first == "sites"; });
      if (sites != document.objectItems.end())
      {
        return static_cast<std::size_t>(
            std::distance(document.objectItems.begin(), sites) + 1);
      }

      const auto trust = std::find_if(
          document.objectItems.begin(), document.objectItems.end(),
          [](const auto &item)
          { return item.first == "tlsTrust"; });

      return trust == document.objectItems.end()
                 ? CsonDocumentEditor::Append
                 : static_cast<std::size_t>(
                       std::distance(document.objectItems.begin(), trust));
    }

    void ApplyData(LosslessValue &document, const ConfigData &data,
                   const ConfigData *previous = nullptr,
                   const bool updateSchema = true)
    {
      CsonDocumentEditor editor(document);

      const bool sitesChanged = previous == nullptr ||
                                !std::ranges::equal(data.sites, previous->sites, SameStoredSite);

      SetMember(editor, document, "formatVersion",
                Scalar(static_cast<double>(CurrentFormatVersion)));

      auto *settings = editor.FindMember(document, "settings");
      if (settings == nullptr || !settings->value.isObject())
      {
        SetMember(editor, document, "settings", MakeSettings(data.settings, editor));
      }
      else if (updateSchema || previous == nullptr || data.settings != previous->settings)
      {
        UpdateSettings(editor, *settings, data.settings);
      }

      auto *workspace = editor.FindMember(document, "workspace");
      if (workspace == nullptr)
      {
        (void)editor.InsertMember(
            document, "workspace", MakeWorkspace(data.workspace, editor),
            WorkspaceInsertionPosition(document));
      }
      else if (!workspace->value.isObject())
      {
        SetMember(editor, document, "workspace",
                  MakeWorkspace(data.workspace, editor));
      }
      else if (updateSchema || previous == nullptr || sitesChanged ||
               !SameStoredWorkspace(data.workspace, previous->workspace))
      {
        UpdateWorkspace(editor, *workspace, data.workspace, data.sites,
                        previous ? &previous->workspace : nullptr, updateSchema);
      }

      auto *history = editor.FindMember(document, "quickConnectHistory");
      if (history == nullptr)
      {
        auto replacement = Array();

        for (const auto &entry : data.quickConnectHistory)
        {
          (void)editor.AppendArrayItem(
              replacement, MakeQuickConnectHistoryEntry(entry, editor));
        }

        (void)editor.InsertMember(
            document, "quickConnectHistory", std::move(replacement),
            QuickConnectHistoryInsertionPosition(document));
      }
      else if (!history->value.isArray())
      {
        auto replacement = Array();

        for (const auto &entry : data.quickConnectHistory)
        {
          (void)editor.AppendArrayItem(
              replacement, MakeQuickConnectHistoryEntry(entry, editor));
        }

        SetMember(editor, document, "quickConnectHistory",
                  std::move(replacement));
      }
      else if (previous == nullptr ||
               !std::ranges::equal(data.quickConnectHistory, previous->quickConnectHistory,
                                   SameStoredEndpoint))
      {
        UpdateQuickConnectHistoryArray(editor, *history,
                                       data.quickConnectHistory);
      }

      auto *sites = editor.FindMember(document, "sites");
      if (sites == nullptr || !sites->value.isArray())
      {
        auto replacement = Array();

        for (const auto &site : data.sites)
        {
          (void)editor.AppendArrayItem(replacement, MakeSite(site, editor));
        }

        SetMember(editor, document, "sites", std::move(replacement));
      }
      else if (updateSchema || sitesChanged)
      {
        UpdateSiteArray(editor, *sites, data.sites);
      }

      auto *siteFolders = editor.FindMember(document, "siteFolders");
      if (siteFolders == nullptr)
      {
        auto replacement = Array();

        for (const auto &folder : data.siteFolders)
        {
          (void)editor.AppendArrayItem(
              replacement, MakeSiteFolder(folder, editor));
        }

        (void)editor.InsertMember(
            document, "siteFolders", std::move(replacement),
            SiteFoldersInsertionPosition(document));
      }
      else if (!siteFolders->value.isArray())
      {
        auto replacement = Array();

        for (const auto &folder : data.siteFolders)
        {
          (void)editor.AppendArrayItem(
              replacement, MakeSiteFolder(folder, editor));
        }

        SetMember(editor, document, "siteFolders", std::move(replacement));
      }
      else if (previous == nullptr || data.siteFolders != previous->siteFolders)
      {
        UpdateSiteFolderArray(editor, *siteFolders, data.siteFolders);
      }

      auto *siteManagerOrder = editor.FindMember(document, "siteManagerOrder");
      if (siteManagerOrder == nullptr)
      {
        auto replacement = Array();

        for (const auto &id : data.siteManagerOrder)
        {
          (void)editor.AppendArrayItem(replacement, Scalar(id));
        }

        (void)editor.InsertMember(
            document, "siteManagerOrder", std::move(replacement),
            SiteManagerOrderInsertionPosition(document));
      }
      else if (!siteManagerOrder->value.isArray())
      {
        auto replacement = Array();

        for (const auto &id : data.siteManagerOrder)
        {
          (void)editor.AppendArrayItem(replacement, Scalar(id));
        }

        SetMember(editor, document, "siteManagerOrder",
                  std::move(replacement));
      }
      else if (previous == nullptr || data.siteManagerOrder != previous->siteManagerOrder)
      {
        UpdateIdentifierArray(editor, *siteManagerOrder, data.siteManagerOrder);
      }

      auto *pendingCredentialDeletions =
          editor.FindMember(document, "pendingCredentialDeletions");
      if (pendingCredentialDeletions == nullptr ||
          !pendingCredentialDeletions->value.isArray())
      {
        auto replacement = Array();

        for (const auto &id : data.pendingCredentialDeletions)
        {
          (void)editor.AppendArrayItem(replacement, Scalar(id));
        }

        if (pendingCredentialDeletions == nullptr)
        {
          const auto following = std::find_if(
              document.objectItems.begin(), document.objectItems.end(),
              [](const auto &item)
              { return item.first == "tlsTrust"; });

          const auto position = following == document.objectItems.end()
                                    ? CsonDocumentEditor::Append
                                    : static_cast<std::size_t>(
                                          std::distance(document.objectItems.begin(), following));

          (void)editor.InsertMember(document, "pendingCredentialDeletions",
                                    std::move(replacement), position);
        }
        else
        {
          SetMember(editor, document, "pendingCredentialDeletions",
                    std::move(replacement));
        }
      }
      else if (previous == nullptr || data.pendingCredentialDeletions !=
                                          previous->pendingCredentialDeletions)
      {
        UpdateIdentifierArray(editor, *pendingCredentialDeletions,
                              data.pendingCredentialDeletions);
      }

      auto *trust = editor.FindMember(document, "tlsTrust");
      if (trust == nullptr || !trust->value.isArray())
      {
        auto replacement = Array();

        for (const auto &record : data.tlsTrust)
        {
          (void)editor.AppendArrayItem(replacement, MakeTlsTrust(record, editor));
        }

        SetMember(editor, document, "tlsTrust", std::move(replacement));
      }
      else if (previous == nullptr || data.tlsTrust != previous->tlsTrust)
      {
        UpdateTrustArray(editor, *trust, data.tlsTrust);
      }
    }

    std::expected<std::uint32_t, ConfigError> DocumentVersion(
        const LosslessValue &document,
        const std::filesystem::path &file)
    {
      if (!document.value.isObject())
      {
        return std::unexpected(Error(ConfigErrorKind::Validation, file,
                                     "The configuration root must be an object"));
      }

      const auto *version = Member(document, "formatVersion");
      if (version == nullptr)
      {
        return 0U;
      }

      return UnsignedIntegerValue(*version, "formatVersion", 0,
                                  std::numeric_limits<std::uint32_t>::max(), file);
    }

    std::expected<void, ConfigError> ValidateDocumentVersion(
        const LosslessValue &document,
        const std::filesystem::path &file)
    {
      auto version = DocumentVersion(document, file);
      if (!version)
      {
        return std::unexpected(version.error());
      }
      if (*version != CurrentFormatVersion)
      {
        return std::unexpected(Error(
            ConfigErrorKind::UnsupportedVersion, file,
            "Configuration format version " + std::to_string(*version) +
                " is not supported. Expected version " +
                std::to_string(CurrentFormatVersion),
            SourceLocation(Member(document, "formatVersion") != nullptr
                               ? Member(document, "formatVersion")->Source()
                               : document.Source())));
      }

      return {};
    }

    std::expected<void, ConfigError> EnsureParentDirectory(
        const std::filesystem::path &path)
    {
      const auto parent = path.parent_path();
      if (parent.empty())
      {
        return {};
      }

      std::error_code ec;
      std::filesystem::create_directories(parent, ec);

      if (ec)
      {
        return std::unexpected(Error(
            ConfigErrorKind::Io, path,
            "Failed to create the configuration directory: " + ec.message()));
      }

      return {};
    }

    std::expected<void, ConfigError> WriteDocument(
        const std::filesystem::path &path,
        const LosslessValue &document)
    {
      auto directory = EnsureParentDirectory(path);
      if (!directory)
      {
        return std::unexpected(directory.error());
      }

      havCSON::WriteOptions options;
      options.indentWidth = 2;
      options.sortObjectKeys = false;

      havCSON::Error writeError;
      {
        std::string serialized;
        if (!havCSON::ToStringLossless(document, serialized, options, &writeError))
        {
          return std::unexpected(Error(
              ConfigErrorKind::AtomicWrite, path,
              DescribeCsonError(writeError, "Failed to serialize configuration")));
        }

        if (serialized.size() > MaxConfigInputBytes)
        {
          return std::unexpected(Error(
              ConfigErrorKind::Validation, path,
              "Configuration exceeds the 64 MiB file size limit"));
        }
      }

      if (!havCSON::WriteFileLosslessAtomic(PathToUtf8(path), document, options,
                                            &writeError))
      {
        return std::unexpected(Error(
            ConfigErrorKind::AtomicWrite, path,
            DescribeCsonError(writeError, "Failed to atomically save configuration")));
      }

      return {};
    }

    std::expected<bool, ConfigError> FileExists(const std::filesystem::path &path)
    {
      std::error_code ec;
      const auto status = std::filesystem::status(path, ec);

      if (ec && ec != std::errc::no_such_file_or_directory)
      {
        return std::unexpected(Error(
            ConfigErrorKind::Io, path,
            "Failed to inspect the configuration file: " + ec.message()));
      }

      return !ec && std::filesystem::exists(status);
    }

  } // namespace

  struct CsonConfigRepository::Impl final
  {
    explicit Impl(std::shared_ptr<const IConfigPathProvider> provider)
        : pathProvider(std::move(provider)) {}

    std::shared_ptr<const IConfigPathProvider> pathProvider;
    mutable std::mutex mutex;
    std::optional<LosslessValue> activeDocument;
    // A loaded file can omit optional fields that DecodeDocument defaults.
    // Complete them once. Only a successful save advances this state.
    bool needsSchemaUpdate{true};

    std::expected<std::filesystem::path, ConfigError> Path() const
    {
      if (!pathProvider)
      {
        return std::unexpected(Error(ConfigErrorKind::PathUnavailable, {},
                                     "No configuration path provider is installed"));
      }

      return pathProvider->ConfigPath();
    }

    std::expected<ConfigLoadResult, ConfigError> LoadUnlocked(
        const std::filesystem::path &file)
    {
      auto exists = FileExists(file);
      if (!exists)
      {
        return std::unexpected(exists.error());
      }
      if (!*exists)
      {
        ConfigData defaults;
        auto document = MakeDocument(defaults);
        auto written = WriteDocument(file, document);
        if (!written)
        {
          return std::unexpected(written.error());
        }

        activeDocument = std::move(document);

        needsSchemaUpdate = false;

        return ConfigLoadResult{
            .data = std::move(defaults),
            .createdDefaults = true,
        };
      }

      LosslessValue parsed;
      havCSON::Error sourceError;
      havCSON::ParseOptions parseOptions;
      parseOptions.trackSourceLocations = true;
      parseOptions.maxInputBytes = MaxConfigInputBytes;

      if (havCSON::ParseFileLossless(PathToUtf8(file), parsed, &sourceError,
                                     parseOptions) !=
          havCSON::ErrorCode::OK)
      {
        return std::unexpected(ParseError(file, sourceError));
      }

      auto validVersion = ValidateDocumentVersion(parsed, file);
      if (!validVersion)
      {
        return std::unexpected(validVersion.error());
      }

      auto decoded = DecodeDocument(parsed, file);
      if (!decoded)
      {
        return std::unexpected(WithSource(decoded.error(), parsed));
      }

      // Commit only after parse, version checking, and validation succeed
      activeDocument = std::move(parsed);

      needsSchemaUpdate = true;

      return ConfigLoadResult{
          .data = std::move(*decoded),
          .createdDefaults = false,
      };
    }
  };

  CsonConfigRepository::CsonConfigRepository(
      std::shared_ptr<const IConfigPathProvider> pathProvider)
      : mImpl(std::make_unique<Impl>(std::move(pathProvider))) {}

  CsonConfigRepository::CsonConfigRepository(std::filesystem::path path)
      : CsonConfigRepository(
            std::make_shared<FixedConfigPathProvider>(std::move(path))) {}

  CsonConfigRepository::~CsonConfigRepository() = default;
  CsonConfigRepository::CsonConfigRepository(CsonConfigRepository &&) noexcept = default;
  CsonConfigRepository &CsonConfigRepository::operator=(
      CsonConfigRepository &&) noexcept = default;

  std::expected<ConfigLoadResult, ConfigError> CsonConfigRepository::Load()
  {
    std::scoped_lock lock(mImpl->mutex);

    auto file = mImpl->Path();
    if (!file)
    {
      return std::unexpected(file.error());
    }

    return mImpl->LoadUnlocked(*file);
  }

  std::expected<void, ConfigError> CsonConfigRepository::Save(
      const ConfigData &data)
  {
    std::scoped_lock lock(mImpl->mutex);

    auto file = mImpl->Path();
    if (!file)
    {
      return std::unexpected(file.error());
    }

    auto valid = ValidateTypedData(data, *file);
    if (!valid)
    {
      return std::unexpected(valid.error());
    }

    if (!mImpl->activeDocument)
    {
      auto exists = FileExists(*file);
      if (!exists)
      {
        return std::unexpected(exists.error());
      }
      if (*exists)
      {
        auto loaded = mImpl->LoadUnlocked(*file);
        if (!loaded)
        {
          return std::unexpected(loaded.error());
        }
      }
      else
      {
        mImpl->activeDocument = MakeDocument(ConfigData{});
      }
    }

    // Compare against the committed document, never against a failed candidate
    const auto previous = DecodeDocument(*mImpl->activeDocument, *file);
    if (!previous)
    {
      return std::unexpected(previous.error());
    }

    LosslessValue candidate = *mImpl->activeDocument;

    ApplyData(candidate, data, &*previous, mImpl->needsSchemaUpdate);

    auto decoded = DecodeDocument(candidate, *file);
    if (!decoded)
    {
      return std::unexpected(decoded.error());
    }

    auto written = WriteDocument(*file, candidate);
    if (!written)
    {
      return std::unexpected(written.error());
    }

    mImpl->activeDocument = std::move(candidate);
    mImpl->needsSchemaUpdate = false;

    return {};
  }

  std::expected<std::filesystem::path, ConfigError>
  CsonConfigRepository::ConfigPath() const
  {
    std::scoped_lock lock(mImpl->mutex);
    return mImpl->Path();
  }
} // namespace havremote::config
