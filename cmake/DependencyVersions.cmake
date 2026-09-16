# SPDX-License-Identifier: MIT

# Dependency refs can be overridden through the CMake cache
set(HAVREMOTE_WXWIDGETS_REF "v3.3.3" CACHE STRING "wxWidgets Git branch or tag")
set(HAVREMOTE_CURL_REF "curl-8_21_0" CACHE STRING "curl Git branch or tag")
set(HAVREMOTE_LIBSSH2_REF "libssh2-1.11.1" CACHE STRING "libssh2 Git branch or tag")
set(HAVREMOTE_LIBRESSL_REF "v4.3.2" CACHE STRING "LibreSSL Git branch or tag")
set(HAVREMOTE_CATCH2_REF "v3.16.0" CACHE STRING "Catch2 Git branch or tag")
set(HAVREMOTE_HAVCSON_REF "main" CACHE STRING "havCSON Git branch or tag")

mark_as_advanced(
  HAVREMOTE_WXWIDGETS_REF
  HAVREMOTE_CURL_REF
  HAVREMOTE_LIBSSH2_REF
  HAVREMOTE_LIBRESSL_REF
  HAVREMOTE_CATCH2_REF
  HAVREMOTE_HAVCSON_REF
)
