#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace touchfc {

// Bump allocator. Not thread-safe. One arena per thread, reset when that
// thread finishes a frame's scratch. Allocation failure returns nullptr;
// there is no throw on the hot path. The backing store is one allocation
// made by the constructor.
class Arena {
 public:
  explicit Arena(std::size_t bytes)
      : mem_(static_cast<std::byte*>(::operator new(bytes, std::align_val_t{64}))),
        cap_(bytes),
        off_(0) {}

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  Arena(Arena&& other) noexcept : mem_(other.mem_), cap_(other.cap_), off_(other.off_) {
    other.mem_ = nullptr;
    other.cap_ = 0;
    other.off_ = 0;
  }

  Arena& operator=(Arena&&) = delete;

  ~Arena() { release(); }

  void* allocate(std::size_t bytes, std::size_t alignment = 64) noexcept {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mem_);
    const std::uintptr_t raw = base + off_;
    const std::uintptr_t aligned = (raw + (alignment - 1)) & ~(alignment - 1);
    const std::size_t next = static_cast<std::size_t>(aligned - base + bytes);
    if (mem_ == nullptr || next > cap_) return nullptr;
    off_ = next;
    return reinterpret_cast<void*>(aligned);
  }

  template <class T>
  T* create_default() noexcept {
    void* p = allocate(sizeof(T), alignof(T) < 64 ? 64 : alignof(T));
    if (!p) return nullptr;
    return new (p) T{};
  }

  void reset() noexcept { off_ = 0; }
  std::size_t used() const noexcept { return off_; }
  std::size_t capacity() const noexcept { return cap_; }

 private:
  void release() noexcept {
    if (mem_) ::operator delete(mem_, std::align_val_t{64});
    mem_ = nullptr;
  }

  std::byte* mem_;
  std::size_t cap_;
  std::size_t off_;
};

}  // namespace touchfc
