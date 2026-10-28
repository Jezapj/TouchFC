#pragma once

#include "touchfc/pitch.hpp"
#include "touchfc/types.hpp"

#include <cstdio>

namespace touchfc {

// One closed possession spell. Pass and shot are not produced: there is no
// labeled set to quote a recall for them. type_id 21 (possession) and 22
// (dead ball) are outside the SPADL action list on purpose.
struct Possession {
  bool held = false;
  int miss = 0;
  std::uint8_t team = 0;
  std::uint16_t track = 0;
  float sx = 0.f;
  float sy = 0.f;
  std::int64_t start_ns = 0;
};

struct PossOut {
  bool emitted = false;
  Event event{};
};

inline PossOut step_possession(Possession& s, bool have_ball, float bx, float by, std::uint16_t track,
                               std::uint8_t team, float dist_m, float poss_dist_m, std::int64_t now_ns,
                               std::int64_t t0_ns) noexcept {
  PossOut out;
  const bool close = have_ball && track != 0 && team != 0 && dist_m <= poss_dist_m;
  if (s.held && close && track == s.track) {
    s.miss = 0;
    return out;
  }
  if (s.held && !close) {
    if (++s.miss < 5) return out;
  }
  if (s.held && (!close || track != s.track)) {
    out.emitted = true;
    out.event.type = EventType::PossessionEnd;
    out.event.team = s.team;
    out.event.track = s.track;
    out.event.sx = s.sx;
    out.event.sy = s.sy;
    out.event.ex = bx;
    out.event.ey = by;
    out.event.match_ms_start = static_cast<std::int32_t>((s.start_ns - t0_ns) / 1000000LL);
    out.event.match_ms_end = static_cast<std::int32_t>((now_ns - t0_ns) / 1000000LL);
    s = {};
  }
  if (!s.held && close) {
    s.held = true;
    s.team = team;
    s.track = track;
    s.sx = bx;
    s.sy = by;
    s.start_ns = now_ns;
    s.miss = 0;
  }
  return out;
}

inline constexpr const char* kSpadlHeader =
    "game_id,original_event_id,period_id,time_seconds,team_id,player_id,start_x,start_y,end_x,end_y,"
    "type_id,result_id,bodypart_id\n";

// SPADL type_id: pass 0, interception 10, shot 11. Outside the SPADL list:
// possession spell 21, dead ball 22, scorebug goal 23.
inline int spadl_type(const Event& e) noexcept {
  switch (e.type) {
    case EventType::Pass: return 0;
    case EventType::Interception: return 10;
    case EventType::Shot: return 11;
    case EventType::DeadBall: return 22;
    case EventType::Goal: return 23;
    default: return 21;
  }
}

// result_id 1 success, 0 fail. A shot succeeds only as a goal.
inline int spadl_result(const Event& e) noexcept {
  if (e.type == EventType::Pass || e.type == EventType::Shot)
    return e.result == EventResult::Success || e.result == EventResult::Goal ? 1 : 0;
  return 1;
}

inline void format_spadl_row(char* buf, int n, const Event& e, int attack_dir) noexcept {
  const int dir = e.team == 2 ? -attack_dir : attack_dir;
  const PitchPoint s = internal_to_spadl({e.sx, e.sy}, dir);
  const PitchPoint t = internal_to_spadl({e.ex, e.ey}, dir);
  std::snprintf(buf, static_cast<std::size_t>(n),
                "touchfc,%llu,1,%.3f,%u,%u,%.2f,%.2f,%.2f,%.2f,%d,%d,0\n",
                static_cast<unsigned long long>(e.id), e.match_ms_end / 1000.0,
                static_cast<unsigned>(e.team), static_cast<unsigned>(e.track), s.x, s.y, t.x, t.y,
                spadl_type(e), spadl_result(e));
}

}  // namespace touchfc
