#pragma once

#include "touchfc/time.hpp"
#include "win_prelude.hpp"

#include <atomic>

namespace touchfc {

inline std::int64_t qpc_100ns() noexcept {
  static const std::int64_t freq = [] {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  LARGE_INTEGER c{};
  QueryPerformanceCounter(&c);
  return c.QuadPart * 10000000ll / freq;
}

// WGC SystemRelativeTime is a 100 ns QPC timestamp. Convert it into the
// steady_clock domain used by now_ns().
inline TimeNs qpc_100ns_to_app_ns(std::int64_t system_relative_100ns) noexcept {
  const std::int64_t age_ns = (qpc_100ns() - system_relative_100ns) * 100;
  const TimeNs now = now_ns();
  if (age_ns <= 0) return now;
  return now - age_ns;
}

class PreciseWait {
 public:
  PreciseWait() {
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
    if (!timer_) timer_ = CreateWaitableTimerW(nullptr, TRUE, nullptr);
  }
  ~PreciseWait() {
    if (timer_) CloseHandle(timer_);
  }
  PreciseWait(const PreciseWait&) = delete;
  PreciseWait& operator=(const PreciseWait&) = delete;

  void until(TimeNs deadline, const std::atomic<bool>& stop) noexcept {
    if (!timer_) return;
    while (!stop.load(std::memory_order_acquire)) {
      const TimeNs now = now_ns();
      if (now >= deadline) return;
      std::int64_t remain_100ns = (deadline - now) / 100;
      if (remain_100ns > 200000) remain_100ns = 200000;  // 20 ms slices so stop stays responsive
      if (remain_100ns < 1) remain_100ns = 1;
      LARGE_INTEGER due{};
      due.QuadPart = -remain_100ns;
      if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) return;
      WaitForSingleObject(timer_, 30);
    }
  }

 private:
  HANDLE timer_ = nullptr;
};

}  // namespace touchfc
