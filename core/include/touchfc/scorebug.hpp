#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
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
inline bool is_lower(char c) noexcept { return c >= 'a' && c <= 'z'; }
inline bool digit(char c) noexcept { return c >= '0' && c <= '9'; }
inline bool alnum(char c) noexcept { return upper(c) || is_lower(c) || digit(c); }
// A letter standing alone where a score belongs is OCR's 0 or 1.
inline int lone_digit(char c) noexcept {
  return c == 'O' || c == 'o' || c == 'Q' || c == 'D' ? 0 : c == 'I' || c == 'l' || c == 'i' ? 1 : -1;
}
// The same length as the hint, at most one letter different (3+ letters).
inline bool code_near(const char* code, const char* hint) noexcept {
  if (!hint || !hint[0]) return false;
  const std::size_t n = std::strlen(code);
  if (n != std::strlen(hint)) return false;
  int diff = 0;
  for (std::size_t i = 0; i < n; ++i) diff += code[i] != hint[i];
  return diff == 0 || (diff == 1 && n >= 3);
}
// code_near, or the hint with one stray letter before or after it ("KCHE":
// a patterned panel beside the code read as a letter).
inline bool code_fits(const char* code, const char* hint) noexcept {
  if (code_near(code, hint)) return true;
  if (!hint || !hint[0]) return false;
  const std::size_t n = std::strlen(code), m = std::strlen(hint);
  return m >= 3 && n == m + 1 && (std::strcmp(code + 1, hint) == 0 || std::strncmp(code, hint, m) == 0);
}
}  // namespace detail

// How far h, a is from the score on show: 0 the same, 1 one more goal for
// one side, 2 one fewer for one side, 3 anything else (or cur unknown). A
// side of -1 (not read) counts as unchanged.
inline int score_step(int cur_h, int cur_a, int h, int a) noexcept {
  if (cur_h < 0 || cur_a < 0) return 3;
  const int dh = h < 0 ? 0 : h - cur_h, da = a < 0 ? 0 : a - cur_a;
  if (dh == 0 && da == 0) return 0;
  if ((dh == 0 && da == 1) || (da == 0 && dh == 1)) return 1;
  if ((dh == 0 && da == -1) || (da == 0 && dh == -1)) return 2;
  return 3;
}

