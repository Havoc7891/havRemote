// SPDX-License-Identifier: MIT

#include "../../src/protocol/ftpControlTrace.hpp"
#include "../../src/network/curlRuntime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace havremote;

namespace
{
  using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;

  CurlHandle MakeUnusedHandle()
  {
    // Tests inspect only easy-handle metadata. No URL or network operation is
    // configured. Share the application's process-wide curl initialization.
    REQUIRE(network::CurlRuntimeResult() == CURLE_OK);

    CurlHandle handle{curl_easy_init(), &curl_easy_cleanup};

    REQUIRE(handle != nullptr);

    return handle;
  }

  struct TraceFixture final
  {
    std::vector<std::string> messages;
    std::vector<DiagnosticLevel> levels;
    std::string password{"unlabelled-credential-947"};
    DiagnosticCallback sink = [this](const DiagnosticLevel level,
                                     const std::string_view message)
    {
      levels.push_back(level);
      messages.emplace_back(message);
    };
    ftp::ControlTrace trace;

    explicit TraceFixture(const bool enabled = true)
    {
      trace.Initialize(&sink, &password, enabled);
    }

    [[nodiscard]] bool Contains(const std::string_view text) const
    {
      return std::ranges::any_of(messages, [text](const auto &message)
                                 { return message.find(text) != std::string::npos; });
    }

    [[nodiscard]] std::size_t Count(const std::string_view text) const
    {
      return static_cast<std::size_t>(std::ranges::count_if(
          messages, [text](const auto &message)
          { return message.find(text) != std::string::npos; }));
    }
  };
} // namespace

TEST_CASE("FTP control trace emits nothing without opt-in and a diagnostic sink",
          "[ftp][control-trace][security]")
{
  const auto handle = MakeUnusedHandle();

  TraceFixture fixture{false};

  CHECK_FALSE(fixture.trace.Enabled());

  fixture.trace.Begin(handle.get(), "LIST");
  fixture.trace.Header(false, "USER alice\r\n");
  fixture.trace.Header(true, "421 Connection timed out\r\n");
  fixture.trace.Text("Connection closed");
  fixture.trace.Record("manually observed event");
  fixture.trace.Endpoint("192.0.2.1", 21, "192.0.2.2", 49152);
  fixture.trace.Finish(CURLE_RECV_ERROR, "Injected error");

  CHECK(fixture.messages.empty());

  fixture.trace.Initialize(nullptr, &fixture.password, true);

  CHECK_FALSE(fixture.trace.Enabled());
  CHECK_NOTHROW(fixture.trace.Record("No callback object"));

  DiagnosticCallback emptySink;

  fixture.trace.Initialize(&emptySink, &fixture.password, true);

  CHECK_FALSE(fixture.trace.Enabled());
  CHECK_NOTHROW(fixture.trace.Record("Empty callback object"));
  CHECK(fixture.messages.empty());
}

TEST_CASE("FTP control trace reports command verbs without arguments or credentials",
          "[ftp][control-trace][security]")
{
  TraceFixture fixture;

  for (const auto command : {"USER AlicePrivateIdentity\r\n",
                             "PASS DifferentPrivateCredential\r\n",
                             "ACCT PrivateAccountName\r\n",
                             "ADAT OpaqueAuthenticationBlob\r\n",
                             "STOR /private/customer/report.txt\r\n",
                             "RETR /private/customer/report.txt\r\n",
                             " \tRNFR /private/customer/report.txt\r\n"})
  {
    fixture.trace.Header(false, command);
  }

  REQUIRE(fixture.messages.size() == 7U);
  CHECK(fixture.messages[0].ends_with("FTP command USER"));
  CHECK(fixture.messages[1].ends_with("FTP command PASS"));
  CHECK(fixture.messages[2].ends_with("FTP command ACCT"));
  CHECK(fixture.messages[3].ends_with("FTP command ADAT"));
  CHECK(fixture.messages[4].ends_with("FTP command STOR"));
  CHECK(fixture.messages[5].ends_with("FTP command RETR"));
  CHECK(fixture.messages[6].ends_with("FTP command RNFR"));
  CHECK_FALSE(fixture.Contains("AlicePrivateIdentity"));
  CHECK_FALSE(fixture.Contains("DifferentPrivateCredential"));
  CHECK_FALSE(fixture.Contains("PrivateAccountName"));
  CHECK_FALSE(fixture.Contains("OpaqueAuthenticationBlob"));
  CHECK_FALSE(fixture.Contains("/private/customer"));

  fixture.trace.Header(false, "lower-case-and-private-contents\r\n");

  CHECK(fixture.messages.back().ends_with("FTP command <unparsed>"));

  const auto previous = fixture.messages.size();

  fixture.trace.Header(false, " \t\r\n");

  CHECK(fixture.messages.size() == previous);
  CHECK(std::ranges::all_of(fixture.levels, [](const auto level)
                            { return level == DiagnosticLevel::Debug; }));
}

