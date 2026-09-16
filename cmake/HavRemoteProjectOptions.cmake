# SPDX-License-Identifier: MIT

include_guard(GLOBAL)

# Static runtime options for GCC/MinGW builds
set(HAVREMOTE_GNU_MINGW OFF)
if(MINGW AND CMAKE_C_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(HAVREMOTE_GNU_MINGW ON)
endif()

function(_havremote_add_release_prefix_map path replacement)
  if(NOT HAVREMOTE_GNU_MINGW OR "${path}" STREQUAL "")
    return()
  endif()

  cmake_path(CONVERT "${path}" TO_CMAKE_PATH_LIST
    _havremote_cmake_prefix NORMALIZE)
  # GCC normalizes Windows paths before applying this option, so the canonical
  # CMake spelling covers both slash styles.
  set(_havremote_map_option
    "-ffile-prefix-map=${_havremote_cmake_prefix}=${replacement}")
  add_compile_options(
    "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:C,GNU>>:${_havremote_map_option}>"
    "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:CXX,GNU>>:${_havremote_map_option}>"
    "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:ASM,GNU>>:${_havremote_map_option}>"
  )
endfunction()

if(HAVREMOTE_GNU_MINGW)
  # Release binaries and detached symbols must not disclose the developer's
  # checkout or toolchain location. Keep Debug paths real so source-level F5
  # debugging continues to work without substitute-path configuration.
  # Retain normal DWARF information in Release objects: the package strips it
  # from shipped PEs and publishes it in the separate symbol archive.
  add_compile_options(
    "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:C,GNU>>:-g2>"
    "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:CXX,GNU>>:-g2>"
    "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:ASM,GNU>>:-g2>"
  )
  if(DEFINED HAVREMOTE_MINGW_ROOT AND NOT HAVREMOTE_MINGW_ROOT STREQUAL "")
    cmake_path(GET HAVREMOTE_MINGW_ROOT PARENT_PATH
      _havremote_mingw_variant_root)
  endif()
  _havremote_add_release_prefix_map(
    "${PROJECT_SOURCE_DIR}" "/havRemote/source")
  _havremote_add_release_prefix_map(
    "${CMAKE_BINARY_DIR}" "/havRemote/build")
  _havremote_add_release_prefix_map(
    "${_havremote_mingw_variant_root}" "/havRemote/toolchain")
  _havremote_add_release_prefix_map(
    "${HAVREMOTE_MINGW_ROOT}" "/havRemote/toolchain/mingw64")
  unset(_havremote_mingw_variant_root)
endif()

add_library(havremote_project_options INTERFACE)
target_compile_features(havremote_project_options INTERFACE cxx_std_23)
set_target_properties(havremote_project_options PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(havremote_project_options INTERFACE
    "$<$<COMPILE_LANGUAGE:CXX>:/utf-8>"
    "$<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>"
    "$<$<COMPILE_LANGUAGE:CXX>:/permissive->")
endif()
if(WIN32)
  target_compile_definitions(havremote_project_options INTERFACE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
  )
endif()

add_library(havremote_project_warnings INTERFACE)
if(MSVC)
  target_compile_options(havremote_project_warnings INTERFACE /W4)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  target_compile_options(havremote_project_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wformat=2
  )
endif()

function(havremote_apply_defaults target)
  target_link_libraries("${target}" PUBLIC havremote_project_options)
  target_link_libraries("${target}" PRIVATE havremote_project_warnings)
  set_target_properties("${target}" PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
  )
endfunction()

function(_havremote_resolve_mingw_static_archive archive output_variable)
  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" "-print-file-name=${archive}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _archive_path
    ERROR_VARIABLE _error
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  cmake_path(CONVERT "${_archive_path}" TO_CMAKE_PATH_LIST _archive_path NORMALIZE)
  if(NOT _result EQUAL 0 OR NOT IS_ABSOLUTE "${_archive_path}" OR
     NOT EXISTS "${_archive_path}")
    message(FATAL_ERROR
      "Could not resolve MinGW static runtime archive ${archive}: ${_error}")
  endif()
  set("${output_variable}" "${_archive_path}" PARENT_SCOPE)
endfunction()

function(_havremote_strip_mingw_archive_debug input_archive output_variable)
  if(NOT CMAKE_OBJCOPY OR NOT EXISTS "${CMAKE_OBJCOPY}")
    message(FATAL_ERROR
      "Cannot sanitize the MinGW static runtime because objcopy is unavailable")
  endif()
  if(NOT CMAKE_AR OR NOT EXISTS "${CMAKE_AR}")
    message(FATAL_ERROR
      "Cannot validate the sanitized MinGW static runtime because ar is unavailable")
  endif()

  set(_havremote_runtime_directory
    "${CMAKE_BINARY_DIR}/CMakeFiles/havremote-static-runtime")
  file(MAKE_DIRECTORY "${_havremote_runtime_directory}")
  cmake_path(GET input_archive FILENAME _havremote_archive_name)
  set(_havremote_sanitized_archive
    "${_havremote_runtime_directory}/${_havremote_archive_name}")
  set(_havremote_hash_marker
    "${_havremote_sanitized_archive}.sha256")
  set(_havremote_staged_archive
    "${_havremote_sanitized_archive}.tmp")
  set(_havremote_staged_marker
    "${_havremote_hash_marker}.tmp")
  set(_havremote_sanitizer_policy "strip-debug-v1")

  file(SHA256 "${input_archive}" _havremote_source_hash)

  # A valid marker includes hashes for both the source and sanitized archive.
  # This keeps configure idempotent without trusting a stale or externally
  # modified output merely because its source archive is unchanged.
  set(_havremote_cached_policy "")
  set(_havremote_cached_source_hash "")
  set(_havremote_cached_output_hash "")
  if(EXISTS "${_havremote_hash_marker}" AND
     EXISTS "${_havremote_sanitized_archive}")
    file(STRINGS "${_havremote_hash_marker}" _havremote_marker_lines
      ENCODING UTF-8)
    foreach(_havremote_marker_line IN LISTS _havremote_marker_lines)
      if(_havremote_marker_line MATCHES "^policy=(.+)$")
        set(_havremote_cached_policy "${CMAKE_MATCH_1}")
      elseif(_havremote_marker_line MATCHES
          "^source-sha256=([0-9a-fA-F]+)$")
        string(TOLOWER "${CMAKE_MATCH_1}"
          _havremote_cached_source_hash)
      elseif(_havremote_marker_line MATCHES
          "^sanitized-sha256=([0-9a-fA-F]+)$")
        string(TOLOWER "${CMAKE_MATCH_1}"
          _havremote_cached_output_hash)
      endif()
    endforeach()

    if(_havremote_cached_policy STREQUAL _havremote_sanitizer_policy AND
       _havremote_cached_source_hash STREQUAL _havremote_source_hash AND
       NOT _havremote_cached_output_hash STREQUAL "")
      file(SHA256 "${_havremote_sanitized_archive}"
        _havremote_existing_output_hash)
      if(_havremote_existing_output_hash STREQUAL
          _havremote_cached_output_hash)
        set("${output_variable}" "${_havremote_sanitized_archive}"
          PARENT_SCOPE)
        return()
      endif()
    endif()
  endif()

  # Work only on deterministic temporary files. An interrupted or failed
  # attempt never changes the last known-good archive and cannot accumulate
  # uniquely named staging files across configure runs.
  file(REMOVE
    "${_havremote_staged_archive}"
    "${_havremote_staged_marker}")

  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy
      "${input_archive}" "${_havremote_staged_archive}"
    RESULT_VARIABLE _havremote_copy_result
    ERROR_VARIABLE _havremote_copy_error
  )
  if(NOT _havremote_copy_result EQUAL 0)
    message(FATAL_ERROR
      "Could not copy ${_havremote_archive_name} for path-safe linking: "
      "${_havremote_copy_error}")
  endif()

  execute_process(
    COMMAND "${CMAKE_OBJCOPY}" --strip-debug
      "${_havremote_staged_archive}"
    RESULT_VARIABLE _havremote_strip_result
    ERROR_VARIABLE _havremote_strip_error
  )
  if(NOT _havremote_strip_result EQUAL 0)
    file(REMOVE "${_havremote_staged_archive}")
    message(FATAL_ERROR
      "Could not strip build-specific debug data from "
      "${_havremote_archive_name}: ${_havremote_strip_error}")
  endif()

  execute_process(
    COMMAND "${CMAKE_AR}" t "${_havremote_staged_archive}"
    RESULT_VARIABLE _havremote_archive_result
    ERROR_VARIABLE _havremote_archive_error
    OUTPUT_QUIET
  )
  if(NOT _havremote_archive_result EQUAL 0)
    file(REMOVE "${_havremote_staged_archive}")
    message(FATAL_ERROR
      "The sanitized ${_havremote_archive_name} is not a valid archive: "
      "${_havremote_archive_error}")
  endif()

  file(SHA256 "${_havremote_staged_archive}" _havremote_output_hash)
  file(WRITE "${_havremote_staged_marker}"
    "policy=${_havremote_sanitizer_policy}\n"
    "source-sha256=${_havremote_source_hash}\n"
    "sanitized-sha256=${_havremote_output_hash}\n")

  # Commit the marker first and the archive last. If either atomic replacement
  # fails, the prior sanitized archive remains intact. A marker/output mismatch
  # is detected and repaired by the next configure.
  file(RENAME "${_havremote_staged_marker}" "${_havremote_hash_marker}"
    RESULT _havremote_marker_commit_result)
  if(NOT _havremote_marker_commit_result STREQUAL "0")
    file(REMOVE "${_havremote_staged_archive}")
    message(FATAL_ERROR
      "Could not commit the hash marker for ${_havremote_archive_name}: "
      "${_havremote_marker_commit_result}")
  endif()
  file(RENAME "${_havremote_staged_archive}"
    "${_havremote_sanitized_archive}"
    RESULT _havremote_archive_commit_result)
  if(NOT _havremote_archive_commit_result STREQUAL "0")
    message(FATAL_ERROR
      "Could not atomically replace sanitized ${_havremote_archive_name}: "
      "${_havremote_archive_commit_result}")
  endif()

  set("${output_variable}" "${_havremote_sanitized_archive}" PARENT_SCOPE)
endfunction()

if(HAVREMOTE_GNU_MINGW)
  _havremote_resolve_mingw_static_archive(
    libstdc++.a _havremote_libstdcxx_archive)
  _havremote_resolve_mingw_static_archive(
    libwinpthread.a _havremote_winpthread_archive)
  _havremote_resolve_mingw_static_archive(libatomic.a _havremote_atomic_archive)
  _havremote_resolve_mingw_static_archive(libgcc.a _havremote_libgcc_archive)
  _havremote_resolve_mingw_static_archive(libgcc_eh.a _havremote_libgcc_eh_archive)

  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    _havremote_strip_mingw_archive_debug(
      "${_havremote_libstdcxx_archive}" _havremote_libstdcxx_archive)
    _havremote_strip_mingw_archive_debug(
      "${_havremote_winpthread_archive}" _havremote_winpthread_archive)
    _havremote_strip_mingw_archive_debug(
      "${_havremote_atomic_archive}" _havremote_atomic_archive)
    _havremote_strip_mingw_archive_debug(
      "${_havremote_libgcc_archive}" _havremote_libgcc_archive)
    _havremote_strip_mingw_archive_debug(
      "${_havremote_libgcc_eh_archive}" _havremote_libgcc_eh_archive)
    cmake_path(GET _havremote_libgcc_archive PARENT_PATH
      HAVREMOTE_MINGW_STATIC_RUNTIME_DIRECTORY)
  endif()

  add_library(havremote_mingw_libstdcxx_static STATIC IMPORTED GLOBAL)
  set_target_properties(havremote_mingw_libstdcxx_static PROPERTIES
    IMPORTED_LOCATION "${_havremote_libstdcxx_archive}")
  add_library(havremote_mingw_winpthread_static STATIC IMPORTED GLOBAL)
  set_target_properties(havremote_mingw_winpthread_static PROPERTIES
    IMPORTED_LOCATION "${_havremote_winpthread_archive}")
  add_library(havremote_mingw_atomic_static STATIC IMPORTED GLOBAL)
  set_target_properties(havremote_mingw_atomic_static PROPERTIES
    IMPORTED_LOCATION "${_havremote_atomic_archive}")

  # CMake emits the language standard-library fragment after the complete
  # direct and transitive link-library graph. Put the exact MinGW archives in
  # that terminal position so symbols introduced by any project or dependency
  # archive are resolved without making unrelated -l options prefer static
  # libraries. Keep the normal Windows system libraries after them because the
  # runtime archives can themselves reference Win32 APIs.
  set(CMAKE_C_STANDARD_LIBRARIES
    "\"${_havremote_atomic_archive}\" \"${_havremote_winpthread_archive}\" ${CMAKE_C_STANDARD_LIBRARIES}")
  set(CMAKE_CXX_STANDARD_LIBRARIES
    "\"${_havremote_libstdcxx_archive}\" \"${_havremote_atomic_archive}\" \"${_havremote_libgcc_eh_archive}\" \"${_havremote_winpthread_archive}\" ${CMAKE_CXX_STANDARD_LIBRARIES}")

  unset(_havremote_libstdcxx_archive)
  unset(_havremote_winpthread_archive)
  unset(_havremote_atomic_archive)
  unset(_havremote_libgcc_archive)
  unset(_havremote_libgcc_eh_archive)
endif()

function(havremote_enforce_static_mingw_threads)
  if(NOT HAVREMOTE_GNU_MINGW OR NOT TARGET Threads::Threads)
    return()
  endif()

  # FindThreads represents MinGW POSIX threads as -pthread/-lpthread. Replace
  # only its link-side selection. Any thread-related compile options remain.
  foreach(_property IN ITEMS INTERFACE_LINK_LIBRARIES INTERFACE_LINK_OPTIONS)
    get_target_property(_thread_items Threads::Threads "${_property}")
    if(NOT _thread_items)
      set(_thread_items)
    endif()
    set(_filtered_items)
    foreach(_item IN LISTS _thread_items)
      if(NOT _item MATCHES "pthread")
        list(APPEND _filtered_items "${_item}")
      endif()
    endforeach()
    set_property(TARGET Threads::Threads PROPERTY
      "${_property}" "${_filtered_items}")
  endforeach()
  set_property(TARGET Threads::Threads APPEND PROPERTY
    INTERFACE_LINK_LIBRARIES havremote_mingw_winpthread_static)
endfunction()

# Share these flags with the configure-time C++ capability link probe
function(havremote_mingw_runtime_link_options output_variable)
  set("${output_variable}"
    -static-libgcc
    -no-pthread
    $<$<LINK_LANGUAGE:CXX>:-nostdlib++>
    LINKER:--no-undefined
    PARENT_SCOPE)
endfunction()

function(havremote_apply_mingw_static_runtime target)
  if(HAVREMOTE_GNU_MINGW)
    if(NOT TARGET "${target}")
      message(FATAL_ERROR "Cannot apply MinGW runtime policy to ${target}")
    endif()
    if(HAVREMOTE_MINGW_STATIC_RUNTIME_DIRECTORY)
      target_link_directories("${target}" BEFORE PRIVATE
        "${HAVREMOTE_MINGW_STATIC_RUNTIME_DIRECTORY}")
    endif()
    havremote_mingw_runtime_link_options(_runtime_link_options)
    target_link_options("${target}" PRIVATE ${_runtime_link_options})
    # MinGW POSIX GCC specs normally append dynamic libstdc++ and pthread
    # selections. Disable those defaults. The exact static archives are in the
    # terminal C/C++ standard-library fragments configured above.
  endif()
endfunction()
