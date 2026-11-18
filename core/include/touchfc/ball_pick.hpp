#pragma once

#include <cmath>
#include <cstdint>

namespace touchfc {

// One ball detection, in the picture and on the pitch.
struct BallCand {
  float px = 0.f, py = 0.f;  // box centre, px
  float score = 0.f;
  bool pitch_ok = false;
  float x = 0.f, y = 0.f;    // metres
  float near_m = 99.f;       // nearest player on the pitch
};

struct BallPickParams {
  float max_px_s = 2900.f;  // fastest ball across the picture
  float slack_px = 40.f;    // ... plus this, for box jitter
  float ui_px = 5.f;        // a spot fixed on the screen
  float lost_s = 0.8f;      // unseen this long: the track ends
  float near_m = 4.f;       // a new ball has a player this close, or is moving
  float move_m = 2.f;
  float park_s = 2.f;       // alone and still this long: a spare ball
  float edge_m = 1.f;       // a new ball this far outside the lines is not the match ball
  int confirm = 3;          // sightings in a row before a new ball counts
};

// At most one ball per frame. The ball in play continues from where it was
// (speed gate, camera shift taken out). A ball anywhere else must be seen
// `confirm` frames in a row, inside the lines, and either beside a player or
// moving. That leaves out one-frame false hits, spare balls by the touchline
// (remembered as parked spots) and ball-shaped broadcast graphics (fixed on
// the screen while the camera moves). Fixed-size, no allocation.
class BallPicker {
 public:
  static constexpr int kMaxCand = 16;

  void reset() noexcept { *this = BallPicker{}; }
  bool tracking() const noexcept { return have_; }

  // Index of the ball in c[0..n), or -1. cam_dx/dy: picture shift since the
  // last call.
  int pick(const BallCand* c, int n, float t_s, float cam_dx, float cam_dy, const BallPickParams& p) noexcept {
    if (n > kMaxCand) n = kMaxCand;
    const float cam = std::hypot(cam_dx, cam_dy);
    const bool panning = cam >= 3.f;
    if (have_ && t_s - seen_t_ > p.lost_s) have_ = false;
    if (have_) {
      tx_ += cam_dx;
      ty_ += cam_dy;
    }
    if (pend_n_ > 0) {
      pend_px_ += cam_dx;
      pend_py_ += cam_dy;
      pend_cam_ += cam;
    }

    bool ui[kMaxCand] = {};
    for (int i = 0; i < n; ++i) ui[i] = ui_hit(c[i], t_s, panning, p);

    // The ball in play, continued. The reach stops growing after kReachS
    // unseen, and away from where it was only a ball that could be a new one
    // continues it (not a spare ball that happens to be in reach).
    int cont = -1;
    if (have_) {
      const float allow = p.slack_px + p.max_px_s * std::fmin(kReachS, t_s - seen_t_);
      float best = -1e9f;
      for (int i = 0; i < n; ++i) {
        if (ui[i]) continue;
        const float d = std::hypot(c[i].px - tx_, c[i].py - ty_);
        if (d > allow || (d > p.slack_px && !eligible(c[i], false, p))) continue;
        float v = c[i].score - 0.5f * d / allow;
        if (parked(c[i], p)) v -= 1.f;
        if (v > best) {
          best = v;
          cont = i;
        }
      }
    }

    // Everything else: spare-ball memory, then the one new ball being confirmed.
    for (int i = 0; i < n; ++i)
      if (i != cont && c[i].pitch_ok) park_note(c[i], t_s, p);
    int fresh = -1;
    bool matched = false;
    if (pend_n_ > 0 && t_s - pend_t_ <= kReachS) {
      const float allow = p.slack_px + p.max_px_s * (t_s - pend_t_);
      float best = allow;
      for (int i = 0; i < n; ++i) {
        if (i == cont || !eligible(c[i], ui[i], p)) continue;
        const float d = std::hypot(c[i].px - pend_px_, c[i].py - pend_py_);
        if (d <= best) {
          best = d;
          fresh = i;
          matched = true;
        }
      }
    }
    if (!matched) {
      pend_n_ = 0;
      float best = -1e9f;
      for (int i = 0; i < n; ++i) {
        if (i == cont || !eligible(c[i], ui[i], p)) continue;
        const float v = c[i].score + (c[i].near_m <= p.near_m ? 1.f : 0.f);
        if (v > best) {
          best = v;
          fresh = i;
        }
      }
      if (fresh >= 0) {
        pend_x0_ = c[fresh].x;
        pend_y0_ = c[fresh].y;
        pend_px0_ = c[fresh].px;
        pend_py0_ = c[fresh].py;
        pend_cam_ = 0.f;
        pend_moved_ = false;
      }
    }
    if (fresh >= 0) {
      const BallCand& b = c[fresh];
      ++pend_n_;
      pend_px_ = b.px;
      pend_py_ = b.py;
      pend_t_ = t_s;
      if (std::hypot(b.x - pend_x0_, b.y - pend_y0_) >= p.move_m) pend_moved_ = true;
      // Fixed on the screen while the camera pans: a graphic.
      const bool glued = pend_cam_ > p.ui_px &&
                         std::hypot(b.px - pend_px0_, b.py - pend_py0_) <= p.ui_px;
      const bool live = b.near_m <= p.near_m || pend_moved_;
      // It takes over a tracked ball only when that one sits alone, or has
      // not moved for a while as this one did.
      const bool free = cont < 0 || c[cont].near_m > p.near_m || (pend_moved_ && t_s - still_t_ > 1.5f);
      if (pend_n_ >= p.confirm && live && !glued && free) {
        pend_n_ = 0;
        cont = fresh;
      }
    }
    if (cont < 0) return -1;
    if (!have_ || !c[cont].pitch_ok || std::hypot(c[cont].x - still_x_, c[cont].y - still_y_) > 1.f) {
      still_x_ = c[cont].x;
      still_y_ = c[cont].y;
      still_t_ = t_s;
    }
    have_ = true;
    tx_ = c[cont].px;
    ty_ = c[cont].py;
    seen_t_ = t_s;
    return cont;
  }

