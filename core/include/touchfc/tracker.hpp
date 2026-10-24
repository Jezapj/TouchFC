#pragma once

// Bounded, allocation-free tracker. ByteTrack-lite:
//   1. Predict every active track forward by dt. Shift by GMC if available.
//   2. Match high-score detections to confirmed tracks greedily by IoU.
//   3. Match remaining tracks to low-score detections (ByteTrack's trick).
//   4. Spawn new tentative tracks from unmatched high-score detections.
//   5. Confirm tentative tracks that survive a few hits. Delete tracks that
//      miss too many frames.
//
// Capacity is fixed. Every scratch array is on the stack or in the tracker.
// This is M3 of docs/spec_v2.md.

#include "touchfc/detection.hpp"
#include "touchfc/gmc.hpp"
#include "touchfc/kalman.hpp"
#include "touchfc/nms.hpp"

#include <algorithm>
#include <cstdint>

namespace touchfc {

inline constexpr std::uint32_t kMaxTracks = 64;

struct Track {
  TrackKalman kf{};
  std::uint16_t id = 0;        // 0 means empty slot
  std::uint16_t class_id = 0;
  float score = 0.f;
  std::uint32_t age = 0;        // frames since spawn
  std::uint32_t hits = 0;       // consecutive successful matches
  std::uint32_t misses = 0;     // consecutive missed frames
  bool confirmed = false;

  float cx() const noexcept { return kf.cx.p; }
  float cy() const noexcept { return kf.cy.p; }
  float w() const noexcept { return kf.w.p; }
  float h() const noexcept { return kf.h.p; }

  Detection to_detection() const noexcept {
    Detection d{};
    d.w = w();
    d.h = h();
    d.x = cx() - 0.5f * d.w;
    d.y = cy() - 0.5f * d.h;
    d.score = score;
    d.class_id = class_id;
    d.track_id = id;
    return d;
  }
};

struct TrackerParams {
  float dt_default = 0.02f;         // 50 fps nominal
  float iou_hi = 0.30f;             // match threshold for high-conf pass
  float iou_lo = 0.10f;             // match threshold for low-conf pass
  float conf_high = 0.50f;          // split threshold between hi/lo detections
  float conf_new_track = 0.60f;     // minimum to spawn a new track
  std::uint32_t confirm_hits = 3;   // consecutive hits to confirm a tentative
  std::uint32_t max_misses_tent = 5;
  std::uint32_t max_misses_conf = 30;
  TrackKalman::NoiseParams kalman{};
};

class Tracker {
 public:
  Track tracks[kMaxTracks]{};

  // Returns the number of active tracks after update. Fills `tracks` in place.
  // `dets` must be sorted or unsorted; the tracker reads them as-is.
  // On return `dets[i].track_id` is set for every associated detection and 0
  // for the unmatched ones.
  std::uint32_t update(Detection* dets, std::uint32_t n_dets, float dt,
                       const GmcTranslation& gmc, const TrackerParams& p) noexcept {
    // Predict.
    for (auto& t : tracks) {
      if (t.id == 0) continue;
      t.kf.predict(dt, p.kalman);
      t.kf.shift(gmc.dx, gmc.dy);
      ++t.age;
    }

    // Separate detections by confidence. We work on indices into dets[].
    std::uint32_t hi_idx[kMaxDetections];
    std::uint32_t lo_idx[kMaxDetections];
    std::uint32_t n_hi = 0, n_lo = 0;
    for (std::uint32_t i = 0; i < n_dets; ++i) {
      dets[i].track_id = 0;
      if (dets[i].score >= p.conf_high) hi_idx[n_hi++] = i;
      else lo_idx[n_lo++] = i;
    }

    bool det_used[kMaxDetections]{};
    bool trk_used[kMaxTracks]{};

    greedy_match(dets, hi_idx, n_hi, trk_used, det_used, p.iou_hi);
    greedy_match(dets, lo_idx, n_lo, trk_used, det_used, p.iou_lo);

    // Update matched tracks (both passes handled inside greedy_match).
    // Mark unmatched tracks as missed.
    for (auto& t : tracks) {
      if (t.id == 0) continue;
      if (!trk_used[&t - tracks]) {
        ++t.misses;
        t.hits = 0;
      }
    }

    // Spawn new tracks from unmatched high-confidence detections.
    for (std::uint32_t i = 0; i < n_hi; ++i) {
      const std::uint32_t di = hi_idx[i];
      if (det_used[di]) continue;
      if (dets[di].score < p.conf_new_track) continue;
      if (!spawn(dets[di])) break;  // tracker full
    }

    // Age out dead tracks.
    for (auto& t : tracks) {
      if (t.id == 0) continue;
      const std::uint32_t cap = t.confirmed ? p.max_misses_conf : p.max_misses_tent;
      if (t.misses > cap) t = {};
    }

    // Confirm tentatives.
    for (auto& t : tracks) {
      if (t.id == 0 || t.confirmed) continue;
      if (t.hits >= p.confirm_hits) t.confirmed = true;
    }

    std::uint32_t count = 0;
    for (const auto& t : tracks) {
      if (t.id != 0) ++count;
    }
    return count;
  }

  void reset() noexcept {
    for (auto& t : tracks) t = {};
    next_id_ = 1;
  }

 private:
  std::uint16_t next_id_ = 1;

  void greedy_match(Detection* dets, const std::uint32_t* pool, std::uint32_t n_pool,
                    bool* trk_used, bool* det_used, float iou_thresh) noexcept {
    for (;;) {
      float best = iou_thresh;
      int best_t = -1;
      int best_d = -1;
      for (std::uint32_t ti = 0; ti < kMaxTracks; ++ti) {
        if (tracks[ti].id == 0 || trk_used[ti]) continue;
        const Detection pred = tracks[ti].to_detection();
        for (std::uint32_t pi = 0; pi < n_pool; ++pi) {
          const std::uint32_t di = pool[pi];
          if (det_used[di]) continue;
          // Only match within class, but let the ball (class 3) match itself
          // generously.
          if (dets[di].class_id != tracks[ti].class_id) continue;
          const float v = iou_xywh(pred, dets[di]);
          if (v > best) {
            best = v;
            best_t = static_cast<int>(ti);
            best_d = static_cast<int>(di);
          }
        }
      }
      if (best_t < 0) break;
      Track& t = tracks[best_t];
      Detection& d = dets[best_d];
      const float cx = d.x + 0.5f * d.w;
      const float cy = d.y + 0.5f * d.h;
      t.kf.update_box(cx, cy, d.w, d.h, TrackKalman::NoiseParams{});
      t.score = d.score;
      t.misses = 0;
      ++t.hits;
      d.track_id = t.id;
      trk_used[best_t] = true;
      det_used[best_d] = true;
    }
  }

  bool spawn(Detection& d) noexcept {
    for (auto& t : tracks) {
      if (t.id != 0) continue;
      const std::uint16_t new_id = next_id_++;
      if (next_id_ == 0) next_id_ = 1;  // u16 wrap-around; collisions over
                                         // 65k frames of unique tracks are
                                         // very unlikely in a 90-minute match
      t = {};
      t.id = new_id;
      t.class_id = d.class_id;
      t.score = d.score;
      t.hits = 1;
      t.kf.init(d.x + 0.5f * d.w, d.y + 0.5f * d.h, d.w, d.h);
      d.track_id = new_id;
      return true;
    }
    return false;
  }
};

}  // namespace touchfc
