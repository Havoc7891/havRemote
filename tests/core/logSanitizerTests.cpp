// SPDX-License-Identifier: MIT

#include "core/logSanitizer.hpp"

#include <catch2/catch_test_macros.hpp>

using havremote::SanitizeDiagnosticText;

TEST_CASE("Diagnostic sanitization removes credential fields and line injection",
          "[core][security][logging]")
{
  const auto result = SanitizeDiagnosticText(
      "server said\r\nAuthorization: Basic dXNlcjpzZWNyZXQ=; password=hunter2");

  REQUIRE(result.find('\r') == std::string::npos);
  REQUIRE(result.find('\n') == std::string::npos);
  REQUIRE(result.find("dXNlcjpzZWNyZXQ=") == std::string::npos);
  REQUIRE(result.find("hunter2") == std::string::npos);
  REQUIRE(result.find("<redacted>") != std::string::npos);
}

TEST_CASE("Diagnostic sanitization removes URL user information",
          "[core][security][logging]")
{
  const auto result = SanitizeDiagnosticText(
      "connect failed for ftp://alice:swordfish@example.test/private");

  REQUIRE(result.find("alice") == std::string::npos);
  REQUIRE(result.find("swordfish") == std::string::npos);
  REQUIRE(result.find("example.test/private") != std::string::npos);
}

TEST_CASE("Diagnostic sanitization keeps ordinary password diagnostics useful",
          "[core][security][logging]")
{
  REQUIRE(SanitizeDiagnosticText("Password authentication failed") ==
          "Password authentication failed");
}

TEST_CASE("Diagnostic sanitization redacts FTP credential commands",
          "[core][security][logging]")
{
  CHECK(SanitizeDiagnosticText("USER alice") == "USER <redacted>");
  CHECK(SanitizeDiagnosticText("PASS swordfish") == "PASS <redacted>");
  CHECK(SanitizeDiagnosticText("ACCT billing-secret") == "ACCT <redacted>");
  CHECK(SanitizeDiagnosticText("ADAT opaque-token") == "ADAT <redacted>");
  CHECK(SanitizeDiagnosticText("> PASS traced-secret") ==
        "> PASS <redacted>");
}
