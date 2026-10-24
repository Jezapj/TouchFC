#pragma once

#include "touchfc/detection.hpp"
#include "touchfc/gmc.hpp"
#include "touchfc/kalman.hpp"
#include "touchfc/nms.hpp"
#include "touchfc/team.hpp"

#include <cstdint>

namespace touchfc {

inline constexpr std::uint32_t kMaxTracks = 64;

struct Track {
  TrackKalman kf{};
  std::uint16_t id = 0;
  std::uint16_t class_id = 0;
  float score = 0.f;
  std::uint32_t hits = 0;
  std::uint32_t misses = 0;
  bool confirmed = false;
  bool team_lock = false;
  std::uint8_t team = 0;
  Lab lab{};
  Detection to_detection() const noexcept {
    Detection d;
    d.w = kf.w.p;
    d.h = kf.h.p;
    d.x = kf.cx.p - 0.5f * d.w;
    d.y = kf.cy.p - 0.5f * d.h;
    d.score = score;
    d.class_id = class_id;
    d.track_id = id;
    return d;
  }
};

struct TrackerParams {
  float dt_default = 0.02f;
  float iou_hi = 0.30f;
  float iou_lo = 0.10f;
  float conf_high = 0.50f;
  float conf_new_track = 0.60f;
  std::uint32_t confirm_hits = 3;
  std::uint32_t max_misses_tent = 5;
  std::uint32_t max_misses_conf = 30;
};

class Tracker {
 public:
  Track tracks[kMaxTracks]{};

  std::uint32_t update(Detection* dets, std::uint32_t n, float dt, const GmcTranslation& gmc,
                       const TrackerParams& p) noexcept {
    for (auto& t : tracks) {
      if (t.id == 0) continue;
      t.kf.predict(dt, {});
      t.kf.shift(gmc.dx, gmc.dy);
    }
    std::uint32_t hi[kMaxDetections], lo[kMaxDetections];
    std::uint32_t n_hi = 0, n_lo = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
      dets[i].track_id = 0;
      (dets[i].score >= p.conf_high ? hi[n_hi++] : lo[n_lo++]) = i;
    }
    bool det_used[kMaxDetections]{};
    bool trk_used[kMaxTracks]{};
    match(dets, hi, n_hi, trk_used, det_used, p.iou_hi);
    match(dets, lo, n_lo, trk_used, det_used, p.iou_lo);
    for (std::uint32_t ti = 0; ti < kMaxTracks; ++ti) {
      if (tracks[ti].id == 0 || trk_used[ti]) continue;
      ++tracks[ti].misses;
      tracks[ti].hits = 0;
    }
    for (std::uint32_t i = 0; i < n_hi; ++i) {
      if (det_used[hi[i]] || dets[hi[i]].score < p.conf_new_track) continue;
      if (!spawn(dets[hi[i]])) break;
    }
    for (auto& t : tracks) {
      if (t.id == 0) continue;
      if (t.misses > (t.confirmed ? p.max_misses_conf : p.max_misses_tent)) t = {};
      else if (!t.confirmed && t.hits >= p.confirm_hits) t.confirmed = true;
    }
    std::uint32_t count = 0;
    for (const auto& t : tracks)
      if (t.id != 0) ++count;
    return count;
  }

 private:
  std::uint16_t next_id_ = 1;

  void match(Detection* dets, const std::uint32_t* pool, std::uint32_t n_pool, bool* trk_used,
             bool* det_used, float iou_thresh) noexcept {
    for (;;) {
      float best = iou_thresh;
      int bt = -1, bd = -1;
      for (std::uint32_t ti = 0; ti < kMaxTracks; ++ti) {
        if (tracks[ti].id == 0 || trk_used[ti]) continue;
        const Detection pred = tracks[ti].to_detection();
        for (std::uint32_t pi = 0; pi < n_pool; ++pi) {
          const std::uint32_t di = pool[pi];
          if (det_used[di] || dets[di].class_id != tracks[ti].class_id) continue;
          const float v = iou_xywh(pred, dets[di]);
          if (v > best) {
            best = v;
            bt = static_cast<int>(ti);
            bd = static_cast<int>(di);
          }
        }
      }
      if (bt < 0) break;
      Track& t = tracks[bt];
      Detection& d = dets[bd];
      t.kf.update_box(d.x + 0.5f * d.w, d.y + 0.5f * d.h, d.w, d.h, {});
      t.score = d.score;
      t.misses = 0;
      ++t.hits;
      d.track_id = t.id;
      trk_used[bt] = true;
      det_used[bd] = true;
    }
  }

  bool spawn(Detection& d) noexcept {
    for (auto& t : tracks) {
      if (t.id != 0) continue;
      t = {};
      t.id = next_id_++;
      if (next_id_ == 0) next_id_ = 1;
      t.class_id = d.class_id;
      t.score = d.score;
      t.hits = 1;
      t.kf.init(d.x + 0.5f * d.w, d.y + 0.5f * d.h, d.w, d.h);
      d.track_id = t.id;
      return true;
    }
    return false;
  }
};

}  // namespace touchfc
