#pragma once

#include "touchfc/types.hpp"

#include <cstdint>
#include <string>

namespace touchfc {

struct LatencySnapshot {
  std::int64_t p50_ns[static_cast<int>(Stage::Count)]{};
  std::int64_t p99_ns[static_cast<int>(Stage::Count)]{};
  std::int64_t p999_ns[static_cast<int>(Stage::Count)]{};
  std::int64_t max_ns[static_cast<int>(Stage::Count)]{};
  std::uint64_t count[static_cast<int>(Stage::Count)]{};
};

// HdrHistogram per stage. Only the telemetry thread calls record/snapshot/write.
class HdrSink {
 public:
  HdrSink();
  ~HdrSink();
  HdrSink(const HdrSink&) = delete;
  HdrSink& operator=(const HdrSink&) = delete;

  void record(const TelemetrySample& sample) noexcept;
  LatencySnapshot snapshot() const noexcept;
  // Writes one CSV of percentiles. Returns false if the file cannot be opened.
  bool write_csv(const std::string& path) const;

 private:
  struct Hist;
  Hist* hist_ = nullptr;
};

}  // namespace touchfc
