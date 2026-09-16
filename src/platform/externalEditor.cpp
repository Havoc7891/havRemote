// SPDX-License-Identifier: MIT

#include "platform/externalEditor.hpp"

#include <wx/log.h>
#include <wx/string.h>
#include <wx/utils.h>

#include <optional>
#include <system_error>
#include <utility>

namespace havremote::platform
{
  namespace
  {
    constexpr std::string_view FilePlaceholder = "{file}";

    [[nodiscard]] PlatformError InvalidArgument(std::string message)
    {
      return PlatformError{PlatformErrorCode::InvalidArgument, std::move(message), 0};
    }

    [[nodiscard]] bool IsArgumentWhitespace(const char value) noexcept
    {
      return value == ' ' || value == '\t' || value == '\r' || value == '\n';
    }

    [[nodiscard]] std::optional<PlatformError> ValidateRegularFile(
        const std::filesystem::path &path,
        const std::string_view description)
    {
      if (path.empty())
      {
        return InvalidArgument(std::string{description} + " path is empty");
      }

      std::error_code error;

      const bool exists = std::filesystem::exists(path, error);

      if (error)
      {
        return PlatformError{
            error == std::errc::permission_denied ? PlatformErrorCode::AccessDenied
                                                  : PlatformErrorCode::Io,
            "Could not inspect " + std::string{description} + ": " + error.message(),
            static_cast<unsigned long>(error.value())};
      }

      if (!exists)
      {
        return PlatformError{PlatformErrorCode::NotFound,
                             std::string{description} + " does not exist", 0};
      }

      const bool regular = std::filesystem::is_regular_file(path, error);

      if (error)
      {
        return PlatformError{
            error == std::errc::permission_denied ? PlatformErrorCode::AccessDenied
                                                  : PlatformErrorCode::Io,
            "Could not inspect " + std::string{description} + ": " + error.message(),
            static_cast<unsigned long>(error.value())};
      }

      if (!regular)
      {
        return InvalidArgument(std::string{description} + " is not a regular file");
      }

      return std::nullopt;
    }

    [[nodiscard]] Result<std::string> PathToUtf8(const std::filesystem::path &path)
    {
#if defined(_WIN32)
      const wxString value{path.wstring()};
      const auto utf8 = value.ToUTF8();

      if (!utf8)
      {
        return std::unexpected(InvalidArgument(
            "The file path could not be converted to UTF-8"));
      }

      return std::string{utf8.data(), utf8.length()};
#else
      const auto utf8 = path.u8string();

      return std::string{reinterpret_cast<const char *>(utf8.data()), utf8.size()};
#endif
    }

    [[nodiscard]] bool LaunchDefaultWithWx(const std::filesystem::path &file)
    {
#if wxUSE_GUI
      const auto path = PathToUtf8(file);
      if (!path)
      {
        return false;
      }

      wxLogNull suppressLaunchErrors;

      return wxLaunchDefaultApplication(
          wxString::FromUTF8(path->data(), path->size()));
#else
      (void)file;

      return false;
#endif
    }

