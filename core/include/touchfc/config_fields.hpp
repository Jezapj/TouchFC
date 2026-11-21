#pragma once

#include "touchfc/types.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace touchfc {

// One row per Config field: the Tuning tab draws a slider per row, reset works
// per group, and profiles save by key. Hidden rows are saved but not shown.
enum class CfgGroup : std::uint8_t { Detection, Pitch, Teams, PlayState, Events, Rating, Match, Overlay, Hidden, Count };
enum class CfgType : std::uint8_t { F32, U8, U16, U32 };

inline constexpr const char* kCfgGroupName[] = {"Detection", "Pitch", "Teams",   "Play state", "Events",
                                                "Rating",    "Match", "Overlay", "Hidden"};

struct CfgField {
  const char* key;
  const char* label;
  CfgGroup group;
  CfgType type;
  std::uint16_t offset;
  float lo;
  float hi;
  const char* hint;  // hover text in the Tuning tab
};

namespace detail {
template <class T>
constexpr CfgType cfg_type_of() noexcept {
  if constexpr (sizeof(T) == 1) return CfgType::U8;
  else if constexpr (sizeof(T) == 2) return CfgType::U16;
  else if constexpr (std::is_floating_point_v<T>) return CfgType::F32;
  else return CfgType::U32;
}
}  // namespace detail

#define TOUCHFC_CFG(name, group, label, lo, hi, hint)                                                 \
  CfgField {                                                                                      \
    #name, label, CfgGroup::group, detail::cfg_type_of<decltype(Config::name)>(),                 \
        static_cast<std::uint16_t>(offsetof(Config, name)), lo, hi, hint                          \
  }

