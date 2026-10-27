#pragma once

#include "touchfc/homography.hpp"

#include <cmath>
#include <cstdint>

namespace touchfc {

enum class PlayState : std::uint8_t { InPlay = 0, Dead = 1 };

// Signals that can call the ball dead. Each fails somewhere, so several are used.
enum PlaySignal : std::uint8_t {
  kSigAngle = 1u << 0,     // pitch long axis tilted: behind-goal, corner or reverse angle
  kSigSize = 1u << 1,      // players too big: close-up, bench, subs board
  kSigScene = 1u << 2,     // not a wide pitch view
  kSigScorebug = 1u << 3,  // scorebug hidden (replays); M12 learns the ROI
  kSigBallOff = 1u << 4,   // ball more than 1 m outside the lines
};

inline constexpr int kPlaySignals = 5;
inline constexpr const char* kPlaySignalName[kPlaySignals] = {"angle", "size", "scene", "scorebug", "ball off"};

// Enter Dead when any available signal holds for enter_ms. Return to InPlay
// only after every available signal is clear for live_ms. The hysteresis stops
// a quick pan from flickering the state.
class PlayGate {
 public:
  PlayState state() const noexcept { return state_; }
  std::uint8_t reasons() const noexcept { return reasons_; }

  // True when the state changed on this call.
  bool update(std::int64_t t_ns, std::uint8_t available, std::uint8_t dead, int enter_ms,
              int live_ms) noexcept {
    const std::uint8_t hit = static_cast<std::uint8_t>(available & dead);
    if (hit) {
      clear_since_ = -1;
      if (dead_since_ < 0) dead_since_ = t_ns;
      reasons_ = hit;
    } else {
      dead_since_ = -1;
      if (clear_since_ < 0) clear_since_ = t_ns;
    }
    if (state_ == PlayState::InPlay && hit && t_ns - dead_since_ >= enter_ms * 1000000LL) {
      state_ = PlayState::Dead;
      return true;
    }
    if (state_ == PlayState::Dead && !hit && t_ns - clear_since_ >= live_ms * 1000000LL) {
      state_ = PlayState::InPlay;
      return true;
    }
    return false;
  }

 private:
  PlayState state_ = PlayState::InPlay;
  std::uint8_t reasons_ = 0;
  std::int64_t dead_since_ = -1;
  std::int64_t clear_since_ = -1;
};

// Tilt of the pitch long axis at the image centre, degrees: 0 side-on, 90 end-on,
// above 90 a reverse angle. False when the centre is off the fit.
inline bool view_angle_deg(const Homography& H, float img_w, float img_h, float& deg) noexcept {
  float X, Y;
  if (!apply_homography(H, 0.5f * img_w, 0.5f * img_h, X, Y)) return false;
  Homography inv;
  if (!invert3(H.h, inv.h)) return false;
  float x0, y0, x1, y1;
  if (!apply_homography(inv, X, Y, x0, y0) || !apply_homography(inv, X + 1.f, Y, x1, y1)) return false;
  deg = std::atan2(std::fabs(y1 - y0), x1 - x0) * 57.2957795f;
  return true;
}

}  // namespace touchfc
