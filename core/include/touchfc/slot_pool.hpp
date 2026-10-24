#pragma once

#include <atomic>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>

namespace touchfc {

// Fixed pool of at most 64 slot indices. Many threads may acquire and release.
// A set bit means free. acquire clears the lowest set bit with a CAS;
// release sets the bit with fetch_or.
//
// Successful CAS is acq_rel, so it synchronises with the releaser's release
// fetch_or: the new owner sees the previous owner's writes to that slot.
// The pool index is not the payload. Callers still need their own
// happens-before for the bytes in the slot (the queue or mailbox that hands
// the index over).
class SlotPool {
 public:
  static constexpr std::uint32_t kMaxSlots = 64;

  explicit SlotPool(std::uint32_t count) noexcept : count_(count) {
    assert(count >= 1 && count <= kMaxSlots);
    const std::uint64_t mask = (count == kMaxSlots) ? ~0ull : ((1ull << count) - 1ull);
    free_.store(mask, std::memory_order_relaxed);
  }

  SlotPool(const SlotPool&) = delete;
  SlotPool& operator=(const SlotPool&) = delete;

  // Slot index, or -1 if none are free.
  int try_acquire() noexcept {
    std::uint64_t cur = free_.load(std::memory_order_relaxed);
    for (;;) {
      if (cur == 0) return -1;
      const std::uint32_t bit = static_cast<std::uint32_t>(std::countr_zero(cur));
      const std::uint64_t mask = 1ull << bit;
      if (free_.compare_exchange_weak(cur, cur & ~mask, std::memory_order_acq_rel,
                                      std::memory_order_relaxed)) {
        return static_cast<int>(bit);
      }
    }
  }

  void release(std::uint32_t index) noexcept {
    assert(index < count_);
    free_.fetch_or(1ull << index, std::memory_order_release);
  }

  std::uint32_t count() const noexcept { return count_; }

  // How many slots look free. Racy; for tests and the HUD.
  int free_approx() const noexcept {
    return std::popcount(free_.load(std::memory_order_relaxed));
  }

 private:
  std::uint32_t count_;
  alignas(64) std::atomic<std::uint64_t> free_{0};
};

}  // namespace touchfc
