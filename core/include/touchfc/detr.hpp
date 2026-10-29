#pragma once

#include "touchfc/detection.hpp"
#include "touchfc/preprocess.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace touchfc {

// TouchFC class ids: 0 player, 1 goalkeeper, 2 referee, 3 ball. A model's own
// labels map onto these by name; anything else (crowd, ads, a background
// slot) maps to -1 and is dropped.
inline int touchfc_class_of(std::string_view name) noexcept {
  if (name == "player" || name == "players" || name == "person") return 0;
  if (name == "goalkeeper" || name == "goalkeepers" || name == "goalie") return 1;
  if (name == "referee" || name == "referees") return 2;
  if (name == "ball" || name == "football" || name == "sports ball") return 3;
  return -1;
}

// Largest class count a map covers (RF-DETR COCO heads are 91).
inline constexpr std::uint32_t kMaxModelClasses = 128;

// Keep detections whose class maps to a TouchFC id, rewriting class_id.
// Order is kept, so a score-sorted list stays sorted.
inline std::uint32_t apply_class_map(Detection* d, std::uint32_t n, const std::int8_t* map,
                                     std::uint32_t map_n) noexcept {
  if (map == nullptr) return n;
  std::uint32_t kept = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::uint32_t c = d[i].class_id;
    if (c >= map_n || map[c] < 0) continue;
    d[kept] = d[i];
    d[kept++].class_id = static_cast<std::uint16_t>(map[c]);
  }
  return kept;
}

// DETR-family heads (RT-DETR, D-FINE, DEIM, RF-DETR) emit a fixed set of
// queries, one box each, with duplicates already suppressed: no NMS.
enum class DetrBox : std::uint8_t {
  CxcywhNorm,  // centre and size in [0,1] of the model input (Ultralytics, RF-DETR)
  XyxyPx,      // corners in model-input pixels (D-FINE / RT-DETR deploy export)
};

namespace detr_detail {

inline bool to_box(const float* b, DetrBox fmt, const LetterboxParams& lb, Detection& d) noexcept {
  float x, y, w, h;
  if (fmt == DetrBox::CxcywhNorm) {
    const float dw = static_cast<float>(lb.dst_w), dh = static_cast<float>(lb.dst_h);
    w = b[2] * dw;
    h = b[3] * dh;
    x = b[0] * dw - 0.5f * w;
    y = b[1] * dh - 0.5f * h;
  } else {
    x = b[0];
    y = b[1];
    w = b[2] - b[0];
    h = b[3] - b[1];
  }
  if (!(w > 0.f && h > 0.f)) return false;
  unletterbox_box(lb, x, y, w, h);
  d.x = x;
  d.y = y;
  d.w = w;
  d.h = h;
  d.track_id = 0;
  return true;
}

// Insert keeping the best `cap`: a full list replaces its weakest entry.
inline void keep_best(const Detection& d, Detection* out, std::uint32_t cap, std::uint32_t& n,
                      bool& truncated) noexcept {
  if (n < cap) {
    out[n++] = d;
    return;
  }
  truncated = true;
  std::uint32_t lo = 0;
  for (std::uint32_t i = 1; i < n; ++i)
    if (out[i].score < out[lo].score) lo = i;
  if (d.score > out[lo].score) out[lo] = d;
}

inline void sort_by_score(Detection* out, std::uint32_t n) noexcept {
  for (std::uint32_t i = 1; i < n; ++i) {  // n <= kMaxDetections, insertion sort
    const Detection d = out[i];
    std::uint32_t j = i;
    for (; j > 0 && out[j - 1].score < d.score; --j) out[j] = out[j - 1];
    out[j] = d;
  }
}

inline float sigmoid(float x) noexcept { return 1.f / (1.f + std::exp(-x)); }

}  // namespace detr_detail

