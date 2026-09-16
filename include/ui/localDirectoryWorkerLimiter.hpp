// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UI_LOCAL_DIRECTORY_WORKER_LIMITER_HPP
#define HAVREMOTE_INCLUDE_UI_LOCAL_DIRECTORY_WORKER_LIMITER_HPP

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace havremote::ui::detail
{
  class LocalDirectoryWorkerLimiter final
  {
  private:
    struct State final
    {
      explicit State(const std::size_t workerLimit) noexcept
          : limit(workerLimit) {}

      const std::size_t limit;
      std::atomic<std::size_t> active{};
    };

  public:
    class Permit final
    {
    public:
      Permit(const Permit &) = delete;
      Permit &operator=(const Permit &) = delete;

      Permit(Permit &&other) noexcept
          : mState(std::exchange(other.mState, {})) {}

      Permit &operator=(Permit &&other) noexcept
      {
        if (this != &other)
        {
          Release();
          mState = std::exchange(other.mState, {});
        }
        return *this;
      }

      ~Permit() { Release(); }

    private:
      friend class LocalDirectoryWorkerLimiter;

      explicit Permit(std::shared_ptr<State> state) noexcept
          : mState(std::move(state)) {}

      void Release() noexcept
      {
        if (mState)
        {
          mState->active.fetch_sub(1U, std::memory_order_acq_rel);
          mState.reset();
        }
      }

      std::shared_ptr<State> mState;
    };

    explicit LocalDirectoryWorkerLimiter(const std::size_t limit)
        : mState(std::make_shared<State>(limit))
    {
      if (limit == 0U)
      {
        throw std::invalid_argument{
            "A local directory worker limit must be positive"};
      }
    }

    LocalDirectoryWorkerLimiter(const LocalDirectoryWorkerLimiter &) = delete;
    LocalDirectoryWorkerLimiter &operator=(
        const LocalDirectoryWorkerLimiter &) = delete;

    [[nodiscard]] std::optional<Permit> TryAcquire() noexcept
    {
      auto active = mState->active.load(std::memory_order_acquire);
      while (active < mState->limit)
      {
        if (mState->active.compare_exchange_weak(
                active,
                active + 1U,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
          return Permit{mState};
        }
      }
      return std::nullopt;
    }

    [[nodiscard]] std::size_t ActiveCount() const noexcept
    {
      return mState->active.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t Limit() const noexcept { return mState->limit; }

  private:
    std::shared_ptr<State> mState;
  };
} // namespace havremote::ui::detail

#endif // HAVREMOTE_INCLUDE_UI_LOCAL_DIRECTORY_WORKER_LIMITER_HPP
