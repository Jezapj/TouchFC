#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace touchfc {

// One OCR read of the scorebug: "ARS 1 - 0 EVE 23:41". Left code is home.
struct ScorebugRead {
  char home[6]{};
  char away[6]{};
  int home_score = -1;
  int away_score = -1;
  int clock_ms = -1;
};

namespace detail {
inline bool upper(char c) noexcept { return c >= 'A' && c <= 'Z'; }
inline bool digit(char c) noexcept { return c >= '0' && c <= '9'; }
}  // namespace detail

// Pattern CODE n [sep] n CODE, codes 2 to 4 capitals, separators anything that
// is not a letter or digit (spaces, '-', ':', '|', an en dash's UTF-8 bytes).
// A mm:ss anywhere is the clock. False when no score pattern is found.
inline bool parse_scorebug(const char* text, ScorebugRead& out) noexcept {
  out = {};
  struct Tok {
    int kind;  // 0 code, 1 number
    int a, b;  // span
    int value;
  };
  Tok t[48];
  int n = 0;
  const int len = static_cast<int>(std::strlen(text));
  for (int i = 0; i < len && n < 48;) {
    const char c = text[i];
    if (detail::digit(c)) {
      int j = i, v = 0;
      while (j < len && detail::digit(text[j]) && j - i < 4) v = v * 10 + (text[j++] - '0');
      // mm:ss clock
      if (j < len - 2 && text[j] == ':' && detail::digit(text[j + 1]) && detail::digit(text[j + 2]) &&
          (j + 3 >= len || !detail::digit(text[j + 3]))) {
        const int ss = (text[j + 1] - '0') * 10 + (text[j + 2] - '0');
        if (v <= 130 && ss < 60) out.clock_ms = (v * 60 + ss) * 1000;
        i = j + 3;
        continue;
      }
      t[n++] = {1, i, j, v};
      i = j;
    } else if (detail::upper(c)) {
      int j = i;
      while (j < len && (detail::upper(text[j]) || detail::digit(text[j]))) ++j;
      const int w = j - i;
      bool letters = true;
      for (int k = i; k < j; ++k) letters = letters && detail::upper(text[k]);
      if (letters && w >= 2 && w <= 4) t[n++] = {0, i, j, 0};
      else if (!letters) {
        // "ARS1" style: split letters and a trailing score.
        int k = i;
        while (k < j && detail::upper(text[k])) ++k;
        if (k - i >= 2 && k - i <= 4 && n < 47) {
          t[n++] = {0, i, k, 0};
          int v = 0;
          for (int q = k; q < j; ++q) v = v * 10 + (text[q] - '0');
          t[n++] = {1, k, j, v};
        }
      }
      i = j;
    } else {
      ++i;
    }
  }
  for (int k = 0; k + 3 < n; ++k) {
    if (t[k].kind != 0 || t[k + 1].kind != 1 || t[k + 2].kind != 1 || t[k + 3].kind != 0) continue;
    if (t[k + 1].value > 30 || t[k + 2].value > 30) continue;
    const int wh = t[k].b - t[k].a, wa = t[k + 3].b - t[k + 3].a;
    std::memcpy(out.home, text + t[k].a, static_cast<std::size_t>(wh));
    std::memcpy(out.away, text + t[k + 3].a, static_cast<std::size_t>(wa));
    out.home_score = t[k + 1].value;
    out.away_score = t[k + 2].value;
    return true;
  }
  return false;
}

// Confident when the same codes are read N times in a row.
class ScorebugVote {
 public:
  // True once confident; stays true while reads agree.
  bool add(const ScorebugRead& r, int need = 3) noexcept {
    if (std::strcmp(r.home, home_) == 0 && std::strcmp(r.away, away_) == 0) {
      if (run_ < 1000) ++run_;
    } else {
      std::memcpy(home_, r.home, sizeof(home_));
      std::memcpy(away_, r.away, sizeof(away_));
      run_ = 1;
    }
    return run_ >= need;
  }
  const char* home() const noexcept { return home_; }
  const char* away() const noexcept { return away_; }

 private:
  char home_[6]{};
  char away_[6]{};
  int run_ = 0;
};

// Finds the scorebug: over wide frames, cells in the top 20% of the picture
// that are both high-contrast and barely change between frames. Screen-fixed
// graphics pass; the crowd, boards and pitch move with the camera. The largest
// wide cluster wins (a channel logo is smaller and squarer). Fixed arrays, no
// allocation, so it can run on T3 until it locks.
class ScorebugRoiFinder {
 public:
  static constexpr int kCell = 16;
  static constexpr int kMaxCx = 160;  // up to 2560 px wide
  static constexpr int kMaxCy = 18;   // top 20% of up to 1440 px

  void reset() noexcept { *this = ScorebugRoiFinder{}; }
  bool done() const noexcept { return done_; }
  const int* roi() const noexcept { return roi_; }