 private:
  static constexpr int kSpots = 8;
  static constexpr int kUiHits = 30;  // panning frames on the same screen spot
  static constexpr float kReachS = 0.25f;

  bool eligible(const BallCand& b, bool ui, const BallPickParams& p) const noexcept {
    if (ui || !b.pitch_ok) return false;
    if (std::fabs(b.x) > 52.5f + p.edge_m || std::fabs(b.y) > 34.f + p.edge_m) return false;
    return !parked(b, p);
  }

  // Screen-fixed spots: hits count only while the camera moves.
  bool ui_hit(const BallCand& b, float t, bool panning, const BallPickParams& p) noexcept {
    int free = 0;
    for (int k = 0; k < kSpots; ++k) {
      Ui& s = ui_[k];
      const bool banned = s.hits >= kUiHits;
      if (s.hits > 0 && t - s.seen > (banned ? 120.f : 1.f)) s.hits = 0;
      if (s.hits > 0 && std::hypot(b.px - s.x, b.py - s.y) <= p.ui_px) {
        s.seen = t;
        if (panning && s.hits < kUiHits) ++s.hits;
        return s.hits >= kUiHits;
      }
      if (s.hits < ui_[free].hits || (s.hits == ui_[free].hits && s.seen < ui_[free].seen)) free = k;
    }
    if (panning && ui_[free].hits < kUiHits) ui_[free] = {b.px, b.py, t, 1};
    return false;
  }

  bool parked(const BallCand& b, const BallPickParams& p) const noexcept {
    if (!b.pitch_ok) return false;
    for (const Park& k : park_)
      if (k.on && k.dur >= p.park_s && std::hypot(b.x - k.x, b.y - k.y) <= kParkM) return true;
    return false;
  }

  // Time a ball that is not the one in play has sat alone on one spot. A
  // player standing over it (a penalty, a corner) runs the time back down.
  void park_note(const BallCand& b, float t, const BallPickParams& p) noexcept {
    int free = 0;  // an unused slot, else the one seen longest ago
    for (int k = 0; k < kSpots; ++k) {
      Park& s = park_[k];
      if (s.on && t - s.seen > 60.f) s.on = false;
      if (s.on && std::hypot(b.x - s.x, b.y - s.y) <= kParkM) {
        const float dt = std::fmin(0.25f, std::fmax(0.f, t - s.seen));
        if (b.near_m > p.near_m) s.dur = std::fmin(p.park_s + 2.f, s.dur + dt);
        else if (b.near_m < 2.f) s.dur = std::fmax(0.f, s.dur - 2.f * dt);
        s.seen = t;
        return;
      }
      if (park_[free].on && (!s.on || s.seen < park_[free].seen)) free = k;
    }
    park_[free] = {b.x, b.y, t, 0.f, true};
  }

  static constexpr float kParkM = 1.5f;
  struct Ui {
    float x = 0.f, y = 0.f, seen = 0.f;
    int hits = 0;
  };
  struct Park {
    float x = 0.f, y = 0.f, seen = 0.f, dur = 0.f;
    bool on = false;
  };
  Ui ui_[kSpots]{};
  Park park_[kSpots]{};
  bool have_ = false;
  float tx_ = 0.f, ty_ = 0.f, seen_t_ = 0.f;
  float still_x_ = 0.f, still_y_ = 0.f, still_t_ = 0.f;  // where the tracked ball last moved from
  int pend_n_ = 0;
  float pend_px_ = 0.f, pend_py_ = 0.f, pend_t_ = 0.f;
  float pend_x0_ = 0.f, pend_y0_ = 0.f, pend_px0_ = 0.f, pend_py0_ = 0.f;
  float pend_cam_ = 0.f;
  bool pend_moved_ = false;
};

}  // namespace touchfc
