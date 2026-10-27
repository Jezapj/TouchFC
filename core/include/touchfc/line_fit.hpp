#pragma once

#include "touchfc/homography.hpp"

#include <cmath>
#include <cstdint>

namespace touchfc {

// Model-based line tracking. The pitch markings are projected through H^-1;
// each sample searches along its image normal for a bright ridge between two
// grass flanks, and H is refit from the matches. Any visible line helps, so a
// broadcast window of a third of the pitch keeps the fit alive. The same pass
// scores a candidate fit: a fit with no painted lines under it has low support.
struct LineFitParams {
  float search_px = 12.f;    // +- along the normal, at 1920 wide
  float contrast = 18.f;     // ridge over the brighter flank, grey levels
  float min_support = 0.35f; // matched / in-view samples to trust a fit
  int green_margin = 8;      // flank must be this much greener than red and blue
  int green_min = 40;
  int iters = 3;
};

struct LineFitStats {
  int in_view = 0;
  int matched = 0;
  float support = 0.f;
  bool constrained = false;  // matches pin both directions, not one family of parallel lines
};

namespace detail {

struct MarkSeg {
  float x0, y0, x1, y1;
};

// Straight markings in metres. Circles and the penalty arcs are generated.
inline constexpr MarkSeg kMarkSegs[] = {
    {-52.5f, 34.f, 52.5f, 34.f},       {-52.5f, -34.f, 52.5f, -34.f},
    {-52.5f, -34.f, -52.5f, 34.f},     {52.5f, -34.f, 52.5f, 34.f},
    {0.f, -34.f, 0.f, 34.f},
    {-52.5f, 20.16f, -36.f, 20.16f},   {-36.f, 20.16f, -36.f, -20.16f},  {-52.5f, -20.16f, -36.f, -20.16f},
    {52.5f, 20.16f, 36.f, 20.16f},     {36.f, 20.16f, 36.f, -20.16f},    {52.5f, -20.16f, 36.f, -20.16f},
    {-52.5f, 9.16f, -47.f, 9.16f},     {-47.f, 9.16f, -47.f, -9.16f},    {-52.5f, -9.16f, -47.f, -9.16f},
    {52.5f, 9.16f, 47.f, 9.16f},       {47.f, 9.16f, 47.f, -9.16f},      {52.5f, -9.16f, 47.f, -9.16f},
};

// f(X, Y, tX, tY, cls): point and unit tangent in metres, roughly every metre.
// cls 0 = runs along x, 1 = along y, 2 = curved.
template <class F>
void for_each_mark_sample(F&& f) {
  for (const MarkSeg& s : kMarkSegs) {
    const float dx = s.x1 - s.x0, dy = s.y1 - s.y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    const int n = static_cast<int>(len);
    const float tx = dx / len, ty = dy / len;
    const int cls = std::fabs(tx) > std::fabs(ty) ? 0 : 1;
    for (int i = 0; i <= n; ++i) {
      const float a = static_cast<float>(i) / static_cast<float>(n);
      f(s.x0 + a * dx, s.y0 + a * dy, tx, ty, cls);
    }
  }
  constexpr float kR = 9.15f;
  for (int i = 0; i < 58; ++i) {
    const float t = static_cast<float>(i) * 6.2831853f / 58.f;
    f(kR * std::cos(t), kR * std::sin(t), -std::sin(t), std::cos(t), 2);
  }
  // Penalty arcs: the part outside the box, |angle| < acos(5.5 / 9.15) = 0.927.
  for (int i = -8; i <= 8; ++i) {
    const float t = static_cast<float>(i) * 0.927f / 8.f;
    const float c = std::cos(t), s = std::sin(t);
    f(-41.5f + kR * c, kR * s, -s, c, 2);
    f(41.5f - kR * c, kR * s, s, c, 2);
  }
}

inline int luma(const std::uint8_t* p) noexcept { return (p[0] + 2 * p[1] + p[2]) >> 2; }

inline bool grass_px(const std::uint8_t* p, int m, int lo) noexcept {
  return p[1] > p[2] + m && p[1] > p[0] + m && p[1] > lo;
}

struct LineView {
  const std::uint8_t* px;
  int w, h, stride;
  const std::uint8_t* at(int x, int y) const noexcept {
    return px + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(x) * 4u;
  }
};

// -1: search runs off the frame. 0: no ridge. 1: ridge at `off` along (nx, ny).
inline int ridge_search(const LineView& v, float x, float y, float nx, float ny, int r, int d,
                        const LineFitParams& p, float& off) noexcept {
  const float reach = static_cast<float>(r + d + 1);
  for (const float s : {-reach, reach}) {
    const float ex = x + s * nx, ey = y + s * ny;
    if (ex < 0.f || ey < 0.f || ex >= v.w - 1.f || ey >= v.h - 1.f) return -1;
  }
  constexpr int kMax = 40;
  if (r > kMax) r = kMax;
  float resp[2 * kMax + 1];
  int best = -1;
  for (int s = -r; s <= r; ++s) {
    const float qx = x + s * nx, qy = y + s * ny;
    const std::uint8_t* c = v.at(static_cast<int>(qx + 0.5f), static_cast<int>(qy + 0.5f));
    const std::uint8_t* a = v.at(static_cast<int>(qx + d * nx + 0.5f), static_cast<int>(qy + d * ny + 0.5f));
    const std::uint8_t* b = v.at(static_cast<int>(qx - d * nx + 0.5f), static_cast<int>(qy - d * ny + 0.5f));
    float e = -1e9f;
    if (grass_px(a, p.green_margin, p.green_min) && grass_px(b, p.green_margin, p.green_min)) {
      const int fl = luma(a) > luma(b) ? luma(a) : luma(b);
      e = static_cast<float>(luma(c) - fl);
    }
    resp[s + r] = e;
    if (best < 0 || e > resp[best]) best = s + r;
  }
  if (resp[best] < p.contrast) return 0;
  float sub = 0.f;
  if (best > 0 && best < 2 * r && resp[best - 1] > -1e8f && resp[best + 1] > -1e8f) {
    const float den = resp[best - 1] - 2.f * resp[best] + resp[best + 1];
    if (den < -1e-6f) sub = 0.5f * (resp[best - 1] - resp[best + 1]) / den;
  }
  off = static_cast<float>(best - r) + sub;
  return 1;
}

inline bool project(const Homography& inv, float X, float Y, float& x, float& y) noexcept {
  return apply_homography(inv, X, Y, x, y);
}

struct LineMatches {
  static constexpr int kCap = 512;
  float qx[kCap], qy[kCap];  // image point on the painted line
  float px[kCap], py[kCap];  // model sample, metres
  float tx[kCap], ty[kCap];  // model tangent there
  int n = 0;
};

// One search pass over every in-view marking sample.
inline LineFitStats line_pass(const LineView& v, const Homography& H, int r, const LineFitParams& p,
                              LineMatches& m) noexcept {
  LineFitStats st{};
  m.n = 0;
  Homography inv;
  if (!H.valid || !invert3(H.h, inv.h)) return st;
  int cls_n[3] = {0, 0, 0};
  for_each_mark_sample([&](float X, float Y, float tX, float tY, int cls) {
    float x, y, x1, y1, x2, y2;
    if (!project(inv, X, Y, x, y) || x < 0.f || y < 0.f || x >= v.w || y >= v.h) return;
    if (!project(inv, X + 0.5f * tX, Y + 0.5f * tY, x1, y1)) return;
    if (!project(inv, X - 0.12f * tY, Y + 0.12f * tX, x2, y2)) return;
    float ix = x1 - x, iy = y1 - y;
    const float il = std::sqrt(ix * ix + iy * iy);
    if (il < 1e-3f) return;
    ix /= il;
    iy /= il;
    const float nx = -iy, ny = ix;
    const float width = std::fabs((x2 - x) * nx + (y2 - y) * ny);  // line width in pixels
    int d = static_cast<int>(0.5f * width + 2.5f);
    d = d < 2 ? 2 : d > 10 ? 10 : d;
    float off = 0.f;
    const int got = ridge_search(v, x, y, nx, ny, r, d, p, off);
    if (got < 0) return;
    ++st.in_view;
    if (got == 0 || m.n >= LineMatches::kCap) return;
    m.qx[m.n] = x + off * nx;
    m.qy[m.n] = y + off * ny;
    m.px[m.n] = X;
    m.py[m.n] = Y;
    m.tx[m.n] = tX;
    m.ty[m.n] = tY;
    ++m.n;
    ++cls_n[cls];
  });
  st.matched = m.n;
  st.support = st.in_view ? static_cast<float>(m.n) / static_cast<float>(st.in_view) : 0.f;
  st.constrained = (cls_n[0] >= 4 && cls_n[1] >= 4) || cls_n[2] >= 8 ||
                   ((cls_n[0] >= 4 || cls_n[1] >= 4) && cls_n[2] >= 4);
  return st;
}

// Homography that puts each matched image point on its pitch line: one
// linear row per match, n . H(q) = c (curves use the tangent line, and the
// outer iterations re-linearise). Four frame points mapped by the prior are
// added at low weight, so a direction the lines do not pin stays where it was
// instead of going singular. Hartley-normalised like fit_homography.
inline bool fit_homography_lines(const LineMatches& m, const Homography& prior, float img_w, float img_h,
                                 Homography& out) noexcept {
  const int n = m.n;
  if (n < 4) return false;
  float smx = 0, smy = 0, dmx = 0, dmy = 0;
  for (int i = 0; i < n; ++i) {
    smx += m.qx[i];
    smy += m.qy[i];
    dmx += m.px[i];
    dmy += m.py[i];
  }
  smx /= n;
  smy /= n;
  dmx /= n;
  dmy /= n;
  float ss = 0, ds = 0;
  for (int i = 0; i < n; ++i) {
    ss += std::hypot(m.qx[i] - smx, m.qy[i] - smy);
    ds += std::hypot(m.px[i] - dmx, m.py[i] - dmy);
  }
  ss = ss > 1e-6f ? (1.41421356f * n / ss) : 1.f;
  ds = ds > 1e-6f ? (1.41421356f * n / ds) : 1.f;

  float Ata[8][8]{};
  float Atb[8]{};
  auto add = [&](const float* row, float w) {
    for (int c = 0; c < 8; ++c) {
      Atb[c] -= w * row[c] * row[8];
      for (int k = 0; k < 8; ++k) Ata[c][k] += w * row[c] * row[k];
    }
  };
  for (int i = 0; i < n; ++i) {
    const float x = (m.qx[i] - smx) * ss, y = (m.qy[i] - smy) * ss;
    const float nX = -m.ty[i], nY = m.tx[i];
    const float c = nX * (m.px[i] - dmx) * ds + nY * (m.py[i] - dmy) * ds;
    const float row[9] = {nX * x, nX * y, nX, nY * x, nY * y, nY, -c * x, -c * y, -c};
    add(row, 1.f);
  }
  const float wp = 0.05f * static_cast<float>(n) / 8.f;
  for (const float fy : {0.5f, 0.95f}) {
    for (const float fx : {0.1f, 0.9f}) {
      float X, Y;
      if (!apply_homography(prior, fx * img_w, fy * img_h, X, Y)) continue;
      const float x = (fx * img_w - smx) * ss, y = (fy * img_h - smy) * ss;
      X = (X - dmx) * ds;
      Y = (Y - dmy) * ds;
      const float rx[9] = {x, y, 1, 0, 0, 0, -X * x, -X * y, -X};
      const float ry[9] = {0, 0, 0, x, y, 1, -Y * x, -Y * y, -Y};
      add(rx, wp);
      add(ry, wp);
    }
  }
  float hn[8];
  if (!solve8(Ata, Atb, hn)) return false;
  const float Hn[9] = {hn[0], hn[1], hn[2], hn[3], hn[4], hn[5], hn[6], hn[7], 1.f};
  const float Tsrc[9] = {ss, 0, -ss * smx, 0, ss, -ss * smy, 0, 0, 1.f};
  const float invDst[9] = {1.f / ds, 0, dmx, 0, 1.f / ds, dmy, 0, 0, 1.f};
  float tmp[9];
  mul3(Hn, Tsrc, tmp);
  out = {};
  mul3(invDst, tmp, out.h);
  normalise(out.h, smx, smy);
  // Mean distance of the matches from their lines, metres.
  float err = 0.f;
  for (int i = 0; i < n; ++i) {
    float X, Y;
    if (!apply_homography(out, m.qx[i], m.qy[i], X, Y)) return false;
    err += std::fabs(-m.ty[i] * (X - m.px[i]) + m.tx[i] * (Y - m.py[i]));
  }
  out.rms_m = err / static_cast<float>(n);
  out.valid = true;
  return true;
}

inline int scaled_radius(float px, int w) noexcept {
  const int r = static_cast<int>(px * static_cast<float>(w) / 1920.f + 0.5f);
  return r < 3 ? 3 : r;
}

}  // namespace detail

// How much of the projected markings sits on painted lines.
inline LineFitStats line_support(const std::uint8_t* bgra, int w, int h, int stride, const Homography& H,
                                 const LineFitParams& p) noexcept {
  const detail::LineView v{bgra, w, h, stride};
  detail::LineMatches m;
  return detail::line_pass(v, H, detail::scaled_radius(p.search_px, w), p, m);
}

// Refit `pred` to the painted lines. False (out = pred) when the lines do not
// pin the fit down; stats are the last pass either way.
inline bool line_refine(const std::uint8_t* bgra, int w, int h, int stride, const Homography& pred,
                        const LineFitParams& p, Homography& out, LineFitStats& stats) noexcept {
  const detail::LineView v{bgra, w, h, stride};
  detail::LineMatches m;
  out = pred;
  int r = detail::scaled_radius(p.search_px, w);
  Homography H = pred;
  for (int it = 0; it < p.iters; ++it) {
    stats = detail::line_pass(v, H, r, p, m);
    if (!stats.constrained || m.n < 8) return false;
    Homography next;
    if (!detail::fit_homography_lines(m, H, static_cast<float>(w), static_cast<float>(h), next)) return false;
    H = next;
    r = r / 2 < 3 ? 3 : r / 2;
  }
  stats = detail::line_pass(v, H, r, p, m);
  if (!stats.constrained) return false;
  out = H;
  return true;
}

}  // namespace touchfc
