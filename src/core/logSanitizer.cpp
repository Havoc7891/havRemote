// SPDX-License-Identifier: MIT

#include "core/logSanitizer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ranges>

namespace havremote
{
  namespace
  {
    bool IsBoundary(const std::string_view value, const std::size_t position) noexcept
    {
      if (position == 0)
      {
        return true;
      }

      const auto previous = static_cast<unsigned char>(value[position - 1]);

      return !std::isalnum(previous) && previous != '_' && previous != '-';
    }

    void RedactUrlUserInfo(std::string &text, std::string &folded)
    {
      std::size_t search = 0;

      while ((search = folded.find("://", search)) != std::string::npos)
      {
        const auto authority = search + 3;
        const auto authorityEnd = folded.find_first_of("/ \t;", authority);
        const auto at = folded.find('@', authority);

        if (at == std::string::npos ||
            (authorityEnd != std::string::npos && at >= authorityEnd))
        {
          search = authority;

          continue;
        }

        const auto colon = folded.find(':', authority);

        if (colon == std::string::npos || colon >= at)
        {
          search = at + 1;

          continue;
        }

        constexpr std::string_view replacement{"<credentials>"};

        text.replace(authority, at - authority, replacement);
        folded.replace(authority, at - authority, replacement);

        search = authority + replacement.size() + 1;
      }
    }

    void RedactFtpCredentialCommand(std::string &text, std::string &folded)
    {
      auto command = folded.find_first_not_of(" \t");
      if (command == std::string::npos)
      {
        return;
      }

      if (folded[command] == '>')
      {
        command = folded.find_first_not_of(" \t", command + 1U);

        if (command == std::string::npos)
        {
          return;
        }
      }

      constexpr std::array<std::string_view, 4> credentialCommands{"user", "pass", "acct", "adat"};

      for (const auto candidate : credentialCommands)
      {
        if (folded.compare(command, candidate.size(), candidate) != 0)
        {
          continue;
        }

        const auto argument = command + candidate.size();
        if (argument >= folded.size() ||
            (folded[argument] != ' ' && folded[argument] != '\t'))
        {
          continue;
        }

        const auto value = folded.find_first_not_of(" \t", argument);
        if (value != std::string::npos)
        {
          constexpr std::string_view replacement{"<redacted>"};

          text.replace(value, std::string::npos, replacement);
          folded.replace(value, std::string::npos, replacement);
        }

        return;
      }
    }
  } // namespace

  std::string SanitizeDiagnosticText(const std::string_view input)
  {
    std::string text{input};

    for (auto &character : text)
    {
      const auto byte = static_cast<unsigned char>(character);

      if (character == '\r' || character == '\n' ||
          (byte < 0x20U && character != '\t'))
      {
        character = ' ';
      }
    }

    std::string folded = text;
    std::ranges::transform(folded, folded.begin(), [](const unsigned char value)
                           { return static_cast<char>(std::tolower(value)); });

    RedactUrlUserInfo(text, folded);
    RedactFtpCredentialCommand(text, folded);

    constexpr std::array<std::string_view, 10> sensitive{
        "proxy-authorization", "authorization", "private-key", "privatekey",
        "passphrase", "password", "passwd", "access-token", "token", "secret"};

    std::size_t earliest = std::string::npos;
    std::size_t markerEnd = 0;

    for (const auto marker : sensitive)
    {
      std::size_t position = 0;

      while ((position = folded.find(marker, position)) != std::string::npos)
      {
        if (!IsBoundary(folded, position))
        {
          position += marker.size();

          continue;
        }

        auto separator = position + marker.size();
        while (separator < folded.size() &&
               (folded[separator] == ' ' || folded[separator] == '\t'))
        {
          ++separator;
        }

        if (separator >= folded.size() ||
            (folded[separator] != '=' && folded[separator] != ':'))
        {
          position += marker.size();

          continue;
        }

        if (position < earliest)
        {
          earliest = position;
          markerEnd = separator + 1;
        }

        break;
      }
    }

    if (earliest != std::string::npos)
    {
      while (markerEnd < text.size() &&
             (text[markerEnd] == ' ' || text[markerEnd] == '\t'))
      {
        ++markerEnd;
      }

      text.replace(markerEnd, std::string::npos, "<redacted>");
    }

    return text;
  }
} // namespace havremote
