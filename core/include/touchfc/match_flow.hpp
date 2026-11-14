#pragma once

#include "touchfc/events.hpp"
#include "touchfc/match_info.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

namespace touchfc {

// M18: match flow. Momentum per match minute, defensive line heights, the
// half-time change of ends, pop-up news for present, the lineup graphic's
// layout and scorer names from OCR text. Fixed-size, no allocation: the
// trackers run on T3.

// T3 -> present, for the pop-ups.
enum class FeedKind : std::uint8_t { Shot = 0, KeyPass, Turnover, Goal, SecondHalf };

struct FeedItem {
  FeedKind kind = FeedKind::Shot;
  std::uint8_t team = 0;  // 1 A, 2 B: the team that shot, passed, won the ball or scored
  std::uint8_t pad[2]{};
  float xg = 0.f;         // shots
};

inline constexpr int kFlowBins = 130;  // one per match minute
inline constexpr float kFlowShotBase = 1.f;
inline constexpr float kFlowShotXg = 10.f;
inline constexpr float kFlowKeyPass = 0.5f;

// Back line height is metres from the team's own goal line.
struct LineStats {
  float sum_m[2]{};    // height x seconds
  float seen_s[2]{};   // seconds the line was in view
  float high_s[2]{};   // ... and at or past the high-line mark
  float cur_m[2] = {-1.f, -1.f};  // smoothed; -1 not seen yet
  float box_s[2]{};    // seconds with two or more players in the other box
  float play_s = 0.f;  // in-play seconds with a valid pitch

  float avg_m(int t) const noexcept { return seen_s[t] > 0.f ? sum_m[t] / seen_s[t] : -1.f; }
  float high_pct(int t) const noexcept { return seen_s[t] > 0.f ? 100.f * high_s[t] / seen_s[t] : 0.f; }
  float box_pct(int t) const noexcept { return play_s > 0.f ? 100.f * box_s[t] / play_s : 0.f; }
  void swap() noexcept {
    std::swap(sum_m[0], sum_m[1]);
    std::swap(seen_s[0], seen_s[1]);
    std::swap(high_s[0], high_s[1]);
    std::swap(cur_m[0], cur_m[1]);
    std::swap(box_s[0], box_s[1]);
  }
};

struct FlowSnapshot {
  float threat[2][kFlowBins]{};  // attacking threat per team per minute
  LineStats line{};
  std::int16_t cur_bin = 0;
  std::int16_t max_bin = -1;    // last minute with data
  std::int16_t half2_bin = -1;  // minute the second half started
  std::uint8_t goal_n = 0;
  std::uint8_t pad = 0;
  struct Mark {
    std::uint8_t bin = 0;
    std::uint8_t team = 0;  // 1 or 2
  } goals[16]{};
};

static_assert(std::is_trivially_copyable_v<FlowSnapshot>);
static_assert(std::is_trivially_copyable_v<FeedItem>);

// Chart value for a minute: team A's threat minus team B's, smoothed 1-2-1 so
// one shot does not make a spike one bin wide. Positive: A more threatening.
inline float flow_value(const FlowSnapshot& s, int bin) noexcept {
  auto d = [&](int b) { return b < 0 || b >= kFlowBins ? 0.f : s.threat[0][b] - s.threat[1][b]; };
  return 0.25f * d(bin - 1) + 0.5f * d(bin) + 0.25f * d(bin + 1);
}

struct LinePlayer {
  float x = 0.f, y = 0.f;
  std::uint8_t team = 0;    // 1 or 2
  std::uint8_t keeper = 0;  // the detector's goalkeeper class
};

// Height of one team's back line. dir: +1 when that team attacks +x.
// view_x0..view_x1 is the pitch x range in the picture: the broadcast shows
// about a third of the pitch, so the deepest players on screen are the back
// line only when the picture reaches behind them (or to the goal line).
inline bool def_line_m(const LinePlayer* p, int n, int team, int dir, float view_x0, float view_x1,
                       float& out) noexcept {
  float ax[32];
  int m = 0;
  for (int i = 0; i < n && m < 32; ++i)
    if (p[i].team == team && !p[i].keeper) ax[m++] = dir >= 0 ? p[i].x : -p[i].x;
  if (m < 3) return false;
  std::sort(ax, ax + m);
  int first = 0;
  // A keeper the detector called a player: alone near the goal line.
  if (ax[0] < -52.5f + 12.f && ax[1] - ax[0] > 8.f) first = 1;
  if (m - first < 3) return false;
  const float edge = dir >= 0 ? view_x0 : -view_x1;  // own-goal side of the picture
  if (edge > -52.5f + 2.f && ax[first] < edge + 4.f) return false;
  const int k = m - first >= 5 ? 3 : 2;
  float sum = 0.f;
  for (int i = 0; i < k; ++i) sum += ax[first + i];
  out = std::clamp(sum / static_cast<float>(k) + 52.5f, 0.f, 105.f);
  return true;
}

// Accumulates the flow on T3. Bins are match minutes (the caller picks the
// scorebug clock or the play clock).
class MatchFlow {
 public:
  const FlowSnapshot& snapshot() const noexcept { return s_; }