inline constexpr CfgField kConfigFields[] = {
    TOUCHFC_CFG(conf_player, Detection, "player confidence", 0.05f, 0.95f, "Lowest detector score kept as a player. Higher: fewer false players, more missed ones."),
    TOUCHFC_CFG(conf_ball, Detection, "ball confidence", 0.05f, 0.95f, "Lowest detector score kept as the ball. Higher: fewer fake balls, more frames with no ball."),
    TOUCHFC_CFG(nms_iou, Detection, "NMS overlap", 0.1f, 0.9f, "Overlap above which two boxes are taken as the same object and merged. Lower merges more."),
    TOUCHFC_CFG(iou_match_thresh, Detection, "track match overlap", 0.05f, 0.9f, "Overlap a box needs with a track to continue it. Lower keeps tracks through fast moves but swaps IDs more."),
    TOUCHFC_CFG(det_interval, Detection, "detect every Nth frame", 1, 4, "Run the detector every Nth frame; tracking fills the rest. Higher saves GPU, lowers accuracy."),
    TOUCHFC_CFG(ball_tiling, Detection, "ball tiling", 0, 1, "Also search the frame in tiles for the ball. Finds small, far balls; costs extra detector time."),
    TOUCHFC_CFG(ball_confirm, Detection, "new ball: frames to confirm (0 off)", 0, 8, "Sightings in a row before a new ball is believed. Higher drops flicker but reacts later. 0 turns the filter off."),
    TOUCHFC_CFG(ball_max_speed, Detection, "ball max speed, widths/s", 0.5f, 4.f, "Fastest the ball may cross the picture, in widths per second. A faster jump is ignored."),
    TOUCHFC_CFG(ball_park_s, Detection, "spare ball after s alone", 0.5f, 10.f, "A ball sitting alone and still this long is treated as a spare ball off the pitch."),

    TOUCHFC_CFG(conf_keypoint, Pitch, "keypoint confidence", 0.05f, 0.95f, "Lowest score for a pitch landmark to be used in the pitch fit."),
    TOUCHFC_CFG(kp_interval, Pitch, "keypoint interval", 1, 16, "Run the pitch keypoint model every Nth wide frame. Higher saves GPU; camera motion carries the fit between."),
    TOUCHFC_CFG(h_max_rms_m, Pitch, "fit max error m", 0.3f, 4.f, "A pitch fit with more error than this (metres) is thrown away."),
    TOUCHFC_CFG(h_jump_m, Pitch, "fit jump m", 0.5f, 10.f, "A new fit this far (metres) from the predicted one waits for a second fit before it is trusted."),
    TOUCHFC_CFG(h_smooth, Pitch, "fit smoothing", 0.05f, 1.f, "Weight of each new pitch fit against the prediction. Higher reacts faster but shakes more."),
    TOUCHFC_CFG(h_holdover, Pitch, "holdover frames", 5, 250, "Frames a pitch fit lives on camera motion alone before it is dropped."),
    TOUCHFC_CFG(gmc_max_cost, Pitch, "motion max cost", 4.f, 40.f, "Image motion worse than this is treated as a camera cut."),
    TOUCHFC_CFG(line_interval, Pitch, "line refit interval", 1, 8, "Refit the pitch to the painted lines every Nth wide frame."),
    TOUCHFC_CFG(line_search_px, Pitch, "line search px", 4.f, 40.f, "How far (px at 1920 wide) either side of a predicted line to look for the painted one."),
    TOUCHFC_CFG(line_contrast, Pitch, "line contrast", 5.f, 60.f, "How much brighter than the grass a painted line must be."),
    TOUCHFC_CFG(line_min_support, Pitch, "line min support", 0.05f, 0.9f, "Share of expected markings that must land on painted lines for a line refit to count."),
    TOUCHFC_CFG(green_margin, Pitch, "green margin", 2, 30, "Grass test: green must beat red and blue by this much."),
    TOUCHFC_CFG(green_min, Pitch, "green minimum", 10, 120, "Grass test: green must be at least this bright."),
    TOUCHFC_CFG(scene_wide_frac, Pitch, "wide: grass share", 0.05f, 0.9f, "Grass share of the frame needed to call a shot wide. Tracking runs on wide shots only."),
    TOUCHFC_CFG(scene_mid_frac, Pitch, "wide: middle grass share", 0.02f, 0.9f, "Grass share of the middle third also needed for a wide shot."),
    TOUCHFC_CFG(scene_close_frac, Pitch, "pitch view: grass share", 0.02f, 0.5f, "Below this grass share the frame is not a pitch view (crowd, studio, graphics)."),

    TOUCHFC_CFG(team_max_dist, Teams, "kit colour distance", 5.f, 120.f, "Shirt colour distance to the nearer kit beyond which a player gets no team."),
    TOUCHFC_CFG(team_force, Teams, "force teams (one spare box)", 0, 1, "Put every player on a team, except one spare box (the referee)."),
    TOUCHFC_CFG(team_auto, Teams, "find kits automatically", 0, 1, "Find the two kit colours from the players without clicking."),
    TOUCHFC_CFG(kit_auto_sep, Teams, "auto kit separation", 10.f, 80.f, "Automatic kits: the two colour clusters must be at least this far apart."),
    TOUCHFC_CFG(kit_auto_share, Teams, "auto kit min share", 0.05f, 0.45f, "Automatic kits: each kit must cover at least this share of the shirt samples."),
    TOUCHFC_CFG(dir_auto, Teams, "attack direction from positions", 0, 1, "Work out which way each team attacks from where its players stand."),
    TOUCHFC_CFG(team_min_px, Teams, "min torso samples", 2, 200, "Shirt pixels (not grass) needed before a player's kit colour is read."),

    TOUCHFC_CFG(view_angle_max_deg, PlayState, "view angle max deg", 5.f, 90.f, "Pitch tilt above which the view is not the main camera, so play counts as dead."),
    TOUCHFC_CFG(player_height_max_frac, PlayState, "player height max", 0.05f, 0.9f, "Players taller than this share of the frame mean a close-up, not live play."),
    TOUCHFC_CFG(ball_off_on, PlayState, "ball off signal", 0, 1, "Use the ball crossing a line as a dead-ball signal. Off by default: lofted balls project outside the lines."),
    TOUCHFC_CFG(dead_enter_ms, PlayState, "dead after ms", 50, 3000, "Signs of a stoppage must last this long before play is called dead."),
    TOUCHFC_CFG(live_enter_ms, PlayState, "live after ms", 50, 5000, "Signs of live play must last this long before play is called live again."),
    TOUCHFC_CFG(scorebug_match_min, PlayState, "scorebug match min", 0.1f, 0.99f, "How closely the scorebug must match its learned look for the picture to count as live (not a replay)."),

    TOUCHFC_CFG(poss_dist_m, Events, "possession distance m", 0.3f, 5.f, "Ball within this distance of a player's feet gives that player possession."),
    TOUCHFC_CFG(pass_min_dist_m, Events, "pass min distance m", 1.f, 30.f, "Ball moves shorter than this between players do not count as passes."),
    TOUCHFC_CFG(ball_flight_min_mps, Events, "ball flight min m/s", 1.f, 30.f, "Ball speed above which it is taken to be travelling between players."),
    TOUCHFC_CFG(shot_speed_mps, Events, "shot speed m/s", 2.f, 40.f, "Ball speed toward goal needed for a shot."),
    TOUCHFC_CFG(shot_zone_m, Events, "shot zone m", 10.f, 60.f, "A shot must start this close to the goal."),
    TOUCHFC_CFG(shot_end_m, Events, "shot end depth m", 2.f, 20.f, "A loose ball this close to the goal line ends a shot."),
    TOUCHFC_CFG(shot_blind_m, Events, "unseen shot range m (0 off)", 0.f, 40.f, "A ball released this close to goal, then lost with play going dead, counts as a shot. 0 turns it off."),
    TOUCHFC_CFG(key_pass_window_s, Events, "key pass window s", 1.f, 15.f, "A completed pass followed by a shot from the same team within this time is a key pass."),

    TOUCHFC_CFG(rating_base, Rating, "base", 0.f, 10.f, "Rating every player starts the match on."),
    TOUCHFC_CFG(rating_pass_k, Rating, "pass: xT gain x", 0.f, 20.f, "Rating added per unit of threat (xT) a completed pass creates."),
    TOUCHFC_CFG(rating_press_mult, Rating, "pass under pressure x", 1.f, 3.f, "Extra multiplier for a pass made under pressure."),
    TOUCHFC_CFG(rating_press_m, Rating, "pressure distance m", 0.5f, 6.f, "An opponent this close to the passer means pressure."),
    TOUCHFC_CFG(rating_pass_floor, Rating, "pass floor", 0.f, 0.5f, "Smallest rating a completed pass gives, even when it adds no threat."),
    TOUCHFC_CFG(rating_loss_k, Rating, "lost pass: xT x", 0.f, 20.f, "Rating lost per unit of threat given away by a lost pass."),
    TOUCHFC_CFG(rating_loss_base, Rating, "lost pass: base", 0.f, 0.5f, "Fixed rating lost for any lost pass."),
    TOUCHFC_CFG(rating_int, Rating, "interception", 0.f, 1.f, "Rating for an interception."),
    TOUCHFC_CFG(rating_shot, Rating, "shot", 0.f, 1.f, "Rating for a shot."),
    TOUCHFC_CFG(rating_goal, Rating, "goal", 0.f, 3.f, "Rating for a goal."),

    TOUCHFC_CFG(kit_seed_max_dist, Match, "kit colour seed distance", 5.f, 80.f, "Provider kit colours must be this close to a player colour cluster to be used."),
    TOUCHFC_CFG(roi_frames, Match, "scorebug search frames", 10, 500, "Wide frames watched to find the scorebug box. Higher is surer but slower to lock."),
    TOUCHFC_CFG(roi_stable_max, Match, "scorebug max change", 0.02f, 5.f, "Scorebug box search: cells must change less than this per frame (graphics barely change)."),
    TOUCHFC_CFG(roi_contrast_min, Match, "scorebug min detail", 2.f, 60.f, "Scorebug box search: cells need at least this much detail (text edges)."),
    TOUCHFC_CFG(bug_area, Match, "scorebug: read top-left area", 0, 1, "On: OCR reads the whole top-left area set below, where scorebugs sit. Off: only the found box. A box set by hand wins either way."),
    TOUCHFC_CFG(bug_area_w_pct, Match, "scorebug area width %", 10, 60, "Width of the top-left scorebug area, as a % of the picture."),
    TOUCHFC_CFG(bug_area_h_pct, Match, "scorebug area height %", 5, 40, "Height of the top-left scorebug area, as a % of the picture."),
    TOUCHFC_CFG(bug_jump_reads, Match, "scorebug: reads for a score jump", 3, 30, "One more goal for one side counts after 2 agreeing reads. Any other change (two goals at once, a jump like 2 to 16) needs this many, read at the normal pace (about 5 s apart)."),

    TOUCHFC_CFG(show_boxes, Overlay, "show boxes", 0, 1, "Draw detection boxes on the video."),
    TOUCHFC_CFG(delay_margin_ms, Overlay, "present margin ms", 0.f, 80.f, "Extra delay on the overlay so it lines up with the video."),
    TOUCHFC_CFG(panel_side, Overlay, "panel side (auto/left/right)", 0, 2, "Where panels sit: auto (away from the action), left, or right."),
    TOUCHFC_CFG(flip_margin_px, Overlay, "flip margin px", 0.f, 200.f, "Clear space a panel keeps from players, ball and scorebug before it flips side."),
    TOUCHFC_CFG(flip_hold_ms, Overlay, "flip hold ms", 100, 10000, "After a flip, a panel stays put at least this long."),
    TOUCHFC_CFG(flip_react_ms, Overlay, "flip react ms", 0, 2000, "A panel must be in the way this long before it moves."),
    TOUCHFC_CFG(minimap_dead_mode, Overlay, "dead ball minimap (freeze/hide)", 0, 1, "On a dead ball the minimap freezes (0) or hides (1)."),
    TOUCHFC_CFG(mini_replay, Overlay, "mini replay", 0, 1, "Show mini replays of key moments."),
    TOUCHFC_CFG(replay_budget_pct, Overlay, "mini replay budget % (0 off)", 0, 100, "Mini replays pause when this % of frames are late or dropped. 0: never pause."),
    TOUCHFC_CFG(replay_log_n, Overlay, "replay log size (0 off)", 0, 20, "Key moments kept in the replay log. 0 turns the log off."),
    TOUCHFC_CFG(popup_s, Overlay, "pop-up seconds", 1.f, 10.f, "How long shot, key pass and turnover pop-ups stay up."),
    TOUCHFC_CFG(goal_banner_s, Overlay, "goal banner seconds", 10.f, 180.f, "How long the scorer banner stays up after a goal."),
    TOUCHFC_CFG(dead_panel_s, Overlay, "dead ball panel after s", 0.5f, 30.f, "Play dead this long before a lineup, back line or momentum panel shows."),
    TOUCHFC_CFG(line_high_m, Events, "high back line m", 25.f, 60.f, "A back line this far from its own goal is shown as high."),
    TOUCHFC_CFG(half_gap_s, PlayState, "half time gap s (0 off)", 0, 1200, "A break this long after 40 minutes is half time, and the teams swap ends. 0: never swap."),

    TOUCHFC_CFG(version, Hidden, "version", 0, 0, ""),
    TOUCHFC_CFG(layers, Hidden, "layers", 0, 0, ""),
    TOUCHFC_CFG(attack_dir, Hidden, "attack direction", 0, 0, ""),
};