// Dense head: per-query class scores, as probabilities or logits (any value
// outside [0,1] means logits; checked per call, it costs one pass over Q x C).
// Query q's box is boxes[q * box_stride + 0..3], its class c score
// scores[q * score_stride + c]. Classes the map drops never win a query, so a
// background slot cannot hide a player. Returns the count, sorted by score.
inline std::uint32_t decode_detr_dense(const float* boxes, std::uint32_t box_stride,
                                       const float* scores, std::uint32_t score_stride,
                                       std::uint32_t num_queries, std::uint32_t num_classes,
                                       DetrBox fmt, float conf, const LetterboxParams& lb,
                                       const std::int8_t* map, std::uint32_t map_n, Detection* out,
                                       std::uint32_t cap, bool& truncated) noexcept {
  truncated = false;
  if (boxes == nullptr || scores == nullptr || num_classes == 0 || cap == 0) return 0;
  bool logits = false;
  for (std::uint32_t q = 0; q < num_queries && !logits; ++q)
    for (std::uint32_t c = 0; c < num_classes; ++c) {
      const float s = scores[static_cast<std::size_t>(q) * score_stride + c];
      if (s < 0.f || s > 1.f) {
        logits = true;
        break;
      }
    }
  const float c01 = conf < 1e-6f ? 1e-6f : conf > 1.f - 1e-6f ? 1.f - 1e-6f : conf;
  const float thresh = logits ? std::log(c01 / (1.f - c01)) : conf;
  std::uint32_t n = 0;
  for (std::uint32_t q = 0; q < num_queries; ++q) {
    const float* s = scores + static_cast<std::size_t>(q) * score_stride;
    float best = thresh;
    int best_c = -1;
    for (std::uint32_t c = 0; c < num_classes; ++c) {
      if (map != nullptr && (c >= map_n || map[c] < 0)) continue;
      if (s[c] >= best) {
        best = s[c];
        best_c = static_cast<int>(c);
      }
    }
    if (best_c < 0) continue;
    Detection d{};
    if (!detr_detail::to_box(boxes + static_cast<std::size_t>(q) * box_stride, fmt, lb, d)) continue;
    d.score = logits ? detr_detail::sigmoid(best) : best;
    d.class_id = static_cast<std::uint16_t>(map != nullptr ? map[best_c] : best_c);
    detr_detail::keep_best(d, out, cap, n, truncated);
  }
  detr_detail::sort_by_score(out, n);
  return n;
}

// Sparse head (D-FINE / RT-DETR deploy export): labels [Q] int64, boxes
// [Q, 4], scores [Q] already post-sigmoid and top-k.
inline std::uint32_t decode_detr_sparse(const std::int64_t* labels, const float* boxes,
                                        const float* scores, std::uint32_t num_queries,
                                        DetrBox fmt, float conf, const LetterboxParams& lb,
                                        const std::int8_t* map, std::uint32_t map_n,
                                        Detection* out, std::uint32_t cap,
                                        bool& truncated) noexcept {
  truncated = false;
  if (labels == nullptr || boxes == nullptr || scores == nullptr || cap == 0) return 0;
  std::uint32_t n = 0;
  for (std::uint32_t q = 0; q < num_queries; ++q) {
    if (scores[q] < conf || labels[q] < 0) continue;
    const std::uint32_t c = static_cast<std::uint32_t>(labels[q]);
    int id = static_cast<int>(c);
    if (map != nullptr) {
      if (c >= map_n || map[c] < 0) continue;
      id = map[c];
    } else if (c > 0xffffu) {
      continue;
    }
    Detection d{};
    if (!detr_detail::to_box(boxes + static_cast<std::size_t>(q) * 4, fmt, lb, d)) continue;
    d.score = scores[q];
    d.class_id = static_cast<std::uint16_t>(id);
    detr_detail::keep_best(d, out, cap, n, truncated);
  }
  detr_detail::sort_by_score(out, n);
  return n;
}

}  // namespace touchfc
