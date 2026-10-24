#pragma once

#include <atomic>
#include <cstdint>

namespace touchfc {

// Latest-wins slot index. The producer publishes with exchange and gets back
// the index it displaced. If that is not kEmpty, the previous item was never
// taken: the producer releases it to the pool and counts a drop.
// The consumer takes with exchange(kEmpty).
//
// acq_rel on both exchanges: the producer writes the slot payload before
// publish (release), and the consumer reads it after take (acquire).
class Mailbox {
 public:
  static constexpr std::uint32_t kEmpty = 0xffffffffu;

  Mailbox() noexcept = default;
  Mailbox(const Mailbox&) = delete;
  Mailbox& operator=(const Mailbox&) = delete;

  // Returns the displaced index (recycle it) or kEmpty.
  std::uint32_t publish(std::uint32_t index) noexcept {
    return slot_.exchange(index, std::memory_order_acq_rel);
  }

  // Returns the newest index, or kEmpty if the mailbox was empty.
  std::uint32_t take() noexcept {
    return slot_.exchange(kEmpty, std::memory_order_acq_rel);
  }

  std::uint32_t peek() const noexcept { return slot_.load(std::memory_order_acquire); }

 private:
  alignas(64) std::atomic<std::uint32_t> slot_{kEmpty};
};

}  // namespace touchfc