// Codes are 2 to 4 capitals, separators anything that is not a letter or
// digit (spaces, '-', ':', '|', an en dash's UTF-8 bytes). Two layouts, text
// in reading order: CODE n [sep] n CODE (side by side) and CODE n CODE n
// (stacked, the top row is home). A mm:ss anywhere is the clock. With the
// teams known, their codes are passed as hints: a read one letter off is
// taken as that code, and a match on both codes wins over any other. With the
// score on show also known (cur_h, cur_a in hint order), extra numbers by the
// codes (a crest or logo read as a digit) are passed over for the numbers
// within one goal of it. False when no score pattern is found.
inline bool parse_scorebug(const char* text, ScorebugRead& out, const char* hint_home = nullptr,
                           const char* hint_away = nullptr, int cur_h = -1, int cur_a = -1) noexcept {
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
    const bool alone = (i == 0 || !detail::alnum(text[i - 1])) && (i + 1 >= len || !detail::alnum(text[i + 1]));
    if (alone && detail::lone_digit(c) >= 0) {
      t[n++] = {1, i, i + 1, detail::lone_digit(c)};
      ++i;
    } else if (detail::digit(c)) {
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
      if (j - i == 3 && n < 47) {
        // "261": two scores with the logo between them read as a digit.
        t[n++] = {1, i, i + 1, text[i] - '0'};
        t[n++] = {1, j - 1, j, text[j - 1] - '0'};
        i = j;
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
  const bool hinted = hint_home && hint_away && hint_home[0] && hint_away[0];
  // Copies the code; true when it is (nearly) one of the known teams'.
  auto name = [&](const Tok& tk, char* dst) {
    std::memset(dst, 0, 6);
    std::memcpy(dst, text + tk.a, static_cast<std::size_t>(tk.b - tk.a));
    if (!hinted) return false;
    const char* h = detail::code_fits(dst, hint_home)   ? hint_home
                    : detail::code_fits(dst, hint_away) ? hint_away
                                                        : nullptr;
    if (h) std::snprintf(dst, 6, "%s", h);
    return h != nullptr;
  };
  // The first CODE n [n] CODE [n] pattern; pass 0 must name both known teams.
  auto pattern = [&](int pass, ScorebugRead& r) {
    for (int k = 0; k + 3 < n; ++k) {
      if (t[k].kind != 0 || t[k + 1].kind != 1) continue;
      int ia, is;  // away code, away score
      if (t[k + 2].kind == 1 && t[k + 3].kind == 0) ia = k + 3, is = k + 2;
      else if (t[k + 2].kind == 0 && t[k + 3].kind == 1) ia = k + 2, is = k + 3;
      else continue;
      int vh = t[k + 1].value, va = t[is].value;
      // "26 1" or "2 61" side by side: three digits between the codes, the
      // middle one the logo.
      if (is == k + 2) {
        const int dh = t[k + 1].b - t[k + 1].a, da = t[is].b - t[is].a;
        if (dh == 2 && da == 1) vh /= 10;
        else if (dh == 1 && da == 2) va %= 10;
      }
      if (vh > 30 || va > 30) continue;
      r = {};
      r.clock_ms = out.clock_ms;
      const bool kh = name(t[k], r.home), ka = name(t[ia], r.away);
      if (pass == 0 && (!kh || !ka || std::strcmp(r.home, r.away) == 0)) continue;
      r.home_score = vh;
      r.away_score = va;
      return true;
    }
    return false;
  };
  // Extra numbers by the codes (a crest or logo read as a digit) break the
  // pattern. With the score on show known (cur, in hint order): the numbers
  // between the two codes (side by side) or after each (stacked) within one
  // goal of it, no change first, then fewest numbers skipped. False when
  // none is.
  auto nearest = [&](ScorebugRead& r) {
    if (!hinted || cur_h < 0 || cur_a < 0) return false;
    int side[48];
    for (int k = 0; k < n; ++k) {
      char s[6];
      side[k] = -1;
      if (t[k].kind == 0 && name(t[k], s)) side[k] = std::strcmp(s, hint_home) == 0 ? 0 : 1;
    }
    int best_step = 3, best_skip = 1 << 20, bp = -1, bi = -1, bj = -1;
    for (int p = 0; p < n; ++p) {
      if (side[p] < 0) continue;
      int q = p + 1;
      while (q < n && t[q].kind == 1) ++q;
      if (q >= n || side[q] < 0 || side[q] == side[p]) continue;
      int e = q + 1;  // numbers after q, up to the next code
      while (e < n && t[e].kind == 1) ++e;
      for (int i = p + 1; i < q; ++i)
        for (int j = i + 1; j < e; ++j) {  // j before q: side by side; after: stacked
          if (j == q || t[i].value > 30 || t[j].value > 30) continue;
          const int skip = (i - p - 1) + (j < q ? q - j - 1 : j - q - 1);
          const int vh = side[p] == 0 ? t[i].value : t[j].value, va = side[p] == 0 ? t[j].value : t[i].value;
          const int st = score_step(cur_h, cur_a, vh, va);
          if (st < best_step || (st == best_step && skip < best_skip))
            best_step = st, best_skip = skip, bp = p, bi = i, bj = j;
        }
    }
    if (bp < 0 || best_step > 2) return false;
    r = {};
    r.clock_ms = out.clock_ms;
    std::snprintf(r.home, sizeof(r.home), "%s", side[bp] == 0 ? hint_home : hint_away);
    std::snprintf(r.away, sizeof(r.away), "%s", side[bp] == 0 ? hint_away : hint_home);
    r.home_score = t[bi].value;
    r.away_score = t[bj].value;
    return true;
  };
  // r's change from cur, its scores put in hint order.
  auto step_of = [&](const ScorebugRead& r) {
    const bool swapped = std::strcmp(r.home, hint_away) == 0;
    return score_step(cur_h, cur_a, swapped ? r.away_score : r.home_score, swapped ? r.home_score : r.away_score);
  };
  ScorebugRead r;
  if (hinted && pattern(0, r)) {
    // A read more than one goal off: the extra numbers may hold the real one.
    ScorebugRead alt;
    out = step_of(r) == 3 && nearest(alt) ? alt : r;
    return true;
  }
  if (nearest(r) || pattern(1, r)) {
    out = r;
    return true;
  }
  return false;
}

// One OCR word and its box (any pixel scale, y down).
struct BugWord {
  float x = 0, y = 0, w = 0, h = 0;
  char s[24]{};
};

// The same read from word boxes, so each score is tied to the code it sits
// beside: a team's score is on the code's row, directly left or right of it
// (no other number between), whatever row the other team is on. With the
// teams known (hints), out.home/away are the hints in that order, a side not
// seen is -1, and one team with its score is enough: the other's is the
// number just past it on the same row ("TOT 1 - 1"), or the one under or over
// it on the next row (stacked), when no code sits beside that number.
// Without hints both codes and scores are needed; home is the left (or top)
// code. With the score on show also known (cur_h, cur_a in hint order), a
// read more than one goal off it gives way to other numbers on the codes'
// rows (a crest or logo read as a digit) that are within one goal of it.
// False when no score is found.
inline bool parse_scorebug_words(const BugWord* words, int nw, ScorebugRead& out, const char* hint_home = nullptr,
                                 const char* hint_away = nullptr, int cur_h = -1, int cur_a = -1) noexcept {
  out = {};
  struct Tok {
    bool code;
    float x0, x1, cy, h;
    int value;
    int nd;  // digits read
    char s[6];
  };
  Tok t[64];
  int n = 0;
  const bool hinted = hint_home && hint_away && hint_home[0] && hint_away[0];
  auto up = [](char c) { return detail::is_lower(c) ? static_cast<char>(c - 32) : c; };
  for (int wi = 0; wi < nw; ++wi) {
    const BugWord& w = words[wi];
    const char* s = w.s;
    const int len = static_cast<int>(std::strlen(s));
    if (len == 0 || w.h <= 0) continue;
    const float cw = w.w / static_cast<float>(len);
    auto push = [&](bool code, int a, int b, int v) {
      if (n >= 64) return;
      Tok& k = t[n++];
      k.code = code;
      k.x0 = w.x + cw * static_cast<float>(a);
      k.x1 = w.x + cw * static_cast<float>(b);
      k.cy = w.y + 0.5f * w.h;
      k.h = w.h;
      k.value = v;
      k.nd = b - a;
      std::memset(k.s, 0, sizeof(k.s));
      if (code)
        for (int q = a; q < b && q - a < 5; ++q) k.s[q - a] = up(s[q]);
    };
    for (int i = 0; i < len;) {
      const char c = s[i];
      const bool alone = (i == 0 || !detail::alnum(s[i - 1])) && (i + 1 >= len || !detail::alnum(s[i + 1]));
      if (alone && detail::lone_digit(c) >= 0) {
        push(false, i, i + 1, detail::lone_digit(c));
        ++i;
      } else if (detail::digit(c)) {
        int j = i, v = 0;
        while (j < len && detail::digit(s[j])) v = v * 10 + (s[j++] - '0');
        if (j < len - 2 && s[j] == ':' && detail::digit(s[j + 1]) && detail::digit(s[j + 2]) &&
            (j + 3 >= len || !detail::digit(s[j + 3]))) {
          const int ss = (s[j + 1] - '0') * 10 + (s[j + 2] - '0');
          if (j - i <= 3 && v <= 130 && ss < 60) out.clock_ms = (v * 60 + ss) * 1000;
          i = j + 3;
          continue;
        }
        // Added time ("+3") and long numbers are never a score.
        const bool added = i > 0 && s[i - 1] == '+';
        if (!added && j - i == 3) {
          // "261": two scores with the logo between them read as a digit.
          push(false, i, i + 1, s[i] - '0');
          push(false, j - 1, j, s[j - 1] - '0');
        } else if (!added && j - i <= 2) {
          push(false, i, j, v);  // past 30 dropped after the logo trim below
        }
        i = j;
      } else if (detail::upper(c) || detail::is_lower(c)) {
        int j = i;
        while (j < len && (detail::upper(s[j]) || detail::is_lower(s[j]))) ++j;
        bool caps = true;
        for (int k = i; k < j; ++k) caps = caps && detail::upper(s[k]);
        char cu[6]{};
        if (j - i >= 2 && j - i <= 4)
          for (int k = i; k < j; ++k) cu[k - i] = up(s[k]);
        // Lower case only for a known code ("Ars"); words are not codes.
        if (cu[0] && (caps || (hinted && (detail::code_near(cu, hint_home) || detail::code_near(cu, hint_away))))) {
          push(true, i, j, 0);
        } else if (!caps) {
          // Capitals with stray lower case at the ends ("kCHE", "CHEk"): a
          // patterned panel beside the code read as letters.
          int a = i, b = j;
          while (a < b && detail::is_lower(s[a])) ++a;
          while (b > a && detail::is_lower(s[b - 1])) --b;
          bool inner = b - a >= 2 && b - a <= 4 && (a > i || b < j);
          for (int k = a; inner && k < b; ++k) inner = detail::upper(s[k]);
          if (inner) push(true, a, b, 0);
        }
        i = j;
      } else {
        ++i;
      }
    }
  }
  auto row = [](const Tok& a, const Tok& b) {
    return std::fabs(a.cy - b.cy) <= 0.5f * (a.h > b.h ? a.h : b.h);
  };
  // "26 1" or "2 61": three digits close together on a row are two scores
  // with the logo between them read as a digit; the two-digit word loses it.
  for (int p = 0; p < n; ++p) {
    if (t[p].code) continue;
    for (int q = 0; q < n; ++q) {
      if (q == p || t[q].code || t[p].nd + t[q].nd != 3 || !row(t[p], t[q])) continue;
      const float g = t[q].x0 - t[p].x1;  // q right of p
      if (g < -1.f || g > 1.5f * t[p].h) continue;
      bool mid = false;
      for (int k = 0; k < n; ++k)
        mid = mid || (k != p && k != q && row(t[k], t[p]) && t[k].x0 >= t[p].x1 - 1.f && t[k].x1 <= t[q].x0 + 1.f);
      if (mid) continue;
      Tok& two = t[p].nd == 2 ? t[p] : t[q];
      const float half = 0.5f * (two.x0 + two.x1);
      if (&two == &t[p]) two.value /= 10, two.x1 = half;  // keep the left digit
      else two.value %= 10, two.x0 = half;               // keep the right one
      two.nd = 1;
    }
  }
  {  // Numbers past 30 are never a score.
    int m = 0;
    for (int k = 0; k < n; ++k)
      if (t[k].code || t[k].value <= 30) t[m++] = t[k];
    n = m;
  }
  // Something of kind `code` strictly between a and b on their row.
  auto between = [&](int a, int b, int skip_kind) {
    const float l = t[a].x1 < t[b].x1 ? t[a].x1 : t[b].x1, r = t[a].x0 > t[b].x0 ? t[a].x0 : t[b].x0;
    for (int k = 0; k < n; ++k)
      if (k != a && k != b && static_cast<int>(t[k].code) != skip_kind && row(t[k], t[a]) && t[k].x0 >= l - 1.f &&
          t[k].x1 <= r + 1.f)
        return true;
    return false;
  };
  // Gap from a code to a number directly beside it on its row; -1: not beside.
  auto beside = [&](int c, int m) -> float {
    if (!t[c].code || t[m].code || !row(t[c], t[m]) || between(c, m, 2)) return -1.f;
    const float g = t[m].x0 >= t[c].x1 - 1.f ? t[m].x0 - t[c].x1 : t[c].x0 - t[m].x1;
    return g >= -t[c].h && g <= 6.f * t[c].h ? (g > 0.f ? g : 0.f) : -1.f;
  };
  // Side of each code: 0, 1 (a known team), 2 (other, unhinted only), -1.
  int side[64];
  static constexpr const char* kStop[] = {"HT", "FT", "ET", "AET", "PEN", "LIVE", "UHD", "HD", "VAR", "MIN", "TV"};
  for (int k = 0; k < n; ++k) {
    side[k] = -1;
    if (!t[k].code) continue;
    if (hinted) {
      side[k] = detail::code_fits(t[k].s, hint_home) ? 0 : detail::code_fits(t[k].s, hint_away) ? 1 : -1;
      if (side[k] >= 0 && std::strcmp(hint_home, hint_away) == 0) side[k] = -1;
    } else {
      bool stop = false;
      for (const char* w : kStop) stop = stop || std::strcmp(t[k].s, w) == 0;
      side[k] = stop ? -1 : 2;
    }
  }
  // Pair codes and numbers, nearest first, each used once.
  int score_of[64], code_of[64];
  for (int k = 0; k < n; ++k) score_of[k] = code_of[k] = -1;
  for (;;) {
    int bc = -1, bm = -1;
    float bg = 1e9f;
    for (int c = 0; c < n; ++c) {
      if (side[c] < 0 || score_of[c] >= 0) continue;
      if (hinted) {  // one code per known team
        bool taken = false;
        for (int q = 0; q < n; ++q) taken = taken || (q != c && side[q] == side[c] && score_of[q] >= 0);
        if (taken) continue;
      }
      for (int m = 0; m < n; ++m) {
        if (t[m].code || code_of[m] >= 0) continue;
        const float g = beside(c, m);
        if (g >= 0.f && g < bg) bg = g, bc = c, bm = m;
      }
    }
    if (bc < 0) break;
    score_of[bc] = bm;
    code_of[bm] = bc;
  }
  if (hinted) {
    int sc[2] = {-1, -1}, at[2] = {-1, -1};
    for (int c = 0; c < n; ++c)
      if (side[c] >= 0 && score_of[c] >= 0 && at[side[c]] < 0) at[side[c]] = c, sc[side[c]] = t[score_of[c]].value;
    if (sc[0] < 0 && sc[1] < 0) return false;
    if (sc[0] < 0 || sc[1] < 0) {
      const int known = sc[0] >= 0 ? 0 : 1, c = at[known], m = score_of[c];
      // A number no code sits beside: past the score on its row, or on the next row in line with it.
      int best = -1;
      float bd = 1e9f;
      for (int u = 0; u < n; ++u) {
        if (t[u].code || u == m) continue;
        bool owned = false;
        for (int q = 0; q < n; ++q) owned = owned || beside(q, u) >= 0.f;
        if (owned) continue;
        float d = -1.f;
        if (row(t[u], t[m])) {
          const bool right = t[m].x0 >= t[c].x1 - 1.f;
          const float g = right ? t[u].x0 - t[m].x1 : t[m].x0 - t[u].x1;
          if (g >= -1.f && g <= 3.f * t[m].h && !between(m, u, 2)) d = g;
        } else {
          const float dx = std::fabs(0.5f * (t[u].x0 + t[u].x1) - 0.5f * (t[m].x0 + t[m].x1));
          const float dy = std::fabs(t[u].cy - t[m].cy);
          if (dx <= t[m].h && dy <= 2.5f * t[m].h) d = dy;
        }
        if (d >= 0.f && d < bd) bd = d, best = u;
      }
      if (best >= 0) sc[1 - known] = t[best].value;
    }
    // More than one goal off the score on show: a crest or logo by a code
    // may have been read as a digit and paired. Each code's candidates are
    // the numbers on its row with no code between; the pair within one goal
    // of the score on show wins, no change first, then fewest skipped. Side
    // by side, the home number stays on the home code's side of the away one.
    if (cur_h >= 0 && cur_a >= 0 && score_step(cur_h, cur_a, sc[0], sc[1]) == 3) {
      int ci[2] = {at[0], at[1]};
      for (int k = 0; k < n; ++k)
        if (side[k] >= 0 && ci[side[k]] < 0) ci[side[k]] = k;
      if (ci[0] >= 0 && ci[1] >= 0) {
        auto reach = [&](int c, int u) {
          if (t[u].code || !row(t[c], t[u]) || between(c, u, 0)) return -1;
          const float g = t[u].x0 >= t[c].x1 - 1.f ? t[u].x0 - t[c].x1 : t[c].x0 - t[u].x1;
          if (g > 8.f * t[c].h) return -1;
          int skip = 0;  // numbers between the code and u
          const float l = t[c].x1 < t[u].x1 ? t[c].x1 : t[u].x1, r = t[c].x0 > t[u].x0 ? t[c].x0 : t[u].x0;
          for (int k = 0; k < n; ++k)
            skip += k != u && !t[k].code && row(t[k], t[c]) && t[k].x0 >= l - 1.f && t[k].x1 <= r + 1.f;
          return skip;
        };
        const bool one_row = row(t[ci[0]], t[ci[1]]);
        const bool home_left = t[ci[0]].x0 < t[ci[1]].x0;
        int best_step = 3, best_skip = 1 << 20, b0 = -1, b1 = -1;
        for (int u0 = 0; u0 < n; ++u0) {
          const int s0 = reach(ci[0], u0);
          if (s0 < 0) continue;
          for (int u1 = 0; u1 < n; ++u1) {
            const int s1 = u1 == u0 ? -1 : reach(ci[1], u1);
            if (s1 < 0) continue;
            if (one_row && (t[u0].x0 < t[u1].x0) != home_left) continue;
            const int st = score_step(cur_h, cur_a, t[u0].value, t[u1].value);
            if (st < best_step || (st == best_step && s0 + s1 < best_skip))
              best_step = st, best_skip = s0 + s1, b0 = u0, b1 = u1;
          }
        }
        if (b0 >= 0 && best_step <= 2) sc[0] = t[b0].value, sc[1] = t[b1].value;
      }
    }
    std::snprintf(out.home, sizeof(out.home), "%s", hint_home);
    std::snprintf(out.away, sizeof(out.away), "%s", hint_away);
    out.home_score = sc[0];
    out.away_score = sc[1];
    return true;
  }
  // Unknown teams: two scored codes on one row with the scores between them,
  // or on adjacent rows, left aligned. Reading order: the first pair wins.
  for (int a = 0; a < n; ++a) {
    if (side[a] < 0 || score_of[a] < 0) continue;
    for (int b = 0; b < n; ++b) {
      if (b == a || side[b] < 0 || score_of[b] < 0 || std::strcmp(t[a].s, t[b].s) == 0) continue;
      const Tok &A = t[a], &B = t[b], &SA = t[score_of[a]], &SB = t[score_of[b]];
      bool ok = false;
      if (row(A, B)) {
        ok = A.x1 <= SA.x0 + 1.f && SA.x1 <= SB.x0 + 1.f && SB.x1 <= B.x0 + 1.f;  // A sa - sb B
      } else {
        const float dy = B.cy - A.cy;
        ok = dy > 0.f && dy <= 3.f * A.h && std::fabs(A.x0 - B.x0) <= 1.5f * A.h &&
             std::fabs(0.5f * (SA.x0 + SA.x1) - 0.5f * (SB.x0 + SB.x1)) <= 1.5f * A.h;
      }
      if (!ok) continue;
      std::snprintf(out.home, sizeof(out.home), "%s", A.s);
      std::snprintf(out.away, sizeof(out.away), "%s", B.s);
      out.home_score = SA.value;
      out.away_score = SB.value;
      return true;
    }
  }
  return false;
}

// How well a bug's code fits a team name, for codes the provider spells
// differently: the code's letters must appear in order, starting with the
// name's first letter. One point per letter that starts a word or continues
// the name's opening letters ("MCI": Manchester City 2, Manchester United 1).
// -1: no fit. Case-insensitive.
inline int code_name_score(const char* code, const char* name) noexcept {
  auto up = [](char c) { return detail::is_lower(c) ? static_cast<char>(c - 32) : c; };
  const int nc = static_cast<int>(std::strlen(code));
  const int full = static_cast<int>(std::strlen(name));
  const int nn = full < 63 ? full : 63;
  if (nc < 2 || nc > 4 || nn < nc || up(code[0]) != up(name[0])) return -1;
  // best[i][j]: code[0..i] matched with code[i] on name[j].
  int best[4][64];
  for (int i = 0; i < nc; ++i)
    for (int j = 0; j < nn; ++j) {
      best[i][j] = -1;
      if (up(code[i]) != up(name[j])) continue;
      if (i == 0) {
        if (j == 0) best[i][j] = 1;
        continue;
      }
      const bool word = !detail::alnum(name[j - 1]);
      for (int q = i - 1; q < j; ++q) {
        if (best[i - 1][q] < 0) continue;
        const int v = best[i - 1][q] + (word || (j == i && q == i - 1) ? 1 : 0);
        if (v > best[i][j]) best[i][j] = v;
      }
    }
  int top = -1;
  for (int j = 0; j < nn; ++j) top = best[nc - 1][j] > top ? best[nc - 1][j] : top;
  return top;
}

// A changed score counts once it is read the same several times in a row.
// Goals come one at a time: one more for one side takes 2 reads (so goals in
// quick succession each count), one fewer (ruled out) 3. Any other change (a
// jump like 2-1 to 16-1, a logo read as a digit; both sides at once) takes
// `jump` reads at the normal cadence, with no quick re-read. The first score
// read takes 2, or `jump` past 9 goals. A read of the score on show in
// between starts the count again.
class ScoreVote {
 public:
  // cur: the score on show (-1 unknown). True when h, a should replace it.
  bool add(int cur_h, int cur_a, int h, int a, int jump = 6) noexcept {
    if (h == cur_h && a == cur_a) {
      run_ = 0;
      return false;
    }
    if (h == h_ && a == a_ && run_ > 0) ++run_;
    else h_ = h, a_ = a, run_ = 1;
    if (jump < 3) jump = 3;
    const int st = score_step(cur_h, cur_a, h, a);
    if (cur_h < 0 || cur_a < 0) need_ = h > 9 || a > 9 ? jump : 2;
    else need_ = st == 1 ? 2 : st == 2 ? 3 : jump;
    if (run_ < need_) return false;
    run_ = 0;
    return true;
  }
  // A small change waiting on a quick re-read.
  bool pending() const noexcept { return run_ > 0 && need_ <= 3; }

 private:
  int h_ = -1, a_ = -1, run_ = 0, need_ = 2;
};

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
// cluster a quarter wider than tall wins (a stacked bug is about that shape; a
// channel logo is smaller and squarer). Fixed arrays, no
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
        if (count > best && 4 * (x1 - x0 + 1) >= 5 * (y1 - y0 + 1) && count >= 4) {
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
