#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace touchfc {

// Single-writer, single-reader latest-value buffer. Three slots so the writer
// can fill one, the reader can hold one, and one is in handoff.
//
// Packed index: bits 0-1 are a slot index, bit 2 (kFresh) means the reader has
// not consumed the published slot yet.
//
// Writer: prev = idx.exchange(w | kFresh, acq_rel); w = prev & 3
// Reader: if the acquire-load shows kFresh, r = idx.exchange(r, acq_rel) & 3
//
// acq_rel both ways. The writer's release publishes the bytes in the slot.
// The reader's acquire sees them. The reader's release publishes "I am done
// with my old slot" so the writer may reuse it only after that release.
// The freshness load is acquire, not relaxed. A relaxed load is allowed to
// observe a stale set bit after this thread has already cleared it, and the
// following exchange would then swap away a buffer the reader still wants.
//
// The pointer from read() is valid until the next successful read().
// write_slot() is valid until the next publish().
template <class T>
class TripleBuffer {
  static_assert(std::is_nothrow_default_constructible_v<T>);

 public:
  static constexpr std::uint32_t kFresh = 4u;

  TripleBuffer() noexcept = default;
  TripleBuffer(const TripleBuffer&) = delete;
  TripleBuffer& operator=(const TripleBuffer&) = delete;

  T& write_slot() noexcept { return slots_[w_]; }

  void publish() noexcept {
    const std::uint32_t prev = idx_.exchange(w_ | kFresh, std::memory_order_acq_rel);
    w_ = prev & 3u;
  }

  // nullptr if nothing new has been published since the last successful read.
  T* try_read() noexcept {
    const std::uint32_t cur = idx_.load(std::memory_order_acquire);
    if ((cur & kFresh) == 0) return nullptr;
    const std::uint32_t prev = idx_.exchange(r_, std::memory_order_acq_rel);
    r_ = prev & 3u;
    have_read_ = true;
    return &slots_[r_];
  }

  // Last slot this reader swapped in. Stays valid when a later try_read finds
  // nothing new. nullptr until the first successful try_read.
  T* current() noexcept { return have_read_ ? &slots_[r_] : nullptr; }

 private:
  // Distinct start indices: writer 0, reader 1, middle 2 (not fresh).
  // Writer and reader touch different lines. idx_ is the only shared word.
  alignas(64) std::uint32_t w_ = 0;
  alignas(64) std::uint32_t r_ = 1;
  bool have_read_ = false;
  alignas(64) std::atomic<std::uint32_t> idx_{2};
  alignas(64) T slots_[3]{};
};

}  // namespace touchfc
