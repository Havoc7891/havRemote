// SPDX-License-Identifier: MIT

#include "platform/externalEditor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

using namespace havremote;

namespace
{
  class TemporaryDirectory final
  {
  public:
    TemporaryDirectory()
        : mPath(std::filesystem::temp_directory_path() /
                ("havremote-external-editor-" + GenerateId()))
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

    [[nodiscard]] std::filesystem::path CreateTestFile(const std::filesystem::path &name) const
    {
      const auto file = mPath / name;

      std::ofstream output{file, std::ios::binary};
      output << "test";

      return file;
    }

    [[nodiscard]] const std::filesystem::path &Path() const noexcept { return mPath; }

  private:
    std::filesystem::path mPath;
  };
} // namespace

TEST_CASE("External editor arguments replace the file placeholder as one argv item",
          "[platform][external-editor]")
{
  const auto arguments = platform::ExpandExternalEditorArguments(
      "--wait {file}", "C:\\Documents and Settings\\report.txt");

  REQUIRE(arguments);
  REQUIRE(arguments->size() == 2);
  CHECK((*arguments)[0] == "--wait");
  CHECK((*arguments)[1] == "C:\\Documents and Settings\\report.txt");
}

TEST_CASE("External editor argument quotes group values and preserve empty arguments",
          "[platform][external-editor]")
{
  const auto arguments = platform::ExpandExternalEditorArguments(
      "--line \"12:4\" '--reuse window' \"{file}\" \"\"",
      "C:\\A folder\\notes.txt");

  REQUIRE(arguments);
  REQUIRE(arguments->size() == 5);
  CHECK((*arguments)[0] == "--line");
  CHECK((*arguments)[1] == "12:4");
  CHECK((*arguments)[2] == "--reuse window");
  CHECK((*arguments)[3] == "C:\\A folder\\notes.txt");
  CHECK((*arguments)[4].empty());
}

TEST_CASE("External editor metacharacters remain literal argv data",
          "[platform][external-editor]")
{
  const auto arguments = platform::ExpandExternalEditorArguments(
      "--literal=a&b \"x|y;>$(not-a-command)\" {file}",
      "C:\\Temp\\report & start calc;.txt");

  REQUIRE(arguments);
  REQUIRE(arguments->size() == 3);
  CHECK((*arguments)[0] == "--literal=a&b");
  CHECK((*arguments)[1] == "x|y;>$(not-a-command)");
  CHECK((*arguments)[2] == "C:\\Temp\\report & start calc;.txt");
}

TEST_CASE("External editor argument templates require exactly one file placeholder",
          "[platform][external-editor]")
{
  const auto missing =
      platform::ExpandExternalEditorArguments("--wait", "file.txt");

  REQUIRE_FALSE(missing);
  CHECK(missing.error().code == platform::PlatformErrorCode::InvalidArgument);

  const auto duplicate = platform::ExpandExternalEditorArguments(
      "{file} --compare {file}", "file.txt");

  REQUIRE_FALSE(duplicate);
  CHECK(duplicate.error().code == platform::PlatformErrorCode::InvalidArgument);
}

TEST_CASE("External editor launch validates the selected file before launching",
          "[platform][external-editor]")
{
  TemporaryDirectory temporary;

  config::ExternalEditorSettings settings;

  bool launchAttempted{};

  platform::ExternalEditorLaunchFunctions launchFunctions;

  launchFunctions.launchDefault = [&](const std::filesystem::path &)
  {
    launchAttempted = true;

    return true;
  };

  const auto result = platform::LaunchExternalEditor(
      settings, temporary.Path() / "missing.txt", launchFunctions);

  REQUIRE_FALSE(result);
  CHECK(result.error().code == platform::PlatformErrorCode::NotFound);
  CHECK_FALSE(launchAttempted);
}

