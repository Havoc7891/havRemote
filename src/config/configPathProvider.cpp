// SPDX-License-Identifier: MIT

#include "config/configPathProvider.hpp"

#include <utility>

namespace havremote::config
{
  namespace
  {
    std::expected<std::filesystem::path, ConfigError> ValidatePath(
        const std::filesystem::path &path,
        const char *description)
    {
      if (path.empty())
      {
        return std::unexpected(ConfigError{
            .kind = ConfigErrorKind::PathUnavailable,
            .path = path,
            .message = description,
            .line = std::nullopt,
            .column = std::nullopt,
        });
      }

      return path;
    }
  } // namespace

  FixedConfigPathProvider::FixedConfigPathProvider(std::filesystem::path path)
      : mPath(std::move(path)) {}

  std::expected<std::filesystem::path, ConfigError>
  FixedConfigPathProvider::ConfigPath() const
  {
    return ValidatePath(mPath, "The configured CSON path is empty");
  }

  AppDataConfigPathProvider::AppDataConfigPathProvider(
      std::filesystem::path appDataDirectory)
      : mAppDataDirectory(std::move(appDataDirectory)) {}

  std::expected<std::filesystem::path, ConfigError>
  AppDataConfigPathProvider::ConfigPath() const
  {
    if (mAppDataDirectory.empty())
    {
      return std::unexpected(ConfigError{
          .kind = ConfigErrorKind::PathUnavailable,
          .path = mAppDataDirectory,
          .message = "The user configuration directory is unavailable",
          .line = std::nullopt,
          .column = std::nullopt,
      });
    }

    return mAppDataDirectory / "havRemote" / "havRemote.cson";
  }
} // namespace havremote::config
