#pragma once

#include <cstdint>

namespace touchfc {

struct PanelRect {
  float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
  bool hits(const PanelRect& o, float margin) const noexcept {
    return o.x1 > x0 - margin && o.x0 < x1 + margin && o.y1 > y0 - margin && o.y0 < y1 + margin;
  }
};

// M14 ball-avoid flip for one overlay panel. hit[s] says whether an obstacle
// (ball, on-ball player, scorebug) touches the panel placed on side s (0 the
// preferred side, 1 mirrored) grown by the margin. A hit on the current side
// for react_ms moves the panel when the other side is clear; it then stays for
// at least hold_ms. After the hold it goes home once home has been clear for
// react_ms. pos slides from 0 (home) to 1 (mirrored).
class PanelFlip {
 public:
  void update(const bool hit[2], std::int64_t now_ms, int hold_ms, int react_ms, float dt_s) noexcept {
    for (int s = 0; s < 2; ++s) {
      if (hit[s]) {
        if (hit_since_[s] < 0) hit_since_[s] = now_ms;
        clear_since_[s] = -1;
      } else {
        hit_since_[s] = -1;
        if (clear_since_[s] < 0) clear_since_[s] = now_ms;
      }
    }
    const bool held = now_ms - moved_ms_ < hold_ms;
    const int other = 1 - side_;
    if (!held) {
      const bool stuck = hit_since_[side_] >= 0 && now_ms - hit_since_[side_] >= react_ms && !hit[other];
      const bool home = side_ == 1 && clear_since_[0] >= 0 && now_ms - clear_since_[0] >= react_ms;
      if (stuck || home) {
        side_ = other;
        moved_ms_ = now_ms;
      }
    }
    const float target = static_cast<float>(side_);
    const float step = dt_s / kSlideS;
    pos_ = pos_ < target ? (pos_ + step > target ? target : pos_ + step)
                         : (pos_ - step < target ? target : pos_ - step);
  }
  void force(int side) noexcept {
    side_ = side;
    pos_ = static_cast<float>(side);
  }
  int side() const noexcept { return side_; }
  float pos() const noexcept { return pos_; }

 private:
  static constexpr float kSlideS = 0.25f;
  int side_ = 0;
  float pos_ = 0.f;
  std::int64_t moved_ms_ = -1000000;
  std::int64_t hit_since_[2] = {-1, -1};
  std::int64_t clear_since_[2] = {-1, -1};
};

}  // namespace touchfc