TEST_CASE("System-default editing blocks launchable file families",
          "[platform][external-editor]")
{
  TemporaryDirectory temporary;

  config::ExternalEditorSettings settings;

  platform::ExternalEditorLaunchFunctions launchFunctions;

  bool launchAttempted{};

  launchFunctions.launchDefault = [&](const std::filesystem::path &)
  {
    launchAttempted = true;

    return true;
  };

  for (const std::string_view extension :
       {".EXE", ".CmD", ".Ps1", ".JsE", ".MsI", ".LnK", ".UrL",
        ".desktop", ".AppImage", ".command", ".workflow", ".scpt", ".pkg",
        ".dmg", ".deb", ".rpm", ".sh", ".py", ".jar"})
  {
    CAPTURE(extension);

    launchAttempted = false;

    const auto file = temporary.CreateTestFile(std::string{"payload"} +
                                               std::string{extension});

    const auto result =
        platform::LaunchExternalEditor(settings, file, launchFunctions);

    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          platform::PlatformErrorCode::UnsafeFileType);
    CHECK(result.error().message.find("custom editor") != std::string::npos);
    CHECK_FALSE(launchAttempted);
  }
}

TEST_CASE("System-default editing still opens ordinary document types",
          "[platform][external-editor]")
{
  TemporaryDirectory temporary;

  const auto file = temporary.CreateTestFile("notes.TxT");

  config::ExternalEditorSettings settings;

  platform::ExternalEditorLaunchFunctions launchFunctions;

  std::filesystem::path launchedFile;

  launchFunctions.launchDefault = [&](const std::filesystem::path &selected)
  {
    launchedFile = selected;

    return true;
  };

  const auto result =
      platform::LaunchExternalEditor(settings, file, launchFunctions);

  REQUIRE(result);
  CHECK(launchedFile == file);
}

TEST_CASE("Custom external editor launch validates its executable",
          "[platform][external-editor]")
{
  TemporaryDirectory temporary;

  const auto file = temporary.CreateTestFile("document.txt");

  config::ExternalEditorSettings settings;
  settings.mode = config::ExternalEditorMode::Custom;
  settings.executable = temporary.Path() / "missing-editor.exe";
  settings.arguments = "{file}";

  bool launchAttempted{};

  platform::ExternalEditorLaunchFunctions launchFunctions;

  launchFunctions.launchCustom =
      [&](const std::filesystem::path &, std::span<const std::string>)
  {
    launchAttempted = true;

    return 42L;
  };

  const auto result =
      platform::LaunchExternalEditor(settings, file, launchFunctions);

  REQUIRE_FALSE(result);
  CHECK(result.error().code == platform::PlatformErrorCode::NotFound);
  CHECK_FALSE(launchAttempted);
}

TEST_CASE("Custom external editor receives an argv vector without shell expansion",
          "[platform][external-editor]")
{
  TemporaryDirectory temporary;

  const auto executable = temporary.CreateTestFile("editor.exe");

  const auto file = temporary.CreateTestFile("report & echo-owned; $(noop).txt");

  config::ExternalEditorSettings settings;
  settings.mode = config::ExternalEditorMode::Custom;
  settings.executable = executable;
  settings.arguments = "--reuse-window \"{file}\"";

  std::filesystem::path receivedExecutable;

  std::vector<std::string> receivedArguments;

  platform::ExternalEditorLaunchFunctions launchFunctions;

  launchFunctions.launchCustom =
      [&](const std::filesystem::path &program,
          const std::span<const std::string> arguments)
  {
    receivedExecutable = program;

    receivedArguments.assign(arguments.begin(), arguments.end());

    return 42L;
  };

  const auto result =
      platform::LaunchExternalEditor(settings, file, launchFunctions);

  REQUIRE(result);
  CHECK(receivedExecutable == executable);
  REQUIRE(receivedArguments.size() == 2);
  CHECK(receivedArguments[0] == "--reuse-window");
  CHECK(receivedArguments[1] == file.string());
}

