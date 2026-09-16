// SPDX-License-Identifier: MIT

#include "localization/translationCatalog.hpp"

#include <havCSON.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace havremote::localization
{
  namespace
  {
    constexpr std::uint32_t CatalogFormatVersion = 1;
    constexpr std::size_t MaximumLanguageCodeLength = 35;
    constexpr std::size_t MaximumCatalogBytes = 4U * 1024U * 1024U;

    struct ParsedLanguage final
    {
      LanguageInfo info;
      std::filesystem::path sourcePath;
      std::unordered_map<std::string, std::string> strings;
    };

    TranslationError MakeError(const TranslationErrorKind kind,
                               std::filesystem::path path,
                               std::string message,
                               const std::optional<std::size_t> line = std::nullopt,
                               const std::optional<std::size_t> column = std::nullopt)
    {
      return TranslationError{
          .kind = kind,
          .path = std::move(path),
          .message = std::move(message),
          .line = line,
          .column = column,
      };
    }

    std::string CanonicalLanguageCode(const std::string_view code)
    {
      std::string result;
      result.reserve(code.size());

      for (const unsigned char character : code)
      {
        result.push_back(static_cast<char>(
            character >= 'A' && character <= 'Z' ? character + ('a' - 'A')
                                                 : character));
      }

      return result;
    }

    bool IsAsciiAlphanumeric(const unsigned char character) noexcept
    {
      return (character >= 'a' && character <= 'z') ||
             (character >= 'A' && character <= 'Z') ||
             (character >= '0' && character <= '9');
    }

    bool IsValidLanguageCode(const std::string_view code) noexcept
    {
      if (code.empty() || code.size() > MaximumLanguageCodeLength ||
          !IsAsciiAlphanumeric(static_cast<unsigned char>(code.front())) ||
          !IsAsciiAlphanumeric(static_cast<unsigned char>(code.back())))
      {
        return false;
      }

      bool previousWasHyphen = false;

      for (const unsigned char character : code)
      {
        if (IsAsciiAlphanumeric(character))
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

    bool IsValidTranslationKey(const std::string_view key) noexcept
    {
      bool atSegmentStart = true;

      for (const unsigned char character : key)
      {
        if (character == '.')
        {
          if (atSegmentStart)
          {
            return false;
          }

          atSegmentStart = true;

          continue;
        }

        if (atSegmentStart ? !(character >= 'a' && character <= 'z')
                           : !IsAsciiAlphanumeric(character))
        {
          return false;
        }

        atSegmentStart = false;
      }

      return !atSegmentStart;
    }

    std::expected<std::map<std::size_t, std::size_t>, std::string>
    PlaceholderCounts(const std::string_view pattern)
    {
      std::map<std::size_t, std::size_t> counts;

      for (std::size_t cursor = 0; cursor < pattern.size();)
      {
        if (pattern[cursor] == '}')
        {
          if (cursor + 1 < pattern.size() && pattern[cursor + 1] == '}')
          {
            cursor += 2;

            continue;
          }

          return std::unexpected("contains an unmatched '}'");
        }

        if (pattern[cursor] != '{')
        {
          ++cursor;

          continue;
        }

        if (cursor + 1 < pattern.size() && pattern[cursor + 1] == '{')
        {
          cursor += 2;

          continue;
        }

        const auto closing = pattern.find('}', cursor + 1);
        if (closing == std::string_view::npos)
        {
          return std::unexpected("contains an unmatched '{'");
        }

        const auto indexText = pattern.substr(cursor + 1, closing - cursor - 1);
        if (indexText.empty() ||
            !std::ranges::all_of(indexText, [](const unsigned char character)
                                 { return character >= '0' && character <= '9'; }))
        {
          return std::unexpected("contains a placeholder other than {0}, {1}, ...");
        }

        std::size_t index = 0;

        for (const unsigned char digit : indexText)
        {
          if (index > (std::numeric_limits<std::size_t>::max() - (digit - '0')) / 10)
          {
            return std::unexpected("contains a placeholder index that is too large");
          }

          index = index * 10 + (digit - '0');
        }

        ++counts[index];

        cursor = closing + 1;
      }

      if (!counts.empty())
      {
        std::size_t expectedIndex = 0;

        for (const auto &[index, count] : counts)
        {
          (void)count;

          if (index != expectedIndex)
          {
            return std::unexpected("has non-contiguous placeholder indexes");
          }

          ++expectedIndex;
        }
      }

      return counts;
    }

    TranslationError AccessFailure(const havCSON::AccessError &error,
                                   const std::filesystem::path &path)
    {
      return MakeError(TranslationErrorKind::Validation, path, error.message,
                       error.source ? std::optional{error.source->valueSpan.begin.line}
                                    : std::nullopt,
                       error.source ? std::optional{error.source->valueSpan.begin.column}
                                    : std::nullopt);
    }

    TranslationError ValidationFailure(
        const havCSON::ValueView &value,
        const std::filesystem::path &path,
        std::string message)
    {
      const auto node = value.Get();
      const auto *source = node ? node->get().Source() : nullptr;

      return MakeError(TranslationErrorKind::Validation, path, std::move(message),
                       source ? std::optional{source->valueSpan.begin.line} : std::nullopt,
                       source ? std::optional{source->valueSpan.begin.column} : std::nullopt);
    }

    std::expected<const havCSON::Object *, TranslationError> RequireObject(
        const havCSON::ValueView &value,
        const std::filesystem::path &path)
    {
      const auto object = value.AsObject();

      if (!object)
      {
        return std::unexpected(AccessFailure(object.error(), path));
      }

      return &object->get();
    }

    std::expected<void, TranslationError> RequireExactMembers(
        const havCSON::ValueView &view,
        const havCSON::Object &object,
        const std::span<const std::string_view> expected,
        const std::filesystem::path &path,
        const std::string_view logicalPath)
    {
      for (const auto &[key, value] : object)
      {
        (void)value;

        if (std::ranges::find(expected, key) == expected.end())
        {
          return std::unexpected(ValidationFailure(
              view.Member(key), path,
              std::string(logicalPath) + " contains unknown member '" + key + "'"));
        }
      }

      return {};
    }

    std::expected<std::string, TranslationError> RequireString(
        const havCSON::ValueView &view,
        const std::filesystem::path &path,
        const bool allowEmpty = false)
    {
      const auto text = view.AsString();
      if (!text)
      {
        return std::unexpected(AccessFailure(text.error(), path));
      }

      auto result = text->get();
      if (!allowEmpty && result.empty())
      {
        return std::unexpected(ValidationFailure(
            view, path, havCSON::FormatValuePath(view.Path()) + " must not be empty"));
      }

      return result;
    }

    std::expected<ParsedLanguage, TranslationError> ParseCatalog(
        const std::filesystem::path &path)
    {
      havCSON::Value document;
      havCSON::Error parseFailure;

      const auto utf8Path = path.generic_u8string();

      const havCSON::ParseOptions options{
          .trackSourceLocations = true,
          .maxDepth = 256,
          .maxInputBytes = MaximumCatalogBytes,
      };

      if (havCSON::ParseFile(std::string{utf8Path.begin(), utf8Path.end()},
                             document, &parseFailure, options) != havCSON::ErrorCode::OK)
      {
        const bool ioFailure = parseFailure.code == havCSON::ErrorCode::IoError;

        return std::unexpected(MakeError(
            ioFailure ? TranslationErrorKind::Io : TranslationErrorKind::Parse,
            path,
            parseFailure.message.empty() ? "Could not parse translation catalog"
                                         : parseFailure.message,
            ioFailure ? std::nullopt : std::optional{parseFailure.where.line},
            ioFailure ? std::nullopt : std::optional{parseFailure.where.column}));
      }

      const havCSON::ValueView rootView(document);
      const auto root = RequireObject(rootView, path);
      if (!root)
      {
        return std::unexpected(root.error());
      }

      constexpr std::array rootMembers{
          std::string_view{"formatVersion"},
          std::string_view{"language"},
          std::string_view{"strings"},
      };

      if (auto valid = RequireExactMembers(rootView, **root, rootMembers, path, "root"); !valid)
      {
        return std::unexpected(valid.error());
      }

      const auto versionView = rootView.Member("formatVersion");
      const auto version = versionView.AsInteger<std::uint32_t>();
      if (!version)
      {
        return std::unexpected(AccessFailure(version.error(), path));
      }
      if (*version != CatalogFormatVersion)
      {
        return std::unexpected(ValidationFailure(
            versionView, path,
            "Unsupported translation catalog formatVersion. Expected " +
                std::to_string(CatalogFormatVersion)));
      }

      const auto languageView = rootView.Member("language");
      const auto language = RequireObject(languageView, path);
      if (!language)
      {
        return std::unexpected(language.error());
      }

      constexpr std::array languageMembers{
          std::string_view{"code"},
          std::string_view{"displayName"},
      };
      if (auto valid = RequireExactMembers(languageView, **language, languageMembers, path,
                                           "root.language");
          !valid)
      {
        return std::unexpected(valid.error());
      }

      auto code = RequireString(languageView.Member("code"), path);
      if (!code)
      {
        return std::unexpected(code.error());
      }
      if (!IsValidLanguageCode(*code))
      {
        return std::unexpected(ValidationFailure(
            languageView.Member("code"), path,
            "root.language.code must be 1-35 ASCII alphanumeric/hyphen characters, "
            "start and end with an alphanumeric character, and contain no consecutive hyphens"));
      }

      auto displayName = RequireString(languageView.Member("displayName"), path);
      if (!displayName)
      {
        return std::unexpected(displayName.error());
      }
      if (std::ranges::any_of(*displayName, [](const unsigned char character)
                              { return character < 0x20U || character == 0x7fU; }))
      {
        return std::unexpected(ValidationFailure(
            languageView.Member("displayName"), path,
            "root.language.displayName must not contain control characters"));
      }

      const auto stringsView = rootView.Member("strings");
      const auto strings = RequireObject(stringsView, path);
      if (!strings)
      {
        return std::unexpected(strings.error());
      }
      if ((**strings).empty())
      {
        return std::unexpected(ValidationFailure(
            stringsView, path,
            "root.strings must contain at least one translation"));
      }

      ParsedLanguage parsed{
          .info = LanguageInfo{.code = std::move(*code),
                               .displayName = std::move(*displayName)},
          .sourcePath = path,
          .strings = {},
      };

      parsed.strings.reserve((**strings).size());

      for (const auto &[key, value] : **strings)
      {
        if (!IsValidTranslationKey(key))
        {
          return std::unexpected(ValidationFailure(
              stringsView.Member(key), path,
              "root.strings contains invalid key '" + key + "'"));
        }

        const auto text = RequireString(stringsView.Member(key), path);
        if (!text)
        {
          return std::unexpected(text.error());
        }

        auto translation = *text;

        if (const auto placeholders = PlaceholderCounts(translation); !placeholders)
        {
          return std::unexpected(ValidationFailure(
              stringsView.Member(key), path,
              "root.strings." + key + ' ' + placeholders.error()));
        }

        parsed.strings.emplace(key, std::move(translation));
      }

      return parsed;
    }

    std::expected<std::vector<std::filesystem::path>, TranslationError> CatalogPaths(
        const std::filesystem::path &directory)
    {
      std::error_code filesystemError;

      if (!std::filesystem::is_directory(directory, filesystemError))
      {
        return std::unexpected(MakeError(
            TranslationErrorKind::Io,
            directory,
            filesystemError ? "Could not inspect translations directory: " +
                                  filesystemError.message()
                            : "Translations directory does not exist"));
      }

      std::vector<std::filesystem::path> result;
      std::filesystem::directory_iterator iterator{directory, filesystemError};

      const std::filesystem::directory_iterator end;

      while (!filesystemError && iterator != end)
      {
        std::error_code entryError;

        if (iterator->is_regular_file(entryError) &&
            iterator->path().extension() == ".cson")
        {
          result.push_back(iterator->path());
        }

        if (entryError)
        {
          return std::unexpected(MakeError(
              TranslationErrorKind::Io,
              iterator->path(),
              "Could not inspect translation catalog: " + entryError.message()));
        }

        iterator.increment(filesystemError);
      }

      if (filesystemError)
      {
        return std::unexpected(MakeError(
            TranslationErrorKind::Io,
            directory,
            "Could not enumerate translations directory: " + filesystemError.message()));
      }

      if (result.empty())
      {
        return std::unexpected(MakeError(
            TranslationErrorKind::Validation,
            directory,
            "Translations directory contains no .cson catalogs"));
      }

      std::ranges::sort(result);

      return result;
    }

  } // namespace

  struct TranslationCatalog::Storage final
  {
    std::vector<LanguageInfo> languageInfo;
    std::vector<std::unordered_map<std::string, std::string>> translations;
    std::size_t defaultLanguage{};
  };

  TranslationCatalog::TranslationCatalog(std::shared_ptr<const Storage> storage,
                                         const std::size_t selectedLanguage,
                                         const bool usedFallback) noexcept
      : mStorage(std::move(storage)),
        mSelectedLanguage(selectedLanguage),
        mUsedFallback(usedFallback) {}

  std::expected<TranslationCatalog, TranslationError> TranslationCatalog::Load(
      const std::filesystem::path &directory,
      const std::string_view requestedLanguageCode)
  {
    const auto paths = CatalogPaths(directory);
    if (!paths)
    {
      return std::unexpected(paths.error());
    }

    std::vector<ParsedLanguage> parsed;
    parsed.reserve(paths->size());

    std::unordered_set<std::string> languageCodes;

    for (const auto &path : *paths)
    {
      auto catalog = ParseCatalog(path);
      if (!catalog)
      {
        return std::unexpected(catalog.error());
      }

      const auto canonicalCode = CanonicalLanguageCode(catalog->info.code);
      if (!languageCodes.emplace(canonicalCode).second)
      {
        return std::unexpected(MakeError(
            TranslationErrorKind::Validation,
            path,
            "Language code '" + catalog->info.code +
                "' duplicates another translation catalog"));
      }

      parsed.push_back(std::move(*catalog));
    }

    std::size_t defaultIndex = 0;

    for (std::size_t index = 0; index < parsed.size(); ++index)
    {
      if (CanonicalLanguageCode(parsed[index].info.code) == "en")
      {
        defaultIndex = index;

        break;
      }
    }

    const auto &reference = parsed[defaultIndex];

    for (const auto &catalog : parsed)
    {
      for (const auto &[key, referenceText] : reference.strings)
      {
        const auto found = catalog.strings.find(key);
        if (found == catalog.strings.end())
        {
          return std::unexpected(MakeError(
              TranslationErrorKind::Validation,
              catalog.sourcePath,
              "Translation key '" + key + "' is missing"));
        }

        const auto expectedPlaceholders = PlaceholderCounts(referenceText);
        const auto actualPlaceholders = PlaceholderCounts(found->second);
        if (*expectedPlaceholders != *actualPlaceholders)
        {
          return std::unexpected(MakeError(
              TranslationErrorKind::Validation,
              catalog.sourcePath,
              "Translation key '" + key +
                  "' does not use the reference catalog's placeholders"));
        }
      }

      if (catalog.strings.size() != reference.strings.size())
      {
        const auto extra = std::ranges::find_if(
            catalog.strings,
            [&](const auto &item)
            { return !reference.strings.contains(item.first); });

        return std::unexpected(MakeError(
            TranslationErrorKind::Validation,
            catalog.sourcePath,
            extra == catalog.strings.end()
                ? "Translation catalog key set does not match the reference catalog"
                : "Translation key '" + extra->first +
                      "' is not present in the reference catalog"));
      }
    }

    std::size_t selected = defaultIndex;

    bool usedFallback = true;

    const auto requested = CanonicalLanguageCode(requestedLanguageCode);

    for (std::size_t index = 0; index < parsed.size(); ++index)
    {
      if (CanonicalLanguageCode(parsed[index].info.code) == requested)
      {
        selected = index;

        usedFallback = false;

        break;
      }
    }

    auto storage = std::make_shared<Storage>();
    storage->defaultLanguage = defaultIndex;
    storage->languageInfo.reserve(parsed.size());
    storage->translations.reserve(parsed.size());

    for (auto &catalog : parsed)
    {
      storage->languageInfo.push_back(std::move(catalog.info));
      storage->translations.push_back(std::move(catalog.strings));
    }

    return TranslationCatalog{std::move(storage), selected, usedFallback};
  }

  std::span<const LanguageInfo> TranslationCatalog::Languages() const noexcept
  {
    return mStorage ? std::span<const LanguageInfo>{mStorage->languageInfo}
                    : std::span<const LanguageInfo>{};
  }

  std::string_view TranslationCatalog::SelectedLanguageCode() const noexcept
  {
    if (!mStorage || mSelectedLanguage >= mStorage->languageInfo.size())
    {
      return {};
    }

    return mStorage->languageInfo[mSelectedLanguage].code;
  }

  bool TranslationCatalog::UsedFallback() const noexcept { return mUsedFallback; }

  TranslationCatalog TranslationCatalog::WithLanguage(
      const std::string_view requestedLanguageCode) const noexcept
  {
    if (!mStorage || mStorage->languageInfo.empty())
    {
      return *this;
    }

    const auto requested = CanonicalLanguageCode(requestedLanguageCode);

    for (std::size_t index = 0; index < mStorage->languageInfo.size(); ++index)
    {
      if (CanonicalLanguageCode(mStorage->languageInfo[index].code) == requested)
      {
        return TranslationCatalog{mStorage, index, false};
      }
    }

    return TranslationCatalog{mStorage, mStorage->defaultLanguage, true};
  }

  std::string_view TranslationCatalog::Text(const std::string_view key) const noexcept
  {
    if (!mStorage || mSelectedLanguage >= mStorage->translations.size())
    {
      return key;
    }

    const auto &translations = mStorage->translations[mSelectedLanguage];
    const auto found = translations.find(std::string{key});

    return found == translations.end() ? key : std::string_view{found->second};
  }

  std::string TranslationCatalog::Format(
      const std::string_view key,
      const std::span<const std::string_view> arguments) const
  {
    const auto pattern = Text(key);

    std::string result;
    result.reserve(pattern.size());

    for (std::size_t cursor = 0; cursor < pattern.size();)
    {
      if (pattern[cursor] == '{' && cursor + 1 < pattern.size() &&
          pattern[cursor + 1] == '{')
      {
        result.push_back('{');

        cursor += 2;

        continue;
      }

      if (pattern[cursor] == '}' && cursor + 1 < pattern.size() &&
          pattern[cursor + 1] == '}')
      {
        result.push_back('}');

        cursor += 2;

        continue;
      }

      if (pattern[cursor] != '{')
      {
        result.push_back(pattern[cursor++]);

        continue;
      }

      const auto closing = pattern.find('}', cursor + 1);

      std::size_t index = 0;

      for (std::size_t digit = cursor + 1; digit < closing; ++digit)
      {
        index = index * 10 + static_cast<std::size_t>(pattern[digit] - '0');
      }

      if (index < arguments.size())
      {
        result.append(arguments[index]);
      }
      else
      {
        result.append(pattern.substr(cursor, closing - cursor + 1));
      }

      cursor = closing + 1;
    }

    return result;
  }

  std::string TranslationCatalog::Format(
      const std::string_view key,
      const std::initializer_list<std::string_view> arguments) const
  {
    return Format(key, std::span<const std::string_view>{arguments.begin(), arguments.size()});
  }
} // namespace havremote::localization
