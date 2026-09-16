// SPDX-License-Identifier: MIT

#include "protocol/sftpSession.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace havremote;

namespace
{
  class ReadAheadFixture final
  {
  public:
    ReadAheadFixture()
        : mLocal(std::filesystem::temp_directory_path() /
                 ("havremote-read-ahead-" + GenerateId())),
          mSession(MakeSftpSession())
    {
      std::filesystem::create_directories(mLocal);

      SiteProfile site;
      site.id = "read-ahead-fixture";
      site.name = "Synthetic SFTP READ fixture";
      site.protocol = ProtocolKind::Sftp;
      site.host = std::getenv("HAVREMOTE_SFTP_READAHEAD_HOST");
      site.username = "havremote-read-ahead";
      site.authentication.kind = AuthenticationKind::Password;

      const auto *portText = std::getenv("HAVREMOTE_SFTP_READAHEAD_PORT");

      REQUIRE(portText != nullptr);

      const std::string_view port{portText};

      unsigned int parsed{};

      const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), parsed);

      REQUIRE(error == std::errc{});
      REQUIRE(end == port.data() + port.size());
      REQUIRE(parsed > 0U);
      REQUIRE(parsed <= 65535U);

      site.port = static_cast<std::uint16_t>(parsed);

      SessionCallbacks callbacks;
      callbacks.knownHostsFile = mLocal / "known_hosts";
      callbacks.requestCredential = [](const CredentialRequest &, std::stop_token)
      {
        const auto *password = std::getenv("HAVREMOTE_SFTP_READAHEAD_PASSWORD");

        REQUIRE(password != nullptr);

        return Result<std::string>{std::string{password}};
      };
      callbacks.verifyTrust = [](const TrustChallenge &, std::stop_token)
      {
        return Result<TrustDecision>{TrustDecision::AcceptOnce};
      };

      const auto connected = mSession->Connect(site, callbacks, {});

      INFO((connected ? "Connected" : connected.error().message));
      REQUIRE(connected);
    }

    ~ReadAheadFixture()
    {
      mSession->Disconnect();

      std::error_code ignored;
      std::filesystem::remove_all(mLocal, ignored);
    }

    struct Read final
    {
      std::uint64_t offset{};
      std::uint64_t requested{};
      std::uint64_t delivered{};
    };

    [[nodiscard]] IRemoteSession &Session() { return *mSession; }

    [[nodiscard]] std::filesystem::path Path(std::string_view name) const
    {
      return mLocal / name;
    }

    struct Transcript final
    {
      std::vector<Read> reads;
      std::size_t observedPipelineDepth{};
    };

    [[nodiscard]] Transcript GetTranscript(const std::string &label)
    {
      const auto file = Path(label + "-report.txt");

      const auto downloaded = mSession->Download(
          RemotePath{"/report/" + label}, file,
          TransferOptions{.jobId = "transcript",
                          .temporaryRemotePath = std::nullopt,
                          .beforeFinalize = {}},
          {}, {});

      INFO((downloaded ? "Transcript downloaded" : downloaded.error().message));
      REQUIRE(downloaded);

      std::ifstream input{file};

      REQUIRE(input);

      Transcript result;

      std::string heading;

      input >> heading >> result.observedPipelineDepth;

      REQUIRE(heading == "pipeline");

      Read read;

      while (input >> read.offset >> read.requested >> read.delivered)
      {
        result.reads.push_back(read);
      }

      CHECK(input.eof());

      return result;
    }

  private:
    std::filesystem::path mLocal;
    RemoteSessionPtr mSession;
  };

  [[nodiscard]] char ExpectedByte(std::uint64_t offset)
  {
    return static_cast<char>((offset * 31U + 17U) % 251U);
  }

  void WritePrefix(const std::filesystem::path &path, std::uint64_t size)
  {
    std::ofstream output{path, std::ios::binary};

    REQUIRE(output);

    for (std::uint64_t offset = 0; offset < size; ++offset)
    {
      output.put(ExpectedByte(offset));
    }

    REQUIRE(output);
  }

  void VerifyContents(const std::filesystem::path &path, std::uint64_t size)
  {
    REQUIRE(std::filesystem::file_size(path) == size);

    std::ifstream input{path, std::ios::binary};

    REQUIRE(input);

    std::array<char, 65536> bytes{};

    std::uint64_t offset{};

    while (input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
           input.gcount() > 0)
    {
      bool matches = true;

      for (std::streamsize index = 0; index < input.gcount(); ++index)
      {
        if (bytes[static_cast<std::size_t>(index)] !=
            ExpectedByte(offset + static_cast<std::uint64_t>(index)))
        {
          matches = false;

          break;
        }
      }

      INFO("Payload block beginning at " << offset);
      REQUIRE(matches);

      offset += static_cast<std::uint64_t>(input.gcount());
    }

    CHECK(input.eof());
    CHECK(offset == size);
  }

  [[nodiscard]] RemotePath DataPath(const std::string &label,
                                    std::optional<std::uint64_t> knownSize,
                                    std::uint64_t actualSize)
  {
    return RemotePath{"/data/" + label + "/" +
                      (knownSize ? std::to_string(*knownSize) : "unknown") + "/" +
                      std::to_string(actualSize)};
  }

  void RequireFixture()
  {
    if (!std::getenv("HAVREMOTE_SFTP_READAHEAD_HOST"))
    {
      SKIP("Run with tests/integration/sftpReadAheadServer.py and Paramiko");
    }
  }
} // namespace

