// SPDX-License-Identifier: MIT

#include "ui/messageLogModel.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace havremote;
using namespace havremote::ui;

TEST_CASE("message log severities use readable light appearance colours",
          "[ui][message-log]")
{
  constexpr auto theme = config::AppearanceTheme::Light;

  CHECK(MessageLogColour(DiagnosticLevel::Debug, theme) ==
        (RgbColour{85, 85, 85}));
  CHECK(MessageLogColour(DiagnosticLevel::Information, theme) ==
        (RgbColour{17, 17, 17}));
  CHECK(MessageLogColour(DiagnosticLevel::Warning, theme) ==
        (RgbColour{138, 101, 0}));
  CHECK(MessageLogColour(DiagnosticLevel::Error, theme) ==
        (RgbColour{176, 0, 32}));
}

TEST_CASE("message log severities use readable dark appearance colours",
          "[ui][message-log]")
{
  constexpr auto theme = config::AppearanceTheme::Dark;

  CHECK(MessageLogColour(DiagnosticLevel::Debug, theme) ==
        (RgbColour{197, 197, 197}));
  CHECK(MessageLogColour(DiagnosticLevel::Information, theme) ==
        (RgbColour{246, 246, 246}));
  CHECK(MessageLogColour(DiagnosticLevel::Warning, theme) ==
        (RgbColour{202, 169, 78}));
  CHECK(MessageLogColour(DiagnosticLevel::Error, theme) ==
        (RgbColour{255, 114, 126}));
}
