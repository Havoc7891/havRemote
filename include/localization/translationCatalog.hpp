// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_LOCALIZATION_TRANSLATION_CATALOG_HPP
#define HAVREMOTE_INCLUDE_LOCALIZATION_TRANSLATION_CATALOG_HPP

#include <cstddef>
#include <expected>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace havremote::localization
{
  enum class TranslationErrorKind
  {
    Io,
    Parse,
    Validation,
  };

  struct TranslationError final
  {
    TranslationErrorKind kind{TranslationErrorKind::Validation};
    std::filesystem::path path;
    std::string message;
    std::optional<std::size_t> line;
    std::optional<std::size_t> column;
  };

  struct LanguageInfo final
  {
    std::string code;
    std::string displayName;
  };

  class TranslationCatalog final
  {
  public:
    [[nodiscard]] static std::expected<TranslationCatalog, TranslationError>
    Load(const std::filesystem::path &directory,
         std::string_view requestedLanguageCode);

    [[nodiscard]] std::span<const LanguageInfo> Languages() const noexcept;
    [[nodiscard]] std::string_view SelectedLanguageCode() const noexcept;
    [[nodiscard]] bool UsedFallback() const noexcept;
    [[nodiscard]] TranslationCatalog WithLanguage(
        std::string_view requestedLanguageCode) const noexcept;

    // A missing key is returned verbatim
    [[nodiscard]] std::string_view Text(std::string_view key) const noexcept;

    [[nodiscard]] std::string Format(
        std::string_view key,
        std::span<const std::string_view> arguments) const;
    [[nodiscard]] std::string Format(
        std::string_view key,
        std::initializer_list<std::string_view> arguments) const;

  private:
    struct Storage;

    TranslationCatalog(std::shared_ptr<const Storage> storage,
                       std::size_t selectedLanguage,
                       bool usedFallback) noexcept;

    std::shared_ptr<const Storage> mStorage;
    std::size_t mSelectedLanguage{};
    bool mUsedFallback{};
  };
} // namespace havremote::localization

#endif // HAVREMOTE_INCLUDE_LOCALIZATION_TRANSLATION_CATALOG_HPP
