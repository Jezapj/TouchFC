#pragma once

#include "touchfc/cpu.hpp"

#include <atomic>
#include <cstdint>

namespace touchfc {

// Generation counter for "something changed". notify() is a release fetch_add
// plus notify_one (WaitOnAddress on Windows). wait() spins, then sleeps until
// the generation moves. A notify that lands before wait() is not lost: wait
// reloads and returns if the value already differs.
class WakeSeq {
 public:
  void notify() noexcept {
    seq_.fetch_add(1, std::memory_order_release);
    seq_.notify_one();
  }

  void notify_all() noexcept {
    seq_.fetch_add(1, std::memory_order_release);
    seq_.notify_all();
  }

  // Returns the generation observed on return.
  std::uint32_t wait_from(std::uint32_t seen, int spin_iters) const noexcept {
    for (int i = 0; i < spin_iters; ++i) {
      const std::uint32_t now = seq_.load(std::memory_order_acquire);
      if (now != seen) return now;
      cpu_pause();
    }
    seq_.wait(seen, std::memory_order_acquire);
    return seq_.load(std::memory_order_acquire);
  }

  std::uint32_t load() const noexcept { return seq_.load(std::memory_order_acquire); }

 private:
  alignas(64) std::atomic<std::uint32_t> seq_{0};
};

}  // namespace touchfc
