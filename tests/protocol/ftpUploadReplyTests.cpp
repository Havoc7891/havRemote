// SPDX-License-Identifier: MIT

#include "../../src/protocol/ftpUploadReply.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace havremote;

TEST_CASE("FTP upload reply observer only captures replies to STOR",
          "[ftp][upload][reply]")
{
  ftp::UploadReplyObserver observer;
  observer.Reset(nullptr);

  for (const auto command : {"USER alice\r\n", "PASS hidden\r\n",
                             "MLSD /\r\n", "REST 42\r\n", "APPE file\r\n"})
  {
    observer.Header(false, command);
    observer.Header(true, "552 Not an upload reply\r\n");

    CHECK(observer.Reply() == nullptr);
  }

  observer.Header(false, "STOR /.file.havremote.part\r\n");
  observer.Header(true, "150 Opening data connection\r\n");

  CHECK(observer.Reply() == nullptr);

  observer.Header(true, "552 Transfer aborted. Disk quota exceeded\r\n");

  REQUIRE(observer.Reply() != nullptr);
  CHECK(observer.Reply()->code == 552);
  CHECK(observer.Reply()->text == "Transfer aborted. Disk quota exceeded");

  observer.Header(false, "RNFR /.file.havremote.part\r\n");
  observer.Header(true, "550 Unrelated rename failure\r\n");

  CHECK(observer.Reply()->code == 552);
  CHECK(observer.Reply()->text == "Transfer aborted. Disk quota exceeded");
}

TEST_CASE("FTP upload reply observer retains failures over subsequent cleanup success",
          "[ftp][upload][reply]")
{
  ftp::UploadReplyObserver observer;
  observer.Reset(nullptr);
  observer.Header(false, "STOR file\r\n");
  observer.Header(true, "226 Transfer complete\r\n");

  REQUIRE(observer.Reply() != nullptr);
  CHECK(observer.Reply()->code == 226);

  observer.Header(true, "451 Requested action aborted: local error\r\n");
  observer.Header(true, "226 Abort successful\r\n");

  CHECK(observer.Reply()->code == 451);
  CHECK(observer.Reply()->text == "Requested action aborted: local error");

  observer.Header(true, "452 Insufficient storage space\r\n");

  CHECK(observer.Reply()->code == 452);

  observer.Header(false, "QUIT\r\n");
  observer.Header(true, "221 Goodbye\r\n");

  CHECK(observer.Reply()->code == 452);
}

TEST_CASE("FTP upload reply observer recognizes partial outgoing command traces",
          "[ftp][upload][reply]")
{
  ftp::UploadReplyObserver observer;
  observer.Reset(nullptr);
  observer.Header(false, "STOR /partial-command");
  observer.Header(true, "552 First upload failed\r\n");

  REQUIRE(observer.Reply() != nullptr);
  CHECK(observer.Reply()->code == 552);

  observer.Header(false, "STOR ");

  CHECK(observer.Reply() == nullptr);

  observer.Header(true, "226 Next upload succeeded\r\n");

  REQUIRE(observer.Reply() != nullptr);
  CHECK(observer.Reply()->code == 226);

  observer.Header(false, "UNIDENTIFIED");
  observer.Header(true, "552 Not an upload reply\r\n");

  CHECK(observer.Reply()->code == 226);

  observer.Header(false, "STOR ");
  observer.Header(true, "552-Interrupted multiline response\r\n");
  observer.Header(false, "R");
  observer.Header(true, "552 Unrelated terminator\r\n");

  CHECK(observer.Reply() == nullptr);
}

TEST_CASE("FTP upload reply observer requires complete matching multiline replies",
          "[ftp][upload][reply]")
{
  ftp::UploadReplyObserver observer;
  observer.Reset(nullptr);
  observer.Header(false, "STOR file\r\n");
  observer.Header(true, "552-Storage allocation exceeded\r\n");
  observer.Header(true, " Details from server\r\n");
  observer.Header(true, "451 Not the terminating code\r\n");
  observer.Header(true, "552-Still not the terminator\r\n");
  observer.Header(true, "552\r\n");
  observer.Header(true, "552 Incomplete terminator");

  CHECK(observer.Reply() == nullptr);

  observer.Header(true, "552 Upload rejected\r\n");

  REQUIRE(observer.Reply() != nullptr);
  CHECK(observer.Reply()->code == 552);
  CHECK(observer.Reply()->text.find("Storage allocation exceeded") != std::string::npos);
  CHECK(observer.Reply()->text.find("451 Not the terminating code") != std::string::npos);
  CHECK(observer.Reply()->text.find("Incomplete terminator") == std::string::npos);
  CHECK(observer.Reply()->text.ends_with("Upload rejected"));
}

TEST_CASE("FTP upload reply observer does not interpret preliminary multiline bodies as failures",
          "[ftp][upload][reply]")
{
  ftp::UploadReplyObserver observer;
  observer.Reset(nullptr);
  observer.Header(false, "STOR file\r\n");
  observer.Header(true, "150-Opening transfer\r\n");
  observer.Header(true, "552 This is continuation text\r\n");
  observer.Header(true, "150 Ready\r\n");

  CHECK(observer.Reply() == nullptr);

  observer.Header(true, "226 Transfer complete\r\n");

  REQUIRE(observer.Reply() != nullptr);
  CHECK(observer.Reply()->code == 226);
  CHECK(observer.Reply()->text == "Transfer complete");
}

