# SPDX-License-Identifier: MIT

foreach(_required_variable IN ITEMS
    OBJDUMP OBJCOPY STRIP PE_FILE RUNTIME_DLL_MANIFEST RUNTIME_PACKAGE_DIR)
  if(NOT DEFINED ${_required_variable} OR "${${_required_variable}}" STREQUAL "")
    message(FATAL_ERROR "${_required_variable} is required")
  endif()
endforeach()
foreach(_tool IN ITEMS OBJDUMP OBJCOPY STRIP)
  if(NOT EXISTS "${${_tool}}")
    message(FATAL_ERROR "${_tool} does not exist: ${${_tool}}")
  endif()
endforeach()
if(NOT EXISTS "${PE_FILE}")
  message(FATAL_ERROR "Application PE does not exist: ${PE_FILE}")
endif()
if(NOT EXISTS "${RUNTIME_DLL_MANIFEST}")
  message(FATAL_ERROR
    "Runtime DLL manifest does not exist: ${RUNTIME_DLL_MANIFEST}")
endif()
if(NOT IS_DIRECTORY "${RUNTIME_PACKAGE_DIR}")
  message(FATAL_ERROR
    "Runtime package directory does not exist: ${RUNTIME_PACKAGE_DIR}")
endif()

function(_havremote_run description)
  execute_process(
    COMMAND ${ARGN}
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _error
  )
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR
      "${description} failed (${_result}): ${_output} ${_error}")
  endif()
endfunction()

function(_havremote_read_sections artifact output_variable)
  execute_process(
    COMMAND "${OBJDUMP}" -h "${artifact}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _headers
    ERROR_VARIABLE _error
  )
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Could not inspect ${artifact}: ${_error}")
  endif()
  set("${output_variable}" "${_headers}" PARENT_SCOPE)
endfunction()

function(_havremote_validate_debug_file debug_file)
  _havremote_read_sections("${debug_file}" _debug_headers)
  if(NOT _debug_headers MATCHES "\\.debug_info" OR
     NOT _debug_headers MATCHES "\\.debug_line")
    message(FATAL_ERROR
      "Detached symbols are not source-level debug information: ${debug_file}. "
      "Rebuild the PE-producing target before packaging.")
  endif()
endfunction()

function(_havremote_validate_debug_link pe_file debug_file)
  # `objdump --dwarf=links` follows .gnu_debuglink using the GNU search rules
  # and verifies its CRC. The command still exits successfully for a CRC
  # mismatch, so validate its diagnostics as well as its exit status. Run from
  # the PE directory using its basename: objdump otherwise probes its process
  # working directory first and can report a mismatch against an unrelated,
  # stale file having the same debuglink basename.
  cmake_path(GET pe_file PARENT_PATH _pe_directory)
  cmake_path(GET pe_file FILENAME _pe_basename)
  execute_process(
    COMMAND "${OBJDUMP}" --dwarf=links "${_pe_basename}"
    WORKING_DIRECTORY "${_pe_directory}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _error
  )
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR
      "Could not validate the GNU debug link in ${pe_file}: ${_error}")
  endif()

  set(_diagnostics "${_output}\n${_error}")
  string(REPLACE "\\" "/" _diagnostics "${_diagnostics}")
  string(TOLOWER "${_diagnostics}" _diagnostics)
  cmake_path(GET debug_file FILENAME _debug_basename)
  string(TOLOWER "${_debug_basename}" _debug_basename)

  string(FIND "${_diagnostics}" "found separate debug info file:"
    _found_link)
  string(FIND "${_diagnostics}" "${_debug_basename}" _found_name)
  string(FIND "${_diagnostics}" "crc does not match" _crc_mismatch)
  string(FIND "${_diagnostics}" "could not find separate debug file"
    _missing_link)
  if(_found_link EQUAL -1 OR _found_name EQUAL -1 OR
     NOT _crc_mismatch EQUAL -1 OR NOT _missing_link EQUAL -1)
    message(FATAL_ERROR
      "The GNU debug link in ${pe_file} does not match ${debug_file}: "
      "${_output} ${_error}")
  endif()
endfunction()

