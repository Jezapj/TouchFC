#pragma once

#include "touchfc/homography.hpp"

#include <cmath>
#include <cstdint>

namespace touchfc {

// Holds the current image -> pitch homography. A keypoint fit is validated,
// then blended in; line refits (track) keep it on the painted lines; between
// fits camera motion moves it: H_t = H_fit * G^-1.
// Built for the broadcast main camera: image right is +x, image up is the far
// touchline. That pins the left/right and near/far symmetry of the markings.
enum class FitReject : std::uint8_t {
  None = 0,
  Rms,      // error above max_rms_m
  Inliers,  // fewer than 4 inliers, or RANSAC failed
  Spread,   // inliers all in one image quadrant
  Orient,   // image right is not +x or image up is not the far side (mirrored ids)
  Horizon,  // horizon below the top of the players
  Bottom,   // image bottom centre maps more than 10 m off the pitch
  Jump,     // far from the prediction; held for one more fit
  Lines,    // too few projected markings sit on painted lines
};

inline constexpr int kFitRejectCount = 9;

inline const char* fit_reject_name(FitReject r) noexcept {
  static constexpr const char* kNames[] = {"ok", "error", "inliers", "spread",
                                           "orientation", "horizon", "bottom", "jump (held)",
                                           "lines"};
  return kNames[static_cast<int>(r)];
}

struct PitchCalibParams {
  float max_rms_m = 1.5f;
  float jump_m = 3.f;
  float smooth = 0.4f;     // weight of a new fit against the prediction
  int holdover = 50;       // frames a fit lives on camera motion alone
  float inlier_m = 2.f;
};

class PitchCalib {
 public:
  bool valid() const noexcept { return valid_; }
  bool manual() const noexcept { return manual_; }
  int age() const noexcept { return age_; }
  const Homography& current() const noexcept { return h_; }

  void reset() noexcept { *this = PitchCalib{}; }

  // Once per frame, before offer. G maps the previous image to this one.
  void predict(const float* G, bool motion_ok, const PitchCalibParams& p) noexcept {
    if (pending_) pending_ = motion_ok && compose(pending_h_, G);
    if (!valid_) return;
    if (!motion_ok || !compose(h_, G)) {
      valid_ = false;
      return;
    }
    ++age_;
    if (!manual_ && age_ > p.holdover) valid_ = false;
  }

  // head_y: image y of the top 10% of player boxes, < 0 when unknown.
  FitReject offer(const Homography& fit, const float* sx, const float* sy, const float* dx,
                  const float* dy, int n, float img_w, float img_h, float head_y,
                  const PitchCalibParams& p) noexcept {
    const FitReject r = check(fit, sx, sy, dx, dy, n, img_w, img_h, head_y, p);
    if (r != FitReject::None) return r;
    const float rx[4] = {0.15f * img_w, 0.85f * img_w, 0.15f * img_w, 0.85f * img_w};
    const float ry[4] = {0.55f * img_h, 0.55f * img_h, 0.92f * img_h, 0.92f * img_h};
    if (valid_ && !manual_) {
      if (jump_m(fit, rx, ry) <= p.jump_m) {
        blend(fit, rx, ry, p);
        return FitReject::None;
      }
      // Far from the prediction: accept only if the held fit agrees with this one.
      if (!pending_ || max_dist(fit, pending_h_, rx, ry) > p.jump_m) {
        pending_h_ = fit;
        pending_ = true;
        return FitReject::Jump;
      }
    }
    accept(fit);
    return FitReject::None;
  }

  // A line refit of the current fit: small by construction, so no jump hold.
  FitReject track(const Homography& fit, float img_w, float img_h, const PitchCalibParams& p) noexcept {
    if (!valid_) return FitReject::Lines;
    const FitReject r = check_pose(fit, img_w, img_h, -1.f);
    if (r != FitReject::None) return r;
    const float rx[4] = {0.15f * img_w, 0.85f * img_w, 0.15f * img_w, 0.85f * img_w};
    const float ry[4] = {0.55f * img_h, 0.55f * img_h, 0.92f * img_h, 0.92f * img_h};
    blend(fit, rx, ry, p);
    return FitReject::None;
  }

  // Manual calibration: no holdover cap, still moved by camera motion.
  void set_manual(const Homography& H) noexcept {
    accept(H);
    manual_ = true;
  }

