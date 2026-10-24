#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace touchfc {

// Single-producer, single-consumer ring. Capacity N is usable slots (full when
// head - tail == N). One producer thread, one consumer thread, no concurrent
// calls on the same side.
//
// Memory orders (C++ model):
// - head_ is released by the producer after the slot is written, and acquired
//   by the consumer before the slot is read. That is the happens-before edge
//   for the payload.
// - tail_ is released by the consumer after it has finished reading a slot,
//   and acquired by the producer before it reuses that slot.
// - The cached remote indices are private. A stale cache can only make a
//   side believe the ring is empty or full; the refresh uses acquire.
//
// Pad separates the producer line (head, tail cache) from the consumer line
// (tail, head cache). 128 is worth measuring: some CPUs prefetch the adjacent
// line, so 64-byte padding still false-shares.
template <class T, std::size_t N, std::size_t Pad = 64>
class SpscRing {
  static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of two and at least 2");
  static_assert(Pad >= 64 && (Pad & (Pad - 1)) == 0, "Pad must be a power of two >= 64");
  static_assert(std::is_nothrow_move_assignable_v<T> || std::is_nothrow_copy_assignable_v<T>,
                "T is assigned into a live slot");

 public:
  SpscRing() = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  bool try_push(const T& value) noexcept {
    return try_push_with([&](T& slot) noexcept { slot = value; });
  }

  // Construct straight into the slot so a large T is not copied twice.
  // `fill` must not throw: the slot is unpublished if it does not return.
  template <class F>
  bool try_push_with(F&& fill) noexcept(noexcept(fill(std::declval<T&>()))) {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    if (h - tail_cache_ == N) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (h - tail_cache_ == N) return false;
    }
    fill(buf_[h & (N - 1)]);
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_cache_) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (t == head_cache_) return false;
    }
    out = buf_[t & (N - 1)];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  // Approximate occupancy. Racy by design; not for control flow.
  std::size_t size_approx() const noexcept {
    return head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_relaxed);
  }

  static constexpr std::size_t capacity() noexcept { return N; }

 private:
  // Each of these is aligned to Pad, so the producer line and the consumer
  // line do not share a cache line. The following member then starts on the
  // next Pad boundary.
  alignas(Pad) std::atomic<std::size_t> head_{0};
  alignas(Pad) std::size_t tail_cache_{0};  // producer-only
  alignas(Pad) std::atomic<std::size_t> tail_{0};
  alignas(Pad) std::size_t head_cache_{0};  // consumer-only
  alignas(Pad) T buf_[N]{};
};

}  // namespace touchfc