    [[nodiscard]] Result<bool> IsPotentiallyExecutableFile(
        const std::filesystem::path &file)
    {
      const auto extensionPath = PathToUtf8(file.extension());
      if (!extensionPath)
      {
        return std::unexpected(extensionPath.error());
      }

      auto extension = *extensionPath;

      for (auto &character : extension)
      {
        if (character >= 'A' && character <= 'Z')
        {
          character += 'a' - 'A';
        }
      }

      // wxLaunchDefaultApplication() performs the registered shell action. For
      // these families, that action can run, install, import, or follow the file
      // instead of opening it as editor data. Keep system-default mode fail-closed.
      // A custom editor receives each filename as a shell-free argv element.
      static constexpr std::string_view potentiallyExecutableExtensions[]{
          ".app",
          ".application",
          ".appinstaller",
          ".appref-ms",
          ".appx",
          ".appxbundle",
          ".bat",
          ".chm",
          ".cmd",
          ".com",
          ".cpl",
          ".csh",
          ".diagcab",
          ".dll",
          ".drv",
          ".exe",
          ".gadget",
          ".hlp",
          ".hta",
          ".inf",
          ".ins",
          ".isp",
          ".jar",
          ".jnlp",
          ".job",
          ".js",
          ".jse",
          ".ksh",
          ".lnk",
          ".msc",
          ".msh",
          ".msh1",
          ".msh1xml",
          ".msh2",
          ".msh2xml",
          ".msi",
          ".msix",
          ".msixbundle",
          ".msp",
          ".mst",
          ".mshxml",
          ".ocx",
          ".pif",
          ".pl",
          ".ps1",
          ".ps1xml",
          ".ps2",
          ".ps2xml",
          ".psc1",
          ".psc2",
          ".psd1",
          ".psm1",
          ".py",
          ".pyw",
          ".rb",
          ".reg",
          ".rgs",
          ".scf",
          ".scr",
          ".sct",
          ".search-ms",
          ".sh",
          ".shb",
          ".shs",
          ".sys",
          ".url",
          ".vb",
          ".vbe",
          ".vbs",
          ".website",
          ".ws",
          ".wsc",
          ".wsf",
          ".wsh",
          ".xnk",
          ".appimage",
          ".applescript",
          ".command",
          ".deb",
          ".desktop",
          ".dmg",
          ".flatpak",
          ".flatpakref",
          ".mpkg",
          ".pkg",
          ".rpm",
          ".run",
          ".scpt",
          ".scptd",
          ".snap",
          ".webloc",
          ".workflow",
      };

      for (const auto candidate : potentiallyExecutableExtensions)
      {
        if (extension == candidate)
        {
          return true;
        }
      }

#if !defined(_WIN32)
      std::error_code error;

      const auto permissions = std::filesystem::status(file, error).permissions();

      if (error || permissions == std::filesystem::perms::unknown)
      {
        return std::unexpected(PlatformError{
            error == std::errc::permission_denied ? PlatformErrorCode::AccessDenied
                                                  : PlatformErrorCode::Io,
            "Could not inspect the file's executable permissions" +
                (error ? ": " + error.message() : std::string{}),
            static_cast<unsigned long>(error.value())});
      }

      constexpr auto executable = std::filesystem::perms::owner_exec |
                                  std::filesystem::perms::group_exec |
                                  std::filesystem::perms::others_exec;

      if ((permissions & executable) != std::filesystem::perms::none)
      {
        return true;
      }
#endif

      return false;
    }

    [[nodiscard]] long LaunchCustomWithWx(
        const std::filesystem::path &executable,
        const std::span<const std::string> arguments)
    {
      std::vector<wxString> values;
      values.reserve(arguments.size() + 1U);

      const auto program = PathToUtf8(executable);
      if (!program)
      {
        return 0;
      }

      values.emplace_back(wxString::FromUTF8(program->data(), program->size()));

      for (const auto &argument : arguments)
      {
        values.emplace_back(wxString::FromUTF8(argument.data(), argument.size()));
      }

      std::vector<const wchar_t *> argv;
      argv.reserve(values.size() + 1U);

      for (const auto &value : values)
      {
        argv.push_back(value.wc_str());
      }

      argv.push_back(nullptr);

      wxLogNull suppressLaunchErrors;

      return wxExecute(argv.data(), wxEXEC_ASYNC);
    }
  } // namespace

