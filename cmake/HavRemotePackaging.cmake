# SPDX-License-Identifier: MIT

include_guard(GLOBAL)

function(_havremote_resolve_mingw_license
    output_variable cache_variable label basename path_regex)
  set("${cache_variable}" "" CACHE FILEPATH
    "Optional path to the ${label} distributed with the selected MinGW toolchain")
  mark_as_advanced("${cache_variable}")

  set(_license_path "${${cache_variable}}")
  if(NOT _license_path STREQUAL "")
    if(NOT EXISTS "${_license_path}" OR IS_DIRECTORY "${_license_path}")
      message(FATAL_ERROR
        "${cache_variable} must name the ${label}. The configured file does "
        "not exist: ${_license_path}")
    endif()
  else()
    foreach(_relative_candidate IN LISTS ARGN)
      if(EXISTS "${HAVREMOTE_MINGW_ROOT}/${_relative_candidate}" AND
         NOT IS_DIRECTORY "${HAVREMOTE_MINGW_ROOT}/${_relative_candidate}")
        set(_license_path
          "${HAVREMOTE_MINGW_ROOT}/${_relative_candidate}")
        break()
      endif()
    endforeach()
  endif()

  # Compatible distributions use several package-specific directory layouts.
  # If none of the common locations matched, search by the stable upstream
  # filename and constrain the containing directory to the relevant package.
  if(_license_path STREQUAL "")
    file(GLOB_RECURSE _license_candidates LIST_DIRECTORIES FALSE
      "${HAVREMOTE_MINGW_ROOT}/*/${basename}")
    list(SORT _license_candidates)
    foreach(_candidate IN LISTS _license_candidates)
      cmake_path(CONVERT "${_candidate}" TO_CMAKE_PATH_LIST
        _candidate_normalized NORMALIZE)
      string(TOLOWER "${_candidate_normalized}" _candidate_folded)
      if(_candidate_folded MATCHES "${path_regex}")
        set(_license_path "${_candidate_normalized}")
        break()
      endif()
    endforeach()
  endif()

  if(_license_path STREQUAL "")
    message(FATAL_ERROR
      "Could not locate the ${label} beneath HAVREMOTE_MINGW_ROOT "
      "('${HAVREMOTE_MINGW_ROOT}'). Set ${cache_variable} to the corresponding "
      "file supplied with this compatible MinGW distribution.")
  endif()

  message(STATUS "Using ${label}: ${_license_path}")
  set("${output_variable}" "${_license_path}" PARENT_SCOPE)
endfunction()

function(havremote_resolve_mingw_licenses)
  if(NOT MINGW OR NOT DEFINED HAVREMOTE_MINGW_ROOT OR
     HAVREMOTE_MINGW_ROOT STREQUAL "")
    message(FATAL_ERROR
      "MinGW license discovery requires HAVREMOTE_MINGW_ROOT")
  endif()

  _havremote_resolve_mingw_license(
    _gcc_gplv3
    HAVREMOTE_GCC_GPLV3_LICENSE_FILE
    "GCC GPLv3 license"
    "COPYING3"
    "/gcc[^/]*/copying3$"
    "licenses/gcc/COPYING3"
    "share/licenses/gcc/COPYING3"
    "share/licenses/gcc-libs/COPYING3")
  _havremote_resolve_mingw_license(
    _gcc_runtime_exception
    HAVREMOTE_GCC_RUNTIME_EXCEPTION_FILE
    "GCC Runtime Library Exception"
    "COPYING.RUNTIME"
    "/gcc[^/]*/copying\\.runtime$"
    "licenses/gcc/COPYING.RUNTIME"
    "share/licenses/gcc/COPYING.RUNTIME"
    "share/licenses/gcc-libs/COPYING.RUNTIME")
  _havremote_resolve_mingw_license(
    _mingw_runtime
    HAVREMOTE_MINGW_RUNTIME_LICENSE_FILE
    "MinGW-w64 runtime license"
    "COPYING.MinGW-w64-runtime.txt"
    "/mingw[^/]*/copying\\.mingw-w64-runtime\\.txt$"
    "licenses/mingw-w64/COPYING.MinGW-w64-runtime.txt"
    "share/licenses/mingw-w64/COPYING.MinGW-w64-runtime.txt"
    "share/licenses/mingw-w64-crt/COPYING.MinGW-w64-runtime.txt")
  _havremote_resolve_mingw_license(
    _winpthreads
    HAVREMOTE_WINPTHREADS_LICENSE_FILE
    "winpthreads license"
    "COPYING"
    "/[^/]*winpthread[^/]*/copying$"
    "licenses/winpthreads/COPYING"
    "share/licenses/winpthreads/COPYING"
    "share/licenses/mingw-w64-winpthreads/COPYING")

  set(HAVREMOTE_GCC_GPLV3_LICENSE "${_gcc_gplv3}" PARENT_SCOPE)
  set(HAVREMOTE_GCC_RUNTIME_EXCEPTION
    "${_gcc_runtime_exception}" PARENT_SCOPE)
  set(HAVREMOTE_MINGW_RUNTIME_LICENSE "${_mingw_runtime}" PARENT_SCOPE)
  set(HAVREMOTE_WINPTHREADS_LICENSE "${_winpthreads}" PARENT_SCOPE)
