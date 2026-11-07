#pragma once

#include <cmath>
#include <cstdint>
#include <utility>

namespace touchfc {

struct Lab {
  float L = 0.f;
  float a = 0.f;
  float b = 0.f;
};

inline float lab_dist(Lab p, Lab q) noexcept {
  const float dL = p.L - q.L;
  const float da = p.a - q.a;
  const float db = p.b - q.b;
  return std::sqrt(dL * dL + da * da + db * db);
}

inline Lab rgb_to_lab(std::uint8_t R, std::uint8_t G, std::uint8_t B) noexcept {
  auto lin = [](float u) {
    u /= 255.f;
    return u <= 0.04045f ? u / 12.92f : std::pow((u + 0.055f) / 1.055f, 2.4f);
  };
  const float r = lin(R), g = lin(G), b = lin(B);
  float X = (r * 0.4124f + g * 0.3576f + b * 0.1805f) / 0.95047f;
  float Y = r * 0.2126f + g * 0.7152f + b * 0.0722f;
  float Z = (r * 0.0193f + g * 0.1192f + b * 0.9505f) / 1.08883f;
  auto f = [](float t) {
    return t > 0.008856f ? std::pow(t, 1.f / 3.f) : (7.787f * t + 16.f / 116.f);
  };
  X = f(X);
  Y = f(Y);
  Z = f(Z);
  return {116.f * Y - 16.f, 500.f * (X - Y), 200.f * (Y - Z)};
}

// Mean jersey colour. The upper half of the box, skipping green grass.
// Returns false when the crop has almost no non-grass pixels.
inline bool torso_lab(const std::uint8_t* bgra, std::uint32_t stride, std::uint32_t frame_w,
                      std::uint32_t frame_h, float x, float y, float w, float h, Lab& out,
                      int margin = 12, int min_green = 40, int min_px = 8) noexcept {
  if (bgra == nullptr || w < 4.f || h < 8.f) return false;
  int x0 = static_cast<int>(x);
  int y0 = static_cast<int>(y);
  int x1 = static_cast<int>(x + w);
  int y1 = static_cast<int>(y + 0.45f * h);
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > static_cast<int>(frame_w)) x1 = static_cast<int>(frame_w);
  if (y1 > static_cast<int>(frame_h)) y1 = static_cast<int>(frame_h);
  if (x1 - x0 < 2 || y1 - y0 < 2) return false;
  float r = 0, g = 0, b = 0;
  int n = 0;
  for (int py = y0; py < y1; py += 2) {
    const std::uint8_t* row = bgra + static_cast<std::uint32_t>(py) * stride;
    for (int px = x0; px < x1; px += 2) {
      const std::uint8_t* p = row + static_cast<std::uint32_t>(px) * 4u;
      if (p[1] > p[2] + margin && p[1] > p[0] + margin && p[1] > min_green) continue;
      b += p[0];
      g += p[1];
      r += p[2];
      ++n;
    }
  }
  if (n < min_px) return false;
  out = rgb_to_lab(static_cast<std::uint8_t>(r / n), static_cast<std::uint8_t>(g / n),
                   static_cast<std::uint8_t>(b / n));
  return true;
}

// 1 or 2 when that seed is nearer. 0 when neither seed is set, or the nearer
// one is further than max_dist (a referee or keeper in a third colour).
inline int nearest_team(Lab sample, Lab seed_a, bool a_ok, Lab seed_b, bool b_ok,
                        float max_dist = 1e9f) noexcept {
  const float da = a_ok ? lab_dist(sample, seed_a) : 1e30f;
  const float db = b_ok ? lab_dist(sample, seed_b) : 1e30f;
  const float d = da <= db ? da : db;
  if (d > max_dist) return 0;
  return da <= db ? 1 : 2;
}

// The two kit colours from the players themselves, so no click is needed. A
// ring of torso samples (tracked outfield players, wide shots), then k-means
// with k = 3: the third cluster takes referees, keepers and shirts in shadow.
// The two biggest clusters are the kits when each holds min_share of the
// samples and they are at least min_sep apart. Fixed storage, no allocation.
struct KitClusterer {
  static constexpr int kCap = 512;
  Lab s[kCap]{};
  int n = 0;
  int head = 0;
  int added = 0;  // since the last solve
  float last_sep = 0.f;    // last solve: distance between the two biggest clusters
  float last_share = 0.f;  // ... and the second one's share of the samples

