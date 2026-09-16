# SPDX-License-Identifier: MIT

include_guard(GLOBAL)

include(FetchContent)
include("${CMAKE_CURRENT_LIST_DIR}/DependencyVersions.cmake")

# Configure-only mode validates the target graph without buildable dependencies
if(HAVREMOTE_CONFIGURE_ONLY)
  message(STATUS "HAVREMOTE_CONFIGURE_ONLY: dependency downloads are disabled")

  foreach(_target IN ITEMS
      CURL::libcurl
      libssh2::libssh2
      HavRemoteLibreSSL::Crypto
      HavRemoteLibreSSL::SSL
      OpenSSL::Crypto
      OpenSSL::SSL
      wx::base
      wx::core
      wx::aui
      Catch2::Catch2WithMain
      havCSON::havCSON)
    if(NOT TARGET "${_target}")
      add_library("${_target}" INTERFACE IMPORTED GLOBAL)
    endif()
  endforeach()
  return()
endif()

set(FETCHCONTENT_QUIET OFF)

# Runtime dependencies are DLLs shipped beside havRemote. The MinGW compiler
# support libraries use a separate static-runtime policy applied after these
# targets have been created.
set(BUILD_SHARED_LIBS ON CACHE BOOL "Build shared third-party libraries" FORCE)
set(BUILD_STATIC_LIBS OFF CACHE BOOL "Do not build static third-party libraries" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Disable dependency tests while populating" FORCE)

# wxWidgets
set(wxUSE_GUI ${HAVREMOTE_BUILD_APP} CACHE BOOL "Build GUI support for the application target" FORCE)
set(wxUSE_SECRETSTORE ${HAVREMOTE_BUILD_APP} CACHE BOOL
  "Use the operating system credential store in desktop builds" FORCE)
set(wxBUILD_SHARED ON CACHE BOOL "" FORCE)
set(wxBUILD_USE_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
set(wxBUILD_STRIPPED_RELEASE OFF CACHE BOOL "" FORCE)
set(wxBUILD_MONOLITHIC OFF CACHE BOOL "" FORCE)
set(wxBUILD_SAMPLES OFF CACHE STRING "" FORCE)
set(wxBUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(wxBUILD_TESTS OFF CACHE BOOL "" FORCE)
set(wxBUILD_INSTALL OFF CACHE BOOL "" FORCE)
set(wxBUILD_INSTALL_USE_SYMLINK OFF CACHE BOOL "" FORCE)
set(wxUSE_WEBVIEW OFF CACHE BOOL "" FORCE)
set(wxUSE_MEDIACTRL OFF CACHE BOOL "" FORCE)
set(wxUSE_LIBJPEG OFF CACHE STRING "" FORCE)
set(wxUSE_LIBTIFF OFF CACHE STRING "" FORCE)
set(wxUSE_LIBWEBP OFF CACHE STRING "" FORCE)
set(wxUSE_LUNASVG OFF CACHE STRING "" FORCE)
set(wxUSE_NANOSVG builtin CACHE STRING "" FORCE)
set(wxUSE_REGEX OFF CACHE STRING "" FORCE)
set(wxUSE_EXPAT OFF CACHE STRING "" FORCE)
foreach(_feature IN ITEMS XML XRC HTML WXHTML_HELP RICHTEXT STC)
  set("wxUSE_${_feature}" OFF CACHE BOOL "" FORCE)
endforeach()
unset(_feature)
# GCC's Windows PCH writer can fail for wxCore with full Release DWARF and
# reproducible-path maps enabled.
if(HAVREMOTE_GNU_MINGW AND CMAKE_BUILD_TYPE STREQUAL "Release")
  set(wxBUILD_PRECOMP OFF CACHE STRING "" FORCE)
endif()
FetchContent_Declare(wxwidgets
  GIT_REPOSITORY https://github.com/wxWidgets/wxWidgets.git
  GIT_TAG "${HAVREMOTE_WXWIDGETS_REF}"
  GIT_SHALLOW TRUE
  GIT_PROGRESS TRUE
  EXCLUDE_FROM_ALL
)

# curl supplies FTP/FTPS and HTTPS with only the required protocol surfaces
set(BUILD_STATIC_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(CURL_ENABLE_SSL ON CACHE BOOL "" FORCE)
set(CURL_USE_SCHANNEL OFF CACHE BOOL "" FORCE)
set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
if(WIN32)
  set(CURL_CA_NATIVE ON CACHE BOOL "Use the Windows certificate trust store" FORCE)
  set(CURL_CA_BUNDLE none CACHE STRING "No bundled certificate authority file" FORCE)
  set(CURL_CA_PATH none CACHE STRING "No build-host certificate authority directory" FORCE)
  set(CURL_CA_FALLBACK OFF CACHE BOOL "Do not use LibreSSL's default CA paths" FORCE)
  set(CURL_CA_BUNDLE_SET FALSE CACHE BOOL "No certificate authority file configured" FORCE)
  set(CURL_CA_PATH_SET FALSE CACHE BOOL "No certificate authority directory configured" FORCE)
endif()
if(APPLE)
  set(USE_APPLE_SECTRUST ON CACHE BOOL "Use the native certificate trust store" FORCE)
endif()
set(CURL_USE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(CURL_USE_WOLFSSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_GNUTLS OFF CACHE BOOL "" FORCE)
set(CURL_USE_RUSTLS OFF CACHE BOOL "" FORCE)
set(CURL_USE_PKGCONFIG OFF CACHE BOOL "" FORCE)
set(CURL_USE_CMAKECONFIG OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
foreach(_protocol IN ITEMS
    DICT FILE GOPHER IMAP IPFS LDAP LDAPS MQTT POP3 RTSP SMTP TELNET TFTP)
  set("CURL_DISABLE_${_protocol}" ON CACHE BOOL "" FORCE)
endforeach()
set(CURL_DISABLE_HTTP OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_FTP OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_DOH ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_PROXY ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_WEBSOCKETS ON CACHE BOOL "" FORCE)
set(CURL_ENABLE_SMB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(curl
  GIT_REPOSITORY https://github.com/curl/curl.git
  GIT_TAG "${HAVREMOTE_CURL_REF}"
  GIT_SHALLOW TRUE
  GIT_PROGRESS TRUE
  EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(wxwidgets)

# Windows shares fetched LibreSSL between curl and libssh2.
# Other platforms use their system OpenSSL.
if(WIN32)
set(LIBRESSL_APPS OFF CACHE BOOL "" FORCE)
set(LIBRESSL_TESTS OFF CACHE BOOL "" FORCE)
set(LIBRESSL_SKIP_INSTALL ON CACHE BOOL "" FORCE)
set(ENABLE_ASM ON CACHE BOOL "" FORCE)
# Some MinGW distributions expose a linkable syslog() compatibility symbol but no
# vsyslog(). LibreSSL's generic check treats that as a complete Unix syslog API
# and then its syslog_r shim fails to compile. Disable both probes to select
# the shim's no-op logging path.
if(MINGW)
  set(HAVE_GETENTROPY 0 CACHE INTERNAL
    "Use LibreSSL's BCrypt-based Windows getentropy implementation" FORCE)
  set(HAVE_SYSLOG 0 CACHE INTERNAL "MinGW has no complete syslog API" FORCE)
  set(HAVE_SYSLOG_R 0 CACHE INTERNAL "MinGW has no syslog_r API" FORCE)
endif()
# LibreSSL's update.sh imports the matching OpenBSD sources required by its
# portable Git tree. Run it during FetchContent preparation, before configuring.
set(_havremote_bash_hints)
if(DEFINED ENV{MSYS2_ROOT} AND NOT "$ENV{MSYS2_ROOT}" STREQUAL "")
  list(APPEND _havremote_bash_hints "$ENV{MSYS2_ROOT}/usr/bin")
endif()

# Search the MinGW root's ancestors for MSYS2's usr/bin
if(WIN32 AND DEFINED HAVREMOTE_MINGW_ROOT AND
   NOT HAVREMOTE_MINGW_ROOT STREQUAL "")
  set(_havremote_mingw_ancestor "${HAVREMOTE_MINGW_ROOT}")
  while(NOT _havremote_mingw_ancestor STREQUAL "")
    if(EXISTS "${_havremote_mingw_ancestor}/usr/bin/bash.exe")
      list(APPEND _havremote_bash_hints
        "${_havremote_mingw_ancestor}/usr/bin")
    endif()
    cmake_path(GET _havremote_mingw_ancestor PARENT_PATH
      _havremote_mingw_parent)
    if(_havremote_mingw_parent STREQUAL _havremote_mingw_ancestor)
      break()
    endif()
    set(_havremote_mingw_ancestor "${_havremote_mingw_parent}")
  endwhile()
endif()
if(DEFINED ENV{ProgramFiles} AND NOT "$ENV{ProgramFiles}" STREQUAL "")
  list(APPEND _havremote_bash_hints
    "$ENV{ProgramFiles}/Git/bin"
    "$ENV{ProgramFiles}/Git/usr/bin")
endif()
list(REMOVE_DUPLICATES _havremote_bash_hints)

find_program(HAVREMOTE_BASH_EXECUTABLE
  NAMES bash.exe bash
  HINTS ${_havremote_bash_hints}
  NO_CMAKE_FIND_ROOT_PATH
  REQUIRED
)
if(CMAKE_HOST_WIN32)
  execute_process(
    COMMAND "${HAVREMOTE_BASH_EXECUTABLE}" --noprofile --norc -c
      "test -x /usr/bin/cygpath && test -x /usr/bin/perl"
    RESULT_VARIABLE _havremote_bash_compatibility_result
    ERROR_VARIABLE _havremote_bash_compatibility_error
  )
  if(NOT _havremote_bash_compatibility_result EQUAL 0)
    message(FATAL_ERROR
      "HAVREMOTE_BASH_EXECUTABLE must be an MSYS2 or Git for Windows Bash "
      "with /usr/bin/cygpath and /usr/bin/perl. '${HAVREMOTE_BASH_EXECUTABLE}' "
      "is incompatible. ${_havremote_bash_compatibility_error}")
  endif()
  unset(_havremote_bash_compatibility_result)
  unset(_havremote_bash_compatibility_error)
endif()
unset(_havremote_bash_hints)
unset(_havremote_mingw_ancestor)
unset(_havremote_mingw_parent)
find_package(Git REQUIRED)
if(CMAKE_HOST_WIN32)
  set(_havremote_native_windows ON)
else()
  set(_havremote_native_windows OFF)
endif()
FetchContent_Declare(libressl
  GIT_REPOSITORY https://github.com/libressl/portable.git
  GIT_TAG "${HAVREMOTE_LIBRESSL_REF}"
  GIT_SHALLOW TRUE
  GIT_PROGRESS TRUE
  PATCH_COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE_DIR=<SOURCE_DIR>"
    "-DBASH_EXECUTABLE=${HAVREMOTE_BASH_EXECUTABLE}"
    "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
    "-DNATIVE_WINDOWS=${_havremote_native_windows}"
    "-DMINGW_ROOT=${HAVREMOTE_MINGW_ROOT}"
    -P "${CMAKE_CURRENT_LIST_DIR}/scripts/PrepareLibreSSL.cmake"
  EXCLUDE_FROM_ALL
)
unset(_havremote_native_windows)

FetchContent_MakeAvailable(libressl)

if(NOT TARGET crypto OR NOT TARGET ssl)
  message(FATAL_ERROR "Fetched LibreSSL did not define its expected crypto and ssl targets")
endif()
if(NOT TARGET HavRemoteLibreSSL::Crypto)
  # An imported proxy lets libssh2's install export reference the fetched
  # crypto target without exporting LibreSSL's build targets.
  add_library(HavRemoteLibreSSL::Crypto INTERFACE IMPORTED GLOBAL)
  set_property(TARGET HavRemoteLibreSSL::Crypto PROPERTY INTERFACE_LINK_LIBRARIES crypto)
endif()
if(NOT TARGET HavRemoteLibreSSL::SSL)
  add_library(HavRemoteLibreSSL::SSL INTERFACE IMPORTED GLOBAL)
  set_property(TARGET HavRemoteLibreSSL::SSL PROPERTY INTERFACE_LINK_LIBRARIES ssl)
endif()

set(HAVREMOTE_LIBRESSL_INCLUDE_DIR
  "${CMAKE_BINARY_DIR}/include"
  CACHE INTERNAL "LibreSSL include directory exposed to the OpenSSL shim" FORCE)
set(_havremote_libressl_version_file "${libressl_SOURCE_DIR}/VERSION")
if(NOT EXISTS "${_havremote_libressl_version_file}")
  message(FATAL_ERROR
    "Prepared LibreSSL sources do not contain ${_havremote_libressl_version_file}")
endif()
file(STRINGS "${_havremote_libressl_version_file}"
  _havremote_libressl_version LIMIT_COUNT 1)
string(STRIP "${_havremote_libressl_version}" _havremote_libressl_version)
if(NOT _havremote_libressl_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR
    "Prepared LibreSSL sources report an invalid version: '${_havremote_libressl_version}'")
endif()
set(HAVREMOTE_LIBRESSL_VERSION "${_havremote_libressl_version}"
  CACHE INTERNAL "LibreSSL compatibility version" FORCE)
unset(_havremote_libressl_version)
unset(_havremote_libressl_version_file)
endif()

# Package discovery overrides are scoped to the dependencies using LibreSSL
block(SCOPE_FOR VARIABLES
    PROPAGATE curl_SOURCE_DIR curl_BINARY_DIR curl_POPULATED)
  if(WIN32)
    list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/modules/libressl")
    include("${CMAKE_CURRENT_LIST_DIR}/modules/libressl/ConfigureCurl.cmake")
    havremote_configure_libressl_curl()
    set(CMAKE_FIND_PACKAGE_PREFER_CONFIG FALSE)
    set(CMAKE_DISABLE_FIND_PACKAGE_OpenSSL FALSE)
  endif()
  FetchContent_MakeAvailable(curl)
endblock()

set(BUILD_STATIC_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CRYPTO_BACKEND OpenSSL CACHE STRING "" FORCE)
set(ENABLE_ZLIB_COMPRESSION OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libssh2
  GIT_REPOSITORY https://github.com/libssh2/libssh2.git
  GIT_TAG "${HAVREMOTE_LIBSSH2_REF}"
  GIT_SHALLOW TRUE
  GIT_PROGRESS TRUE
  EXCLUDE_FROM_ALL
)

block(SCOPE_FOR VARIABLES
    PROPAGATE libssh2_SOURCE_DIR libssh2_BINARY_DIR libssh2_POPULATED)
  if(WIN32)
    list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/modules/libressl")
    set(CMAKE_DISABLE_FIND_PACKAGE_ZLIB TRUE)
    set(ZLIB_FOUND FALSE)
  endif()
  set(CMAKE_FIND_PACKAGE_PREFER_CONFIG FALSE)
  set(CMAKE_DISABLE_FIND_PACKAGE_OpenSSL FALSE)
  FetchContent_MakeAvailable(libssh2)
endblock()

if(NOT TARGET libssh2::libssh2)
  message(FATAL_ERROR "Fetched libssh2 did not define libssh2::libssh2")
endif()

# Collect and validate the shared-library targets included in the runtime bundle
if(WIN32)
  set(HAVREMOTE_RUNTIME_DLL_TARGETS)
  set(_havremote_runtime_dependencies
    wx::base CURL::libcurl libssh2::libssh2 ssl crypto)
  if(HAVREMOTE_BUILD_APP)
    list(APPEND _havremote_runtime_dependencies wx::core wx::aui)
  endif()
  foreach(_dependency_target IN LISTS _havremote_runtime_dependencies)
    if(NOT TARGET "${_dependency_target}")
      message(FATAL_ERROR
        "Required runtime dependency target is missing: ${_dependency_target}")
    endif()

    get_target_property(_concrete_target
      "${_dependency_target}" ALIASED_TARGET)
    if(NOT _concrete_target)
      set(_concrete_target "${_dependency_target}")
    endif()
    get_target_property(_dependency_type "${_concrete_target}" TYPE)
    if(NOT _dependency_type STREQUAL "SHARED_LIBRARY")
      message(FATAL_ERROR
        "Runtime dependency ${_dependency_target} resolved to "
        "${_dependency_type}. A shared library is required")
    endif()

    set_target_properties("${_concrete_target}" PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    list(APPEND HAVREMOTE_RUNTIME_DLL_TARGETS "${_concrete_target}")
  endforeach()
  list(REMOVE_DUPLICATES HAVREMOTE_RUNTIME_DLL_TARGETS)
  unset(_concrete_target)
  unset(_dependency_target)
  unset(_dependency_type)
  unset(_havremote_runtime_dependencies)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/HavRemoteHavCSON.cmake")

if(HAVREMOTE_BUILD_TESTS)
  # Catch2 is linked statically into test executables and is not shipped
  set(_havremote_saved_build_shared_libs "${BUILD_SHARED_LIBS}")
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  set(CATCH_INSTALL_EXTRAS ON CACHE BOOL "" FORCE)
  FetchContent_Declare(catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG "${HAVREMOTE_CATCH2_REF}"
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(catch2)
  set(BUILD_SHARED_LIBS "${_havremote_saved_build_shared_libs}"
    CACHE BOOL "Build shared third-party libraries" FORCE)
  unset(_havremote_saved_build_shared_libs)
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()