TEST_CASE("SFTP known-size downloads stop at the recorded size with bounded read-ahead",
          "[integration][sftp-read-ahead]")
{
  RequireFixture();

  ReadAheadFixture fixture;

  struct Sample final
  {
    std::uint64_t size;
    std::uint64_t resume;
  };

  for (const auto sample : std::array{
           Sample{0, 0}, Sample{1, 0}, Sample{29999, 0}, Sample{30000, 0},
           Sample{30001, 0}, Sample{65537, 0}, Sample{1048593, 0},
           Sample{209715200, 0}, Sample{65537, 17}, Sample{1048593, 30017},
           Sample{65537, 65536}, Sample{65537, 65537}})
  {
    DYNAMIC_SECTION("size=" << sample.size << " resume=" << sample.resume)
    {
      const auto label = GenerateId();

      const auto output = fixture.Path(label + ".bin");

      auto part = output;
      part += ".havremote.part";

      if (sample.resume > 0)
      {
        WritePrefix(part, sample.resume);
      }

      TransferProgress last;

      const auto result = fixture.Session().Download(
          DataPath(label, sample.size, sample.size), output,
          TransferOptions{.jobId = label,
                          .resumeOffset = sample.resume,
                          .temporaryRemotePath = std::nullopt,
                          .beforeFinalize = {}},
          [&](const TransferProgress &progress)
          {
            last = progress;

            return TransferControl::Continue;
          },
          {});

      INFO((result ? "Downloaded" : result.error().message));
      REQUIRE(result);
      CHECK(last.bytesTransferred == sample.size);
      CHECK(last.activeBytesTransferred == sample.size - sample.resume);
      CHECK(last.totalBytes == sample.size);
      CHECK_FALSE(std::filesystem::exists(part));

      VerifyContents(output, sample.size);

      const auto transcript = fixture.GetTranscript(label);

      const auto &reads = transcript.reads;

      if (sample.resume == sample.size)
      {
        CHECK(reads.empty());

        continue;
      }

      REQUIRE_FALSE(reads.empty());
      CHECK(reads.front().offset == sample.resume);

      std::uint64_t nextOffset = sample.resume;

      std::uint64_t delivered{};

      for (const auto &read : reads)
      {
        CAPTURE(read.offset, read.requested);
        REQUIRE(read.offset == nextOffset);
        REQUIRE(read.requested > 0U);
        REQUIRE(read.requested <= 30000U);

        // The application stops requesting data at its known size.
        // libssh2 can already have one window of READs outstanding, including
        // a request crossing EOF and subsequent normal FX_EOF responses.
        REQUIRE(read.offset + read.requested <
                sample.size + 4U * 65536U + 30000U);
        CHECK(read.delivered ==
              (read.offset < sample.size
                   ? (std::min)(read.requested, sample.size - read.offset)
                   : 0U));

        delivered += read.delivered;
        nextOffset += read.requested;
      }

      CHECK(delivered == sample.size - sample.resume);
      CHECK(nextOffset >= sample.size);

      if (sample.size - sample.resume == 1U)
      {
        // Passing the remaining byte count to libssh2 yields one
        // four-byte speculative request. A full application buffer here would
        // queue nine requests. An unnecessary post-completion EOF probe would
        // queue another request. Verify both caller-side regressions directly.
        REQUIRE(reads.size() == 1U);
        CHECK(reads.front().requested == 4U);
        CHECK(reads.front().delivered == 1U);
      }

      if (sample.size - sample.resume >= 90000U)
      {
        CHECK(transcript.observedPipelineDepth >= 3U);
      }

      // Verify full-size pipelined READs
      if (sample.size - sample.resume > 30000U)
      {
        CHECK(reads.front().requested == 30000U);
      }
    }
  }
}