#undef TOUCHFC_CFG

inline double cfg_get(const Config& c, const CfgField& f) noexcept {
  const auto* p = reinterpret_cast<const std::uint8_t*>(&c) + f.offset;
  switch (f.type) {
    case CfgType::F32: { float v; std::memcpy(&v, p, 4); return v; }
    case CfgType::U8: return *p;
    case CfgType::U16: { std::uint16_t v; std::memcpy(&v, p, 2); return v; }
    case CfgType::U32: { std::uint32_t v; std::memcpy(&v, p, 4); return v; }
  }
  return 0;
}

inline void cfg_set(Config& c, const CfgField& f, double v) noexcept {
  auto* p = reinterpret_cast<std::uint8_t*>(&c) + f.offset;
  switch (f.type) {
    case CfgType::F32: { const float x = static_cast<float>(v); std::memcpy(p, &x, 4); break; }
    case CfgType::U8: *p = static_cast<std::uint8_t>(v); break;
    case CfgType::U16: { const auto x = static_cast<std::uint16_t>(v); std::memcpy(p, &x, 2); break; }
    case CfgType::U32: { const auto x = static_cast<std::uint32_t>(v); std::memcpy(p, &x, 4); break; }
  }
}

inline void cfg_reset_group(Config& c, CfgGroup g) noexcept {
  const Config d{};
  for (const CfgField& f : kConfigFields)
    if (f.group == g) cfg_set(c, f, cfg_get(d, f));
}

}  // namespace touchfc
