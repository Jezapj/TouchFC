#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#if defined(__AVX2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)))
#  ifndef TOUCHFC_HAS_AVX2
#    define TOUCHFC_HAS_AVX2 1
#  endif
#  include <immintrin.h>
#endif

namespace touchfc {

// Spearman-style pitch control on a 2 m grid (53 x 34 cells over 105 x 68).
// Each cell is the chance team 0 gets there first. Team ids are 0 or 1.
// A ball position delays every player until the ball could arrive.
inline constexpr int kPcNx = 53;
inline constexpr int kPcNy = 34;
inline constexpr int kPcCells = kPcNx * kPcNy;
inline constexpr int kPcMaxPlayers = 32;

struct PcPlayer {
  float x = 0.f;
  float y = 0.f;
  float vx = 0.f;
  float vy = 0.f;
  std::uint8_t team = 0;
};

inline float pc_cell_x(int i) noexcept {
  return -52.5f + (static_cast<float>(i) + 0.5f) * (105.f / static_cast<float>(kPcNx));
}
inline float pc_cell_y(int j) noexcept {
  return -34.f + (static_cast<float>(j) + 0.5f) * (68.f / static_cast<float>(kPcNy));
}

// Seconds to reach (tx, ty). Reaction at the current velocity, then accelerate
// up to 5 m/s at 7 m/s^2.
inline float tti_seconds(float px, float py, float vx, float vy, float tx, float ty) noexcept {
  constexpr float kReact = 0.7f;
  constexpr float kVmax = 5.f;
  constexpr float kAmax = 7.f;
  const float dx = tx - px;
  const float dy = ty - py;
  const float d2 = dx * dx + dy * dy;
  if (d2 < 1e-6f) return kReact;
  const float d = std::sqrt(d2);
  float s = (vx * dx + vy * dy) / d;
  if (s < 0.f) s = 0.f;
  if (s > kVmax) s = kVmax;
  float t;
  if (s >= kVmax - 1e-4f) {
    t = d / kVmax;
  } else {
    const float t_vmax = (kVmax - s) / kAmax;
    const float d_vmax = s * t_vmax + 0.5f * kAmax * t_vmax * t_vmax;
    if (d_vmax >= d) {
      t = (-s + std::sqrt(s * s + 2.f * kAmax * d)) / kAmax;
    } else {
      t = t_vmax + (d - d_vmax) / kVmax;
    }
  }
  return kReact + t;
}

inline void tti_row8(float px, float py, float vx, float vy, const float* tx, const float* ty,
                     float* out) noexcept {
#if defined(TOUCHFC_HAS_AVX2)
  constexpr float kReact = 0.7f;
  constexpr float kVmax = 5.f;
  constexpr float kAmax = 7.f;
  const __m256 dx = _mm256_sub_ps(_mm256_loadu_ps(tx), _mm256_set1_ps(px));
  const __m256 dy = _mm256_sub_ps(_mm256_loadu_ps(ty), _mm256_set1_ps(py));
  const __m256 d2 = _mm256_add_ps(_mm256_mul_ps(dx, dx), _mm256_mul_ps(dy, dy));
  const __m256 small = _mm256_cmp_ps(d2, _mm256_set1_ps(1e-6f), _CMP_LT_OQ);
  const __m256 d = _mm256_sqrt_ps(_mm256_blendv_ps(d2, _mm256_set1_ps(1.f), small));
  __m256 s = _mm256_div_ps(
      _mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(vx), dx), _mm256_mul_ps(_mm256_set1_ps(vy), dy)), d);
  s = _mm256_min_ps(_mm256_max_ps(s, _mm256_setzero_ps()), _mm256_set1_ps(kVmax));
  const __m256 vmax = _mm256_set1_ps(kVmax);
  const __m256 amax = _mm256_set1_ps(kAmax);
  const __m256 already = _mm256_cmp_ps(s, _mm256_set1_ps(kVmax - 1e-4f), _CMP_GE_OQ);
  const __m256 t_at_max = _mm256_div_ps(d, vmax);
  const __m256 t_vmax = _mm256_div_ps(_mm256_sub_ps(vmax, s), amax);
  const __m256 d_vmax = _mm256_add_ps(_mm256_mul_ps(s, t_vmax),
                                     _mm256_mul_ps(_mm256_set1_ps(0.5f * kAmax), _mm256_mul_ps(t_vmax, t_vmax)));
  const __m256 accel_reaches = _mm256_cmp_ps(d_vmax, d, _CMP_GE_OQ);
  const __m256 t_acc = _mm256_div_ps(
      _mm256_add_ps(_mm256_sub_ps(_mm256_setzero_ps(), s),
                    _mm256_sqrt_ps(_mm256_add_ps(_mm256_mul_ps(s, s), _mm256_mul_ps(_mm256_set1_ps(2.f * kAmax), d)))),
      amax);
  const __m256 t_cruise = _mm256_add_ps(t_vmax, _mm256_div_ps(_mm256_sub_ps(d, d_vmax), vmax));
  __m256 t = _mm256_blendv_ps(t_cruise, t_acc, accel_reaches);
  t = _mm256_blendv_ps(t, t_at_max, already);
  t = _mm256_add_ps(t, _mm256_set1_ps(kReact));
  t = _mm256_blendv_ps(t, _mm256_set1_ps(kReact), small);
  _mm256_storeu_ps(out, t);
