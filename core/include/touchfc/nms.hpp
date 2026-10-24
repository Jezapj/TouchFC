#pragma once

#include "touchfc/detection.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace touchfc {

inline float iou_xywh(const Detection& a, const Detection& b) noexcept {
  const float ax0 = a.x, ay0 = a.y, ax1 = a.x + a.w, ay1 = a.y + a.h;
  const float bx0 = b.x, by0 = b.y, bx1 = b.x + b.w, by1 = b.y + b.h;
  const float ix0 = ax0 > bx0 ? ax0 : bx0;
  const float iy0 = ay0 > by0 ? ay0 : by0;
  const float ix1 = ax1 < bx1 ? ax1 : bx1;
  const float iy1 = ay1 < by1 ? ay1 : by1;
  const float iw = ix1 - ix0;
  const float ih = iy1 - iy0;
  if (iw <= 0 || ih <= 0) return 0.f;
  const float inter = iw * ih;
  const float ua = a.w * a.h + b.w * b.h - inter;
  return ua > 0 ? inter / ua : 0.f;
}

// Greedy NMS per class. In place: filtered boxes stay at the front, the
// return value is the kept count. Input is sorted by score descending inside
// each class group. The caller sorts.
//
// Hungarian versus greedy is an open argument. Part A3 of the review notes
// that at 22-64 players the comparison is noise; greedy wins on constant
// factor. If a benchmark later shows Hungarian beating greedy on a real
// clip, swap it in behind the same signature.
inline std::uint32_t greedy_nms(Detection* dets, std::uint32_t n,
                                float iou_thresh) noexcept {
  if (n <= 1) return n;
  std::uint32_t write = 0;
  // Per-class sweep. The input is already score-descending, so the first
  // box of a class is the strongest for that class.
  // Mark suppressed by setting score to -1 so the final compact drops them.
  for (std::uint32_t i = 0; i < n; ++i) {
    if (dets[i].score < 0) continue;
    for (std::uint32_t j = i + 1; j < n; ++j) {
      if (dets[j].score < 0) continue;
      if (dets[j].class_id != dets[i].class_id) continue;
      if (iou_xywh(dets[i], dets[j]) >= iou_thresh) dets[j].score = -1.f;
    }
  }
  for (std::uint32_t i = 0; i < n; ++i) {
    if (dets[i].score >= 0) {
      if (write != i) dets[write] = dets[i];
      ++write;
    }
  }
  return write;
}

}  // namespace touchfc
