# SPDX-License-Identifier: MIT

include_guard(GLOBAL)

include(CheckSymbolExists)
include(CMakePushCheckState)

function(havremote_configure_libressl_curl)
  if(NOT TARGET HavRemoteLibreSSL::Crypto OR NOT TARGET HavRemoteLibreSSL::SSL)
    message(FATAL_ERROR "LibreSSL crypto and ssl targets are required before configuring curl")
  endif()

  # The fetched libraries are not built during curl's configure-time probes.
  # Compile their public declarations without linking an import library.
  cmake_push_check_state(RESET)
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
  set(CMAKE_REQUIRED_INCLUDES "${HAVREMOTE_LIBRESSL_INCLUDE_DIR}")
  foreach(_result IN ITEMS
      HAVREMOTE_CURL_HAVE_LIBRESSL
      HAVREMOTE_CURL_HAVE_DES_ECB_ENCRYPT
      HAVREMOTE_CURL_HAVE_SSL_SET0_WBIO
      HAVREMOTE_CURL_HAVE_OPENSSL_SRP)
    unset(${_result} CACHE)
  endforeach()
  check_symbol_exists(LIBRESSL_VERSION_NUMBER "openssl/opensslv.h"
    HAVREMOTE_CURL_HAVE_LIBRESSL)
  check_symbol_exists(DES_ecb_encrypt "openssl/des.h"
    HAVREMOTE_CURL_HAVE_DES_ECB_ENCRYPT)
  check_symbol_exists(SSL_set0_wbio "openssl/ssl.h"
    HAVREMOTE_CURL_HAVE_SSL_SET0_WBIO)
  check_symbol_exists(SSL_CTX_set_srp_username "openssl/ssl.h"
    HAVREMOTE_CURL_HAVE_OPENSSL_SRP)
  cmake_pop_check_state()

  if(NOT HAVREMOTE_CURL_HAVE_LIBRESSL)
    message(FATAL_ERROR "The fetched TLS headers do not identify LibreSSL")
  endif()

  set(HAVE_AWSLC FALSE)
  set(HAVE_BORINGSSL FALSE)
  set(HAVE_LIBRESSL TRUE)
  set(HAVE_DES_ECB_ENCRYPT "${HAVREMOTE_CURL_HAVE_DES_ECB_ENCRYPT}")
  set(HAVE_SSL_SET0_WBIO "${HAVREMOTE_CURL_HAVE_SSL_SET0_WBIO}")
  set(HAVE_OPENSSL_SRP "${HAVREMOTE_CURL_HAVE_OPENSSL_SRP}")
  foreach(_result IN ITEMS HAVE_AWSLC HAVE_BORINGSSL HAVE_LIBRESSL
      HAVE_DES_ECB_ENCRYPT HAVE_SSL_SET0_WBIO HAVE_OPENSSL_SRP)
    set(${_result} "${${_result}}" CACHE INTERNAL
      "curl capability detected from fetched LibreSSL headers" FORCE)
    set(${_result} "${${_result}}" PARENT_SCOPE)
  endforeach()
endfunction()
