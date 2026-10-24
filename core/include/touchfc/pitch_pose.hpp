#pragma once

#include "touchfc/preprocess.hpp"

#include <cstdint>

namespace touchfc {

struct PitchKeypoint {
  float x = 0.f;
  float y = 0.f;
  std::uint8_t id = 0;
};

// YOLOv8-pose, channel-major [1, 5 + 3*K, anchors]: cx, cy, w, h, class
// score, then K triples of (x, y, visibility) in letterbox pixels. One pitch
// fills the frame, so the highest-scoring anchor is the detection.
inline std::uint32_t decode_pitch_pose(const float* output, std::uint32_t num_keypoints,
                                       std::uint32_t num_anchors, float instance_conf,
                                       float keypoint_conf, const LetterboxParams& letterbox,
                                       PitchKeypoint* out, std::uint32_t out_capacity) noexcept {
  if (output == nullptr || num_keypoints == 0 || num_anchors == 0 || out_capacity == 0) return 0;
  const std::size_t stride = num_anchors;
  const float* score = output + 4 * stride;
  std::uint32_t best = 0;
  float best_s = score[0];
  for (std::uint32_t a = 1; a < num_anchors; ++a) {
    if (score[a] > best_s) {
      best_s = score[a];
      best = a;
    }
  }
  if (best_s < instance_conf) return 0;
  const float inv = letterbox.scale > 0.f ? 1.f / letterbox.scale : 1.f;
  std::uint32_t n = 0;
  for (std::uint32_t k = 0; k < num_keypoints && n < out_capacity; ++k) {
    const std::size_t base = static_cast<std::size_t>(5 + 3 * k) * stride;
    const float vis = output[base + 2 * stride + best];
    if (vis < keypoint_conf) continue;
    PitchKeypoint& kp = out[n++];
    kp.x = (output[base + best] - letterbox.pad_x) * inv;
    kp.y = (output[base + stride + best] - letterbox.pad_y) * inv;
    kp.id = static_cast<std::uint8_t>(k);
  }
  return n;
}

}  // namespace touchfc
