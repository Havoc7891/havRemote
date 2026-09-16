# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 4.0)

# Run the preflight without configuring external dependencies.
# Set EXPECT_SUCCESS=OFF when testing an unsupported compiler.
foreach(_required IN ITEMS SOURCE_DIR TEST_BINARY_ROOT CXX_COMPILER)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "${_required} is required")
  endif()
  if(NOT IS_ABSOLUTE "${${_required}}")
    message(FATAL_ERROR "${_required} must be an absolute path")
  endif()
endforeach()
if(NOT EXISTS "${SOURCE_DIR}/cmake/HavRemoteToolchainChecks.cmake")
  message(FATAL_ERROR "SOURCE_DIR does not contain the compiler preflight")
endif()
if(NOT EXISTS "${CXX_COMPILER}")
  message(FATAL_ERROR "CXX_COMPILER does not exist: ${CXX_COMPILER}")
endif()
if(NOT DEFINED EXPECT_SUCCESS)
  set(EXPECT_SUCCESS ON)
endif()

file(TO_CMAKE_PATH "${SOURCE_DIR}" _source_dir)
string(RANDOM LENGTH 24 ALPHABET 0123456789abcdef _fixture_id)
set(_fixture "${TEST_BINARY_ROOT}/toolchain-checks-${_fixture_id}")
if(EXISTS "${_fixture}")
  message(FATAL_ERROR "The compiler-check fixture already exists: ${_fixture}")
endif()
file(MAKE_DIRECTORY "${_fixture}/source")
set(_project [=[
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 4.0)
project(HavRemoteCompilerChecksRegression LANGUAGES CXX)
include("@_source_dir@/cmake/HavRemoteToolchainChecks.cmake")
havremote_verify_cxx_capabilities()
]=])
string(CONFIGURE "${_project}" _project @ONLY)
file(WRITE "${_fixture}/source/CMakeLists.txt" "${_project}")

set(_command "${CMAKE_COMMAND}"
  -S "${_fixture}/source"
  -B "${_fixture}/build"
  "-DCMAKE_CXX_COMPILER:FILEPATH=${CXX_COMPILER}"
  -DCMAKE_BUILD_TYPE:STRING=Debug)
if(DEFINED TEST_GENERATOR AND NOT TEST_GENERATOR STREQUAL "")
  list(APPEND _command -G "${TEST_GENERATOR}")
endif()
if(DEFINED TEST_GENERATOR_PLATFORM AND NOT TEST_GENERATOR_PLATFORM STREQUAL "")
  list(APPEND _command -A "${TEST_GENERATOR_PLATFORM}")
endif()
if(DEFINED TEST_GENERATOR_TOOLSET AND NOT TEST_GENERATOR_TOOLSET STREQUAL "")
  list(APPEND _command -T "${TEST_GENERATOR_TOOLSET}")
endif()
if(DEFINED TEST_MAKE_PROGRAM AND NOT TEST_MAKE_PROGRAM STREQUAL "")
  list(APPEND _command "-DCMAKE_MAKE_PROGRAM:FILEPATH=${TEST_MAKE_PROGRAM}")
endif()

execute_process(
  COMMAND ${_command}
  RESULT_VARIABLE _result
  OUTPUT_VARIABLE _output
  ERROR_VARIABLE _error
  TIMEOUT 90)
set(_output "${_output}\n${_error}")
file(WRITE "${_fixture}/configure.log" "${_output}")
if(EXPECT_SUCCESS)
  if(NOT _result STREQUAL "0" OR
     NOT _output MATCHES "Verified C\\+\\+23 capabilities:")
    message(FATAL_ERROR
      "The compiler preflight did not accept ${CXX_COMPILER}. "
      "Fixture retained at ${_fixture}\n${_output}")
  endif()
  message(STATUS "Compiler preflight accepted ${CXX_COMPILER}")
else()
  if(_result STREQUAL "0" OR NOT _output MATCHES "cannot compile" OR
     NOT _output MATCHES "explicit-object")
    message(FATAL_ERROR
      "The compiler preflight did not reject ${CXX_COMPILER} with its "
      "C++23 capability diagnostic. Fixture retained at ${_fixture}\n${_output}")
  endif()
  message(STATUS "Compiler preflight rejected ${CXX_COMPILER} with the C++23 diagnostic")
endif()

# Only this invocation's uniquely created fixture is removed
file(REMOVE_RECURSE "${_fixture}")
