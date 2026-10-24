#pragma once

#if defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#elif defined(_M_ARM64)
#include <intrin.h>
#endif

namespace touchfc {

// One pause so a spin wait gives up the execution unit without a syscall.
inline void cpu_pause() noexcept {
#if defined(_M_ARM64)
  __yield();
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
  __asm__ __volatile__("pause");
#elif defined(__aarch64__)
  __asm__ __volatile__("yield");
#else
  // No portable pause. The spin is still bounded.
#endif
}

}  // namespace touchfc