file(STRINGS "${RUNTIME_DLL_MANIFEST}" _runtime_dll_names ENCODING UTF-8)
set(_package_pe_files "${PE_FILE}")
foreach(_runtime_dll_name IN LISTS _runtime_dll_names)
  string(STRIP "${_runtime_dll_name}" _runtime_dll_name)
  if(_runtime_dll_name STREQUAL "")
    continue()
  endif()
  cmake_path(GET _runtime_dll_name FILENAME _runtime_dll_basename)
  if(NOT "${_runtime_dll_name}" STREQUAL "${_runtime_dll_basename}")
    message(FATAL_ERROR
      "Runtime DLL manifest entries must be basenames: ${_runtime_dll_name}")
  endif()
  list(APPEND _package_pe_files
    "${RUNTIME_PACKAGE_DIR}/${_runtime_dll_name}")
endforeach()

foreach(_pe_path IN LISTS _package_pe_files)
  if(NOT EXISTS "${_pe_path}")
    message(FATAL_ERROR "Package PE does not exist: ${_pe_path}")
  endif()
  set(_debug_file "${_pe_path}.debug")
  _havremote_read_sections("${_pe_path}" _headers)

  if(_headers MATCHES "\\.gnu_debuglink")
    if(NOT EXISTS "${_debug_file}")
      message(FATAL_ERROR
        "${_pe_path} is already stripped, but ${_debug_file} is missing. "
        "Clean and rebuild the Release targets before packaging symbols.")
    endif()
    _havremote_validate_debug_file("${_debug_file}")
    _havremote_validate_debug_link("${_pe_path}" "${_debug_file}")
    message(STATUS "Keeping existing detached symbols for ${_pe_path}")
  else()
    # Prepare both outputs beside the package on the same filesystem. The
    # original PE is not touched until symbol extraction, stripping, link
    # creation, and CRC validation have all succeeded on staged files.
    cmake_path(GET _pe_path FILENAME _pe_basename)
    cmake_path(GET _debug_file FILENAME _debug_basename)
    set(_transaction_directory
      "${RUNTIME_PACKAGE_DIR}/CMakeFiles/havremote-symbol-staging/${_pe_basename}")
    set(_staged_pe "${_transaction_directory}/${_pe_basename}")
    set(_staged_debug "${_transaction_directory}/${_debug_basename}")
    # A prior interrupted transaction is never an input to the next one.
    # Calls are serialized by the havRemote/package-symbols dependency chain,
    # so a deterministic per-PE directory is safe and cannot accumulate.
    file(REMOVE_RECURSE "${_transaction_directory}")
    file(MAKE_DIRECTORY "${_transaction_directory}")

    _havremote_run("Staging ${_pe_path}"
      "${CMAKE_COMMAND}" -E copy "${_pe_path}" "${_staged_pe}")
    _havremote_run("Extracting debug information from ${_pe_path}"
      "${OBJCOPY}" --only-keep-debug "${_pe_path}" "${_staged_debug}")
    _havremote_validate_debug_file("${_staged_debug}")

    _havremote_run("Stripping staged PE ${_pe_path}"
      "${STRIP}" --strip-debug --strip-unneeded "${_staged_pe}")
    _havremote_run("Adding the GNU debug link to staged PE ${_pe_path}"
      "${OBJCOPY}" "--add-gnu-debuglink=${_staged_debug}" "${_staged_pe}")
    _havremote_read_sections("${_staged_pe}" _staged_headers)
    if(NOT _staged_headers MATCHES "\\.gnu_debuglink" OR
       _staged_headers MATCHES "\\.debug_info" OR
       _staged_headers MATCHES "\\.debug_line")
      message(FATAL_ERROR
        "Staged PE was not stripped correctly: ${_staged_pe}")
    endif()
    _havremote_validate_debug_link("${_staged_pe}" "${_staged_debug}")

    # The debug file is committed first. If the second atomic rename fails,
    # the original unstripped PE remains usable and the new debug file still
    # corresponds to it. A later build can safely retry the transaction.
    file(RENAME "${_staged_debug}" "${_debug_file}"
      RESULT _debug_commit_result)
    if(NOT _debug_commit_result STREQUAL "0")
      message(FATAL_ERROR
        "Could not commit detached symbols ${_debug_file}: "
        "${_debug_commit_result}")
    endif()
    file(RENAME "${_staged_pe}" "${_pe_path}" RESULT _pe_commit_result)
    if(NOT _pe_commit_result STREQUAL "0")
      message(FATAL_ERROR
        "Could not atomically replace ${_pe_path} with its stripped image: "
        "${_pe_commit_result}")
    endif()

    _havremote_validate_debug_link("${_pe_path}" "${_debug_file}")
    file(REMOVE_RECURSE "${_transaction_directory}")
  endif()
endforeach()