TEST_CASE("FTP control trace never copies incoming server reply text",
          "[ftp][control-trace][security]")
{
  TraceFixture fixture;
  fixture.trace.Header(true, "331 User AlicePrivateIdentity requires a password\r\n");
  fixture.trace.Header(true, "530 Supplied credentials DifferentPrivateCredential rejected\r\n");
  fixture.trace.Header(true, "220-Server banner includes PrivateBannerText\r\n");
  fixture.trace.Header(true, " PrivateMultilineContents\r\n");
  fixture.trace.Header(true, "421-PrivateFailureDetail\r\n421 Closing session\r\n");

  REQUIRE(fixture.messages.size() == 5U);
  CHECK(fixture.messages[0].ends_with("FTP response 331"));
  CHECK(fixture.messages[1].ends_with("FTP response 530"));
  CHECK(fixture.messages[2].ends_with("FTP response 220"));
  CHECK(fixture.messages[3].ends_with("FTP response <continuation>"));
  CHECK(fixture.messages[4].ends_with("FTP response 421"));
  CHECK_FALSE(fixture.Contains("Private"));
  CHECK_FALSE(fixture.Contains("Closing session"));
}

TEST_CASE("FTP control trace accepts lifecycle diagnostics but not arbitrary verbose text",
          "[ftp][control-trace][security]")
{
  TraceFixture fixture;

  for (const auto ignored : {"schannel: private TLS negotiation detail",
                             "Server certificate: private certificate detail",
                             "Doing the SSL/TLS handshake",
                             "FTP response: arbitrary authentication reply",
                             "USER AnotherPrivateIdentity",
                             "PASS AnotherPrivateCredential",
                             "<html>arbitrary file payload</html>",
                             "OpaqueBinaryOrFileContents"})
  {
    fixture.trace.Text(ignored);
  }

  CHECK(fixture.messages.empty());

  for (const auto accepted : {"Connection #0 seems to be dead",
                              "Reusing existing connection",
                              "Too old connection (120 seconds idle), disconnect it",
                              "We got a 421 - timeout",
                              "Recv failure: Connection was reset",
                              "closing connection #0"})
  {
    fixture.trace.Text(accepted);
  }

  CHECK(fixture.messages.size() == 6U);
  CHECK(fixture.Contains("seems to be dead"));
  CHECK(fixture.Contains("120 seconds idle"));
  CHECK(fixture.Contains("We got a 421"));
}

TEST_CASE("FTP control trace sanitizes lifecycle text and masks exact credentials",
          "[ftp][control-trace][security]")
{
  TraceFixture fixture;
  fixture.trace.Text("Connection closed\r\n[ERROR] injected line\x01");

  REQUIRE(fixture.messages.size() == 1U);
  CHECK_FALSE(fixture.Contains("\r"));
  CHECK_FALSE(fixture.Contains("\n"));
  CHECK_FALSE(fixture.Contains(std::string_view{"\x01", 1}));

  fixture.trace.Text("Connection to ftp://AlicePrivateIdentity:UrlCredential@192.0.2.1 closed");

  CHECK_FALSE(fixture.Contains("AlicePrivateIdentity"));
  CHECK_FALSE(fixture.Contains("UrlCredential"));
  CHECK(fixture.Contains("192.0.2.1"));

  fixture.trace.Text("Connection failed: token=SensitiveBearerValue");

  CHECK_FALSE(fixture.Contains("SensitiveBearerValue"));

  fixture.trace.Text("Connection rejected " + fixture.password +
                     ". Repeated " + fixture.password);

  CHECK_FALSE(fixture.Contains(fixture.password));
  CHECK(fixture.messages.back().ends_with(
      "Connection rejected <redacted>. Repeated <redacted>"));

  // Mask before normalizing control characters so an unlabeled credential
  // does not survive as a merely normalized version of its original value.
  fixture.password = "TwoPrivate\nCredentialParts";
  fixture.trace.Text("Connection rejected " + fixture.password);

  CHECK_FALSE(fixture.Contains("TwoPrivate"));
  CHECK_FALSE(fixture.Contains("CredentialParts"));
  CHECK(fixture.messages.back().ends_with("Connection rejected <redacted>"));
}