endfunction()

function(_havremote_resolve_git_commit label source_dir output_variable)
  if("${source_dir}" STREQUAL "" OR NOT IS_DIRECTORY "${source_dir}")
    message(FATAL_ERROR "Cannot record ${label} provenance: missing source directory")
  endif()

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" rev-parse --verify HEAD
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _commit
    ERROR_VARIABLE _error
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  string(STRIP "${_commit}" _commit)
  string(LENGTH "${_commit}" _commit_length)
  if(NOT _result EQUAL 0 OR _commit_length LESS 7 OR
     NOT _commit MATCHES "^[0-9a-fA-F]+$")
    message(FATAL_ERROR
      "Cannot record ${label} provenance from ${source_dir}: ${_error}")
  endif()
  set("${output_variable}" "${_commit}" PARENT_SCOPE)
endfunction()

function(_havremote_resolve_compiler_target output_variable)
  # GCC exposes its configured target through -dumpmachine. Other compilers
  # need not implement that option, so use CMake's detected target information.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    execute_process(
      COMMAND "${CMAKE_CXX_COMPILER}" -dumpmachine
      RESULT_VARIABLE _target_result
      OUTPUT_VARIABLE _compiler_target
      ERROR_VARIABLE _target_error
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _target_result EQUAL 0 OR _compiler_target STREQUAL "")
      message(FATAL_ERROR
        "Cannot record compiler target provenance: ${_target_error}")
    endif()
  elseif(NOT "${CMAKE_CXX_COMPILER_TARGET}" STREQUAL "")
    set(_compiler_target "${CMAKE_CXX_COMPILER_TARGET}")
  else()
    set(_target_architecture "${CMAKE_SYSTEM_PROCESSOR}")
    if(NOT "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" STREQUAL "")
      set(_target_architecture "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
    endif()
    set(_compiler_target "${CMAKE_SYSTEM_NAME}/${_target_architecture}")
  endif()
  set("${output_variable}" "${_compiler_target}" PARENT_SCOPE)
endfunction()

function(havremote_configure_third_party_provenance output_file)
  if(HAVREMOTE_CONFIGURE_ONLY)
    message(FATAL_ERROR
      "Third-party provenance is unavailable in HAVREMOTE_CONFIGURE_ONLY mode")
  endif()

  find_package(Git REQUIRED)
  _havremote_resolve_git_commit("wxWidgets" "${wxwidgets_SOURCE_DIR}"
    HAVREMOTE_WXWIDGETS_COMMIT)
  _havremote_resolve_git_commit("curl" "${curl_SOURCE_DIR}"
    HAVREMOTE_CURL_COMMIT)
  _havremote_resolve_git_commit("libssh2" "${libssh2_SOURCE_DIR}"
    HAVREMOTE_LIBSSH2_COMMIT)
  _havremote_resolve_git_commit("LibreSSL portable" "${libressl_SOURCE_DIR}"
    HAVREMOTE_LIBRESSL_COMMIT)
  _havremote_resolve_git_commit("LibreSSL OpenBSD import"
    "${libressl_SOURCE_DIR}/openbsd" HAVREMOTE_LIBRESSL_OPENBSD_COMMIT)
  _havremote_resolve_git_commit("havCSON" "${havcson_SOURCE_DIR}"
    HAVREMOTE_HAVCSON_COMMIT)

  if(HAVREMOTE_BUILD_TESTS)
    _havremote_resolve_git_commit("Catch2" "${catch2_SOURCE_DIR}"
      HAVREMOTE_CATCH2_COMMIT)
  else()
    set(HAVREMOTE_CATCH2_COMMIT "not fetched (HAVREMOTE_BUILD_TESTS=OFF)")
  endif()

  _havremote_resolve_compiler_target(HAVREMOTE_COMPILER_TARGET)

  configure_file(
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/thirdPartyProvenance.txt.in"
    "${output_file}"
    @ONLY
    NEWLINE_STYLE UNIX
  )
  message(STATUS "Recorded third-party provenance in ${output_file}")
endfunction()

function(havremote_enable_windows_packaging target)
  set(HAVREMOTE_AUDITED_PACKAGING OFF PARENT_SCOPE)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "Unknown packaging target: ${target}")
  endif()
  # PE auditing and symbol packaging require the native MinGW toolchain
  if(NOT HAVREMOTE_GNU_MINGW OR NOT CMAKE_HOST_WIN32 OR
     CMAKE_CROSSCOMPILING OR CMAKE_CONFIGURATION_TYPES OR
     HAVREMOTE_CONFIGURE_ONLY OR NOT DEFINED HAVREMOTE_MINGW_ROOT OR
     HAVREMOTE_MINGW_ROOT STREQUAL "")
    return()
  endif()

  if(NOT CMAKE_OBJDUMP OR NOT CMAKE_OBJCOPY OR NOT CMAKE_STRIP)
    message(FATAL_ERROR "The MinGW toolchain did not provide objdump, objcopy, and strip")
  endif()

  if(NOT HAVREMOTE_RUNTIME_DLL_TARGETS)
    message(FATAL_ERROR
      "The Windows runtime DLL target list is empty. Shared dependencies "
      "cannot be packaged or audited")
  endif()

  set(_split_debug_script
    "${PROJECT_SOURCE_DIR}/cmake/scripts/SplitPeDebug.cmake")
  set(_audit_pe_script
    "${PROJECT_SOURCE_DIR}/cmake/scripts/AuditPeImports.cmake")
  # TARGET-form custom commands do not accept DEPENDS. Make their scripts
  # explicit link inputs so changing either policy reliably reruns both
  # post-link steps with MinGW Makefiles.
  set_property(TARGET "${target}" APPEND PROPERTY LINK_DEPENDS
    "${_split_debug_script}"
    "${_audit_pe_script}")

  # Generate the runtime DLL manifest from target filenames for each configuration
  set(_runtime_dll_manifest
    "${CMAKE_BINARY_DIR}/havremote-runtime-dlls-$<CONFIG>.txt")
  set(_runtime_dll_manifest_content "")
  foreach(_runtime_target IN LISTS HAVREMOTE_RUNTIME_DLL_TARGETS)
    if(NOT TARGET "${_runtime_target}")
      message(FATAL_ERROR
        "Unknown Windows runtime dependency target: ${_runtime_target}")
    endif()
    string(APPEND _runtime_dll_manifest_content
      "$<TARGET_FILE_NAME:${_runtime_target}>\n")
  endforeach()
  file(GENERATE
    OUTPUT "${_runtime_dll_manifest}"
    CONTENT "${_runtime_dll_manifest_content}"
    TARGET "${target}"
    NEWLINE_STYLE UNIX
  )

  cmake_path(GET HAVREMOTE_MINGW_ROOT PARENT_PATH
    _havremote_mingw_variant_root)
  set(_local_build_path_manifest
    "${CMAKE_BINARY_DIR}/havremote-local-build-paths.txt")
  file(WRITE "${_local_build_path_manifest}"
    "${PROJECT_SOURCE_DIR}\n"
    "${CMAKE_BINARY_DIR}\n"
    "${HAVREMOTE_MINGW_ROOT}\n"
    "${_havremote_mingw_variant_root}\n")
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(_enforce_reproducible_paths ON)
  else()
    set(_enforce_reproducible_paths OFF)
  endif()

  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # Split symbols for every PE shipped in the portable package. Keep the
    # detached debug files beside the executable and use target-derived names
    # so upstream DLL naming changes cannot make this list stale. The package
    # payload is stripped and receives its GNU debug links before
    # the final audit examines both halves.
    set(_package_pe_targets "${target}" ${HAVREMOTE_RUNTIME_DLL_TARGETS})
    set(_symbol_archive_files "")
    set(_symbol_clean_files "")
    foreach(_pe_target IN LISTS _package_pe_targets)
      list(APPEND _symbol_archive_files
        "$<TARGET_FILE_NAME:${_pe_target}>.debug")
      list(APPEND _symbol_clean_files
        "$<TARGET_FILE:${_pe_target}>.debug")
    endforeach()

    # Target-dependent expressions are forbidden in POST_BUILD BYPRODUCTS,
    # but are supported by ADDITIONAL_CLEAN_FILES. This records every dynamic
    # upstream DLL symbol name and ensures `mingw32-make clean` cannot leave a
    # stale detached-symbol set behind.
    set_property(TARGET "${target}" APPEND PROPERTY ADDITIONAL_CLEAN_FILES
      ${_symbol_clean_files}
      "$<TARGET_FILE_DIR:${target}>/CMakeFiles/havremote-symbol-staging")

    # One idempotent splitter owns all package PEs. Dependency DLLs are not
    # necessarily relinked when the application is, so never extract debug
    # data from a DLL which already carries a valid GNU debug link.
    add_custom_command(TARGET "${target}" POST_BUILD
      COMMAND "${CMAKE_COMMAND}"
        "-DOBJDUMP=${CMAKE_OBJDUMP}"
        "-DOBJCOPY=${CMAKE_OBJCOPY}"
        "-DSTRIP=${CMAKE_STRIP}"
        "-DPE_FILE=$<TARGET_FILE:${target}>"
        "-DRUNTIME_DLL_MANIFEST=${_runtime_dll_manifest}"
        "-DRUNTIME_PACKAGE_DIR=$<TARGET_FILE_DIR:${target}>"
        -P "${_split_debug_script}"
      COMMENT "Splitting GNU debug information from the havRemote runtime bundle"
      VERBATIM
    )
    set(_require_debug_files ON)

    set(_symbol_archive
      "${CMAKE_BINARY_DIR}/packages/havRemote-${PROJECT_VERSION}-windows-x86_64-debug.zip")
    add_custom_target(package-symbols
      # Validate the complete bundle, including detached symbols, before archiving
      COMMAND "${CMAKE_COMMAND}"
        "-DOBJDUMP=${CMAKE_OBJDUMP}"
        "-DOBJCOPY=${CMAKE_OBJCOPY}"
        "-DSTRIP=${CMAKE_STRIP}"
        "-DPE_FILE=$<TARGET_FILE:${target}>"
        "-DRUNTIME_DLL_MANIFEST=${_runtime_dll_manifest}"
        "-DRUNTIME_PACKAGE_DIR=$<TARGET_FILE_DIR:${target}>"
        -P "${_split_debug_script}"
      COMMAND "${CMAKE_COMMAND}"
        "-DOBJDUMP=${CMAKE_OBJDUMP}"
        "-DPE_FILE=$<TARGET_FILE:${target}>"
        "-DRUNTIME_DLL_MANIFEST=${_runtime_dll_manifest}"
        "-DRUNTIME_PACKAGE_DIR=$<TARGET_FILE_DIR:${target}>"
        "-DTRANSLATION_SOURCE_DIR=${PROJECT_SOURCE_DIR}/translations"
        "-DTRANSLATION_PACKAGE_DIR=$<TARGET_FILE_DIR:${target}>/translations"
        "-DHELP_SOURCE_DIR=${PROJECT_SOURCE_DIR}/resources/help"
        "-DHELP_PACKAGE_DIR=$<TARGET_FILE_DIR:${target}>/help"
        "-DHAVREMOTE_HELP_VERSION=${PROJECT_VERSION}"
        "-DHELP_LOGO_SOURCE=${PROJECT_SOURCE_DIR}/resources/icons/havRemote.png"
        "-DHELP_LOGO_PACKAGE=$<TARGET_FILE_DIR:${target}>/icons/havRemote.png"
        "-DHELP_FAVICON_SOURCE=${PROJECT_SOURCE_DIR}/resources/icons/havRemote.ico"
        "-DHELP_FAVICON_PACKAGE=$<TARGET_FILE_DIR:${target}>/icons/havRemote.ico"
        "-DLICENSE_SOURCE=${PROJECT_SOURCE_DIR}/LICENSE"
        "-DLICENSE_PACKAGE=$<TARGET_FILE_DIR:${target}>/LICENSE.txt"
        "-DLOCAL_BUILD_PATH_MANIFEST=${_local_build_path_manifest}"
        "-DENFORCE_REPRODUCIBLE_PATHS=${_enforce_reproducible_paths}"
        -DREQUIRE_DEBUG_FILES=ON
        -P "${_audit_pe_script}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/packages"
      COMMAND "${CMAKE_COMMAND}" -E tar cf "${_symbol_archive}" --format=zip --
        ${_symbol_archive_files}
      DEPENDS "${target}" "${_split_debug_script}" "${_audit_pe_script}"
      WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target}>"
      COMMENT "Creating ${_symbol_archive}"
      VERBATIM
    )

    unset(_package_pe_targets)
    unset(_pe_target)
    unset(_symbol_archive_files)
    unset(_symbol_clean_files)
  else()
    set(_require_debug_files OFF)
  endif()

  # Audit after symbol splitting so both the shipped PEs and the detached
  # debug artifacts are checked, including on incremental application links.
  add_custom_command(TARGET "${target}" POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
      "-DOBJDUMP=${CMAKE_OBJDUMP}"
      "-DPE_FILE=$<TARGET_FILE:${target}>"
      "-DRUNTIME_DLL_MANIFEST=${_runtime_dll_manifest}"
      "-DRUNTIME_PACKAGE_DIR=$<TARGET_FILE_DIR:${target}>"
      "-DTRANSLATION_SOURCE_DIR=${PROJECT_SOURCE_DIR}/translations"
      "-DTRANSLATION_PACKAGE_DIR=$<TARGET_FILE_DIR:${target}>/translations"
      "-DHELP_SOURCE_DIR=${PROJECT_SOURCE_DIR}/resources/help"
      "-DHELP_PACKAGE_DIR=$<TARGET_FILE_DIR:${target}>/help"
      "-DHAVREMOTE_HELP_VERSION=${PROJECT_VERSION}"
      "-DHELP_LOGO_SOURCE=${PROJECT_SOURCE_DIR}/resources/icons/havRemote.png"
      "-DHELP_LOGO_PACKAGE=$<TARGET_FILE_DIR:${target}>/icons/havRemote.png"
      "-DHELP_FAVICON_SOURCE=${PROJECT_SOURCE_DIR}/resources/icons/havRemote.ico"
      "-DHELP_FAVICON_PACKAGE=$<TARGET_FILE_DIR:${target}>/icons/havRemote.ico"
      "-DLICENSE_SOURCE=${PROJECT_SOURCE_DIR}/LICENSE"
      "-DLICENSE_PACKAGE=$<TARGET_FILE_DIR:${target}>/LICENSE.txt"
      "-DLOCAL_BUILD_PATH_MANIFEST=${_local_build_path_manifest}"
      "-DENFORCE_REPRODUCIBLE_PATHS=${_enforce_reproducible_paths}"
      "-DREQUIRE_DEBUG_FILES=${_require_debug_files}"
      -P "${_audit_pe_script}"
    COMMENT "Auditing havRemote runtime DLLs, PE imports, symbols, manifest, and resources"
    VERBATIM
  )

  unset(_runtime_dll_manifest)
  unset(_runtime_dll_manifest_content)
  unset(_runtime_target)
  unset(_split_debug_script)
  unset(_audit_pe_script)
  unset(_enforce_reproducible_paths)
  unset(_havremote_mingw_variant_root)
  unset(_local_build_path_manifest)
  unset(_require_debug_files)
  set(HAVREMOTE_AUDITED_PACKAGING ON PARENT_SCOPE)
endfunction()
