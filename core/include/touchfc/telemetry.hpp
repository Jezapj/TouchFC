#pragma once

#include "touchfc/spsc_ring.hpp"
#include "touchfc/types.hpp"

#include <memory>

namespace touchfc {

// One SPSC ring per producer thread. The telemetry thread is the only consumer.
// A full ring drops the sample. Telemetry must never stall the pipeline.
class TelemetryHub {
 public:
  TelemetryHub() {
    for (int i = 0; i < kTelemetryProducers; ++i) {
      rings_[i] = std::make_unique<Ring>();
    }
  }

  bool push(int producer, const TelemetrySample& sample) noexcept {
    if (producer < 0 || producer >= kTelemetryProducers || !rings_[producer]) return false;
    return rings_[producer]->try_push(sample);
  }

  template <class F>
  std::size_t drain(F&& fn) {
    std::size_t n = 0;
    TelemetrySample sample;
    for (int i = 0; i < kTelemetryProducers; ++i) {
      while (rings_[i] && rings_[i]->try_pop(sample)) {
        fn(sample);
        ++n;
      }
    }
    return n;
  }

 private:
  using Ring = SpscRing<TelemetrySample, 1024>;
  std::unique_ptr<Ring> rings_[kTelemetryProducers]{};
};

}  // namespace touchfc
