# SPDX-License-Identifier: MIT

include_guard(GLOBAL)

# Compile and link the required C++23 facilities without executing the probe
function(havremote_verify_cxx_capabilities)
  set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
  find_package(Threads REQUIRED)
  set(_cxx_link_options)
  set(_cxx_compile_options)
  if(MSVC)
    list(APPEND _cxx_compile_options /utf-8 /Zc:__cplusplus /permissive-)
  endif()
  if(HAVREMOTE_GNU_MINGW)
    # Use the same static runtime selection as application and dependency
    # targets. Archive availability is checked by HavRemoteProjectOptions.
    havremote_mingw_runtime_link_options(_cxx_link_options)
    if(HAVREMOTE_MINGW_STATIC_RUNTIME_DIRECTORY)
      list(PREPEND _cxx_link_options
        "-L${HAVREMOTE_MINGW_STATIC_RUNTIME_DIRECTORY}")
    endif()
  endif()
  try_compile(_cxx_capabilities_ok
    SOURCE_FROM_CONTENT capabilities.cpp [=[
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>
#include "core/fileTime.hpp"

#if defined(_WIN32)
#if !defined(_M_X64) && !defined(__x86_64__)
#error The Windows application currently requires an x64 target
#endif
static_assert(sizeof(void*) == 8, "Expected a 64-bit Windows target");
#endif

int main()
{
  std::expected<int, std::string> value{42};
  std::expected<void, std::string> failure{std::unexpected(std::string{"error"})};
  const auto text = std::format("{}", value.value());
  int number{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
  double decimal{};
  const char decimalText[]{"1.5"};
  const auto parsedDecimal = std::from_chars(decimalText, decimalText + 3, decimal);
  std::vector<int> values{number, 1};
  std::ranges::sort(std::span{values});

  // Match the captured recursive lambdas used by the site manager model.
  unsigned visited{};
  const auto Visit = [&](this const auto &self, const unsigned depth) -> void {
    ++visited;
    if (depth != 0U)
    {
      self(depth - 1U);
    }
  };
  Visit(2U);

  std::error_code error;
  const auto path = std::filesystem::current_path(error);
  const auto modified = std::filesystem::last_write_time(path, error);
  const auto timestamp = havremote::FileTimeToSystemTime(modified);
  const auto fileTime = havremote::SystemTimeToFileTime(timestamp);
  (void)fileTime;

  std::atomic<std::uint64_t> count{};
  std::mutex mutex;
  std::condition_variable_any ready;
  std::jthread worker([&](std::stop_token stop) {
    std::stop_callback onStop(stop, [&] { ready.notify_all(); });
    std::unique_lock lock{mutex};
    ready.wait(lock, stop, [] { return false; });
    ready.wait_for(lock, stop, std::chrono::milliseconds{1}, [] { return false; });
    count.fetch_add(1);
  });
  worker.request_stop();
  worker.join();
  return failure.has_value() || failure.error() != "error" ||
         parsed.ec != std::errc{} || parsedDecimal.ec != std::errc{} ||
         decimal != 1.5 || count.load() != 1 || visited != 3U;
}
]=]
    NO_CACHE
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CMAKE_FLAGS
      "-DCMAKE_CXX_STANDARD_LIBRARIES:STRING=${CMAKE_CXX_STANDARD_LIBRARIES}"
      "-DINCLUDE_DIRECTORIES=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../include"
    COMPILE_DEFINITIONS ${_cxx_compile_options}
    LINK_OPTIONS ${_cxx_link_options}
    LINK_LIBRARIES Threads::Threads
    OUTPUT_VARIABLE _cxx_capabilities_output
  )
  if(NOT _cxx_capabilities_ok)
    message(FATAL_ERROR
      "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} cannot compile "
      "and link havRemote's required C++23 facilities (explicit-object "
      "recursive lambdas, std::expected, "
      "std::format, stoppable threads, filesystem, ranges, atomics, and "
      "numeric conversion). Select a compiler and standard library that "
      "provide these features.\n${_cxx_capabilities_output}")
  endif()
  message(STATUS
    "Verified C++23 capabilities: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endfunction()
