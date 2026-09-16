// SPDX-License-Identifier: MIT

#include "configTestSupport.hpp"
#include "config/siteTransfer.hpp"

#include <havCSON.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <iterator>

namespace havremote::config
{
  TEST_CASE("every atomic site export failure preserves an existing export")
  {
    test::TempDirectory temporary;

    SiteProfile site;
    site.id = "source";
    site.name = "Example";
    site.host = "example.test";

    const SiteTransferData source{{site}, {}, {site.id}};

    constexpr std::array stages{
        havCSON::testing::AtomicWriteStage::TemporaryOpen,
        havCSON::testing::AtomicWriteStage::Write,
        havCSON::testing::AtomicWriteStage::Flush,
        havCSON::testing::AtomicWriteStage::Sync,
        havCSON::testing::AtomicWriteStage::Close,
        havCSON::testing::AtomicWriteStage::Replace};

    constexpr std::array operations{"open", "write", "flush", "sync", "close", "replace"};

    for (std::size_t index = 0; index < stages.size(); ++index)
    {
      CAPTURE(index);

      const auto file = temporary.Path() / (std::to_string(index) + ".cson");

      REQUIRE(WriteSiteExport(file, source));

      const auto previous = test::ReadText(file);

      havCSON::testing::FailAtomicWriteAt(stages[index]);

      const auto result = WriteSiteExport(file, {});

      havCSON::testing::FailAtomicWriteAt(havCSON::testing::AtomicWriteStage::None);

      REQUIRE_FALSE(result);
      CHECK(result.error().kind == ConfigErrorKind::AtomicWrite);
      CHECK(result.error().message.find("(" + std::string{operations[index]} + ")") !=
            std::string::npos);
      CHECK(result.error().message.find(std::make_error_code(std::errc::io_error).message()) !=
            std::string::npos);
      CHECK(test::ReadText(file) == previous);
      CHECK(std::distance(std::filesystem::directory_iterator{temporary.Path()},
                          std::filesystem::directory_iterator{}) ==
            static_cast<std::ptrdiff_t>(index + 1U));

      const auto reloaded = ReadSiteExport(file);

      INFO((reloaded ? "existing export reloaded" : reloaded.error().message));
      REQUIRE(reloaded);
      REQUIRE(WriteSiteExport(file, {}));
      CHECK(test::ReadText(file) != previous);
    }
  }

  TEST_CASE("atomic site export failures never publish partial new files")
  {
    test::TempDirectory temporary;

    constexpr std::array stages{
        havCSON::testing::AtomicWriteStage::TemporaryOpen,
        havCSON::testing::AtomicWriteStage::Write,
        havCSON::testing::AtomicWriteStage::Flush,
        havCSON::testing::AtomicWriteStage::Sync,
        havCSON::testing::AtomicWriteStage::Close,
        havCSON::testing::AtomicWriteStage::Replace};

    for (const auto stage : stages)
    {
      const auto file = temporary.Path() / "new.cson";

      havCSON::testing::FailAtomicWriteAt(stage);

      const auto result = WriteSiteExport(file, {});

      havCSON::testing::FailAtomicWriteAt(havCSON::testing::AtomicWriteStage::None);

      REQUIRE_FALSE(result);
      CHECK(result.error().kind == ConfigErrorKind::AtomicWrite);
      CHECK_FALSE(std::filesystem::exists(file));
      CHECK(std::filesystem::is_empty(temporary.Path()));
    }
  }
} // namespace havremote::config
