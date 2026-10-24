#pragma once

#include <chrono>
#include <cstdint>

namespace touchfc {

// Nanoseconds from a monotonic clock. On the MSVC STL, steady_clock is
// QueryPerformanceCounter, so this is the QPC domain the rest of the app uses.
// WGC SystemRelativeTime is converted into this domain at the capture edge
// (see app/src/win_time.hpp); core itself includes no Windows headers.
using TimeNs = std::int64_t;

inline TimeNs now_ns() noexcept {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace touchfc
