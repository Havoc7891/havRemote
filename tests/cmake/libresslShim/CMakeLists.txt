# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 4.0)
project(LibreSSLShimRegression LANGUAGES C)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime")
set(CMAKE_DEBUG_POSTFIX "-debug")
set(CMAKE_RELEASE_POSTFIX "-release")

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/crypto.c" [=[
#ifdef _WIN32
__declspec(dllexport)
#endif
int CryptoValue(void) { return 42; }
]=])
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ssh.c" [=[
#include <provider.h>
#ifdef _WIN32
__declspec(dllexport)
#endif
int SshValue(void) { return CryptoValue(); }
]=])
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ssl.c" [=[
#include <provider.h>
#ifdef _WIN32
__declspec(dllexport)
#endif
int SslValue(void) { return CryptoValue(); }
]=])
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/curl.c" [=[
#include <provider.h>
#ifdef _WIN32
__declspec(dllexport)
#endif
int CurlValue(void) { return SslValue() + CryptoValue(); }
]=])
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/main.c" [=[
int SshValue(void);
int CurlValue(void);
int main(void) { return SshValue() == 42 && CurlValue() == 84 ? 0 : 1; }
]=])

set(HAVREMOTE_LIBRESSL_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/include")
set(HAVREMOTE_LIBRESSL_VERSION "4.3.2")
file(MAKE_DIRECTORY "${HAVREMOTE_LIBRESSL_INCLUDE_DIR}/openssl")
file(WRITE "${HAVREMOTE_LIBRESSL_INCLUDE_DIR}/provider.h"
  "\tint CryptoValue(void);\n\tint SslValue(void);\n")
file(WRITE "${HAVREMOTE_LIBRESSL_INCLUDE_DIR}/openssl/opensslv.h"
  "#define LIBRESSL_VERSION_NUMBER 0x4030200fL\n")
file(WRITE "${HAVREMOTE_LIBRESSL_INCLUDE_DIR}/openssl/des.h"
  "void DES_ecb_encrypt(void);\n")
file(WRITE "${HAVREMOTE_LIBRESSL_INCLUDE_DIR}/openssl/ssl.h"
  "void SSL_set0_wbio(void);\n")
add_library(crypto SHARED "${CMAKE_CURRENT_BINARY_DIR}/crypto.c")
set_target_properties(crypto PROPERTIES OUTPUT_NAME fixture-crypto)
target_include_directories(crypto PUBLIC "${HAVREMOTE_LIBRESSL_INCLUDE_DIR}")
add_library(HavRemoteLibreSSL::Crypto INTERFACE IMPORTED GLOBAL)
set_property(TARGET HavRemoteLibreSSL::Crypto PROPERTY INTERFACE_LINK_LIBRARIES crypto)
add_library(ssl SHARED "${CMAKE_CURRENT_BINARY_DIR}/ssl.c")
set_target_properties(ssl PROPERTIES OUTPUT_NAME fixture-ssl)
target_link_libraries(ssl PUBLIC crypto)
add_library(HavRemoteLibreSSL::SSL INTERFACE IMPORTED GLOBAL)
set_property(TARGET HavRemoteLibreSSL::SSL PROPERTY INTERFACE_LINK_LIBRARIES ssl)

set(HAVE_AWSLC TRUE)
set(HAVE_BORINGSSL TRUE)
set(HAVE_SSL_SET0_WBIO FALSE)
set(HAVE_OPENSSL_SRP TRUE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
include("${SOURCE_DIR}/cmake/modules/libressl/ConfigureCurl.cmake")
havremote_configure_libressl_curl()
if(HAVE_AWSLC OR HAVE_BORINGSSL OR NOT HAVE_LIBRESSL OR
   NOT HAVE_DES_ECB_ENCRYPT OR NOT HAVE_SSL_SET0_WBIO OR HAVE_OPENSSL_SRP OR
   NOT CMAKE_TRY_COMPILE_TARGET_TYPE STREQUAL "EXECUTABLE")
  message(FATAL_ERROR "Clean curl TLS capability checks did not match the public headers")
endif()

list(PREPEND CMAKE_MODULE_PATH "${SOURCE_DIR}/cmake/modules/libressl")
set(DLL_LIBCRYPTO "${CMAKE_CURRENT_BINARY_DIR}/stale-crypto.dll"
  CACHE FILEPATH "Stale discovery result" FORCE)
find_package(OpenSSL REQUIRED MODULE)
find_package(OpenSSL REQUIRED MODULE)
if(NOT OPENSSL_FOUND OR NOT OpenSSL_FOUND OR
   NOT OPENSSL_VERSION STREQUAL HAVREMOTE_LIBRESSL_VERSION OR
   NOT OPENSSL_INCLUDE_DIR STREQUAL HAVREMOTE_LIBRESSL_INCLUDE_DIR OR
   NOT OPENSSL_LIBRARIES STREQUAL "OpenSSL::SSL;OpenSSL::Crypto")
  message(FATAL_ERROR "The LibreSSL shim did not supply its provider metadata")
endif()

foreach(_target IN ITEMS OpenSSL::Crypto OpenSSL::SSL)
  get_target_property(_system_includes "${_target}" INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
  if(NOT _system_includes STREQUAL HAVREMOTE_LIBRESSL_INCLUDE_DIR)
    message(FATAL_ERROR "External TLS headers are not system includes on ${_target}")
  endif()
  get_target_property(_imported "${_target}" IMPORTED)
  if(_imported)
    get_target_property(_location "${_target}" LOCATION)
    if(NOT _location)
      message(FATAL_ERROR "Bad lib in library list: ${_target}")
    endif()
  endif()
endforeach()

find_file(DLL_LIBCRYPTO NAMES crypto.dll libcrypto-3.dll NO_DEFAULT_PATH)
if(NOT DLL_LIBCRYPTO STREQUAL "$<TARGET_FILE:crypto>")
  message(FATAL_ERROR "libssh2 runtime discovery lost the built crypto target")
endif()

add_library(ssh SHARED "${CMAKE_CURRENT_BINARY_DIR}/ssh.c")
target_link_libraries(ssh PRIVATE OpenSSL::Crypto)
add_library(curl SHARED "${CMAKE_CURRENT_BINARY_DIR}/curl.c")
target_link_libraries(curl PRIVATE OpenSSL::SSL OpenSSL::Crypto)
include(CheckCCompilerFlag)
check_c_compiler_flag("-Wleading-whitespace=spaces" HAVE_LEADING_WHITESPACE_WARNING)
if(HAVE_LEADING_WHITESPACE_WARNING)
  target_compile_options(curl PRIVATE -Wleading-whitespace=spaces -Werror)
endif()
install(TARGETS ssh EXPORT SshTargets)
install(EXPORT SshTargets DESTINATION lib/cmake/ssh)
export(TARGETS ssh FILE "${CMAKE_CURRENT_BINARY_DIR}/ssh-targets.cmake")
install(TARGETS curl EXPORT CurlTargets)
install(EXPORT CurlTargets DESTINATION lib/cmake/curl)
export(TARGETS curl FILE "${CMAKE_CURRENT_BINARY_DIR}/curl-targets.cmake")

add_executable(shim_consumer "${CMAKE_CURRENT_BINARY_DIR}/main.c")
target_link_libraries(shim_consumer PRIVATE ssh curl)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/runtime-$<CONFIG>.txt"
  CONTENT "${DLL_LIBCRYPTO}\n${DLL_LIBSSL}\n$<TARGET_FILE:shim_consumer>\n")

enable_testing()
add_test(NAME shim.consumer COMMAND shim_consumer)
