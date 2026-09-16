# SPDX-License-Identifier: MIT

foreach(_required IN ITEMS SOURCE_DIR BASH_EXECUTABLE GIT_EXECUTABLE NATIVE_WINDOWS)
  if(NOT DEFINED "${_required}" OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "PrepareLibreSSL.cmake requires ${_required}")
  endif()
endforeach()

execute_process(
  COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse HEAD
  RESULT_VARIABLE _git_result
  OUTPUT_VARIABLE _source_head
  ERROR_VARIABLE _git_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT _git_result EQUAL 0 OR _source_head STREQUAL "")
  message(FATAL_ERROR "Could not identify the LibreSSL portable HEAD: ${_git_error}")
endif()

if(IS_DIRECTORY "${SOURCE_DIR}/.git")
  # Keep the preparation marker outside the dependency worktree so FetchContent's Git
  # update logic never treats it as a source modification.
  set(_marker "${SOURCE_DIR}/.git/havremote-prepared-ref")
else()
  set(_marker "${SOURCE_DIR}/.havremote-prepared-ref")
endif()
set(_prepared_head "")
if(EXISTS "${_marker}")
  file(READ "${_marker}" _prepared_head)
  string(STRIP "${_prepared_head}" _prepared_head)
endif()

set(_generated_tree_complete TRUE)
foreach(_generated IN ITEMS
    VERSION
    crypto/VERSION
    crypto/crypto.sym
    include/openssl/opensslv.h
    ssl/VERSION
    tls/VERSION)
  if(NOT EXISTS "${SOURCE_DIR}/${_generated}")
    set(_generated_tree_complete FALSE)
  endif()
endforeach()

if(_prepared_head STREQUAL _source_head AND _generated_tree_complete)
  message(STATUS "LibreSSL sources already prepared for ${_source_head}")
  return()
endif()

message(STATUS "Preparing LibreSSL portable sources for ${_source_head}")
if(NATIVE_WINDOWS)
  # Pass the native path as a positional argument instead of interpolating it
  # into shell source. This keeps valid Windows path characters such as an
  # apostrophe literal and prevents the installation path from changing how
  # Bash parses the preparation command.
  if(DEFINED MINGW_ROOT AND NOT MINGW_ROOT STREQUAL "")
    set(_shell_command
      "mingw_bin=$(/usr/bin/cygpath -u -- \"$1/bin\") && PATH=\"$mingw_bin:/usr/bin:/bin:$PATH\" exec ./update.sh")
  else()
    # Add the preparation utilities while preserving the selected compiler environment
    set(_shell_command "PATH=\"/usr/bin:/bin:$PATH\" exec ./update.sh")
  endif()
  execute_process(
    COMMAND "${BASH_EXECUTABLE}" --noprofile --norc -c
      "${_shell_command}" havremote-prepare "${MINGW_ROOT}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE _prepare_result
    COMMAND_ECHO STDOUT
  )
else()
  execute_process(
    COMMAND "${BASH_EXECUTABLE}" --noprofile --norc ./update.sh
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE _prepare_result
    COMMAND_ECHO STDOUT
  )
endif()

if(NOT _prepare_result EQUAL 0)
  file(REMOVE "${_marker}")
  message(FATAL_ERROR "LibreSSL update.sh failed with exit code ${_prepare_result}")
endif()

foreach(_generated IN ITEMS
    VERSION
    crypto/VERSION
    crypto/crypto.sym
    include/openssl/opensslv.h
    ssl/VERSION
    tls/VERSION)
  if(NOT EXISTS "${SOURCE_DIR}/${_generated}")
    file(REMOVE "${_marker}")
    message(FATAL_ERROR "LibreSSL update.sh did not generate ${_generated}")
  endif()
endforeach()

file(WRITE "${_marker}" "${_source_head}\n")
message(STATUS "LibreSSL source preparation completed")
