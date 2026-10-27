#pragma once

#include <cmath>
#include <cstdint>

#if defined(__AVX2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)))
#  ifndef TOUCHFC_HAS_AVX2
#    define TOUCHFC_HAS_AVX2 1
#  endif
#  include <immintrin.h>
#endif

namespace touchfc {

// Camera motion from the picture, not the players. A frame is reduced to a
// 160x90 grey image of grass and line pixels; 0 marks everything else
// (players, crowd, graphics), so a static scorebug does not pull the match
// toward zero. Coarse search over scale and shift at 80x45, then a +-2 refine
// at 160x90 with a parabolic sub-pixel peak.
inline constexpr int kGmcW = 160;
inline constexpr int kGmcH = 90;
inline constexpr int kGmcCW = 80;
inline constexpr int kGmcCH = 45;

struct GmcImage {
  std::uint8_t fine[kGmcW * kGmcH]{};
  std::uint8_t coarse[kGmcCW * kGmcCH]{};
  std::uint32_t w = 0;
  std::uint32_t h = 0;
};

// A previous-frame point p is at s * (p - c) + c + (tx, ty) now, c = frame centre.
struct GmcMotion {
  float s = 1.f;
  float tx = 0.f;
  float ty = 0.f;
  float cost = 0.f;        // mean abs grey difference at the match
  float valid_frac = 0.f;  // share of the search box valid in both frames
  bool ok = false;
};

struct GmcParams {
  float max_cost = 18.f;      // a cut or a flash is worse than this
  float min_valid = 0.25f;    // close-ups have too little grass
  float min_contrast = 0.75f; // flat grass gives no lock
};

inline void gmc_reduce_bgra(const std::uint8_t* bgra, std::uint32_t w, std::uint32_t h,
                            std::uint32_t stride, GmcImage& out, int margin = 12,
                            int min_green = 40) noexcept {
  out.w = w;
  out.h = h;
  std::uint32_t xs[2 * kGmcW];
  std::uint32_t ys[2 * kGmcH];
  for (int i = 0; i < 2 * kGmcW; ++i)
    xs[i] = static_cast<std::uint32_t>((2ull * i + 1) * w / (4ull * kGmcW)) * 4u;
  for (int j = 0; j < 2 * kGmcH; ++j)
    ys[j] = static_cast<std::uint32_t>((2ull * j + 1) * h / (4ull * kGmcH));
  for (int j = 0; j < kGmcH; ++j) {
    for (int i = 0; i < kGmcW; ++i) {
      int sum = 0, n = 0;
      for (int sy = 0; sy < 2; ++sy) {
        const std::uint8_t* row = bgra + static_cast<std::size_t>(ys[2 * j + sy]) * stride;
        for (int sx = 0; sx < 2; ++sx) {
          const std::uint8_t* p = row + xs[2 * i + sx];
          const int b = p[0], g = p[1], r = p[2];
          const bool grass = g > r + margin && g > b + margin && g > min_green;
          const int hi = b > g ? (b > r ? b : r) : (g > r ? g : r);
          const int lo = b < g ? (b < r ? b : r) : (g < r ? g : r);
          const bool line = lo > 150 && hi - lo < 40;
          if (grass || line) {
            sum += (b + 2 * g + r) >> 2;
            ++n;
          }
        }
      }
      const int v = n >= 2 ? sum / n : 0;
      out.fine[j * kGmcW + i] = static_cast<std::uint8_t>(n >= 2 ? (v < 1 ? 1 : v) : 0);
    }
  }
  for (int j = 0; j < kGmcCH; ++j) {
    for (int i = 0; i < kGmcCW; ++i) {
      const std::uint8_t* f = out.fine + 2 * j * kGmcW + 2 * i;
      const int v[4] = {f[0], f[1], f[kGmcW], f[kGmcW + 1]};
      int sum = 0, n = 0;
      for (int k = 0; k < 4; ++k)
        if (v[k]) {
          sum += v[k];
          ++n;
        }
      out.coarse[j * kGmcCW + i] = static_cast<std::uint8_t>(n >= 2 ? sum / n : 0);
    }
  }
}

namespace detail {

// dst(x) = src(c + (x - c) / s): src zoomed by s about the grid centre.
// Bilinear where all four taps are valid, nearest otherwise.
inline void gmc_zoom(const std::uint8_t* src, int w, int h, float s, std::uint8_t* dst) noexcept {
  const float cx = 0.5f * w - 0.5f, cy = 0.5f * h - 0.5f, inv = 1.f / s;
  for (int y = 0; y < h; ++y) {
    const float fy = cy + (y - cy) * inv;
    const int y0 = static_cast<int>(std::floor(fy));
    const float ay = fy - y0;
    for (int x = 0; x < w; ++x) {
      const float fx = cx + (x - cx) * inv;
      const int x0 = static_cast<int>(std::floor(fx));
      const float ax = fx - x0;
      std::uint8_t v = 0;
      if (x0 >= 0 && y0 >= 0 && x0 + 1 < w && y0 + 1 < h) {
        const std::uint8_t* p = src + y0 * w + x0;
        if (p[0] && p[1] && p[w] && p[w + 1]) {
          const float t = p[0] + ax * (p[1] - p[0]);
          const float b = p[w] + ax * (p[w + 1] - p[w]);
          const int q = static_cast<int>(t + ay * (b - t) + 0.5f);
          v = static_cast<std::uint8_t>(q < 1 ? 1 : q);
        } else {
          v = p[(ay > 0.5f ? w : 0) + (ax > 0.5f ? 1 : 0)];
        }
      }
      dst[y * w + x] = v;
    }
  }
}

// Sum of |cur(x) - ref(x - d)| over pixels valid in both, inside the box
// [m, w - m) x [m, h - m). (w - 2m) is a multiple of 32 and |d| <= m.
inline void gmc_sad(const std::uint8_t* cur, const std::uint8_t* ref, int w, int h, int m, int dx,
                    int dy, std::uint32_t& sum, std::uint32_t& cnt) noexcept {
  sum = 0;
  cnt = 0;
  const int span = w - 2 * m;
#if defined(TOUCHFC_HAS_AVX2)
  __m256i acc = _mm256_setzero_si256();
  const __m256i z = _mm256_setzero_si256();
  for (int y = m; y < h - m; ++y) {
    const std::uint8_t* a = cur + y * w + m;
    const std::uint8_t* b = ref + (y - dy) * w + m - dx;
    for (int k = 0; k < span; k += 32) {
      const __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + k));
      const __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + k));
      const __m256i bad = _mm256_or_si256(_mm256_cmpeq_epi8(va, z), _mm256_cmpeq_epi8(vb, z));
      acc = _mm256_add_epi64(acc, _mm256_sad_epu8(_mm256_andnot_si256(bad, va), _mm256_andnot_si256(bad, vb)));
      cnt += 32u - static_cast<std::uint32_t>(
                       _mm_popcnt_u32(static_cast<unsigned>(_mm256_movemask_epi8(bad))));
    }
  }
  alignas(32) std::uint64_t lanes[4];
  _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), acc);
  sum = static_cast<std::uint32_t>(lanes[0] + lanes[1] + lanes[2] + lanes[3]);
