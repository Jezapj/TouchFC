#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace touchfc {

struct GmcTranslation {
  float dx = 0.f;
  float dy = 0.f;
  float confidence = 0.f;
};

// Median translation of matched centroids. Outliers (a bad match) do not move it.
inline GmcTranslation estimate_translation(const float* px, const float* py, const float* cx,
                                           const float* cy, std::uint32_t n) noexcept {
  if (n == 0) return {};
  constexpr std::uint32_t kCap = 128;
  float dxs[kCap], dys[kCap];
  const std::uint32_t use = n < kCap ? n : kCap;
  for (std::uint32_t i = 0; i < use; ++i) {
    dxs[i] = cx[i] - px[i];
    dys[i] = cy[i] - py[i];
  }
  std::sort(dxs, dxs + use);
  std::sort(dys, dys + use);
  const float mx = dxs[use / 2];
  const float my = dys[use / 2];
  std::uint32_t ok = 0;
  for (std::uint32_t i = 0; i < use; ++i)
    if (std::abs(dxs[i] - mx) < 6.f && std::abs(dys[i] - my) < 6.f) ++ok;
  return {mx, my, static_cast<float>(ok) / static_cast<float>(use)};
}

}  // namespace touchfc
