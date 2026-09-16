// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_TESTS_CONFIG_CONFIG_TEST_SUPPORT_HPP
#define HAVREMOTE_TESTS_CONFIG_CONFIG_TEST_SUPPORT_HPP

#include "core/types.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace havremote::config::test
{
  class TempDirectory final
  {
  public:
    TempDirectory()
    {
      static std::atomic_uint64_t sequence{};

      const std::string name =
          "havremote-config-test-" + GenerateId() + "-" +
          std::to_string(sequence.fetch_add(1));

      std::error_code ec;

      mPath = std::filesystem::temp_directory_path(ec) / name;

      if (ec)
      {
        ec.clear();
        mPath = std::filesystem::current_path() / name;
      }

      std::filesystem::remove_all(mPath, ec);

      ec.clear();

      std::filesystem::create_directories(mPath, ec);

      if (ec)
      {
        ec.clear();

        mPath = std::filesystem::current_path() / name;

        std::filesystem::remove_all(mPath, ec);

        ec.clear();

        std::filesystem::create_directories(mPath, ec);
      }
    }

    ~TempDirectory()
    {
      std::error_code ignored;
      std::filesystem::remove_all(mPath, ignored);
    }

    TempDirectory(const TempDirectory &) = delete;
    TempDirectory &operator=(const TempDirectory &) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const noexcept
    {
      return mPath;
    }

  private:
    std::filesystem::path mPath;
  };

  inline void WriteText(const std::filesystem::path &path, std::string_view text)
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
  }

  inline std::string ReadText(const std::filesystem::path &path)
  {
    std::ifstream input(path, std::ios::binary);

    std::ostringstream contents;
    contents << input.rdbuf();

    return contents.str();
  }

  inline std::string ValidEmptyConfig()
  {
    return R"(formatVersion: 1
settings:
  transferConcurrency: 2
  connectionTimeoutSeconds: 20
  commandIdleTimeoutSeconds: 60
  defaultConflictPolicy: "ask"
  theme: "dark"
  language: "en"
  fileLists:
    local:
      sortColumn: "name"
      sortAscending: true
    remote:
      sortColumn: "name"
      sortAscending: true
  updates:
    checkAutomatically: true
    lastCheckUnixSeconds: ""
    skippedVersion: ""
workspace:
  localDirectory: ""
  connectionDirectories: []
quickConnectHistory: []
sites: []
siteFolders: []
tlsTrust: []
)";
  }
} // namespace havremote::config::test

#endif // HAVREMOTE_TESTS_CONFIG_CONFIG_TEST_SUPPORT_HPP