  void add(Lab lab) noexcept {
    s[head] = lab;
    head = (head + 1) % kCap;
    if (n < kCap) ++n;
    ++added;
  }
  void clear() noexcept { n = head = added = 0; }

  bool solve(float min_sep, float min_share, int min_n, Lab& a, Lab& b) noexcept {
    added = 0;
    if (n < min_n || n < 3) return false;
    // Deterministic spread-out start: far from the mean, far from that, then
    // the sample furthest from both.
    Lab mean{};
    for (int i = 0; i < n; ++i) {
      mean.L += s[i].L;
      mean.a += s[i].a;
      mean.b += s[i].b;
    }
    mean.L /= n;
    mean.a /= n;
    mean.b /= n;
    auto farthest = [&](const Lab* c, int k) {
      int best = 0;
      float best_d = -1.f;
      for (int i = 0; i < n; ++i) {
        float d = 1e30f;
        for (int j = 0; j < k; ++j) d = std::fmin(d, lab_dist(s[i], c[j]));
        if (d > best_d) {
          best_d = d;
          best = i;
        }
      }
      return s[best];
    };
    Lab c[3];
    c[0] = farthest(&mean, 1);
    c[1] = farthest(c, 1);
    c[2] = farthest(c, 2);
    int count[3]{};
    for (int it = 0; it < 12; ++it) {
      Lab sum[3]{};
      for (int& k : count) k = 0;
      for (int i = 0; i < n; ++i) {
        int best = 0;
        for (int k = 1; k < 3; ++k)
          if (lab_dist(s[i], c[k]) < lab_dist(s[i], c[best])) best = k;
        sum[best].L += s[i].L;
        sum[best].a += s[i].a;
        sum[best].b += s[i].b;
        ++count[best];
      }
      for (int k = 0; k < 3; ++k)
        if (count[k] > 0) c[k] = {sum[k].L / count[k], sum[k].a / count[k], sum[k].b / count[k]};
    }
    int i0 = 0, i1 = 1, i2 = 2;  // by count, descending
    if (count[i1] > count[i0]) std::swap(i0, i1);
    if (count[i2] > count[i1]) std::swap(i1, i2);
    if (count[i1] > count[i0]) std::swap(i0, i1);
    last_sep = lab_dist(c[i0], c[i1]);
    last_share = static_cast<float>(count[i1]) / static_cast<float>(n);
    if (static_cast<float>(count[i1]) < min_share * static_cast<float>(n)) return false;
    if (lab_dist(c[i0], c[i1]) < min_sep) return false;
    a = c[i0];
    b = c[i1];
    return true;
  }
};

// True when (a, b) fits (ref_a, ref_b) better crossed: keeps team A on the
// same kit when the clusters are refreshed, or follows the provider's order.
inline bool kits_crossed(Lab a, Lab b, Lab ref_a, Lab ref_b) noexcept {
  return lab_dist(a, ref_b) + lab_dist(b, ref_a) < lab_dist(a, ref_a) + lab_dist(b, ref_b);
}

struct LostLabel {
  float x = 0.f;
  float y = 0.f;
  Lab lab{};
  std::uint8_t team = 0;
  std::int64_t t_ns = 0;
};

// Team of a lost label that is close on the pitch and in colour, or 0.
inline int inherit_team(const LostLabel* lost, int n, float x, float y, Lab lab, std::int64_t now_ns,
                        float radius_m = 8.f, float max_dist = 28.f,
                        std::int64_t window_ns = 2000000000LL) noexcept {
  int best = -1;
  float best_d = radius_m * radius_m;
  for (int i = 0; i < n; ++i) {
    if (lost[i].team == 0) continue;
    if (now_ns - lost[i].t_ns > window_ns) continue;
    if (lab_dist(lab, lost[i].lab) > max_dist) continue;
    const float dx = x - lost[i].x;
    const float dy = y - lost[i].y;
    const float d = dx * dx + dy * dy;
    if (d <= best_d) {
      best_d = d;
      best = i;
    }
  }
  return best >= 0 ? lost[best].team : 0;
}

}  // namespace touchfc