 private:
  static float max_dist(const Homography& a, const Homography& b, const float* rx, const float* ry) noexcept {
    float m = 0.f;
    for (int i = 0; i < 4; ++i) {
      float ax, ay, bx, by;
      apply_homography(a, rx[i], ry[i], ax, ay);
      apply_homography(b, rx[i], ry[i], bx, by);
      m = std::fmax(m, std::hypot(ax - bx, ay - by));
    }
    return m;
  }

  float jump_m(const Homography& fit, const float* rx, const float* ry) const noexcept {
    return max_dist(fit, h_, rx, ry);
  }

  // Blend the corners of the visible pitch area toward the fit and refit from them.
  void blend(const Homography& fit, const float* rx, const float* ry, const PitchCalibParams& p) noexcept {
    float PX[4], PY[4];
    for (int i = 0; i < 4; ++i) {
      float X, Y;
      apply_homography(fit, rx[i], ry[i], X, Y);
      apply_homography(h_, rx[i], ry[i], PX[i], PY[i]);
      PX[i] += p.smooth * (X - PX[i]);
      PY[i] += p.smooth * (Y - PY[i]);
    }
    Homography blended;
    if (fit_homography(rx, ry, PX, PY, 4, blended)) {
      blended.rms_m = fit.rms_m;
      accept(blended);
    } else {
      accept(fit);
    }
  }

  // Main-camera pose: right is +x, up is the far side, horizon above the
  // players, and the bottom of the picture is near the pitch.
  static FitReject check_pose(const Homography& fit, float img_w, float img_h, float head_y) noexcept {
    float x0, y0, x1, y1, x2, y2;
    const float yb = img_h - 1.f;
    if (!apply_homography(fit, 0.5f * img_w, yb, x0, y0)) return FitReject::Bottom;
    if (!apply_homography(fit, 0.6f * img_w, yb, x1, y1) || x1 <= x0) return FitReject::Orient;
    if (!apply_homography(fit, 0.5f * img_w, 0.6f * img_h, x2, y2) || y2 <= y0) return FitReject::Orient;
    if (head_y >= 0.f) {
      for (const float x : {0.f, 0.5f * img_w, img_w}) {
        if (fit.h[6] * x + fit.h[7] * head_y + fit.h[8] <= 0.f) return FitReject::Horizon;
      }
    }
    if (!on_pitch(x0, y0, 10.f)) return FitReject::Bottom;
    return FitReject::None;
  }

  static bool compose(Homography& H, const float* G) noexcept {
    float Gi[9], out[9];
    if (!invert3(G, Gi)) return false;
    detail::mul3(H.h, Gi, out);
    float n = 0.f;
    for (int i = 0; i < 9; ++i) n += out[i] * out[i];
    n = 1.f / std::sqrt(n);
    for (int i = 0; i < 9; ++i) H.h[i] = out[i] * n;
    return true;
  }

  FitReject check(const Homography& fit, const float* sx, const float* sy, const float* dx,
                  const float* dy, int n, float img_w, float img_h, float head_y,
                  const PitchCalibParams& p) const noexcept {
    if (!fit.valid) return FitReject::Inliers;
    if (fit.rms_m > p.max_rms_m) return FitReject::Rms;
    int inl = 0;
    std::uint8_t quads = 0;
    for (int i = 0; i < n; ++i) {
      float X, Y;
      if (!apply_homography(fit, sx[i], sy[i], X, Y)) continue;
      if (std::hypot(X - dx[i], Y - dy[i]) > p.inlier_m) continue;
      ++inl;
      quads |= static_cast<std::uint8_t>(1u << ((sx[i] >= 0.5f * img_w ? 1 : 0) + (sy[i] >= 0.5f * img_h ? 2 : 0)));
    }
    if (inl < 4) return FitReject::Inliers;
    if ((quads & (quads - 1)) == 0) return FitReject::Spread;
    return check_pose(fit, img_w, img_h, head_y);
  }

  void accept(const Homography& H) noexcept {
    h_ = H;
    valid_ = true;
    manual_ = false;
    age_ = 0;
    pending_ = false;
  }

  Homography h_{};
  Homography pending_h_{};
  bool valid_ = false;
  bool manual_ = false;
  bool pending_ = false;
  int age_ = 0;
};

}  // namespace touchfc
