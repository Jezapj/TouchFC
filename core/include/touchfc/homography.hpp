#pragma once

#include <cmath>
#include <cstdint>

namespace touchfc {

// Image pixels -> pitch metres. Row-major 3x3. Invalid until a fit succeeds.
// Fits are scaled to unit norm with w > 0 on the fitted points, so w <= 0 is
// at or above the horizon.
struct Homography {
  float h[9]{};
  bool valid = false;
  float rms_m = 0.f;
};

// False (and X = Y = 0) when the point is at or above the horizon.
inline bool apply_homography(const Homography& H, float x, float y, float& X, float& Y) noexcept {
  const float w = H.h[6] * x + H.h[7] * y + H.h[8];
  if (w <= 1e-9f) {
    X = 0.f;
    Y = 0.f;
    return false;
  }
  const float inv = 1.f / w;
  X = (H.h[0] * x + H.h[1] * y + H.h[2]) * inv;
  Y = (H.h[3] * x + H.h[4] * y + H.h[5]) * inv;
  return true;
}

inline bool on_pitch(float X, float Y, float margin_m) noexcept {
  return std::fabs(X) <= 52.5f + margin_m && std::fabs(Y) <= 34.f + margin_m;
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

inline float det3(const float* m) noexcept {
  return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
         m[2] * (m[3] * m[7] - m[4] * m[6]);
}

// Unit Frobenius norm, sign chosen so w > 0 at (x, y).
inline void normalise(float* m, float x, float y) noexcept {
  float n = 0.f;
  for (int i = 0; i < 9; ++i) n += m[i] * m[i];
  n = n > 0.f ? 1.f / std::sqrt(n) : 1.f;
  if (m[6] * x + m[7] * y + m[8] < 0.f) n = -n;
  for (int i = 0; i < 9; ++i) m[i] *= n;
}

}  // namespace detail

inline bool invert3(const float* m, float* out) noexcept {
  const float d = detail::det3(m);
  if (std::fabs(d) < 1e-20f) return false;
  const float i = 1.f / d;
  out[0] = (m[4] * m[8] - m[5] * m[7]) * i;
  out[1] = (m[2] * m[7] - m[1] * m[8]) * i;
  out[2] = (m[1] * m[5] - m[2] * m[4]) * i;
  out[3] = (m[5] * m[6] - m[3] * m[8]) * i;
  out[4] = (m[0] * m[8] - m[2] * m[6]) * i;
  out[5] = (m[2] * m[3] - m[0] * m[5]) * i;
  out[6] = (m[3] * m[7] - m[4] * m[6]) * i;
  out[7] = (m[1] * m[6] - m[0] * m[7]) * i;
  out[8] = (m[0] * m[4] - m[1] * m[3]) * i;
  return true;
}

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
  detail::normalise(out.h, smx, smy);
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

// Fit, then keep the sample whose inliers agree within inlier_m. A 4-point
// sample can always interpolate, so the refit has to see at least 6 points.
inline bool fit_homography_ransac(const float* sx, const float* sy, const float* dx, const float* dy,
                                  int n, float inlier_m, Homography& out) noexcept {
  out = {};
  if (n < 6 || n > 32) return false;
  const float limit = inlier_m * inlier_m;
  std::uint32_t rng = 0xA341316Cu ^ static_cast<std::uint32_t>(n * 17);
  int best_n = 0;
  std::uint8_t best[32]{};
  for (int iter = 0; iter < 48; ++iter) {
    rng = rng * 1664525u + 1013904223u;
    int idx[4] = {-1, -1, -1, -1};
    for (int j = 0; j < 4; ++j) {
      for (int guard = 0; guard < 16; ++guard) {
        rng = rng * 1664525u + 1013904223u;
        const int k = static_cast<int>(rng % static_cast<std::uint32_t>(n));
        bool dup = false;
        for (int p = 0; p < j; ++p)
          if (idx[p] == k) dup = true;
        if (!dup) {
          idx[j] = k;
          break;
        }
      }
    }
    if (idx[3] < 0) continue;
    float ssx[4], ssy[4], ddx[4], ddy[4];
    for (int j = 0; j < 4; ++j) {
      ssx[j] = sx[idx[j]];
      ssy[j] = sy[idx[j]];
      ddx[j] = dx[idx[j]];
      ddy[j] = dy[idx[j]];
    }
    Homography trial;
    if (!fit_homography(ssx, ssy, ddx, ddy, 4, trial)) continue;
    std::uint8_t inl[32];
    int ni = 0;
    for (int i = 0; i < n; ++i) {
      float X = 0, Y = 0;
      apply_homography(trial, sx[i], sy[i], X, Y);
      const float ex = X - dx[i];
      const float ey = Y - dy[i];
      if (ex * ex + ey * ey <= limit) inl[ni++] = static_cast<std::uint8_t>(i);
    }
    if (ni > best_n) {
      best_n = ni;
      for (int i = 0; i < ni; ++i) best[i] = inl[i];
    }
  }
  if (best_n < 6) return false;
  float fx[32], fy[32], gx[32], gy[32];
  for (int i = 0; i < best_n; ++i) {
    fx[i] = sx[best[i]];
    fy[i] = sy[best[i]];
    gx[i] = dx[best[i]];
    gy[i] = dy[best[i]];
  }
  return fit_homography(fx, fy, gx, gy, best_n, out);
}

}  // namespace touchfc