TEST_CASE("FTP control trace bounds message bodies and session wire events",
          "[ftp][control-trace]")
{
  const auto handle = MakeUnusedHandle();

  TraceFixture fixture;
  fixture.trace.Begin(handle.get(), "LIST");
  fixture.trace.Text("Connection " + std::string(4096U, 'x'));

  REQUIRE(fixture.messages.size() == 2U);

  const auto &longMessage = fixture.messages.back();

  const auto body = longMessage.find("]: ");

  REQUIRE(body != std::string::npos);
  CHECK(longMessage.size() - (body + 3U) <= 768U);
  CHECK(longMessage.ends_with("..."));

  for (std::size_t index = 0; index < 1024U; ++index)
  {
    fixture.trace.Header(false, "NOOP\r\n");
    fixture.trace.Header(true, "200 OK\r\n");
    fixture.trace.Text("Connection observed");
  }

  CHECK(fixture.messages.size() <= 258U); // begin + 256 wire events + limit notice
  CHECK(fixture.Count("event limit reached") == 1U);

  const auto exhaustedCount = fixture.messages.size();

  fixture.trace.Record("after-exhaustion marker");
  fixture.trace.Endpoint("192.0.2.1", 21, "192.0.2.2", 49152);

  CHECK(fixture.messages.size() == exhaustedCount);
  CHECK_FALSE(fixture.Contains("after-exhaustion marker"));

  // A later operation must not reset the wire-event budget
  fixture.trace.Finish(CURLE_OK, {});
  fixture.trace.Begin(handle.get(), "RETR");

  const auto nextOperationCount = fixture.messages.size();

  fixture.trace.Header(false, "NOOP\r\n");
  fixture.trace.Endpoint("192.0.2.1", 21, "192.0.2.2", 49153);

  CHECK(fixture.messages.size() == nextOperationCount);

  fixture.trace.Finish(CURLE_OK, {});
}

TEST_CASE("FTP control trace bounds operation reports and resets only for a new session",
          "[ftp][control-trace]")
{
  const auto handle = MakeUnusedHandle();

  TraceFixture fixture;

  for (std::size_t index = 0; index < 64U; ++index)
  {
    fixture.trace.Begin(handle.get(), "LIST");
    fixture.trace.Header(false, "LIST\r\n");
    fixture.trace.Finish(CURLE_OK, {});
  }

  CHECK(fixture.Contains("operation=32,"));
  CHECK_FALSE(fixture.Contains("operation=33,"));
  CHECK_FALSE(fixture.Contains("operation=64,"));
  CHECK(fixture.messages.size() <= 97U); // 32 begin/command/end groups + limit notice
  CHECK(fixture.Count("operation limit reached") == 1U);

  fixture.messages.clear();
  fixture.trace.Initialize(&fixture.sink, &fixture.password, true);
  fixture.trace.Begin(handle.get(), "NLST");
  fixture.trace.Header(false, "NLST\r\n");
  fixture.trace.Finish(CURLE_OK, {});

  REQUIRE(fixture.messages.size() == 3U);
  CHECK(fixture.Contains("operation=1,"));
  CHECK(fixture.Contains("FTP command NLST"));
}

TEST_CASE("FTP control trace records exact failure status without exposing the password",
          "[ftp][control-trace][security]")
{
  const auto handle = MakeUnusedHandle();

  TraceFixture fixture;
  fixture.trace.Begin(handle.get(), "LIST");
  fixture.trace.Endpoint("192.0.2.1", 21, "192.0.2.2", 49152);
  fixture.trace.Finish(CURLE_RECV_ERROR,
                       "Connection reset. Echoed " + fixture.password);

  CHECK(fixture.Contains("local=[192.0.2.2]:49152"));
  CHECK(fixture.Contains("remote=[192.0.2.1]:21"));
  CHECK(fixture.Contains("CURLcode 56"));
  CHECK(fixture.Contains(curl_easy_strerror(CURLE_RECV_ERROR)));
  CHECK(fixture.Contains("error buffer: Connection reset. Echoed <redacted>"));
  CHECK(fixture.Contains("backend result=failure"));
  CHECK_FALSE(fixture.Contains(fixture.password));
}

TEST_CASE("FTP control trace swallows diagnostic callback exceptions",
          "[ftp][control-trace]")
{
  const auto handle = MakeUnusedHandle();

  std::size_t callbacks{};

  DiagnosticCallback throwingSink = [&](DiagnosticLevel, std::string_view)
  {
    ++callbacks;

    throw std::runtime_error{"Diagnostic consumer failed"};
  };

  ftp::ControlTrace trace;
  trace.Initialize(&throwingSink, nullptr, true);

  CHECK_NOTHROW(trace.Begin(handle.get(), "LIST"));
  CHECK_NOTHROW(trace.Header(false, "LIST\r\n"));
  CHECK_NOTHROW(trace.Header(true, "150 Opening data connection\r\n"));
  CHECK_NOTHROW(trace.Text("Connection closed"));
  CHECK_NOTHROW(trace.Record("additional event"));
  CHECK_NOTHROW(trace.Endpoint("192.0.2.1", 21, "192.0.2.2", 49152));
  CHECK_NOTHROW(trace.Finish(CURLE_RECV_ERROR, "Injected failure"));
  CHECK(callbacks == 7U);
}
