// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_CORE_FILE_TIME_HPP
#define HAVREMOTE_CORE_FILE_TIME_HPP

#include <chrono>
#include <filesystem>

namespace havremote
{
  // The standard permits either the system-clock or UTC conversion pair
  template <typename FileClock = std::filesystem::file_time_type::clock,
            typename UtcClock = std::chrono::utc_clock,
            typename Duration>
  [[nodiscard]] auto FileTimeToSystemTime(
      const std::chrono::time_point<FileClock, Duration> value)
  {
    if constexpr (requires { FileClock::to_sys(value); })
    {
      return FileClock::to_sys(value);
    }
    else
    {
      return UtcClock::to_sys(FileClock::to_utc(value));
    }
  }

  template <typename FileClock = std::filesystem::file_time_type::clock,
            typename UtcClock = std::chrono::utc_clock,
            typename Duration>
  [[nodiscard]] auto SystemTimeToFileTime(
      const std::chrono::sys_time<Duration> value)
  {
    // Use native file precision before shifting epochs. An older file-clock
    // epoch may not fit in nanoseconds even when the input timestamp does.
    const auto nativeTime =
        std::chrono::time_point_cast<typename FileClock::duration>(value);
    if constexpr (requires { FileClock::from_sys(nativeTime); })
    {
      return FileClock::from_sys(nativeTime);
    }
    else
    {
      return FileClock::from_utc(UtcClock::from_sys(nativeTime));
    }
  }
}

#endif // HAVREMOTE_CORE_FILE_TIME_HPP
