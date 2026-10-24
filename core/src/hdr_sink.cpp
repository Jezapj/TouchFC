#include "touchfc/hdr_sink.hpp"

#include <hdr/hdr_histogram.h>

#include <cstdio>

namespace touchfc {
namespace {

constexpr int kStages = static_cast<int>(Stage::Count);

std::int64_t duration_ns(const TelemetrySample& sample) noexcept {
  const std::int64_t d = sample.t_end - sample.t_begin;
  return d > 0 ? d : 1;
}

}  // namespace

struct HdrSink::Hist {
  hdr_histogram* h[kStages]{};
};

HdrSink::HdrSink() : hist_(new Hist) {
  for (int i = 0; i < kStages; ++i) {
    // 1 ns .. 60 s, 3 significant figures.
    hdr_init(1, 60ll * 1000 * 1000 * 1000, 3, &hist_->h[i]);
  }
}

HdrSink::~HdrSink() {
  if (!hist_) return;
  for (int i = 0; i < kStages; ++i) hdr_close(hist_->h[i]);
  delete hist_;
}

void HdrSink::record(const TelemetrySample& sample) noexcept {
  const int stage = static_cast<int>(sample.stage);
  if (!hist_ || stage < 0 || stage >= kStages || hist_->h[stage] == nullptr) return;
  hdr_record_value(hist_->h[stage], duration_ns(sample));
}

LatencySnapshot HdrSink::snapshot() const noexcept {
  LatencySnapshot out;
  if (!hist_) return out;
  for (int i = 0; i < kStages; ++i) {
    hdr_histogram* h = hist_->h[i];
    if (!h || h->total_count == 0) continue;
    out.count[i] = static_cast<std::uint64_t>(h->total_count);
    out.p50_ns[i] = hdr_value_at_percentile(h, 50.0);
    out.p99_ns[i] = hdr_value_at_percentile(h, 99.0);
    out.p999_ns[i] = hdr_value_at_percentile(h, 99.9);
    out.max_ns[i] = hdr_max(h);
  }
  return out;
}

bool HdrSink::write_csv(const std::string& path) const {
#if defined(_MSC_VER)
  std::FILE* f = nullptr;
  if (fopen_s(&f, path.c_str(), "w") != 0) f = nullptr;
#else
  std::FILE* f = std::fopen(path.c_str(), "w");
#endif
  if (!f) return false;
  std::fputs("stage,count,p50_ns,p99_ns,p999_ns,max_ns\n", f);
  static constexpr const char* kNames[] = {"capture", "preprocess", "inference", "track", "present"};
  const LatencySnapshot snap = snapshot();
  for (int i = 0; i < kStages; ++i) {
    std::fprintf(f, "%s,%llu,%lld,%lld,%lld,%lld\n", kNames[i],
                 static_cast<unsigned long long>(snap.count[i]),
                 static_cast<long long>(snap.p50_ns[i]), static_cast<long long>(snap.p99_ns[i]),
                 static_cast<long long>(snap.p999_ns[i]), static_cast<long long>(snap.max_ns[i]));
  }
  std::fclose(f);
  return true;
}

}  // namespace touchfc