TEST_CASE("Custom external editors may open executable file types as data",
          "[platform][external-editor]")
{
  TemporaryDirectory temporary;

  const auto executable = temporary.CreateTestFile("editor.exe");

  const auto file = temporary.CreateTestFile("remote-script.CmD");

  config::ExternalEditorSettings settings;
  settings.mode = config::ExternalEditorMode::Custom;
  settings.executable = executable;
  settings.arguments = "{file}";

  bool launchAttempted{};

  platform::ExternalEditorLaunchFunctions launchFunctions;

  launchFunctions.launchCustom =
      [&](const std::filesystem::path &program,
          const std::span<const std::string> arguments)
  {
    launchAttempted = true;

    CHECK(program == executable);
    REQUIRE(arguments.size() == 1);
    CHECK(arguments.front() == file.string());

    return 42L;
  };

  const auto result =
      platform::LaunchExternalEditor(settings, file, launchFunctions);

  REQUIRE(result);
  CHECK(launchAttempted);
}

TEST_CASE("External editors preserve Unicode paths and extensions",
          "[platform][external-editor]")
{
  TemporaryDirectory temporary;

  const auto file = temporary.CreateTestFile(std::filesystem::path{u8"Grüße 猫.数据"});

  const auto executable = temporary.CreateTestFile(std::filesystem::path{u8"éditeur 猫"});

  config::ExternalEditorSettings settings;

  platform::ExternalEditorLaunchFunctions functions;

  bool defaultAttempted{};

  functions.launchDefault = [&](const std::filesystem::path &selected)
  {
    defaultAttempted = true;

    CHECK(selected == file);

    return true;
  };

  REQUIRE(platform::LaunchExternalEditor(settings, file, functions));
  CHECK(defaultAttempted);

  settings.mode = config::ExternalEditorMode::Custom;
  settings.executable = executable;
  settings.arguments = "--wait {file}";

  bool customAttempted{};

  functions.launchCustom = [&](const std::filesystem::path &program,
                               const std::span<const std::string> arguments)
  {
    customAttempted = true;

    CHECK(program == executable);
    REQUIRE(arguments.size() == 2U);
    const auto expected = file.u8string();
    CHECK(arguments[1] == std::string{
                              reinterpret_cast<const char *>(expected.data()), expected.size()});

    return 42L;
  };

  REQUIRE(platform::LaunchExternalEditor(settings, file, functions));
  CHECK(customAttempted);
}

#if !defined(_WIN32)
TEST_CASE("System-default editing blocks executable permissions without relying on extensions",
          "[platform][external-editor][security]")
{
  TemporaryDirectory temporary;

  const auto file = temporary.CreateTestFile("document.txt");

  const auto executable = temporary.CreateTestFile("editor");

  config::ExternalEditorSettings settings;

  bool defaultAttempted{};

  platform::ExternalEditorLaunchFunctions functions;

  functions.launchDefault = [&](const std::filesystem::path &)
  {
    defaultAttempted = true;

    return true;
  };

  constexpr auto readable = std::filesystem::perms::owner_read |
                            std::filesystem::perms::owner_write;

  for (const auto executablePermission : {
           std::filesystem::perms::owner_exec,
           std::filesystem::perms::group_exec,
           std::filesystem::perms::others_exec})
  {
    CAPTURE(static_cast<unsigned>(executablePermission));

    std::filesystem::permissions(file, readable | executablePermission);

    const auto blocked = platform::LaunchExternalEditor(settings, file, functions);

    REQUIRE_FALSE(blocked);
    CHECK(blocked.error().code == platform::PlatformErrorCode::UnsafeFileType);
    CHECK_FALSE(defaultAttempted);
  }

  settings.mode = config::ExternalEditorMode::Custom;
  settings.executable = executable;

  bool customAttempted{};

  functions.launchCustom = [&](const std::filesystem::path &,
                               const std::span<const std::string>)
  {
    customAttempted = true;

    return 42L;
  };

  REQUIRE(platform::LaunchExternalEditor(settings, file, functions));
  CHECK(customAttempted);

  std::filesystem::permissions(file, readable);

  settings.mode = config::ExternalEditorMode::SystemDefault;

  REQUIRE(platform::LaunchExternalEditor(settings, file, functions));
  CHECK(defaultAttempted);
}
#endif
