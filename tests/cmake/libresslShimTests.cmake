# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 4.0)

foreach(_required IN ITEMS SOURCE_DIR TEST_BINARY_ROOT C_COMPILER)
  if(NOT DEFINED ${_required} OR NOT IS_ABSOLUTE "${${_required}}")
    message(FATAL_ERROR "${_required} must be an absolute path")
  endif()
endforeach()
if(NOT EXISTS "${C_COMPILER}" OR
   NOT EXISTS "${SOURCE_DIR}/tests/cmake/libresslShim/CMakeLists.txt")
  message(FATAL_ERROR "The shim fixture or its C compiler is unavailable")
endif()

string(RANDOM LENGTH 24 ALPHABET 0123456789abcdef _fixture_id)
set(_fixture "${TEST_BINARY_ROOT}/libressl-shim-${_fixture_id}")
if(EXISTS "${_fixture}")
  message(FATAL_ERROR "The shim fixture directory already exists: ${_fixture}")
endif()
file(MAKE_DIRECTORY "${_fixture}")

foreach(_configuration IN ITEMS Debug Release)
  set(_build "${_fixture}/${_configuration}")
  set(_configure "${CMAKE_COMMAND}"
    -S "${SOURCE_DIR}/tests/cmake/libresslShim" -B "${_build}"
    "-DSOURCE_DIR=${SOURCE_DIR}"
    "-DCMAKE_C_COMPILER:FILEPATH=${C_COMPILER}"
    "-DCMAKE_BUILD_TYPE:STRING=${_configuration}")
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

  execute_process(COMMAND ${_configure}
    RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _error TIMEOUT 60)
  file(WRITE "${_fixture}/${_configuration}-configure.log" "${_output}\n${_error}")
  if(NOT _result STREQUAL "0" OR
     "${_output}\n${_error}" MATCHES "Bad lib in library list|Unable to find OpenSSL libcrypto DLL")
    message(FATAL_ERROR "Shim configuration failed. Fixture: ${_fixture}\n${_output}\n${_error}")
  endif()

  file(STRINGS "${_build}/runtime-${_configuration}.txt" _runtime_files)
  list(GET _runtime_files 0 _crypto_file)
  list(GET _runtime_files 1 _ssl_file)
  if(EXISTS "${_crypto_file}" OR EXISTS "${_ssl_file}")
    message(FATAL_ERROR "The clean-build fixture unexpectedly contains a TLS library")
  endif()

  execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_build}"
      --config "${_configuration}" --target shim_consumer --parallel 2
    RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _error TIMEOUT 60)
  file(WRITE "${_fixture}/${_configuration}-build.log" "${_output}\n${_error}")
  if(NOT _result STREQUAL "0" OR NOT EXISTS "${_crypto_file}" OR NOT EXISTS "${_ssl_file}")
    message(FATAL_ERROR "Shim build or runtime path failed. Fixture: ${_fixture}\n${_output}\n${_error}")
  endif()

  execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${_build}"
      --build-config "${_configuration}" --output-on-failure
    RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _error TIMEOUT 30)
  file(WRITE "${_fixture}/${_configuration}-test.log" "${_output}\n${_error}")
  if(NOT _result STREQUAL "0")
    message(FATAL_ERROR "Shim consumer failed. Fixture: ${_fixture}\n${_output}\n${_error}")
  endif()
  message(STATUS "LibreSSL shim ${_configuration}: clean curl probes, TLS/SSH linking, runtime paths and exports passed")
endforeach()

file(REMOVE_RECURSE "${_fixture}")
