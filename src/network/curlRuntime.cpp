// SPDX-License-Identifier: MIT

#include "network/curlRuntime.hpp"

namespace havremote::network
{
  namespace
  {
    class CurlRuntime final
    {
    public:
      CurlRuntime() noexcept
      {
        mResult = curl_global_init(CURL_GLOBAL_DEFAULT);

        if (mResult == CURLE_OK)
        {
          mInitialized = true;

          // Trace components only emit diagnostics on handles with verbose output.
          // Trace selection is diagnostic only: failure must not make the
          // successfully initialized transfer runtime unavailable.
          (void)curl_global_trace("ftp,tcp-accept");
        }
      }

      ~CurlRuntime()
      {
        if (mInitialized)
        {
          curl_global_cleanup();
        }
      }

      [[nodiscard]] CURLcode Result() const noexcept { return mResult; }

    private:
      CURLcode mResult{CURLE_FAILED_INIT};
      bool mInitialized{};
    };
  } // namespace

  CURLcode CurlRuntimeResult() noexcept
  {
    static CurlRuntime runtime;
    return runtime.Result();
  }
} // namespace havremote::network
