// SPDX-License-Identifier: MIT

#include "protocol/sftpHostKeyPolicy.hpp"

#include <cstddef>
#include <string_view>

namespace havremote::sftp
{
  std::string MakeHostKeyAlgorithmPreference(
      const std::span<const char *const> supportedAlgorithms)
  {
    constexpr std::string_view preferredAlgorithm{"ssh-ed25519"};

    std::size_t preferredIndex = supportedAlgorithms.size();
    std::size_t resultSize = supportedAlgorithms.empty()
                                 ? 0U
                                 : supportedAlgorithms.size() - 1U;

    for (std::size_t index = 0; index < supportedAlgorithms.size(); ++index)
    {
      const std::string_view algorithm = supportedAlgorithms[index]
                                             ? supportedAlgorithms[index]
                                             : "";

      resultSize += algorithm.size();

      if (preferredIndex == supportedAlgorithms.size() &&
          algorithm == preferredAlgorithm)
      {
        preferredIndex = index;
      }
    }

    std::string result;
    result.reserve(resultSize);

    const auto append = [&result](const char *algorithm)
    {
      if (!result.empty())
      {
        result.push_back(',');
      }

      if (algorithm)
      {
        result.append(algorithm);
      }
    };

    if (preferredIndex != supportedAlgorithms.size())
    {
      append(supportedAlgorithms[preferredIndex]);
    }

    for (std::size_t index = 0; index < supportedAlgorithms.size(); ++index)
    {
      if (index != preferredIndex)
      {
        append(supportedAlgorithms[index]);
      }
    }

    return result;
  }
} // namespace havremote::sftp
