#pragma once

#include "touchfc/cpu.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <type_traits>

namespace touchfc {

// Single-writer, multi-reader seqlock. The payload is stored as relaxed atomic
// words so a reader racing the writer is not a data race (P1478 / Boehm).
// A plain memcpy of a non-atomic payload would be undefined even if the reader
// retries.
//
// Writer (this thread only):
//   seq = odd            relaxed     (readers who observe this retry)
//   release fence                    (odd seq is ordered before the words)
//   words                relaxed
//   seq = even           release     (words are ordered before the even seq)
//
// Reader:
//   s0 = seq             acquire     (if this observes an even seq, it
//                                     synchronises with that release store,
//                                     so the matching words are visible)
//   if s0 is odd, pause and retry    (do not touch the words)
//   words                relaxed
//   acquire fence                    (word loads stay before the second seq load)
//   s1 = seq             relaxed
//   accept only if s0 == s1
//
// T must be trivially copyable and a multiple of 8 bytes. Pad the caller's
// struct explicitly; do not rely on compiler tail padding.
template <class T>
class SeqLock {
  static_assert(std::is_trivially_copyable_v<T>, "SeqLock copies T as raw words");
  static_assert(sizeof(T) % 8 == 0, "SeqLock requires sizeof(T) to be a multiple of 8");
  static_assert(sizeof(T) <= 4096, "SeqLock keeps a stack copy; keep T small");
  static constexpr std::size_t kWords = sizeof(T) / 8;

 public:
  SeqLock() noexcept = default;
  explicit SeqLock(const T& initial) noexcept { store(initial); }

  SeqLock(const SeqLock&) = delete;
  SeqLock& operator=(const SeqLock&) = delete;

  void store(const T& value) noexcept {
    std::uint64_t tmp[kWords];
    std::memcpy(tmp, &value, sizeof(T));
    const std::uint64_t seq = seq_.load(std::memory_order_relaxed);
    seq_.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (std::size_t i = 0; i < kWords; ++i) {
      words_[i].store(tmp[i], std::memory_order_relaxed);
    }
    seq_.store(seq + 2, std::memory_order_release);
  }

  T load() const noexcept {
    std::uint64_t tmp[kWords];
    for (;;) {
      const std::uint64_t s0 = seq_.load(std::memory_order_acquire);
      if (s0 & 1u) {
        cpu_pause();
        continue;
      }
      for (std::size_t i = 0; i < kWords; ++i) {
        tmp[i] = words_[i].load(std::memory_order_relaxed);
      }
      std::atomic_thread_fence(std::memory_order_acquire);
      const std::uint64_t s1 = seq_.load(std::memory_order_relaxed);
      if (s0 == s1) {
        T out;
        std::memcpy(&out, tmp, sizeof(T));
        return out;
      }
    }
  }

 private:
  alignas(64) std::atomic<std::uint64_t> seq_{0};
  alignas(64) std::atomic<std::uint64_t> words_[kWords]{};
};

}  // namespace touchfc
