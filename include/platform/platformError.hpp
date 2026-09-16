// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_PLATFORM_PLATFORM_ERROR_HPP
#define HAVREMOTE_INCLUDE_PLATFORM_PLATFORM_ERROR_HPP

#include <expected>
#include <string>

namespace havremote::platform
{
  enum class PlatformErrorCode
  {
    InvalidArgument,
    Conflict,
    NotFound,
    AccessDenied,
    UnsafeFileType,
    Cancelled,
    Io,
    Unavailable,
    OperatingSystem,
  };

  struct PlatformError
  {
    PlatformErrorCode code{PlatformErrorCode::OperatingSystem};
    std::string message;
    unsigned long nativeCode{};
  };

  template <typename T>
  using Result = std::expected<T, PlatformError>;
} // namespace havremote::platform

#endif // HAVREMOTE_INCLUDE_PLATFORM_PLATFORM_ERROR_HPP