  Result<std::vector<std::string>> ExpandExternalEditorArguments(
      const std::string_view argumentTemplate,
      const std::string_view fileArgument)
  {
    if (argumentTemplate.find('\0') != std::string_view::npos ||
        fileArgument.find('\0') != std::string_view::npos)
    {
      return std::unexpected(InvalidArgument(
          "External-editor arguments cannot contain NUL characters"));
    }

    std::size_t placeholderCount{};

    for (std::size_t position{}; (position = argumentTemplate.find(FilePlaceholder, position)) != std::string_view::npos; position += FilePlaceholder.size())
    {
      ++placeholderCount;
    }

    if (placeholderCount != 1)
    {
      return std::unexpected(InvalidArgument(
          placeholderCount == 0
              ? "External-editor arguments must contain {file} exactly once"
              : "External-editor arguments cannot contain {file} more than once"));
    }

    std::vector<std::string> arguments;

    std::string current;

    char quote{};

    bool argumentStarted{};

    for (std::size_t index = 0; index < argumentTemplate.size(); ++index)
    {
      const char value = argumentTemplate[index];

      if (quote == 0 && IsArgumentWhitespace(value))
      {
        if (argumentStarted)
        {
          arguments.push_back(std::move(current));

          current.clear();

          argumentStarted = false;
        }

        continue;
      }

      if (value == '\'' || value == '"')
      {
        if (quote == 0)
        {
          quote = value;

          argumentStarted = true;

          continue;
        }

        if (quote == value)
        {
          quote = 0;

          continue;
        }
      }

      if (value == '\\' && index + 1U < argumentTemplate.size())
      {
        const char next = argumentTemplate[index + 1U];

        const bool escapedOutsideQuote =
            quote == 0 &&
            (IsArgumentWhitespace(next) || next == '\\' || next == '\'' ||
             next == '"');

        const bool escapedInsideQuote =
            quote != 0 && (next == quote || next == '\\');

        if (escapedOutsideQuote || escapedInsideQuote)
        {
          current.push_back(next);

          argumentStarted = true;

          ++index;

          continue;
        }
      }

      current.push_back(value);

      argumentStarted = true;
    }

    if (quote != 0)
    {
      return std::unexpected(InvalidArgument(
          "External-editor arguments contain an unterminated quote"));
    }

    if (argumentStarted)
    {
      arguments.push_back(std::move(current));
    }

    for (auto &argument : arguments)
    {
      const auto placeholder = argument.find(FilePlaceholder);

      if (placeholder != std::string::npos)
      {
        argument.replace(placeholder, FilePlaceholder.size(), fileArgument);

        return arguments;
      }
    }

    return std::unexpected(InvalidArgument(
        "External-editor arguments must contain {file} as an argument value"));
  }

  Result<void> LaunchExternalEditor(
      const config::ExternalEditorSettings &settings,
      const std::filesystem::path &file,
      const ExternalEditorLaunchFunctions &launchFunctions)
  {
    if (const auto error = ValidateRegularFile(file, "The file to edit"))
    {
      return std::unexpected(*error);
    }

    if (settings.mode == config::ExternalEditorMode::SystemDefault)
    {
      const auto potentiallyExecutable = IsPotentiallyExecutableFile(file);
      if (!potentiallyExecutable)
      {
        return std::unexpected(potentiallyExecutable.error());
      }
      if (*potentiallyExecutable)
      {
        return std::unexpected(PlatformError{
            PlatformErrorCode::UnsafeFileType,
            "System-default editing is blocked for potentially executable "
            "files. Configure a custom editor to open this "
            "file as data",
            0});
      }

      const bool launched = launchFunctions.launchDefault
                                ? launchFunctions.launchDefault(file)
                                : LaunchDefaultWithWx(file);
      if (!launched)
      {
        return std::unexpected(PlatformError{
            PlatformErrorCode::Unavailable,
            "No application could be launched for the selected file", 0});
      }

      return {};
    }

    if (settings.mode != config::ExternalEditorMode::Custom)
    {
      return std::unexpected(InvalidArgument(
          "The external-editor mode is not supported"));
    }

    if (const auto error = ValidateRegularFile(settings.executable,
                                               "The external-editor executable"))
    {
      return std::unexpected(*error);
    }

    auto utf8File = PathToUtf8(file);
    if (!utf8File)
    {
      return std::unexpected(utf8File.error());
    }

    auto arguments = ExpandExternalEditorArguments(settings.arguments, *utf8File);
    if (!arguments)
    {
      return std::unexpected(arguments.error());
    }

    const long processId = launchFunctions.launchCustom
                               ? launchFunctions.launchCustom(settings.executable,
                                                              *arguments)
                               : LaunchCustomWithWx(settings.executable, *arguments);
    if (processId <= 0)
    {
      return std::unexpected(PlatformError{
          PlatformErrorCode::Unavailable,
          "The configured external editor could not be started", 0});
    }

    return {};
  }
} // namespace havremote::platform
