# SPDX-License-Identifier: MIT

# Windows MinGW-w64 toolchain for the "MinGW Makefiles" generator

if(NOT CMAKE_HOST_WIN32)
  message(FATAL_ERROR
    "The havRemote MinGW toolchain requires Windows cmake.exe. "
    "See docs/building.md for supported build configurations.")
endif()
if(DEFINED ENV{MSYSTEM})
  if(NOT "$ENV{MSYSTEM}" STREQUAL "")
    message(FATAL_ERROR
      "MSYSTEM is set to '$ENV{MSYSTEM}'. Use a clean PowerShell or cmd.exe "
      "environment for this toolchain. See docs/building.md.")
  endif()
endif()
if(DEFINED CMAKE_GENERATOR AND NOT CMAKE_GENERATOR STREQUAL "MinGW Makefiles")
  message(FATAL_ERROR
    "The havRemote MinGW toolchain requires the 'MinGW Makefiles' generator, not '${CMAKE_GENERATOR}'.")
endif()

set(_havremote_need_mingw_root TRUE)
if(DEFINED HAVREMOTE_MINGW_ROOT)
  if(NOT HAVREMOTE_MINGW_ROOT STREQUAL "")
    set(_havremote_need_mingw_root FALSE)
  endif()
endif()
if(_havremote_need_mingw_root)
  if(DEFINED ENV{HAVREMOTE_MINGW_ROOT})
    set(_havremote_mingw_root_env "$ENV{HAVREMOTE_MINGW_ROOT}")
    if(NOT _havremote_mingw_root_env STREQUAL "")
      set(HAVREMOTE_MINGW_ROOT "${_havremote_mingw_root_env}" CACHE PATH
        "Root of the x64 UCRT/POSIX/SEH MinGW GCC distribution" FORCE)
      set(_havremote_need_mingw_root FALSE)
    endif()
  endif()
endif()
if(_havremote_need_mingw_root)
  message(FATAL_ERROR
    "Set HAVREMOTE_MINGW_ROOT to the MinGW distribution's mingw64 directory. "
    "See the build instructions in README.md.")
endif()
unset(_havremote_need_mingw_root)
unset(_havremote_mingw_root_env)

cmake_path(CONVERT "${HAVREMOTE_MINGW_ROOT}" TO_CMAKE_PATH_LIST _havremote_mingw_root NORMALIZE)
set(HAVREMOTE_MINGW_ROOT "${_havremote_mingw_root}" CACHE PATH
  "Root of the x64 UCRT/POSIX/SEH MinGW GCC distribution" FORCE)
unset(_havremote_mingw_root)

# Propagate the cached toolchain root into compiler and feature-probe subprojects.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES HAVREMOTE_MINGW_ROOT)
list(REMOVE_DUPLICATES CMAKE_TRY_COMPILE_PLATFORM_VARIABLES)

if(NOT IS_DIRECTORY "${HAVREMOTE_MINGW_ROOT}/bin")
  message(FATAL_ERROR "HAVREMOTE_MINGW_ROOT has no bin directory: ${HAVREMOTE_MINGW_ROOT}")
endif()

set(_havremote_mingw_bin "${HAVREMOTE_MINGW_ROOT}/bin")
foreach(_tool IN ITEMS
    gcc.exe
    g++.exe
    windres.exe
    mingw32-make.exe
    ar.exe
    ranlib.exe
    strip.exe
    objcopy.exe
    objdump.exe)
  if(NOT EXISTS "${_havremote_mingw_bin}/${_tool}")
    message(FATAL_ERROR "Required MinGW tool is missing: ${_havremote_mingw_bin}/${_tool}")
  endif()
endforeach()

if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND
   NOT EXISTS "${_havremote_mingw_bin}/gdb.exe")
  message(WARNING
    "This MinGW distribution does not include bin/gdb.exe. Compilation will "
    "work, but the checked-in VS Code Debug configuration will not.")
endif()

set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(CMAKE_C_COMPILER "${_havremote_mingw_bin}/gcc.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_havremote_mingw_bin}/g++.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_RC_COMPILER "${_havremote_mingw_bin}/windres.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_MAKE_PROGRAM "${_havremote_mingw_bin}/mingw32-make.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_AR "${_havremote_mingw_bin}/ar.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${_havremote_mingw_bin}/ranlib.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_STRIP "${_havremote_mingw_bin}/strip.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJCOPY "${_havremote_mingw_bin}/objcopy.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJDUMP "${_havremote_mingw_bin}/objdump.exe" CACHE FILEPATH "" FORCE)

# Ensure compiler helper processes resolve the DLLs from this distribution, not
# an unrelated MSYS2 installation earlier on PATH.
set(ENV{PATH} "${_havremote_mingw_bin};$ENV{PATH}")

set(CMAKE_FIND_ROOT_PATH "${HAVREMOTE_MINGW_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Link feature probes to detect functions declared but absent from the runtime
set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)

unset(_havremote_mingw_bin)