#else
  for (int i = 0; i < 8; ++i) out[i] = tti_seconds(px, py, vx, vy, tx[i], ty[i]);
#endif
}

inline float control_at_cell(const float* tti, const std::uint8_t* team, int n) noexcept {
  float p0 = 0.f;
  float ptot = 0.f;
  constexpr float kLambda = 4.3f;
  constexpr float kDt = 0.2f;
  for (float T = kDt; T <= 6.f && ptot < 0.99f; T += kDt) {
    for (int i = 0; i < n; ++i) {
      if (T <= tti[i]) continue;
      const float mass = (1.f - ptot) * kLambda * std::exp(-kLambda * (T - tti[i])) * kDt;
      ptot += mass;
      if (team[i] == 0) p0 += mass;
      if (ptot >= 1.f) {
        const float over = ptot - 1.f;
        if (team[i] == 0) p0 -= over;
        ptot = 1.f;
        break;
      }
    }
  }
  return ptot > 0.05f ? p0 / ptot : 0.5f;
}

// `ball` is used when has_ball is set. Players cannot control a cell before
// the ball could cover the distance at 15 m/s. Returns the mean of team 0.
inline float pitch_control(const PcPlayer* players, int n, float ball_x, float ball_y, bool has_ball,
                           float* grid) noexcept {
  if (n > kPcMaxPlayers) n = kPcMaxPlayers;
  if (n < 0) n = 0;
  float sum = 0.f;
  for (int j = 0; j < kPcNy; ++j) {
    const float cy = pc_cell_y(j);
    float xs[56];
    float ys[56];
    for (int i = 0; i < 56; ++i) {
      const int c = i < kPcNx ? i : kPcNx - 1;
      xs[i] = pc_cell_x(c);
      ys[i] = cy;
    }
    float tti[kPcMaxPlayers][56];
    for (int p = 0; p < n; ++p) {
      for (int i = 0; i < kPcNx; i += 8) {
        tti_row8(players[p].x, players[p].y, players[p].vx, players[p].vy, xs + i, ys + i,
                 tti[p] + i);
      }
    }
    for (int i = 0; i < kPcNx; ++i) {
      float cell_tti[kPcMaxPlayers];
      std::uint8_t teams[kPcMaxPlayers];
      float ball_t = 0.f;
      if (has_ball) {
        const float dx = xs[i] - ball_x;
        const float dy = cy - ball_y;
        ball_t = std::sqrt(dx * dx + dy * dy) / 15.f;
      }
      for (int p = 0; p < n; ++p) {
        cell_tti[p] = std::max(tti[p][i], ball_t);
        teams[p] = players[p].team;
      }
      const float c = control_at_cell(cell_tti, teams, n);
      grid[j * kPcNx + i] = c;
      sum += c;
    }
  }
  return sum / static_cast<float>(kPcCells);
}

}  // namespace touchfc