  void set_bin(int bin) noexcept { s_.cur_bin = static_cast<std::int16_t>(clamp_bin(bin)); }

  // A frame in play with a holder: possession weighted by how dangerous the
  // ball's position is for that team (xT), so a spell in the box counts for
  // far more than one in the team's own half.
  void possession(float dt, std::uint8_t team, float bx, float by, int attack_dir) noexcept {
    if ((team != 1 && team != 2) || dt <= 0.f || dt > 0.5f) return;
    add(team, xt_at(bx, by, team == 2 ? -attack_dir : attack_dir) * dt);
  }
  void shot(std::uint8_t team, float xg) noexcept { add(team, kFlowShotBase + kFlowShotXg * xg); }
  void key_pass(std::uint8_t team) noexcept { add(team, kFlowKeyPass); }
  void goal(std::uint8_t team) noexcept {
    if ((team != 1 && team != 2) || s_.goal_n >= 16) return;
    s_.goals[s_.goal_n++] = {static_cast<std::uint8_t>(s_.cur_bin), team};
  }
  void second_half() noexcept { s_.half2_bin = s_.cur_bin; }

  // attack_dir: team A's. high_m: a line this far from its goal is high.
  void line(float dt, const LinePlayer* p, int n, int attack_dir, float view_x0, float view_x1,
            float high_m) noexcept {
    if (dt <= 0.f || dt > 0.5f) return;
    LineStats& l = s_.line;
    l.play_s += dt;
    for (int t = 0; t < 2; ++t) {
      const int dir = t == 0 ? attack_dir : -attack_dir;
      float h = 0.f;
      if (def_line_m(p, n, t + 1, dir, view_x0, view_x1, h)) {
        l.sum_m[t] += h * dt;
        l.seen_s[t] += dt;
        if (h >= high_m) l.high_s[t] += dt;
        l.cur_m[t] = l.cur_m[t] < 0.f ? h : l.cur_m[t] + std::fmin(1.f, dt / 1.5f) * (h - l.cur_m[t]);
      }
      int in_box = 0;
      for (int i = 0; i < n; ++i) {
        if (p[i].team != t + 1 || p[i].keeper) continue;
        const float ax = dir >= 0 ? p[i].x : -p[i].x;
        if (ax > 36.f && ax <= 52.5f && std::fabs(p[i].y) < 20.16f) ++in_box;
      }
      if (in_box >= 2) l.box_s[t] += dt;
    }
  }

  void swap_teams() noexcept {
    for (int b = 0; b < kFlowBins; ++b) std::swap(s_.threat[0][b], s_.threat[1][b]);
    for (int i = 0; i < s_.goal_n; ++i) s_.goals[i].team = static_cast<std::uint8_t>(3 - s_.goals[i].team);
    s_.line.swap();
  }

 private:
  static int clamp_bin(int b) noexcept { return b < 0 ? 0 : b >= kFlowBins ? kFlowBins - 1 : b; }
  void add(std::uint8_t team, float v) noexcept {
    if (team != 1 && team != 2) return;
    s_.threat[team - 1][s_.cur_bin] += v;
    if (s_.cur_bin > s_.max_bin) s_.max_bin = s_.cur_bin;
  }
  FlowSnapshot s_{};
};

// The teams change ends at half time. The break is the one long gap in play:
// with a scorebug clock it must come after the 40th minute; without one, after
// 25 minutes of match and twice as long. True once, on the first frame in play
// of the second half.
class HalfTime {
 public:
  bool step(float t_s, bool dead, int clock_ms, float gap_s) noexcept {
    if (done_ || dead) return false;
    bool start = false;
    if (last_live_ >= 0.f && gap_s > 0.f) {
      const float gap = t_s - last_live_;
      if (clock_before_ >= 0) start = gap >= gap_s && clock_before_ >= 40 * 60000 && clock_before_ < 60 * 60000;
      else start = gap >= 2.f * gap_s && last_live_ - first_live_ >= 25.f * 60.f;
    }
    if (first_live_ < 0.f) first_live_ = t_s;
    last_live_ = t_s;
    clock_before_ = clock_ms;
    done_ = start;
    return start;
  }
  bool second_half() const noexcept { return done_; }

