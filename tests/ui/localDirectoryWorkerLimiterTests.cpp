// SPDX-License-Identifier: MIT

#include "ui/localDirectoryWorkerLimiter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

TEST_CASE("local directory worker limiter enforces one shared bound")
{
  using havremote::ui::detail::LocalDirectoryWorkerLimiter;

  LocalDirectoryWorkerLimiter limiter{4U};

  std::vector<LocalDirectoryWorkerLimiter::Permit> workers;
  workers.reserve(limiter.Limit());

  for (std::size_t index = 0; index < limiter.Limit(); ++index)
  {
    auto permit = limiter.TryAcquire();

    REQUIRE(permit);

    workers.push_back(std::move(*permit));
  }

  CHECK(limiter.ActiveCount() == 4U);
  CHECK_FALSE(limiter.TryAcquire());

  workers.pop_back();

  CHECK(limiter.ActiveCount() == 3U);
  CHECK(limiter.TryAcquire());
}

TEST_CASE("local directory worker permits release during moves and unwinding")
{
  using havremote::ui::detail::LocalDirectoryWorkerLimiter;

  LocalDirectoryWorkerLimiter limiter{1U};

  try
  {
    auto acquired = limiter.TryAcquire();

    REQUIRE(acquired);

    auto moved = std::move(*acquired);

    acquired.reset();

    CHECK(limiter.ActiveCount() == 1U);

    (void)moved;

    throw std::runtime_error{"simulated thread setup failure"};
  }
  catch (const std::runtime_error &)
  {
  }

  CHECK(limiter.ActiveCount() == 0U);
  CHECK(limiter.TryAcquire());
  CHECK_THROWS_AS(LocalDirectoryWorkerLimiter{0U}, std::invalid_argument);
}
