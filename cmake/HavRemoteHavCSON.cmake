# SPDX-License-Identifier: MIT

include_guard(GLOBAL)
include(FetchContent)

# Consume the published header without enabling any upstream example/test build
FetchContent_Declare(havcson
  GIT_REPOSITORY https://github.com/Havoc7891/havCSON.git
  GIT_TAG "${HAVREMOTE_HAVCSON_REF}"
  GIT_SHALLOW TRUE
  GIT_PROGRESS TRUE
  SOURCE_SUBDIR havremote-header-only
)
FetchContent_MakeAvailable(havcson)
if(NOT EXISTS "${havcson_SOURCE_DIR}/havCSON.hpp")
  message(FATAL_ERROR "Fetched havCSON ref does not contain havCSON.hpp")
endif()

add_library(havcson_headers INTERFACE)
add_library(havCSON::havCSON ALIAS havcson_headers)
target_include_directories(havcson_headers SYSTEM INTERFACE "${havcson_SOURCE_DIR}")
target_compile_features(havcson_headers INTERFACE cxx_std_23)

# Verify havCSON's version and required APIs on every configure
set(_havremote_havcson_probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles/havcson-version-probe")
file(MAKE_DIRECTORY "${_havremote_havcson_probe_dir}")
file(WRITE "${_havremote_havcson_probe_dir}/version.cpp" [=[
#include <havCSON.hpp>
static_assert(havCSON::VersionMajor == 0 && havCSON::VersionMinor == 5,
              "havRemote requires the havCSON 0.5.x API");
int main()
{
  havCSON::ParseOptions options;
  options.trackSourceLocations = true;
  options.maxDepth = 256;
  options.maxInputBytes = 1024;
  havCSON::Value value;
  havCSON::Error error;
  (void)havCSON::Parse("count: 1", value, &error, options);
  auto count = havCSON::ValueView(value).Member("count").AsInteger<int>(0, 2);
  auto document = havCSON::MakeLossless(value);
  havCSON::LosslessDocumentEditor editor(document);
  (void)editor.RenameMember({}, "count", "total", &error);
  (void)document.Source();
  (void)error.operation;
  (void)error.systemError;
  (void)havCSON::ErrorCode::IoError;
  return count && havCSON::ValidateLosslessTree(document, &error) ? 0 : 1;
}
]=])
unset(HAVREMOTE_HAVCSON_COMPATIBLE CACHE)
set(_havremote_havcson_probe_options)
if(WIN32)
  list(APPEND _havremote_havcson_probe_options
    -DUNICODE -D_UNICODE -DNOMINMAX
    -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00)
endif()
if(MSVC)
  list(APPEND _havremote_havcson_probe_options /utf-8 /Zc:__cplusplus /permissive-)
endif()
try_compile(HAVREMOTE_HAVCSON_COMPATIBLE
  SOURCES "${_havremote_havcson_probe_dir}/version.cpp"
  NO_CACHE
  COMPILE_DEFINITIONS ${_havremote_havcson_probe_options}
  CMAKE_FLAGS
    "-DCMAKE_CXX_STANDARD=23"
    "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
    "-DCMAKE_CXX_EXTENSIONS=OFF"
    "-DINCLUDE_DIRECTORIES=${havcson_SOURCE_DIR}"
  OUTPUT_VARIABLE _havremote_havcson_probe_output
)
if(NOT HAVREMOTE_HAVCSON_COMPATIBLE)
  message(FATAL_ERROR
    "Could not compile havCSON ref '${HAVREMOTE_HAVCSON_REF}' with its required "
    "0.5.x API using ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}:\n"
    "${_havremote_havcson_probe_output}")
endif()
unset(_havremote_havcson_probe_dir)
unset(_havremote_havcson_probe_output)
unset(_havremote_havcson_probe_options)
