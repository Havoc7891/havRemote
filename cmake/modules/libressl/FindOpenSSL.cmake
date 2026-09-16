# SPDX-License-Identifier: MIT

# OpenSSL compatibility shim for fetched curl and libssh2. It returns the
# fetched LibreSSL targets without searching disk.
if(NOT TARGET HavRemoteLibreSSL::Crypto)
  set(OpenSSL_FOUND FALSE)
  set(OPENSSL_FOUND FALSE)
  if(OpenSSL_FIND_REQUIRED)
    message(FATAL_ERROR
      "The havRemote LibreSSL shim was invoked before the fetched crypto target existed. "
      "LibreSSL must be populated before curl and libssh2.")
  endif()
  return()
endif()

if(NOT DEFINED HAVREMOTE_LIBRESSL_INCLUDE_DIR)
  message(FATAL_ERROR "HAVREMOTE_LIBRESSL_INCLUDE_DIR is not set")
endif()

if(NOT TARGET OpenSSL::Crypto)
  add_library(havremote_openssl_crypto INTERFACE)
  target_link_libraries(havremote_openssl_crypto INTERFACE HavRemoteLibreSSL::Crypto)
  target_include_directories(havremote_openssl_crypto SYSTEM INTERFACE
    "${HAVREMOTE_LIBRESSL_INCLUDE_DIR}")
  add_library(OpenSSL::Crypto ALIAS havremote_openssl_crypto)
endif()
if(TARGET HavRemoteLibreSSL::SSL AND NOT TARGET OpenSSL::SSL)
  add_library(havremote_openssl_ssl INTERFACE)
  target_link_libraries(havremote_openssl_ssl INTERFACE HavRemoteLibreSSL::SSL)
  target_include_directories(havremote_openssl_ssl SYSTEM INTERFACE
    "${HAVREMOTE_LIBRESSL_INCLUDE_DIR}")
  add_library(OpenSSL::SSL ALIAS havremote_openssl_ssl)
endif()

set(OpenSSL_FOUND TRUE)
set(OPENSSL_FOUND TRUE)
set(OpenSSL_VERSION "${HAVREMOTE_LIBRESSL_VERSION}")
set(OPENSSL_VERSION "${HAVREMOTE_LIBRESSL_VERSION}")
set(OpenSSL_INCLUDE_DIR "${HAVREMOTE_LIBRESSL_INCLUDE_DIR}")
set(OPENSSL_INCLUDE_DIR "${HAVREMOTE_LIBRESSL_INCLUDE_DIR}")
set(OPENSSL_CRYPTO_LIBRARY OpenSSL::Crypto)
set(OPENSSL_LIBRARIES OpenSSL::Crypto)
set(DLL_LIBCRYPTO "$<TARGET_FILE:crypto>")

if(TARGET OpenSSL::SSL)
  set(OPENSSL_SSL_LIBRARY OpenSSL::SSL)
  set(DLL_LIBSSL "$<TARGET_FILE:ssl>")
  list(PREPEND OPENSSL_LIBRARIES OpenSSL::SSL)
endif()

mark_as_advanced(
  OpenSSL_INCLUDE_DIR
  OPENSSL_INCLUDE_DIR
  OPENSSL_CRYPTO_LIBRARY
  OPENSSL_SSL_LIBRARY
)
