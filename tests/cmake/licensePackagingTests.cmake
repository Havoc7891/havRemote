# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 4.0)

foreach(_required IN ITEMS SOURCE_DIR TEST_BINARY_ROOT)
  if(NOT DEFINED ${_required} OR NOT IS_ABSOLUTE "${${_required}}")
    message(FATAL_ERROR "${_required} must be an absolute path")
  endif()
endforeach()
if(NOT EXISTS "${SOURCE_DIR}/LICENSE" OR
   NOT EXISTS "${SOURCE_DIR}/cmake/scripts/SyncHelp.cmake")
  message(FATAL_ERROR "SOURCE_DIR does not contain the license and help staging script")
endif()
if(EXISTS "${SOURCE_DIR}/resources/help/LICENSE")
  message(FATAL_ERROR "The offline help must not contain an extensionless LICENSE")
endif()

file(SHA256 "${SOURCE_DIR}/LICENSE" _license_hash)
string(RANDOM LENGTH 24 ALPHABET 0123456789abcdef _fixture_id)
set(_fixture "${TEST_BINARY_ROOT}/license-packaging-${_fixture_id}")
if(EXISTS "${_fixture}")
  message(FATAL_ERROR "The license fixture already exists: ${_fixture}")
endif()
file(MAKE_DIRECTORY "${_fixture}/staged/help")

function(_run step)
  execute_process(COMMAND ${ARGN}
    RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _error
    TIMEOUT 30)
  file(WRITE "${_fixture}/${step}.log" "${_output}\n${_error}")
  if(NOT _result STREQUAL "0")
    message(FATAL_ERROR
      "${step} failed. Fixture retained at ${_fixture}\n${_output}\n${_error}")
  endif()
endfunction()

function(_check_license path)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "License missing: ${path}")
  endif()
  file(SHA256 "${path}" _actual_hash)
  if(NOT _actual_hash STREQUAL _license_hash)
    message(FATAL_ERROR "License differs from the repository LICENSE: ${path}")
  endif()
endfunction()

# Exercise incremental staging with the old filename already present.
file(WRITE "${_fixture}/staged/help/LICENSE" "stale license\n")
_run(stage-help "${CMAKE_COMMAND}"
  "-DSOURCE_DIR=${SOURCE_DIR}/resources/help"
  "-DDESTINATION_DIR=${_fixture}/staged/help"
  "-DALLOWED_ROOT=${_fixture}"
  -DHAVREMOTE_HELP_VERSION=1.2.3
  -P "${SOURCE_DIR}/cmake/scripts/SyncHelp.cmake")
_check_license("${SOURCE_DIR}/resources/help/LICENSE.txt")
_check_license("${_fixture}/staged/help/LICENSE.txt")
if(EXISTS "${_fixture}/staged/help/LICENSE")
  message(FATAL_ERROR "Help staging retained the extensionless LICENSE")
endif()
foreach(_language IN ITEMS en de)
  file(READ "${_fixture}/staged/help/${_language}/index.html" _page)
  if(NOT _page MATCHES "href=\"\\.\\./LICENSE\\.txt\"" OR
     _page MATCHES "href=\"\\.\\./LICENSE\"")
    message(FATAL_ERROR "${_language} help must link to ../LICENSE.txt")
  endif()
  if(NOT _page MATCHES "Version 1\\.2\\.3" OR
     _page MATCHES "@HAVREMOTE_HELP_VERSION@")
    message(FATAL_ERROR "${_language} help was not versioned during staging")
  endif()
endforeach()

foreach(_platform IN ITEMS windows unix)
  if(_platform STREQUAL "windows")
    set(_rules_file "${SOURCE_DIR}/CMakeLists.txt")
  else()
    set(_rules_file "${SOURCE_DIR}/cmake/HavRemoteUnixPackaging.cmake")
  endif()
  # Execute the real root-license install declaration in isolation, without
  # configuring the application's external dependencies.
  file(READ "${_rules_file}" _rules_source)
  string(REGEX MATCHALL "install\\([^)]*\\)" _install_rules "${_rules_source}")
  set(_license_rules)
  foreach(_rule IN LISTS _install_rules)
    if(_rule MATCHES "[ \t\r\n]\"?(\\$\\{PROJECT_SOURCE_DIR\\}/)?LICENSE\"?[ \t\r\n]")
      list(APPEND _license_rules "${_rule}")
    endif()
  endforeach()
  list(LENGTH _license_rules _rule_count)
  if(NOT _rule_count EQUAL 1)
    message(FATAL_ERROR
      "Expected one repository-license install declaration in ${_rules_file}")
  endif()

  set(_source "${_fixture}/${_platform}/source")
  set(_build "${_fixture}/${_platform}/build")
  set(_install "${_fixture}/${_platform}/install")
  file(MAKE_DIRECTORY "${_source}")
  file(COPY "${SOURCE_DIR}/LICENSE" "${SOURCE_DIR}/thirdPartyNotices.md"
    DESTINATION "${_source}")
  set(_project [=[
cmake_minimum_required(VERSION 4.0)
project(HavRemoteLicensePackagingRegression LANGUAGES NONE)
set(PROJECT_SOURCE_DIR "@SOURCE_DIR@")
@_license_rules@
install(DIRECTORY "@_fixture@/staged/help/"
  DESTINATION help COMPONENT Runtime)
]=])
  string(CONFIGURE "${_project}" _project @ONLY)
  file(WRITE "${_source}/CMakeLists.txt" "${_project}")
  set(_configure "${CMAKE_COMMAND}" -S "${_source}" -B "${_build}")
  foreach(_option IN ITEMS GENERATOR GENERATOR_PLATFORM GENERATOR_TOOLSET)
    if(DEFINED TEST_${_option} AND NOT TEST_${_option} STREQUAL "")
      if(_option STREQUAL "GENERATOR")
        list(APPEND _configure -G "${TEST_${_option}}")
      elseif(_option STREQUAL "GENERATOR_PLATFORM")
        list(APPEND _configure -A "${TEST_${_option}}")
      else()
        list(APPEND _configure -T "${TEST_${_option}}")
      endif()
    endif()
  endforeach()
  if(DEFINED TEST_MAKE_PROGRAM AND NOT TEST_MAKE_PROGRAM STREQUAL "")
    list(APPEND _configure "-DCMAKE_MAKE_PROGRAM:FILEPATH=${TEST_MAKE_PROGRAM}")
  endif()
  _run("${_platform}-configure" ${_configure})
  _run("${_platform}-install" "${CMAKE_COMMAND}" --install "${_build}"
    --prefix "${_install}" --config Release --component Runtime)
  _check_license("${_install}/LICENSE.txt")
  _check_license("${_install}/help/LICENSE.txt")
  if(EXISTS "${_install}/LICENSE" OR EXISTS "${_install}/help/LICENSE")
    message(FATAL_ERROR "${_platform} installation contains an extensionless LICENSE")
  endif()
endforeach()

_check_license("${SOURCE_DIR}/LICENSE")
message(STATUS "License packaging passed: both help languages and Windows/Unix install rules")
# Only this invocation's uniquely created fixture is removed.
file(REMOVE_RECURSE "${_fixture}")
