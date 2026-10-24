#pragma once

#include "touchfc/detection.hpp"
#include "touchfc/nms.hpp"
#include "touchfc/preprocess.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace touchfc {

// Decode a YOLOv8 output tensor shaped [1, 4 + num_classes, num_anchors] into
// per-image detections in pixel coordinates of the original (unletterboxed)
// frame.
//
// Channel layout: for each anchor a, columns 0..3 are (cx, cy, w, h) in
// letterbox pixels, columns 4..4+nc-1 are per-class scores (pre-sigmoid or
// post, YOLOv8 exports with sigmoid baked in). The spec allows a confidence
// threshold applied to the max class score.
//
// This is a channel-major decode: for each anchor we scan nc class scores and
// keep the top one. That is memory-bandwidth bound in the class dimension,
// which is small (80 for COCO). We stop at `out_capacity` candidates and set
// `truncated` so the caller can log it. NMS is a separate call.
inline void decode_yolov8(const float* output, std::uint32_t num_classes,
                          std::uint32_t num_anchors, float conf_thresh,
                          const LetterboxParams& letterbox, Detection* out,
                          std::uint32_t out_capacity, std::uint32_t& out_count,
                          bool& truncated) noexcept {
  out_count = 0;
  truncated = false;
  if (num_classes == 0 || num_anchors == 0 || out_capacity == 0) return;
  const std::size_t stride = num_anchors;  // one row per channel
  const float* cx_row = output + 0 * stride;
  const float* cy_row = output + 1 * stride;
  const float* w_row  = output + 2 * stride;
  const float* h_row  = output + 3 * stride;
  const float* cls_row0 = output + 4 * stride;

  for (std::uint32_t a = 0; a < num_anchors; ++a) {
    float best = 0.f;
    std::uint32_t best_c = 0;
    for (std::uint32_t c = 0; c < num_classes; ++c) {
      const float s = cls_row0[c * stride + a];
      if (s > best) {
        best = s;
        best_c = c;
      }
    }
    if (best < conf_thresh) continue;
    if (out_count >= out_capacity) {
      truncated = true;
      return;
    }
    const float cx = cx_row[a];
    const float cy = cy_row[a];
    const float ww = w_row[a];
    const float hh = h_row[a];
    float x = cx - 0.5f * ww;
    float y = cy - 0.5f * hh;
    float w = ww;
    float h = hh;
    unletterbox_box(letterbox, x, y, w, h);
    Detection& d = out[out_count++];
    d.x = x;
    d.y = y;
    d.w = w;
    d.h = h;
    d.score = best;
    d.class_id = static_cast<std::uint16_t>(best_c);
    d.track_id = 0;
  }

  // Sort by score descending so NMS can sweep in one pass.
  std::sort(out, out + out_count,
            [](const Detection& a, const Detection& b) { return a.score > b.score; });
}

}  // namespace touchfc