TEST_CASE("SFTP unknown-size EOF drains speculative READs without failing",
          "[integration][sftp-read-ahead]")
{
  RequireFixture();

  ReadAheadFixture fixture;

  constexpr std::uint64_t size = 30017;

  const auto label = GenerateId();

  const auto output = fixture.Path("unknown-size.bin");

  TransferProgress last;

  const auto result = fixture.Session().Download(
      DataPath(label, std::nullopt, size), output,
      TransferOptions{.jobId = label,
                      .temporaryRemotePath = std::nullopt,
                      .beforeFinalize = {}},
      [&](const TransferProgress &progress)
      {
        last = progress;

        return TransferControl::Continue;
      },
      {});

  INFO((result ? "Downloaded" : result.error().message));
  REQUIRE(result);
  CHECK(last.bytesTransferred == size);
  CHECK(last.activeBytesTransferred == size);

  VerifyContents(output, size);

  const auto reads = fixture.GetTranscript(label).reads;

  REQUIRE_FALSE(reads.empty());
  CHECK(std::ranges::any_of(reads, [](const auto &read)
                            { return read.delivered == 0; }));

  // The existing window is four application buffers, rounded to SFTP chunks.
  // Already-sent speculative requests can finish, but EOF must not perpetually
  // refill the window. Exact counts depend on socket scheduling.
  for (const auto &read : reads)
  {
    CHECK(read.requested > 0U);
    CHECK(read.requested <= 30000U);
    CHECK(read.offset + read.requested < size + 4U * 65536U + 30000U);
  }
}

TEST_CASE("SFTP changed remote sizes retain the partial file without protocol EOF errors",
          "[integration][sftp-read-ahead]")
{
  RequireFixture();

  ReadAheadFixture fixture;

  for (const auto sizes : std::array{
           std::pair<std::uint64_t, std::uint64_t>{1048593, 30017},
           std::pair<std::uint64_t, std::uint64_t>{30017, 1048593}})
  {
    DYNAMIC_SECTION("STAT=" << sizes.first << " actual=" << sizes.second)
    {
      const auto label = GenerateId();

      const auto output = fixture.Path(label + ".bin");

      auto part = output;
      part += ".havremote.part";

      bool finalized{};

      const auto result = fixture.Session().Download(
          DataPath(label, sizes.first, sizes.second), output,
          TransferOptions{
              .jobId = label,
              .temporaryRemotePath = std::nullopt,
              .beforeFinalize = [&](bool overwrite)
              {
                finalized = true;

                return Result<bool>{overwrite};
              }},
          {}, {});

      REQUIRE_FALSE(result);
      INFO(result.error().message);
      CHECK(result.error().code == RemoteErrorCode::RemoteIo);
      CHECK(result.error().message.find("SFTP Protocol Error") == std::string::npos);
      CHECK_FALSE(finalized);
      CHECK_FALSE(std::filesystem::exists(output));

      VerifyContents(part, (std::min)(sizes.first, sizes.second));

      const auto reads = fixture.GetTranscript(label).reads;

      REQUIRE_FALSE(reads.empty());

      for (const auto &read : reads)
      {
        CHECK(read.requested > 0U);
        CHECK(read.requested <= 30000U);
        CHECK(read.offset + read.requested <
              (std::min)(sizes.first, sizes.second) + 4U * 65536U + 30000U);
      }

      if (sizes.second < sizes.first)
      {
        CHECK(std::ranges::any_of(reads, [](const auto &read)
                                  { return read.delivered == 0; }));
      }
    }
  }
}

TEST_CASE("SFTP unsafe resume offsets are rejected before opening the remote file",
          "[integration][sftp-read-ahead]")
{
  RequireFixture();

  ReadAheadFixture fixture;

  for (const bool knownSize : {false, true})
  {
    DYNAMIC_SECTION("Known size=" << knownSize)
    {
      constexpr std::uint64_t size = 30017;

      const std::uint64_t resume = knownSize ? size + 1U : 17U;

      const auto label = GenerateId();

      const auto output = fixture.Path(label + ".bin");

      auto part = output;
      part += ".havremote.part";

      WritePrefix(part, resume);

      const auto result = fixture.Session().Download(
          DataPath(label, knownSize ? std::optional{size} : std::nullopt, size),
          output,
          TransferOptions{.jobId = label,
                          .resumeOffset = resume,
                          .temporaryRemotePath = std::nullopt,
                          .beforeFinalize = {}},
          {}, {});

      REQUIRE_FALSE(result);
      CHECK(result.error().code == RemoteErrorCode::Conflict);
      CHECK_FALSE(std::filesystem::exists(output));

      VerifyContents(part, resume);

      const auto report = fixture.Session().Stat(RemotePath{"/report/" + label}, {});

      REQUIRE_FALSE(report);
      CHECK(report.error().code == RemoteErrorCode::NotFound);
    }
  }
}