#else
  for (int y = m; y < h - m; ++y) {
    const std::uint8_t* a = cur + y * w + m;
    const std::uint8_t* b = ref + (y - dy) * w + m - dx;
    for (int k = 0; k < span; ++k) {
      if (!a[k] || !b[k]) continue;
      sum += static_cast<std::uint32_t>(a[k] > b[k] ? a[k] - b[k] : b[k] - a[k]);
      ++cnt;
    }
  }
#endif
}

// Offset of the minimum of a parabola through (-1, a), (0, b), (1, c).
inline float parabola_peak(float a, float b, float c) noexcept {
  const float d = a - 2.f * b + c;
  if (d <= 1e-6f) return 0.f;
  const float t = 0.5f * (a - c) / d;
  return t < -0.5f ? -0.5f : t > 0.5f ? 0.5f : t;
}

}  // namespace detail

inline GmcMotion estimate_gmc(const GmcImage& prev, const GmcImage& cur,
                              const GmcParams& p = {}) noexcept {
  GmcMotion out{};
  if (prev.w == 0 || prev.w != cur.w || prev.h != cur.h) return out;
  constexpr int kCm = 8, kCr = 7, kFm = 16;
  constexpr int kScales = 5;
  constexpr float kStep = 0.01f;
  constexpr float kCBox = static_cast<float>((kGmcCW - 2 * kCm) * (kGmcCH - 2 * kCm));
  constexpr float kFBox = static_cast<float>((kGmcW - 2 * kFm) * (kGmcH - 2 * kFm));

  std::uint8_t zoom[kGmcW * kGmcH];
  float best = 1e9f, total = 0.f;
  int nt = 0, bs = kScales / 2, bdx = 0, bdy = 0;
  for (int si = 0; si < kScales; ++si) {
    const float s = 1.f + (si - kScales / 2) * kStep;
    detail::gmc_zoom(prev.coarse, kGmcCW, kGmcCH, s, zoom);
    for (int dy = -kCr; dy <= kCr; ++dy) {
      for (int dx = -kCr; dx <= kCr; ++dx) {
        std::uint32_t sum, cnt;
        detail::gmc_sad(cur.coarse, zoom, kGmcCW, kGmcCH, kCm, dx, dy, sum, cnt);
        if (static_cast<float>(cnt) < p.min_valid * kCBox) continue;
        const float c = static_cast<float>(sum) / static_cast<float>(cnt);
        total += c;
        ++nt;
        if (c < best) {
          best = c;
          bs = si;
          bdx = dx;
          bdy = dy;
        }
      }
    }
  }
  if (nt == 0) return out;

  // Refine at full grid: five half-steps of scale around the coarse one (a 1% zoom
  // moves the coarse edge by under half a pixel), +-2 around 2x the shift.
  const float s0 = 1.f + (bs - kScales / 2) * kStep;
  float grid[5][5][5];
  float fbest = 1e9f;
  int fs = 2, fx = 0, fy = 0;
  std::uint32_t fcnt = 0;
  for (int si = 0; si < 5; ++si) {
    detail::gmc_zoom(prev.fine, kGmcW, kGmcH, s0 + (si - 2) * 0.5f * kStep, zoom);
    for (int j = 0; j < 5; ++j) {
      for (int i = 0; i < 5; ++i) {
        std::uint32_t sum, cnt;
        detail::gmc_sad(cur.fine, zoom, kGmcW, kGmcH, kFm, 2 * bdx + i - 2, 2 * bdy + j - 2, sum, cnt);
        const float c = cnt ? static_cast<float>(sum) / static_cast<float>(cnt) : 1e9f;
        grid[si][j][i] = c;
        if (static_cast<float>(cnt) >= p.min_valid * kFBox && c < fbest) {
          fbest = c;
          fs = si;
          fx = i;
          fy = j;
          fcnt = cnt;
        }
      }
    }
  }
  if (fbest >= 1e9f) return out;
  const float(&g)[5][5] = grid[fs];
  const float sub_x = fx > 0 && fx < 4 ? detail::parabola_peak(g[fy][fx - 1], g[fy][fx], g[fy][fx + 1]) : 0.f;
  const float sub_y = fy > 0 && fy < 4 ? detail::parabola_peak(g[fy - 1][fx], g[fy][fx], g[fy + 1][fx]) : 0.f;
  const float sub_s =
      fs > 0 && fs < 4 ? detail::parabola_peak(grid[fs - 1][fy][fx], grid[fs][fy][fx], grid[fs + 1][fy][fx]) : 0.f;

  out.s = s0 + (fs - 2 + sub_s) * 0.5f * kStep;
  out.tx = (2 * bdx + fx - 2 + sub_x) * static_cast<float>(cur.w) / kGmcW;
  out.ty = (2 * bdy + fy - 2 + sub_y) * static_cast<float>(cur.h) / kGmcH;
  out.cost = fbest;
  out.valid_frac = static_cast<float>(fcnt) / kFBox;
  out.ok = fbest <= p.max_cost && total / nt - best >= p.min_contrast;
  return out;
}

// Row-major 3x3 for GmcMotion: previous image -> current image.
inline void gmc_matrix(const GmcMotion& m, float w, float h, float* G) noexcept {
  const float cx = 0.5f * w, cy = 0.5f * h;
  G[0] = m.s;
  G[1] = 0.f;
  G[2] = (1.f - m.s) * cx + m.tx;
  G[3] = 0.f;
  G[4] = m.s;
  G[5] = (1.f - m.s) * cy + m.ty;
  G[6] = 0.f;
  G[7] = 0.f;
  G[8] = 1.f;
}

}  // namespace touchfc
