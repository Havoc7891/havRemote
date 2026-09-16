// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_SRC_NETWORK_CURL_RUNTIME_HPP
#define HAVREMOTE_SRC_NETWORK_CURL_RUNTIME_HPP

#include <curl/curl.h>

namespace havremote::network
{
  // Initializes libcurl's process-wide runtime exactly once. The implementation
  // owns the matching cleanup after every havRemote worker has stopped.
  [[nodiscard]] CURLcode CurlRuntimeResult() noexcept;
} // namespace havremote::network

#endif // HAVREMOTE_SRC_NETWORK_CURL_RUNTIME_HPP
