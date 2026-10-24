#pragma once

#include "touchfc/spsc_ring.hpp"
#include "touchfc/wake.hpp"

#include <atomic>
#include <cstdint>
#include <utility>

namespace touchfc {

// SpscRing plus a generation so a blocked side can sleep instead of spinning
// for a whole frame time. One producer, one consumer, same rules as SpscRing.
//
// notify_one on every push/pop is a syscall only when a waiter exists. At
// 25-50 fps that cost is invisible. Microbenchmarks that care about the last
// nanosecond should use SpscRing directly.
template <class T, std::size_t N, std::size_t Pad = 64>
class BlockingSpsc {
 public:
  bool try_push(const T& value) noexcept {
    if (!ring_.try_push(value)) return false;
    data_.notify();
    return true;
  }

  bool try_pop(T& out) noexcept {
    if (!ring_.try_pop(out)) return false;
    space_.notify();
    return true;
  }

  // Blocks until pushed or `stop` is set. A true `stop` with a full ring
  // returns false and does not push.
  bool push_wait(const T& value, const std::atomic<bool>& stop, int spins = 256) noexcept {
    std::uint32_t seen = space_.load();
    for (;;) {
      if (ring_.try_push(value)) {
        data_.notify();
        return true;
      }
      if (stop.load(std::memory_order_acquire)) return false;
      seen = space_.wait_from(seen, spins);
    }
  }

  // Blocks until an item is popped, or `done` is set and the ring is empty.
  // The producer must call wake() after storing `done` so a sleeper rechecks.
  bool pop_until(T& out, const std::atomic<bool>& done, int spins = 256) noexcept {
    std::uint32_t seen = data_.load();
    for (;;) {
      if (ring_.try_pop(out)) {
        space_.notify();
        return true;
      }
      if (done.load(std::memory_order_acquire)) return false;
      seen = data_.wait_from(seen, spins);
    }
  }

  // Blocks until an item is popped, or `stop` is set and the ring is empty.
  bool pop_wait(T& out, const std::atomic<bool>& stop, int spins = 256) noexcept {
    std::uint32_t seen = data_.load();
    for (;;) {
      if (ring_.try_pop(out)) {
        space_.notify();
        return true;
      }
      if (stop.load(std::memory_order_acquire)) {
        if (ring_.try_pop(out)) {
          space_.notify();
          return true;
        }
        return false;
      }
      seen = data_.wait_from(seen, spins);
    }
  }

  // Wake both sides. Used on shutdown so a waiter observes `stop`.
  void wake() noexcept {
    data_.notify_all();
    space_.notify_all();
  }

  std::size_t size_approx() const noexcept { return ring_.size_approx(); }
  static constexpr std::size_t capacity() noexcept { return N; }

 private:
  SpscRing<T, N, Pad> ring_{};
  WakeSeq data_{};
  WakeSeq space_{};
};

}  // namespace touchfc