TEST_CASE("FTP upload reply observer resets prior operations and incomplete reply state",
          "[ftp][upload][reply]")
{
  ftp::UploadReplyObserver observer;
  observer.Reset(nullptr);
  observer.Header(false, "STOR file\r\n");
  observer.Header(true, "552 Old failure\r\n");

  REQUIRE(observer.Reply() != nullptr);

  observer.Reset(nullptr);

  CHECK(observer.Reply() == nullptr);

  observer.Header(true, "552 Stale response\r\n");

  CHECK(observer.Reply() == nullptr);

  observer.Header(false, "STOR file\r\n");
  observer.Header(true, "552-Incomplete old reply\r\n");
  observer.Header(false, "NOOP\r\n");
  observer.Header(true, "552 Old reply terminator\r\n");

  CHECK(observer.Reply() == nullptr);

  observer.Header(false, "STOR other-file\r\n");
  observer.Header(true, "226 New successful transfer\r\n");

  REQUIRE(observer.Reply() != nullptr);
  CHECK(observer.Reply()->text == "New successful transfer");
}

TEST_CASE("FTP upload reply observer ignores malformed and incomplete numeric lines",
          "[ftp][upload][reply]")
{
  ftp::UploadReplyObserver observer;
  observer.Reset(nullptr);
  observer.Header(false, "STOR file\r\n");

  for (const auto line : {"552 Partial reply", "55x Invalid code\r\n",
                          "5520 Invalid separator\r\n", "999 Invalid range\r\n",
                          " 552 Continuation without initial line\r\n"})
  {
    observer.Header(true, line);
    CHECK(observer.Reply() == nullptr);
  }

  observer.Header(true, "552\r\n");

  REQUIRE(observer.Reply() != nullptr);
  CHECK(observer.Reply()->code == 552);
  CHECK(observer.Reply()->text.empty());
}

TEST_CASE("FTP upload reply text is bounded even for long multiline errors",
          "[ftp][upload][reply][security]")
{
  ftp::UploadReplyObserver observer;
  observer.Reset(nullptr);
  observer.Header(false, "STOR file\r\n");
  observer.Header(true, "552 " + std::string(100000, 'x') + "\r\n");

  REQUIRE(observer.Reply() != nullptr);
  CHECK(observer.Reply()->text.size() <= 512);
  CHECK(observer.Reply()->text.ends_with("..."));

  observer.Reset(nullptr);
  observer.Header(false, "STOR file\r\n");
  observer.Header(true, "452-Storage failure\r\n");

  for (int index = 0; index < 40; ++index)
  {
    observer.Header(true, " Detail\r\n");
  }

  CHECK(observer.Reply() == nullptr);

  observer.Header(true, "452 Final line beyond the text limit\r\n");

  REQUIRE(observer.Reply() != nullptr);
  CHECK(observer.Reply()->code == 452);
  CHECK(observer.Reply()->text.size() <= 512);
  CHECK(observer.Reply()->text.ends_with("..."));
  CHECK(observer.Reply()->text.find("Final line") == std::string::npos);
}

TEST_CASE("FTP upload reply text masks secrets before truncation and sanitizes controls",
          "[ftp][upload][reply][security]")
{
  ftp::UploadReplyObserver observer;
  const std::string password{"unlabelled-credential-947"};
  observer.Reset(&password);
  observer.Header(false, "STOR private-file\r\n");
  observer.Header(true, "552 Echo " + password + "\t\x01\x7f storage failure\r\n");

  REQUIRE(observer.Reply() != nullptr);
  CHECK(observer.Reply()->text == "Echo <redacted>    storage failure");
  CHECK(observer.Reply()->text.find(password) == std::string::npos);

  observer.Reset(&password);
  observer.Header(false, "STOR file\r\n");
  observer.Header(true, "552 " + std::string(500, 'x') + password + " remaining text\r\n");

  REQUIRE(observer.Reply() != nullptr);
  CHECK(observer.Reply()->text.size() <= 512);
  CHECK(observer.Reply()->text.find("unlabelled") == std::string::npos);
  CHECK(observer.Reply()->text.find("<redacted") != std::string::npos);

  const std::string longPassword(1000, 'q');

  observer.Reset(&longPassword);
  observer.Header(false, "STOR file\r\n");
  observer.Header(true, "552 Echo " + longPassword + " and " + longPassword + "\r\n");

  REQUIRE(observer.Reply() != nullptr);
  CHECK(observer.Reply()->text == "Echo <redacted> and <redacted>");

  observer.Reset(&password);
  observer.Header(false, "STOR file\r\n");
  observer.Header(true, "552 Error password=different-secret\r\n");

  CHECK(observer.Reply()->text == "Error password=<redacted>");
}

TEST_CASE("FTP upload reply observer callback API is non-throwing",
          "[ftp][upload][reply]")
{
  ftp::UploadReplyObserver observer;

  static_assert(noexcept(observer.Reset(nullptr)));
  static_assert(noexcept(observer.Header(true, {})));
  static_assert(noexcept(observer.Reply()));

  CHECK_NOTHROW(observer.Reset(nullptr));
  CHECK_NOTHROW(observer.Header(true, {}));
  CHECK_NOTHROW(observer.Header(false, "STOR file\r\n"));
  CHECK_NOTHROW(observer.Header(true, "552 Failure\r\n"));
}
