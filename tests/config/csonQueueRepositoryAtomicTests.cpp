// SPDX-License-Identifier: MIT

#include "configTestSupport.hpp"

#include "config/csonQueueRepository.hpp"

#include <havCSON.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

namespace havremote::config
{
  namespace
  {
    PersistentQueueItem PausedUpload(const std::filesystem::path &localPath)
    {
      PersistentQueueItem item;
      item.connectionId = "connection-atomic";
      item.transfer.job = TransferJob{
          .id = "job-atomic",
          .siteId = "site-atomic",
          .siteEndpoint = SiteEndpointIdentity{
              ProtocolKind::Sftp, "example.test", 22, "user"},
          .direction = TransferDirection::Upload,
          .localPath = localPath,
          .remotePath = RemotePath{"/data/source.txt"},
          .recursive = false,
          .conflictPolicy = ConflictPolicy::Ask,
          .expectedRemoteRevision = std::nullopt,
      };
      item.transfer.state = TransferState::Paused;
      item.transfer.progress.jobId = item.transfer.job.id;

      return item;
    }
  } // namespace

  TEST_CASE("Every atomic queue write failure preserves the prior queue file")
  {
    test::TempDirectory directory;

    constexpr std::array failureStages{
        havCSON::testing::AtomicWriteStage::TemporaryOpen,
        havCSON::testing::AtomicWriteStage::Write,
        havCSON::testing::AtomicWriteStage::Flush,
        havCSON::testing::AtomicWriteStage::Sync,
        havCSON::testing::AtomicWriteStage::Close,
        havCSON::testing::AtomicWriteStage::Replace,
    };

    for (std::size_t index = 0; index < failureStages.size(); ++index)
    {
      CAPTURE(index);

      const auto file = directory.Path() /
                        ("queue-" + std::to_string(index) + ".cson");

      CsonQueueRepository repository(file);

      const auto originalItem =
          PausedUpload(directory.Path() / "source.txt");

      REQUIRE(repository.Save({originalItem}).has_value());

      const auto original = test::ReadText(file);

      REQUIRE(original.find("job-atomic") != std::string::npos);

      havCSON::testing::FailAtomicWriteAt(failureStages[index]);

      const auto saved = repository.Save({});

      havCSON::testing::FailAtomicWriteAt(
          havCSON::testing::AtomicWriteStage::None);

      REQUIRE_FALSE(saved.has_value());
      CHECK(saved.error().kind == ConfigErrorKind::AtomicWrite);
      CHECK(test::ReadText(file) == original);
      CHECK(repository.Save({}).has_value());
      CHECK(test::ReadText(file) != original);

      CsonQueueRepository reloaded(file);

      const auto loaded = reloaded.Load();

      REQUIRE(loaded.has_value());
      CHECK(loaded->items.empty());
    }
  }

  TEST_CASE("An atomic queue write failure never creates a partial final file")
  {
    test::TempDirectory directory;

    constexpr std::array failureStages{
        havCSON::testing::AtomicWriteStage::TemporaryOpen,
        havCSON::testing::AtomicWriteStage::Write,
        havCSON::testing::AtomicWriteStage::Flush,
        havCSON::testing::AtomicWriteStage::Sync,
        havCSON::testing::AtomicWriteStage::Close,
        havCSON::testing::AtomicWriteStage::Replace,
    };

    for (std::size_t index = 0; index < failureStages.size(); ++index)
    {
      CAPTURE(index);

      const auto file = directory.Path() /
                        ("new-queue-" + std::to_string(index) + ".cson");

      CsonQueueRepository repository(file);

      const auto item = PausedUpload(directory.Path() / "source.txt");

      havCSON::testing::FailAtomicWriteAt(failureStages[index]);

      const auto saved = repository.Save({item});

      havCSON::testing::FailAtomicWriteAt(
          havCSON::testing::AtomicWriteStage::None);

      REQUIRE_FALSE(saved.has_value());
      CHECK(saved.error().kind == ConfigErrorKind::AtomicWrite);
      CHECK_FALSE(std::filesystem::exists(file));
    }
  }
} // namespace havremote::config