  // stable_max: mean cell luma change per frame; contrast_min: mean |dx| luma.
  void add(const std::uint8_t* bgra, int w, int h, int stride, int frames_needed, float stable_max,
           float contrast_min) noexcept {
    if (done_) return;
    const int cx = w / kCell < kMaxCx ? w / kCell : kMaxCx;
    const int cy0 = (h / 5) / kCell;
    const int cy = cy0 < kMaxCy ? cy0 : kMaxCy;
    if (cx <= 0 || cy <= 0) return;
    if (cx != cx_ || cy != cy_) {
      reset();
      cx_ = cx;
      cy_ = cy;
    }
    for (int j = 0; j < cy; ++j) {
      for (int i = 0; i < cx; ++i) {
        int sum = 0, grad = 0, cnt = 0;
        for (int y = j * kCell; y < (j + 1) * kCell; y += 2) {
          const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
          int prev = -1;
          for (int x = i * kCell; x < (i + 1) * kCell; x += 2) {
            const std::uint8_t* p = row + x * 4;
            const int l = (p[0] + 2 * p[1] + p[2]) >> 2;
            sum += l;
            if (prev >= 0) grad += l > prev ? l - prev : prev - l;
            prev = l;
            ++cnt;
          }
        }
        const float mean = static_cast<float>(sum) / static_cast<float>(cnt);
        Cell& c = cells_[j][i];
        if (frames_ > 0) c.change += std::fabs(mean - c.last);
        c.last = mean;
        c.grad += static_cast<float>(grad) / static_cast<float>(cnt);
      }
    }
    if (++frames_ < frames_needed) return;
    // Detailed and stable cells, bridged across up to 3 cells sideways and 1
    // vertically through stable cells (the flat box between glyph groups),
    // then the largest 4-connected cluster that is wider than tall.
    std::uint8_t stable[kMaxCy][kMaxCx]{};
    std::uint8_t seed[kMaxCy][kMaxCx]{};
    std::uint8_t mark[kMaxCy][kMaxCx]{};
    for (int j = 0; j < cy; ++j)
      for (int i = 0; i < cx; ++i) {
        const Cell& c = cells_[j][i];
        stable[j][i] = c.change / (frames_ - 1) <= stable_max ? 1 : 0;
        seed[j][i] = stable[j][i] && c.grad / frames_ >= contrast_min ? 1 : 0;
      }
    for (int j = 0; j < cy; ++j)
      for (int i = 0; i < cx; ++i) {
        if (!stable[j][i]) continue;
        bool left = false, right = false;
        for (int dj = -1; dj <= 1; ++dj) {
          const int y = j + dj;
          if (y < 0 || y >= cy) continue;
          for (int d = 0; d <= 3; ++d) {
            if (i - d >= 0 && seed[y][i - d]) left = true;
            if (i + d < cx && seed[y][i + d]) right = true;
          }
        }
        mark[j][i] = seed[j][i] || (left && right) ? 1 : 0;
      }
    int best = 0;
    std::int16_t stack[kMaxCy * kMaxCx][2];
    for (int j = 0; j < cy; ++j) {
      for (int i = 0; i < cx; ++i) {
        if (mark[j][i] != 1) continue;
        int sp = 0, count = 0, x0 = i, x1 = i, y0 = j, y1 = j;
        stack[sp][0] = static_cast<std::int16_t>(i);
        stack[sp++][1] = static_cast<std::int16_t>(j);
        mark[j][i] = 2;
        while (sp > 0) {
          const int x = stack[--sp][0], y = stack[sp][1];
          ++count;
          x0 = x < x0 ? x : x0;
          x1 = x > x1 ? x : x1;
          y0 = y < y0 ? y : y0;
          y1 = y > y1 ? y : y1;
          const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
          for (const auto& q : nb) {
            if (q[0] < 0 || q[1] < 0 || q[0] >= cx || q[1] >= cy || mark[q[1]][q[0]] != 1) continue;
            mark[q[1]][q[0]] = 2;
            stack[sp][0] = static_cast<std::int16_t>(q[0]);
            stack[sp++][1] = static_cast<std::int16_t>(q[1]);
          }
        }
        if (count > best && x1 - x0 + 1 >= 2 * (y1 - y0 + 1) && count >= 4) {
          best = count;
          // One cell of margin so the edge glyphs are inside.
          const int ax = x0 > 0 ? x0 - 1 : 0, ay = y0 > 0 ? y0 - 1 : 0;
          const int bx = x1 + 1 < cx ? x1 + 1 : cx - 1, by = y1 + 1 < cy ? y1 + 1 : cy - 1;
          roi_[0] = ax * kCell;
          roi_[1] = ay * kCell;
          roi_[2] = (bx - ax + 1) * kCell;
          roi_[3] = (by - ay + 1) * kCell;
        }
      }
    }
    done_ = best > 0;
    if (!done_) {
      frames_ = 0;  // nothing yet (a replay or a close-up); start over
      for (auto& row : cells_)
        for (auto& c : row) c = Cell{};
    }
  }

 private:
  struct Cell {
    float last = 0.f;
    float change = 0.f;
    float grad = 0.f;
  };
  Cell cells_[kMaxCy][kMaxCx]{};
  int cx_ = 0, cy_ = 0;
  int frames_ = 0;
  bool done_ = false;
  int roi_[4]{};
};

}  // namespace touchfc
