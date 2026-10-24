#pragma once

// Per-axis 2x2 covariance Kalman for position + velocity. One instance per
// scalar axis (cx, cy, w, h). The soccer-overlay design review (Part A, item
// 1) is explicit that a *diagonal* P breaks velocity: the position-velocity
// cross term is zero, so a position measurement updates position only and
// velocity never learns from it. The 2x2 covariance here is the minimum
// correct form.
//
// State:   x = [p, v]^T
// Dynamics: F = [1 dt; 0 1]
// Process:  Q = diag(q_p, q_v)         (constant per axis)
// Measure:  z = p + N(0, R)
//
// Predict (closed form expansion of F P F^T + Q):
//   Pxx <- Pxx + 2*dt*Pxv + dt^2*Pvv + q_p
//   Pxv <- Pxv + dt*Pvv
//   Pvv <- Pvv + q_v
//
// Update (one scalar measurement, S = Pxx + R):
//   K_p = Pxx / S, K_v = Pxv / S
//   p   += K_p * (z - p)
//   v   += K_v * (z - p)
//   Pxx <- Pxx * R / S
//   Pxv <- Pxv * R / S
//   Pvv <- Pvv - Pxv^2 / S          (uses OLD Pxv, computed before overwrite)

#include <algorithm>
#include <cmath>

namespace touchfc {

struct Kalman1D {
  float p = 0.f;   // position estimate
  float v = 0.f;   // velocity estimate (per second)
  float Pxx = 1.f; // Var(p)
  float Pxv = 0.f; // Cov(p, v)
  float Pvv = 1.f; // Var(v)

  void predict(float dt, float q_p, float q_v) noexcept {
    const float dt2 = dt * dt;
    p = p + v * dt;
    Pxx = Pxx + 2.f * dt * Pxv + dt2 * Pvv + q_p;
    Pxv = Pxv + dt * Pvv;
    Pvv = Pvv + q_v;
  }

  // Shift the position by a known external motion (camera pan from GMC).
  // Does not change covariance; the shift is treated as a known quantity.
  void shift(float dp) noexcept { p += dp; }

  void update(float z, float R) noexcept {
    const float S = Pxx + R;
    if (!(S > 0.f)) return;  // defensive: R > 0 keeps S > 0
    const float Kp = Pxx / S;
    const float Kv = Pxv / S;
    const float y = z - p;
    p += Kp * y;
    v += Kv * y;
    const float old_Pxv = Pxv;
    const float Rratio = R / S;
    Pxx *= Rratio;
    Pxv *= Rratio;
    Pvv -= (old_Pxv * old_Pxv) / S;
    if (Pvv < 0.f) Pvv = 0.f;  // numerical floor
  }
};

// Soccer-track Kalman: cx, cy, w, h. Each axis is independent. In broadcast
// footage width and height are much slower-moving than position, so their
// process noise is smaller. Numbers are a starting point; the Config sliders
// will tune them in a later milestone.
struct TrackKalman {
  Kalman1D cx{};
  Kalman1D cy{};
  Kalman1D w{};
  Kalman1D h{};

  struct NoiseParams {
    float q_pos = 4.f;      // per second^2, position variance drift
    float q_vel = 2.f;      // per second, velocity variance drift
    float q_wh = 0.5f;      // size position variance drift
    float q_wh_vel = 0.2f;  // size velocity variance drift
    float R_pos = 2.f;      // measurement variance for position
    float R_wh = 4.f;       // measurement variance for size
  };

  void predict(float dt, const NoiseParams& n) noexcept {
    cx.predict(dt, n.q_pos, n.q_vel);
    cy.predict(dt, n.q_pos, n.q_vel);
    w.predict(dt, n.q_wh, n.q_wh_vel);
    h.predict(dt, n.q_wh, n.q_wh_vel);
  }

  void shift(float dx, float dy) noexcept {
    cx.shift(dx);
    cy.shift(dy);
  }

  void update_box(float ocx, float ocy, float ow, float oh,
                  const NoiseParams& n) noexcept {
    cx.update(ocx, n.R_pos);
    cy.update(ocy, n.R_pos);
    w.update(ow, n.R_wh);
    h.update(oh, n.R_wh);
  }

  // Initialise from the first detection; zero velocity, moderate confidence.
  void init(float ocx, float ocy, float ow, float oh) noexcept {
    cx = {ocx, 0.f, 10.f, 0.f, 50.f};
    cy = {ocy, 0.f, 10.f, 0.f, 50.f};
    w = {ow, 0.f, 10.f, 0.f, 10.f};
    h = {oh, 0.f, 10.f, 0.f, 10.f};
  }
};

}  // namespace touchfc
