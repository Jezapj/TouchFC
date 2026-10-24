#pragma once

// Global motion compensation. In a broadcast soccer feed the camera pans
// almost constantly. A tracker that extrapolates in image space without
// correcting for the camera will see every object move together between
// frames and lose IDs on pans.
//
// Image-based GMC (sparse optical flow + estimateAffinePartial2D, or ORB
// features) is the gold standard and is a later milestone. This file ships
// the fallback: estimate a frame-to-frame 2D translation from matched
// detection centroids. It is good enough for a stub detector and for close
// shots; wide pans will still slip IDs until image-based GMC lands.
//
// The estimator is a weighted median of per-pair displacements, which
// is robust to a few outliers (a wrong match or a fast-moving ball). No
// allocation; everything is on the stack.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace touchfc {

struct GmcTranslation {
  float dx = 0.f;
  float dy = 0.f;
  float confidence = 0.f;  // fraction of pairs that voted; 0 means no estimate
};

// Up to 128 pairs. The caller is matched detections from frame N-1 and frame N.
inline GmcTranslation estimate_translation(const float* prev_cx, const float* prev_cy,
                                            const float* curr_cx, const float* curr_cy,
                                            std::uint32_t n) noexcept {
  if (n == 0) return {};
  constexpr std::uint32_t kCap = 128;
  float dxs[kCap];
  float dys[kCap];
  const std::uint32_t use = n < kCap ? n : kCap;
  for (std::uint32_t i = 0; i < use; ++i) {
    dxs[i] = curr_cx[i] - prev_cx[i];
    dys[i] = curr_cy[i] - prev_cy[i];
  }
  std::sort(dxs, dxs + use);
  std::sort(dys, dys + use);
  const float mx = dxs[use / 2];
  const float my = dys[use / 2];
  // Count inliers within a tight window of the median: those are the ones
  // likely to be camera motion and not player motion.
  const float win = 6.f;
  std::uint32_t ok = 0;
  for (std::uint32_t i = 0; i < use; ++i) {
    if (std::abs(dxs[i] - mx) < win && std::abs(dys[i] - my) < win) ++ok;
  }
  return {mx, my, static_cast<float>(ok) / static_cast<float>(use)};
}

}  // namespace touchfc
