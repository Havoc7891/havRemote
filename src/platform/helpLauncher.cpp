// SPDX-License-Identifier: MIT

#include "platform/helpLauncher.hpp"

#include <wx/defs.h>

#if wxUSE_GUI
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/utils.h>
#endif

#include <string>
#include <utility>

namespace havremote::platform
{
  namespace
  {
    constexpr std::size_t MaximumLanguageCodeLength = 35;
    constexpr std::string_view EnglishLanguageCode = "en";

    [[nodiscard]] bool IsAsciiAlphanumeric(const unsigned char character) noexcept
    {
      return (character >= 'a' && character <= 'z') ||
             (character >= 'A' && character <= 'Z') ||
             (character >= '0' && character <= '9');
    }

    [[nodiscard]] bool IsValidLanguageCode(const std::string_view code) noexcept
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

    [[nodiscard]] std::string CanonicalLanguageCode(const std::string_view code)
    {
      std::string result;
      result.reserve(code.size());

      for (const unsigned char character : code)
      {
        result.push_back(static_cast<char>(
            character >= 'A' && character <= 'Z'
                ? character + static_cast<unsigned char>('a' - 'A')
                : character));
      }

      return result;
    }

    [[nodiscard]] bool IsRegularFile(const std::filesystem::path &path,
                                     std::error_code &error)
    {
      const bool regular = std::filesystem::is_regular_file(path, error);

      if (!regular &&
          (error == std::errc::no_such_file_or_directory ||
           error == std::errc::not_a_directory))
      {
        // A missing localized page is the normal signal to try the English
        // fallback. Keep genuine inspection failures visible to the caller.
        error.clear();
      }

      return regular;
    }

    [[nodiscard]] std::string DisplayPath(const std::filesystem::path &path)
    {
      const auto value = path.generic_u8string();

      return {reinterpret_cast<const char *>(value.data()), value.size()};
    }

    [[nodiscard]] PlatformError InspectionError(const std::filesystem::path &path,
                                                const std::error_code &error)
    {
      return PlatformError{
          error == std::errc::permission_denied ? PlatformErrorCode::AccessDenied
                                                : PlatformErrorCode::Io,
          "Could not inspect the help file '" + DisplayPath(path) + "': " +
              error.message(),
          static_cast<unsigned long>(error.value())};
    }

    [[nodiscard]] bool LaunchWithPlatformBrowser(
        const std::filesystem::path &helpFile)
    {
#if wxUSE_GUI
      wxLogNull suppressLaunchErrors;

      const auto path = helpFile.u8string();

      const wxFileName fileName{wxString::FromUTF8(
          reinterpret_cast<const char *>(path.data()), path.size())};

      return wxLaunchDefaultBrowser(wxFileName::FileNameToURL(fileName));
#else
      (void)helpFile;

      return false;
#endif
    }
  } // namespace

  Result<std::filesystem::path> ResolveHelpIndex(
      const std::filesystem::path &helpRoot,
      const std::string_view selectedLanguage,
      const HelpLaunchFunctions &functions)
  {
    if (!IsValidLanguageCode(selectedLanguage))
    {
      return std::unexpected(PlatformError{
          PlatformErrorCode::InvalidArgument,
          "The help language must contain 1-35 ASCII alphanumeric characters "
          "separated by single hyphens",
          0});
    }

    const auto language = CanonicalLanguageCode(selectedLanguage);
    const auto inspect = functions.isRegularFile ? functions.isRegularFile
                                                 : IsRegularFile;

    const auto localizedIndex =
        helpRoot / std::filesystem::path{language} / "index.html";

    std::error_code error;

    if (inspect(localizedIndex, error))
    {
      return localizedIndex;
    }

    if (error)
    {
      return std::unexpected(InspectionError(localizedIndex, error));
    }

    const auto englishIndex =
        helpRoot / std::filesystem::path{EnglishLanguageCode} / "index.html";

    if (language != EnglishLanguageCode)
    {
      error.clear();

      if (inspect(englishIndex, error))
      {
        return englishIndex;
      }

      if (error)
      {
        return std::unexpected(InspectionError(englishIndex, error));
      }
    }

    return std::unexpected(PlatformError{
        PlatformErrorCode::NotFound,
        "The offline help file was not found at '" +
            DisplayPath(localizedIndex) + "' or at its English fallback '" +
            DisplayPath(englishIndex) + "'",
        0});
  }

  Result<void> LaunchOfflineHelp(const std::filesystem::path &helpRoot,
                                 const std::string_view selectedLanguage,
                                 const HelpLaunchFunctions &functions)
  {
    auto helpFile = ResolveHelpIndex(helpRoot, selectedLanguage, functions);
    if (!helpFile)
    {
      return std::unexpected(std::move(helpFile.error()));
    }

    const auto &launch = functions.launchBrowser;
    if (launch ? launch(*helpFile) : LaunchWithPlatformBrowser(*helpFile))
    {
      return {};
    }

    return std::unexpected(PlatformError{
        PlatformErrorCode::Unavailable,
        "The offline help file could not be opened in the default browser",
        0});
  }
} // namespace havremote::platform
