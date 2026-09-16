// SPDX-License-Identifier: MIT

#include "core/fileTime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <ratio>
#include <type_traits>

namespace
{
  using namespace std::chrono_literals;

  // Exercise both standard conversion choices on every host, including the
  // UTC path when the host's standard library uses direct system conversion.
  struct TestUtcClock
  {
    template <typename Duration>
    static auto from_sys(const std::chrono::sys_time<Duration> value)
    {
      return std::chrono::time_point<TestUtcClock, Duration>{
          value.time_since_epoch() + 27s};
    }

    template <typename Duration>
    static auto to_sys(const std::chrono::time_point<TestUtcClock, Duration> value)
    {
      return std::chrono::sys_time<Duration>{value.time_since_epoch() - 27s};
    }
  };

  struct TestUtcFileClock
  {
    using duration = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
    using time_point = std::chrono::time_point<TestUtcFileClock>;

    static constexpr auto EpochOffset = 11'644'473'600s;

    template <typename Duration>
    static auto from_utc(const std::chrono::time_point<TestUtcClock, Duration> value)
    {
      return std::chrono::time_point<TestUtcFileClock, Duration>{
          value.time_since_epoch() + EpochOffset};
    }

    template <typename Duration>
    static auto to_utc(const std::chrono::time_point<TestUtcFileClock, Duration> value)
    {
      return std::chrono::time_point<TestUtcClock, Duration>{
          value.time_since_epoch() - EpochOffset};
    }
  };

  struct TestSystemFileClock
  {
    using duration = std::chrono::nanoseconds;
    using time_point = std::chrono::time_point<TestSystemFileClock>;

    template <typename Duration>
    static auto from_sys(const std::chrono::sys_time<Duration> value)
    {
      return std::chrono::time_point<TestSystemFileClock, Duration>{
          value.time_since_epoch() + 100s};
    }

    template <typename Duration>
    static auto to_sys(const std::chrono::time_point<TestSystemFileClock, Duration> value)
    {
      return std::chrono::sys_time<Duration>{value.time_since_epoch() - 100s};
    }
  };
}

TEST_CASE("file timestamp conversion supports either standard clock interface")
{
  using namespace havremote;

  const auto value = std::chrono::sys_time<std::chrono::nanoseconds>{
      1'700'000'000s + 123'456'789ns};

  const auto direct = SystemTimeToFileTime<TestSystemFileClock>(value);

  CHECK(FileTimeToSystemTime(direct) == value);
  CHECK(direct.time_since_epoch() == value.time_since_epoch() + 100s);

  const auto viaUtc = SystemTimeToFileTime<TestUtcFileClock, TestUtcClock>(value);

  CHECK(FileTimeToSystemTime<TestUtcFileClock, TestUtcClock>(viaUtc) == value - 89ns);
  CHECK(viaUtc.time_since_epoch() ==
        std::chrono::duration_cast<TestUtcFileClock::duration>(value.time_since_epoch()) +
            TestUtcFileClock::EpochOffset + 27s);
  STATIC_REQUIRE(std::is_same_v<typename decltype(viaUtc)::duration,
                                TestUtcFileClock::duration>);
}

TEST_CASE("file timestamp epoch shifts preserve exact native ticks")
{
  using namespace havremote;

  for (const auto seconds : {0s, -1'000'000'000s, 1'700'000'000s})
  {
    const auto value = std::chrono::sys_time<std::chrono::nanoseconds>{
        seconds + 123'456'700ns};

    const auto fileTime = SystemTimeToFileTime<TestUtcFileClock, TestUtcClock>(value);

    CHECK(FileTimeToSystemTime<TestUtcFileClock, TestUtcClock>(fileTime) == value);
    CHECK(fileTime.time_since_epoch().count() ==
          (seconds.count() + TestUtcFileClock::EpochOffset.count() + 27) *
                  10'000'000LL +
              1'234'567LL);

    const auto nextTick = fileTime + TestUtcFileClock::duration{1};

    CHECK(SystemTimeToFileTime<TestUtcFileClock, TestUtcClock>(
              FileTimeToSystemTime<TestUtcFileClock, TestUtcClock>(nextTick)) == nextTick);
  }
}

TEST_CASE("native file timestamps round trip without losing checkpoint precision")
{
  using namespace havremote;

  using Clock = std::filesystem::file_time_type::clock;

  const auto systemTime = std::chrono::sys_time<std::chrono::nanoseconds>{
      1'700'000'000s + 123'456'700ns};

  const auto fileTime = std::chrono::time_point_cast<Clock::duration>(
      SystemTimeToFileTime(systemTime));

  CHECK(FileTimeToSystemTime(fileTime) == systemTime);
  CHECK(SystemTimeToFileTime(FileTimeToSystemTime(fileTime)) == fileTime);

  const auto nextTick = fileTime + Clock::duration{1};

  CHECK(SystemTimeToFileTime(FileTimeToSystemTime(nextTick)) == nextTick);
}
