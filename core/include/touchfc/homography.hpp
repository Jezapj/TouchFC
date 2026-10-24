#pragma once

#include <cmath>
#include <cstdint>

namespace touchfc {

// Image pixels -> pitch metres. Row-major 3x3. Invalid until a fit succeeds.
struct Homography {
  float h[9]{};
  bool valid = false;
  float rms_m = 0.f;
};

inline void apply_homography(const Homography& H, float x, float y, float& X, float& Y) noexcept {
  const float w = H.h[6] * x + H.h[7] * y + H.h[8];
  const float inv = std::fabs(w) > 1e-8f ? 1.f / w : 0.f;
  X = (H.h[0] * x + H.h[1] * y + H.h[2]) * inv;
  Y = (H.h[3] * x + H.h[4] * y + H.h[5]) * inv;
}

namespace detail {

inline void mul3(const float* a, const float* b, float* c) noexcept {
  for (int r = 0; r < 3; ++r)
    for (int col = 0; col < 3; ++col) {
      c[r * 3 + col] = a[r * 3] * b[col] + a[r * 3 + 1] * b[3 + col] + a[r * 3 + 2] * b[6 + col];
    }
}

inline bool solve8(float A[8][8], float b[8], float x[8]) noexcept {
  for (int col = 0; col < 8; ++col) {
    int piv = col;
    for (int r = col + 1; r < 8; ++r)
      if (std::fabs(A[r][col]) > std::fabs(A[piv][col])) piv = r;
    if (std::fabs(A[piv][col]) < 1e-8f) return false;
    if (piv != col) {
      for (int c = col; c < 8; ++c) {
        const float t = A[col][c];
        A[col][c] = A[piv][c];
        A[piv][c] = t;
      }
      const float t = b[col];
      b[col] = b[piv];
      b[piv] = t;
    }
    const float d = A[col][col];
    for (int c = col; c < 8; ++c) A[col][c] /= d;
    b[col] /= d;
    for (int r = 0; r < 8; ++r) {
      if (r == col) continue;
      const float f = A[r][col];
      for (int c = col; c < 8; ++c) A[r][c] -= f * A[col][c];
      b[r] -= f * b[col];
    }
  }
  for (int i = 0; i < 8; ++i) x[i] = b[i];
  return true;
}

}  // namespace detail

// Least-squares homography, Hartley-normalised. Needs at least 4 pairs.
inline bool fit_homography(const float* sx, const float* sy, const float* dx, const float* dy, int n,
                           Homography& out) noexcept {
  out = {};
  if (n < 4) return false;
  float smx = 0, smy = 0, dmx = 0, dmy = 0;
  for (int i = 0; i < n; ++i) {
    smx += sx[i];
    smy += sy[i];
    dmx += dx[i];
    dmy += dy[i];
  }
  smx /= n;
  smy /= n;
  dmx /= n;
  dmy /= n;
  float ss = 0, ds = 0;
  for (int i = 0; i < n; ++i) {
    ss += std::hypot(sx[i] - smx, sy[i] - smy);
    ds += std::hypot(dx[i] - dmx, dy[i] - dmy);
  }
  ss = ss > 1e-6f ? (1.41421356f * n / ss) : 1.f;
  ds = ds > 1e-6f ? (1.41421356f * n / ds) : 1.f;

  float Ata[8][8]{};
  float Atb[8]{};
  for (int i = 0; i < n; ++i) {
    const float x = (sx[i] - smx) * ss;
    const float y = (sy[i] - smy) * ss;
    const float X = (dx[i] - dmx) * ds;
    const float Y = (dy[i] - dmy) * ds;
    const float rows[2][8] = {{x, y, 1, 0, 0, 0, -x * X, -y * X}, {0, 0, 0, x, y, 1, -x * Y, -y * Y}};
    const float rhs[2] = {X, Y};
    for (int r = 0; r < 2; ++r) {
      for (int c = 0; c < 8; ++c) {
        Atb[c] += rows[r][c] * rhs[r];
        for (int k = 0; k < 8; ++k) Ata[c][k] += rows[r][c] * rows[r][k];
      }
    }
  }
  float hn[8];
  if (!detail::solve8(Ata, Atb, hn)) return false;
  const float Hn[9] = {hn[0], hn[1], hn[2], hn[3], hn[4], hn[5], hn[6], hn[7], 1.f};
  const float Tsrc[9] = {ss, 0, -ss * smx, 0, ss, -ss * smy, 0, 0, 1.f};
  const float invDst[9] = {1.f / ds, 0, dmx, 0, 1.f / ds, dmy, 0, 0, 1.f};
  float tmp[9];
  detail::mul3(Hn, Tsrc, tmp);
  detail::mul3(invDst, tmp, out.h);
  float err = 0;
  for (int i = 0; i < n; ++i) {
    float X, Y;
    apply_homography(out, sx[i], sy[i], X, Y);
    const float ex = X - dx[i];
    const float ey = Y - dy[i];
    err += std::sqrt(ex * ex + ey * ey);
  }
  out.rms_m = err / static_cast<float>(n);
  out.valid = true;
  return true;
}

}  // namespace touchfc