 private:
  float first_live_ = -1.f, last_live_ = -1.f;
  int clock_before_ = -1;
  bool done_ = false;
};

namespace detail {
inline char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
// Letters, and the bytes of an accented letter in UTF-8.
inline bool wordch(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (static_cast<unsigned char>(c) & 0x80u) != 0;
}
// needle as a whole word in hay, ASCII case-insensitive.
inline bool find_word(const char* hay, const char* needle, int nlen) noexcept {
  const int hlen = static_cast<int>(std::strlen(hay));
  for (int i = 0; i + nlen <= hlen; ++i) {
    if (i > 0 && wordch(hay[i - 1])) continue;
    if (i + nlen < hlen && wordch(hay[i + nlen])) continue;
    int k = 0;
    while (k < nlen && lower(hay[i + k]) == lower(needle[k])) ++k;
    if (k == nlen) return true;
  }
  return false;
}
}  // namespace detail

// The surname a broadcaster prints: the last word of the name.
inline const char* surname(const char* name) noexcept {
  const char* s = name;
  for (const char* p = name; *p; ++p)
    if (*p == ' ' && p[1]) s = p + 1;
  return s;
}

// A squad surname in OCR text (the goal caption beside the scorebug). The
// longest match wins; surnames under 3 letters are skipped. side: 0 home.
inline bool find_player(const char* text, const MatchInfo& m, int& side, int& idx) noexcept {
  int best = 0, bs = 0, bi = 0;
  for (int s = 0; s < 2; ++s) {
    const TeamInfo& t = m.team[s];
    for (int i = 0; i < t.n && i < kSquadMax; ++i) {
      const char* sn = surname(t.squad[i].name);
      const int len = static_cast<int>(std::strlen(sn));
      if (len < 3) continue;
      // Among equal surnames, a player on the pitch beats one on the bench.
      const bool better = len > best || (len == best && t.squad[i].starter && !m.team[bs].squad[bi].starter);
      if (!better || !detail::find_word(text, sn, len)) continue;
      best = len;
      bs = s;
      bi = i;
    }
  }
  if (best == 0) return false;
  side = bs;
  idx = bi;
  return true;
}

// Lineup graphic. depth 0 is the keeper, 1 the furthest forward; lat -1 is the
// team's left, +1 its right.
struct XiSpot {
  std::uint8_t idx = 0;  // squad index
  float depth = 0.f;
  float lat = 0.f;
};

// Starters into the rows of the formation ("4-2-3-1"), in the provider's
// formation order, each row left to right from the position code ("CD-L",
// "RB"). No usable formation: 4-3-3 for eleven. Returns the count (up to 11).
inline int layout_xi(const TeamInfo& t, XiSpot out[11]) noexcept {
  std::uint8_t ids[11];
  int n = 0;
  for (int i = 0; i < t.n && i < kSquadMax && n < 11; ++i)
    if (t.squad[i].starter) ids[n++] = static_cast<std::uint8_t>(i);
  if (n == 0) return 0;
  // Stable: players without a place keep the roster order, after the placed.
  std::stable_sort(ids, ids + n, [&](std::uint8_t a, std::uint8_t b) {
    const int pa = t.squad[a].place ? t.squad[a].place : 99, pb = t.squad[b].place ? t.squad[b].place : 99;
    return pa < pb;
  });
  int rows[8] = {1};
  int nr = 1, sum = 1;
  for (const char* c = t.formation; *c && nr < 8; ++c)
    if (*c >= '1' && *c <= '9') {
      rows[nr++] = *c - '0';
      sum += *c - '0';
    }
  if (sum != n) {
    nr = 1;
    if (n == 11) {
      rows[1] = 4;
      rows[2] = 3;
      rows[3] = 3;
      nr = 4;
    } else if (n > 1) {
      rows[1] = n - 1;
      nr = 2;
    }
  }
  // Wide players ("LB", "RF") outside the left and right of a pair ("CD-L").
  auto lat_key = [&](std::uint8_t id) {
    const char* p = t.squad[id].pos;
    const std::size_t len = std::strlen(p);
    if (len >= 2 && p[len - 2] == '-') return p[len - 1] == 'L' ? -1 : p[len - 1] == 'R' ? 1 : 0;
    return p[0] == 'L' ? -2 : p[0] == 'R' ? 2 : 0;
  };
  int k = 0;
  for (int r = 0; r < nr; ++r) {
    std::stable_sort(ids + k, ids + k + rows[r], [&](std::uint8_t a, std::uint8_t b) { return lat_key(a) < lat_key(b); });
    for (int j = 0; j < rows[r]; ++j) {
      out[k].idx = ids[k];
      out[k].depth = nr > 1 ? static_cast<float>(r) / static_cast<float>(nr - 1) : 0.f;
      out[k].lat = (static_cast<float>(j) + 0.5f) / static_cast<float>(rows[r]) * 2.f - 1.f;
      ++k;
    }
  }
  return n;
}

}  // namespace touchfc
