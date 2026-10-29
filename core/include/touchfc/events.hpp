#pragma once

#include "touchfc/types.hpp"

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace touchfc {

// Expected threat (Karun Singh, "Introducing Expected Threat", 2018), the
// published 12x8 grid open_xt_12x8_v1.json: rows across the pitch, columns
// along it, attacking toward column 11.
inline constexpr float kXt[8][12] = {
    {0.00638303f, 0.00779616f, 0.00844854f, 0.00977659f, 0.0112627f, 0.0124834f, 0.014736f, 0.0174506f, 0.0212213f, 0.0275631f, 0.0348507f, 0.0379259f},
    {0.00750072f, 0.00878589f, 0.00942382f, 0.0105949f, 0.0121472f, 0.0138454f, 0.0161181f, 0.0187035f, 0.0240152f, 0.0295327f, 0.0406699f, 0.0464772f},
    {0.0088799f, 0.00977745f, 0.010013f, 0.0111046f, 0.0126917f, 0.0142913f, 0.016856f, 0.0193513f, 0.0241224f, 0.028552f, 0.0549114f, 0.0644259f},
    {0.00941056f, 0.0108272f, 0.0101655f, 0.0113238f, 0.0126265f, 0.014846f, 0.0168953f, 0.0199707f, 0.0238515f, 0.0351133f, 0.108051f, 0.257454f},
    {0.00941056f, 0.0108272f, 0.0101655f, 0.0113238f, 0.0126265f, 0.014846f, 0.0168953f, 0.0199707f, 0.0238515f, 0.0351133f, 0.108051f, 0.257454f},
    {0.0088799f, 0.00977745f, 0.010013f, 0.0111046f, 0.0126917f, 0.0142913f, 0.016856f, 0.0193513f, 0.0241224f, 0.028552f, 0.0549114f, 0.0644259f},
    {0.00750072f, 0.00878589f, 0.00942382f, 0.0105949f, 0.0121472f, 0.0138454f, 0.0161181f, 0.0187035f, 0.0240152f, 0.0295327f, 0.0406699f, 0.0464772f},
    {0.00638303f, 0.00779616f, 0.00844854f, 0.00977659f, 0.0112627f, 0.0124834f, 0.014736f, 0.0174506f, 0.0212213f, 0.0275631f, 0.0348507f, 0.0379259f},
};

// xT at a pitch point for a team attacking toward +x (dir = 1) or -x (dir = -1).
inline float xt_at(float x, float y, int dir) noexcept {
  const float ax = dir >= 0 ? x : -x;
  int c = static_cast<int>((ax + 52.5f) / 105.f * 12.f);
  int r = static_cast<int>((y + 34.f) / 68.f * 8.f);
  c = c < 0 ? 0 : c > 11 ? 11 : c;
  r = r < 0 ? 0 : r > 7 ? 7 : r;
  return kXt[r][c];
}

struct EventParams {
  float pass_min_dist_m = 5.f;
  float ball_flight_min_mps = 6.f;
  float shot_speed_mps = 8.f;
  float shot_zone_m = 35.f;
  float shot_end_m = 9.f;     // a loose ball this close to the goal line ends a shot
  float shot_blind_m = 22.f;  // release this close, then unseen until a dead ball: a shot (0 off)
  float key_pass_window_s = 5.f;
  int attack_dir = 1;  // team 1 attacks +x when 1; team 2 the other way
  // TFC rating (spec 11.5)
  float rating_pass_k = 3.f;
  float rating_press_mult = 1.25f;
  float rating_press_m = 2.f;
  float rating_pass_floor = 0.02f;
  float rating_loss_k = 3.f;
  float rating_loss_base = 0.05f;
  float rating_int = 0.05f;
  float rating_shot = 0.1f;
  float rating_goal = 1.f;
};

struct TeamStats {
  std::uint32_t passes[2]{};
  std::uint32_t passes_ok[2]{};
  std::uint32_t shots[2]{};
  std::uint32_t key_passes[2]{};
  float poss_s[2]{};  // in-play seconds with that team in possession
  std::uint32_t prog[2]{};        // completed passes 10 m forward, or into the final third
  std::uint32_t recoveries[2]{};  // ball won from the other team (not a shot ending)
  std::uint32_t high_rec[2]{};    // ... in the other team's half
  float xg[2]{};                  // summed shot xG
  float xt_gain[2]{};             // xT added by completed passes
  float final_third_s[2]{};       // possession seconds in the attacking third
  float pass_pct(int t) const noexcept { return passes[t] ? 100.f * passes_ok[t] / passes[t] : 0.f; }
  float poss_pct(int t) const noexcept {
    const float s = poss_s[0] + poss_s[1];
    return s > 0.f ? 100.f * poss_s[t] / s : 50.f;
  }
};

// Team ratings 0..10, 5 when level. Tracks do not survive re-acquisition, so
// ratings are per team. Mostly shares against the other team (detection
// misses hit both alike), with priors so a quiet start reads 5.
struct TeamRatings {
  float attack = 5.f, build = 5.f, defence = 5.f, control = 5.f, overall = 5.f;
};

inline TeamRatings team_ratings(const TeamStats& s, int t) noexcept {
  const int o = 1 - t;
  auto share = [](float a, float b, float k) { return (a + k) / (a + b + 2.f * k); };
  auto f = [](std::uint32_t v) { return static_cast<float>(v); };
  auto comp = [&](int i) {  // completion 55% -> 0, 95% -> 1; prior 75% over 5 passes
    const float c = (f(s.passes_ok[i]) + 3.75f) / (f(s.passes[i]) + 5.f);
    return std::fmin(1.f, std::fmax(0.f, (c - 0.55f) / 0.4f));
  };
  TeamRatings r;
  r.attack = 10.f * (0.45f * share(s.xg[t], s.xg[o], 0.3f) + 0.2f * share(f(s.shots[t]), f(s.shots[o]), 2.f) +
                     0.15f * share(f(s.key_passes[t]), f(s.key_passes[o]), 1.f) +
                     0.2f * share(s.final_third_s[t], s.final_third_s[o], 30.f));
  r.build = 10.f * (0.4f * comp(t) + 0.3f * share(f(s.prog[t]), f(s.prog[o]), 3.f) +
                    0.3f * share(s.xt_gain[t], s.xt_gain[o], 0.1f));
  r.defence = 10.f * (0.35f * share(f(s.recoveries[t]), f(s.recoveries[o]), 3.f) +
                      0.25f * share(f(s.high_rec[t]), f(s.high_rec[o]), 2.f) + 0.2f * (1.f - comp(o)) +
                      0.2f * (1.f - share(s.xg[o], s.xg[t], 0.3f)));
  r.control = 10.f * (0.7f * share(s.poss_s[t], s.poss_s[o], 30.f) +
                      0.3f * share(f(s.passes[t]), f(s.passes[o]), 5.f));
  r.overall = 0.25f * (r.attack + r.build + r.defence + r.control);
  return r;
}

// Shot xG from distance and the angle between the posts (logistic).
inline float shot_xg(float x, float y, float gx) noexcept {
  const float dx = std::fabs(gx - x), d = std::hypot(dx, y);
  const float th = std::atan2(7.32f * dx, dx * dx + y * y - 3.66f * 3.66f);
  return 1.f / (1.f + std::exp(1.1f + 0.11f * d - 1.6f * th));
}

struct TrackStats {
  std::uint16_t track = 0;
  std::uint8_t team = 0;
  std::uint8_t pad = 0;
  std::uint16_t passes = 0;
  std::uint16_t passes_ok = 0;
  std::uint16_t shots = 0;
  std::uint16_t pad2 = 0;
  float rating = 0.f;  // sum of contributions; shown as rating_base + rating
};

static_assert(std::is_trivially_copyable_v<TeamStats>);
static_assert(std::is_trivially_copyable_v<TrackStats>);

// One frame of input, on T3, only while in play.
struct EventFrame {
  float t_s = 0.f;
  bool ball = false;          // ball seen and on the pitch
  float bx = 0.f, by = 0.f;   // ball, metres
  std::uint16_t holder = 0;   // track within poss_dist_m of the ball, 0 none
  std::uint8_t team = 0;      // holder's team (1 or 2)
  float opp_dist_m = 99.f;    // nearest opponent to the holder
  float nearest_m = 99.f;     // nearest player of either team to the ball
  float poss_dist_m = 1.5f;
};

// Pass, interception, shot and key pass from possession spells and ball flight.
// A fast ball toward a goal mouth is only a shot candidate until the flight
// ends: a team-mate receiving it makes it a pass (a cross, a through ball);
// an opponent or keeper, a dead ball or a long loose ball makes it a shot.
// Fixed-size, no allocation. step() returns up to 4 events for the log.
class EventDetector {
 public:
  static constexpr int kTracks = 96;

  const TeamStats& team_stats() const noexcept { return stats_; }
  const TrackStats* track(std::uint16_t id) const noexcept {
    for (const auto& t : tracks_)
      if (t.track == id) return &t;
    return nullptr;
  }

  // Dead ball: an open release ends. A kicked ball that goes dead is an
  // incomplete pass (out of play).
  int stop(float t_s, Event* out, const EventParams& p) noexcept {
    int n = 0;
    if (free_ && (shot_cand_ || shot_end(t_s, p) || shot_blind(t_s, p))) n += emit_shot(t_s, out + n, p);
    else if (free_ && flew_ && moved() >= p.pass_min_dist_m) n += emit_pass(t_s, false, 0, 0, false, out + n, p);
    holder_ = 0;
    free_ = false;
    hist_n_ = 0;
    return n;
  }

  // A goal on the scorebug credits the last shot within 15 s.
  void goal(float t_s, const EventParams& p) noexcept {
    if (last_shot_track_ && t_s - last_shot_t_ < 15.f) {
      rate(last_shot_track_, last_shot_team_, p.rating_goal);
      last_shot_track_ = 0;
    }
  }

  int step(const EventFrame& in, float dt, Event* out, const EventParams& p) noexcept {
    int n = 0;
    // The ball detector misses frames (blur, occlusion at the feet): keep the
    // holder until the ball is seen again, for up to kHoldMissS.
    EventFrame f = in;
    if (dt > 0.f && dt < 1.f) frame_dt_ += 0.1f * (dt - frame_dt_);
    // Low frame rates: a miss of 2.5 frames is still one touch.
    const float hold_s = std::fmax(kHoldMissS, 2.5f * frame_dt_);
    if (f.ball) {
      push_ball(f.t_s, f.bx, f.by);
      seen_t_ = f.t_s;
    } else if (holder_ && f.t_s - seen_t_ < hold_s) {
      f.holder = holder_;
      f.team = holder_team_;
    }
    const float speed = ball_speed();
    if (f.holder && (f.team == 1 || f.team == 2) && dt > 0.f && dt < 0.5f) {
      stats_.poss_s[f.team - 1] += dt;
      if (f.ball && f.bx * static_cast<float>(dir_of(f.team, p)) > 17.5f) stats_.final_third_s[f.team - 1] += dt;
    }

    // A release: the holder lost the ball.
    if (holder_ && f.holder != holder_) {
      if (!free_) {
        free_ = true;
        flew_ = false;
        shot_cand_ = false;
        rel_track_ = holder_;
        rel_team_ = holder_team_;
        rel_x_ = last_x_;
        rel_y_ = last_y_;
        cur_x_ = last_x_;  // until the ball is seen again
        cur_y_ = last_y_;
        rel_t_ = f.t_s;
        rel_opp_ = holder_opp_;
      }
      holder_ = 0;
    }
    if (free_) {
      if (f.ball) {
        cur_x_ = f.bx;
        cur_y_ = f.by;
      }
      if (speed > p.ball_flight_min_mps && f.nearest_m > f.poss_dist_m) flew_ = true;
      if (!shot_cand_ && flew_ && speed > p.shot_speed_mps) shot_cand_ = shot_heading(f.t_s, p);
      if (f.t_s - rel_t_ > 6.f) {  // loose for too long: no pass to credit
        if (shot_cand_ || shot_end(f.t_s, p)) n += emit_shot(f.t_s, out + n, p);
        free_ = false;
      }
    }
    // A new spell starts.
    if (f.holder && f.holder != holder_) {
      // A fast ball is often not detected in flight, so the distance between
      // release and reception is enough.
      if (free_ && f.holder != rel_track_ && f.ball) {
        cur_x_ = f.bx;
        cur_y_ = f.by;
        const bool same = f.team == rel_team_;
        if (!same && (shot_cand_ || shot_end(f.t_s, p))) {
          n += emit_shot(f.t_s, out + n, p);
        } else {
          if (moved() >= p.pass_min_dist_m)
            n += emit_pass(f.t_s, same, f.holder, f.team, !same && speed > p.ball_flight_min_mps, out + n, p);
          if (!same && (f.team == 1 || f.team == 2)) {
            ++stats_.recoveries[f.team - 1];
            if (f.bx * static_cast<float>(dir_of(f.team, p)) > 0.f) ++stats_.high_rec[f.team - 1];
          }
        }
      }
      free_ = false;
      holder_ = f.holder;
      holder_team_ = f.team;
    }
    if (holder_ && f.ball) {
      last_x_ = f.bx;
      last_y_ = f.by;
      holder_opp_ = f.opp_dist_m;
    }
    return n;
  }

 private:
  float moved() const noexcept { return std::hypot(cur_x_ - rel_x_, cur_y_ - rel_y_); }
  int dir_of(std::uint8_t team, const EventParams& p) const noexcept { return team == 2 ? -p.attack_dir : p.attack_dir; }

  void push_ball(float t, float x, float y) noexcept {
    hist_[hist_i_] = {t, x, y};
    hist_i_ = (hist_i_ + 1) % 5;
    if (hist_n_ < 5) ++hist_n_;
  }
  // Smoothed over the last 3 to 5 samples.
  float ball_speed() const noexcept {
    if (hist_n_ < 3) return 0.f;
    const Sample& a = hist_[(hist_i_ + 5 - hist_n_) % 5];
    const Sample& b = hist_[(hist_i_ + 4) % 5];
    const float dt = b.t - a.t;
    return dt > 1e-3f ? std::hypot(b.x - a.x, b.y - a.y) / dt : 0.f;
  }
  bool ball_vel(float& vx, float& vy) const noexcept {
    if (hist_n_ < 3) return false;
    const Sample& a = hist_[(hist_i_ + 5 - hist_n_) % 5];
    const Sample& b = hist_[(hist_i_ + 4) % 5];
    const float dt = b.t - a.t;
    if (dt <= 1e-3f) return false;
    vx = (b.x - a.x) / dt;
    vy = (b.y - a.y) / dt;
    return true;
  }

  TrackStats* slot(std::uint16_t id, std::uint8_t team) noexcept {
    TrackStats* empty = nullptr;
    for (auto& t : tracks_) {
      if (t.track == id) return &t;
      if (!empty && t.track == 0) empty = &t;
    }
    if (!empty) return nullptr;
    *empty = {};
    empty->track = id;
    empty->team = team;
    return empty;
  }
  void rate(std::uint16_t id, std::uint8_t team, float v) noexcept {
    if (TrackStats* t = slot(id, team)) t->rating += v;
  }

  static bool in_box(float x, float y, int dir) noexcept {
    const float ax = dir >= 0 ? x : -x;
    return ax > 36.f && ax <= 52.5f && std::fabs(y) < 20.16f;
  }

  int emit_pass(float t, bool ok, std::uint16_t recv, std::uint8_t recv_team, bool intercepted, Event* out,
                const EventParams& p) noexcept {
    const int team_i = rel_team_ - 1;
    if (team_i < 0 || team_i > 1) return 0;
    const int dir = dir_of(rel_team_, p);
    ++stats_.passes[team_i];
    TrackStats* ts = slot(rel_track_, rel_team_);
    if (ts) ++ts->passes;
    Event& e = out[0];
    e = {};
    e.type = EventType::Pass;
    e.result = ok ? EventResult::Success : EventResult::Fail;
    e.team = rel_team_;
    e.track = rel_track_;
    e.track_recv = recv;
    e.sx = rel_x_;
    e.sy = rel_y_;
    e.ex = cur_x_;
    e.ey = cur_y_;
    e.match_ms_start = static_cast<std::int32_t>(rel_t_ * 1000.f);
    e.match_ms_end = static_cast<std::int32_t>(t * 1000.f);
    if (ok) {
      ++stats_.passes_ok[team_i];
      if (ts) ++ts->passes_ok;
      const float gain = xt_at(cur_x_, cur_y_, dir) - xt_at(rel_x_, rel_y_, dir);
      if (gain > 0.f) stats_.xt_gain[team_i] += gain;
      const float fx = static_cast<float>(dir);
      if (fx * (cur_x_ - rel_x_) >= 10.f || (fx * cur_x_ > 17.5f && fx * rel_x_ <= 17.5f)) ++stats_.prog[team_i];
      float v = p.rating_pass_k * gain;
      if (rel_opp_ <= p.rating_press_m) v *= p.rating_press_mult;
      rate(rel_track_, rel_team_, v < -p.rating_pass_floor ? -p.rating_pass_floor : v);
      last_ok_t_ = t;
      last_ok_team_ = rel_team_;
      last_ok_counted_ = false;
      if (in_box(cur_x_, cur_y_, dir)) {
        ++stats_.key_passes[team_i];
        last_ok_counted_ = true;
      }
    } else {
      rate(rel_track_, rel_team_, -(p.rating_loss_k * xt_at(rel_x_, rel_y_, dir) + p.rating_loss_base));
    }
    if (!intercepted) return 1;
    Event& i = out[1];
    i = {};
    i.type = EventType::Interception;
    i.result = EventResult::Success;
    i.team = recv_team;
    i.track = recv;
    i.sx = i.ex = cur_x_;
    i.sy = i.ey = cur_y_;
    i.match_ms_start = i.match_ms_end = e.match_ms_end;
    rate(recv, recv_team, p.rating_int);
    return 2;
  }

  // Ball heading for a goal mouth (posts at +-3.66 m, 1 m margin), released
  // within shot_zone_m of that goal.
  bool shot_heading(float t, const EventParams& p) noexcept {
    float vx, vy;
    if (!ball_vel(vx, vy) || std::fabs(vx) < 1e-3f) return false;
    const float gx = vx > 0.f ? 52.5f : -52.5f;
    const float k = (gx - cur_x_) / vx;
    if (k <= 0.f) return false;
    const float y_at = cur_y_ + k * vy;
    if (std::fabs(y_at) > 3.66f + kMouthMarginM) return false;
    if (std::hypot(gx - rel_x_, rel_y_) > p.shot_zone_m) return false;
    shot_gx_ = gx;
    shot_y_ = y_at;
    shot_t_ = t;
    return true;
  }

  // The flight toward goal was not seen (blur, a lofted ball projects wrong):
  // a ball released inside shot_zone_m that ends near the goal mouth, or over
  // the goal line, without a team-mate receiving it, is a shot. Releases from
  // the wings are left out (crosses).
  bool shot_end(float t, const EventParams& p) noexcept {
    const float gx = cur_x_ > 0.f ? 52.5f : -52.5f;
    const float to_goal = std::fabs(gx - cur_x_);
    if (std::fabs(rel_y_) > 24.f || std::hypot(gx - rel_x_, rel_y_) > p.shot_zone_m) return false;
    if (std::fabs(cur_x_) < 52.5f - p.shot_end_m || std::fabs(cur_y_) > 3.66f + p.shot_end_m) return false;
    if (std::fabs(gx - rel_x_) - to_goal < 3.f) return false;  // must travel toward goal
    shot_gx_ = gx;
    shot_y_ = std::fabs(cur_y_) < 3.66f ? cur_y_ : (cur_y_ > 0.f ? 3.66f : -3.66f);
    shot_t_ = t;
    return true;
  }

  // Low frame rates often lose a struck ball at once: a release from a
  // central spot within shot_blind_m of the goal it attacks, the ball not seen
  // to move again, and a dead ball within 4 s.
  bool shot_blind(float t, const EventParams& p) noexcept {
    if (p.shot_blind_m <= 0.f || moved() > 3.f || t - rel_t_ > 4.f || std::fabs(rel_y_) > 20.f) return false;
    const float gx = dir_of(rel_team_, p) >= 0 ? 52.5f : -52.5f;
    if (std::hypot(gx - rel_x_, rel_y_) > p.shot_blind_m) return false;
    shot_gx_ = gx;
    shot_y_ = 0.f;
    shot_t_ = rel_t_;
    return true;
  }

  int emit_shot(float t, Event* out, const EventParams& p) noexcept {
    shot_cand_ = false;
    const int team_i = rel_team_ - 1;
    if (team_i < 0 || team_i > 1) return 0;
    t = shot_t_;
    const float gx = shot_gx_, y_at = shot_y_;
    ++stats_.shots[team_i];
    stats_.xg[team_i] += shot_xg(rel_x_, rel_y_, gx);
    if (TrackStats* ts = slot(rel_track_, rel_team_)) ++ts->shots;
    rate(rel_track_, rel_team_, p.rating_shot);
    last_shot_track_ = rel_track_;
    last_shot_team_ = rel_team_;
    last_shot_t_ = t;
    // Key pass: a completed pass by the same team within the window.
    if (last_ok_team_ == rel_team_ && t - last_ok_t_ <= p.key_pass_window_s && !last_ok_counted_) {
      ++stats_.key_passes[team_i];
      last_ok_counted_ = true;
    }
    Event& e = out[0];
    e = {};
    e.type = EventType::Shot;
    e.result = EventResult::Unknown;
    e.team = rel_team_;
    e.track = rel_track_;
    e.sx = rel_x_;
    e.sy = rel_y_;
    e.ex = gx;
    e.ey = y_at;
    e.match_ms_start = static_cast<std::int32_t>(rel_t_ * 1000.f);
    e.match_ms_end = static_cast<std::int32_t>(t * 1000.f);
    return 1;
  }

  static constexpr float kHoldMissS = 0.4f;
  static constexpr float kMouthMarginM = 2.5f;
  struct Sample {
    float t, x, y;
  };
  Sample hist_[5]{};
  int hist_i_ = 0, hist_n_ = 0;
  float seen_t_ = -1e9f;
  float frame_dt_ = 0.04f;
  std::uint16_t holder_ = 0;
  std::uint8_t holder_team_ = 0;
  float holder_opp_ = 99.f;
  float last_x_ = 0.f, last_y_ = 0.f;
  bool free_ = false, flew_ = false, shot_cand_ = false;
  float shot_gx_ = 0.f, shot_y_ = 0.f, shot_t_ = 0.f;
  std::uint16_t rel_track_ = 0;
  std::uint8_t rel_team_ = 0;
  float rel_x_ = 0.f, rel_y_ = 0.f, rel_t_ = 0.f, rel_opp_ = 99.f;
  float cur_x_ = 0.f, cur_y_ = 0.f;
  float last_ok_t_ = -1e9f;
  std::uint8_t last_ok_team_ = 0;
  bool last_ok_counted_ = false;
  std::uint16_t last_shot_track_ = 0;
  std::uint8_t last_shot_team_ = 0;
  float last_shot_t_ = -1e9f;
  TeamStats stats_{};
  TrackStats tracks_[kTracks]{};
};

}  // namespace touchfc
