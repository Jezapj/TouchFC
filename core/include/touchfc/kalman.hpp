#pragma once

#include <algorithm>

namespace touchfc {

// 2x2 covariance per axis. A diagonal P never updates velocity from a position
// measurement, because the position-velocity cross term stays zero.
struct Kalman1D {
  float p = 0.f;
  float v = 0.f;
  float Pxx = 1.f;
  float Pxv = 0.f;
  float Pvv = 1.f;

  void predict(float dt, float q_p, float q_v) noexcept {
    p += v * dt;
    Pxx += 2.f * dt * Pxv + dt * dt * Pvv + q_p;
    Pxv += dt * Pvv;
    Pvv += q_v;
  }

  void shift(float dp) noexcept { p += dp; }

  void update(float z, float R) noexcept {
    const float S = Pxx + R;
    if (!(S > 0.f)) return;
    const float y = z - p;
    const float Kp = Pxx / S;
    const float Kv = Pxv / S;
    const float old_Pxv = Pxv;
    p += Kp * y;
    v += Kv * y;
    const float r = R / S;
    Pxx *= r;
    Pxv *= r;
    Pvv -= (old_Pxv * old_Pxv) / S;
    if (Pvv < 0.f) Pvv = 0.f;
  }
};

struct TrackKalman {
  Kalman1D cx, cy, w, h;
  struct Noise {
    float q_pos = 4.f, q_vel = 2.f, q_wh = 0.5f, q_wh_vel = 0.2f, R_pos = 2.f, R_wh = 4.f;
  };
  void predict(float dt, const Noise& n) noexcept {
    cx.predict(dt, n.q_pos, n.q_vel);
    cy.predict(dt, n.q_pos, n.q_vel);
    w.predict(dt, n.q_wh, n.q_wh_vel);
    h.predict(dt, n.q_wh, n.q_wh_vel);
  }
  void shift(float dx, float dy) noexcept { cx.shift(dx); cy.shift(dy); }
  void update_box(float ocx, float ocy, float ow, float oh, const Noise& n) noexcept {
    cx.update(ocx, n.R_pos);
    cy.update(ocy, n.R_pos);
    w.update(ow, n.R_wh);
    h.update(oh, n.R_wh);
  }
  void init(float ocx, float ocy, float ow, float oh) noexcept {
    cx = {ocx, 0.f, 10.f, 0.f, 50.f};
    cy = {ocy, 0.f, 10.f, 0.f, 50.f};
    w = {ow, 0.f, 10.f, 0.f, 10.f};
    h = {oh, 0.f, 10.f, 0.f, 10.f};
  }
};

}  // namespace touchfc
