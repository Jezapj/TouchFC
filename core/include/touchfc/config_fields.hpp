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

#define TOUCHFC_CFG(name, group, label, lo, hi)                                                   \
  CfgField {                                                                                      \
    #name, label, CfgGroup::group, detail::cfg_type_of<decltype(Config::name)>(),                 \
        static_cast<std::uint16_t>(offsetof(Config, name)), lo, hi                                \
  }

inline constexpr CfgField kConfigFields[] = {
    TOUCHFC_CFG(conf_player, Detection, "player confidence", 0.05f, 0.95f),
    TOUCHFC_CFG(conf_ball, Detection, "ball confidence", 0.05f, 0.95f),
    TOUCHFC_CFG(nms_iou, Detection, "NMS overlap", 0.1f, 0.9f),
    TOUCHFC_CFG(iou_match_thresh, Detection, "track match overlap", 0.05f, 0.9f),
    TOUCHFC_CFG(det_interval, Detection, "detect every Nth frame", 1, 4),
    TOUCHFC_CFG(ball_tiling, Detection, "ball tiling", 0, 1),

    TOUCHFC_CFG(conf_keypoint, Pitch, "keypoint confidence", 0.05f, 0.95f),
    TOUCHFC_CFG(kp_interval, Pitch, "keypoint interval", 1, 16),
    TOUCHFC_CFG(h_max_rms_m, Pitch, "fit max error m", 0.3f, 4.f),
    TOUCHFC_CFG(h_jump_m, Pitch, "fit jump m", 0.5f, 10.f),
    TOUCHFC_CFG(h_smooth, Pitch, "fit smoothing", 0.05f, 1.f),
    TOUCHFC_CFG(h_holdover, Pitch, "holdover frames", 5, 250),
    TOUCHFC_CFG(gmc_max_cost, Pitch, "motion max cost", 4.f, 40.f),
    TOUCHFC_CFG(line_interval, Pitch, "line refit interval", 1, 8),
    TOUCHFC_CFG(line_search_px, Pitch, "line search px", 4.f, 40.f),
    TOUCHFC_CFG(line_contrast, Pitch, "line contrast", 5.f, 60.f),
    TOUCHFC_CFG(line_min_support, Pitch, "line min support", 0.05f, 0.9f),
    TOUCHFC_CFG(green_margin, Pitch, "green margin", 2, 30),
    TOUCHFC_CFG(green_min, Pitch, "green minimum", 10, 120),
    TOUCHFC_CFG(scene_wide_frac, Pitch, "wide: grass share", 0.05f, 0.9f),
    TOUCHFC_CFG(scene_mid_frac, Pitch, "wide: middle grass share", 0.02f, 0.9f),
    TOUCHFC_CFG(scene_close_frac, Pitch, "pitch view: grass share", 0.02f, 0.5f),

    TOUCHFC_CFG(team_max_dist, Teams, "kit colour distance", 5.f, 120.f),
    TOUCHFC_CFG(team_min_px, Teams, "min torso samples", 2, 200),

    TOUCHFC_CFG(view_angle_max_deg, PlayState, "view angle max deg", 5.f, 90.f),
    TOUCHFC_CFG(player_height_max_frac, PlayState, "player height max", 0.05f, 0.9f),
    TOUCHFC_CFG(ball_off_on, PlayState, "ball off signal", 0, 1),
    TOUCHFC_CFG(dead_enter_ms, PlayState, "dead after ms", 50, 3000),
    TOUCHFC_CFG(live_enter_ms, PlayState, "live after ms", 50, 5000),
    TOUCHFC_CFG(scorebug_match_min, PlayState, "scorebug match min", 0.1f, 0.99f),

    TOUCHFC_CFG(poss_dist_m, Events, "possession distance m", 0.3f, 5.f),
    TOUCHFC_CFG(pass_min_dist_m, Events, "pass min distance m", 1.f, 30.f),
    TOUCHFC_CFG(ball_flight_min_mps, Events, "ball flight min m/s", 1.f, 30.f),
    TOUCHFC_CFG(shot_speed_mps, Events, "shot speed m/s", 2.f, 40.f),
    TOUCHFC_CFG(shot_zone_m, Events, "shot zone m", 10.f, 60.f),
    TOUCHFC_CFG(key_pass_window_s, Events, "key pass window s", 1.f, 15.f),

    TOUCHFC_CFG(rating_base, Rating, "base", 0.f, 10.f),
    TOUCHFC_CFG(rating_pass_k, Rating, "pass: xT gain x", 0.f, 20.f),
    TOUCHFC_CFG(rating_press_mult, Rating, "pass under pressure x", 1.f, 3.f),
    TOUCHFC_CFG(rating_press_m, Rating, "pressure distance m", 0.5f, 6.f),
    TOUCHFC_CFG(rating_pass_floor, Rating, "pass floor", 0.f, 0.5f),
    TOUCHFC_CFG(rating_loss_k, Rating, "lost pass: xT x", 0.f, 20.f),
    TOUCHFC_CFG(rating_loss_base, Rating, "lost pass: base", 0.f, 0.5f),
    TOUCHFC_CFG(rating_int, Rating, "interception", 0.f, 1.f),
    TOUCHFC_CFG(rating_shot, Rating, "shot", 0.f, 1.f),
    TOUCHFC_CFG(rating_goal, Rating, "goal", 0.f, 3.f),

    TOUCHFC_CFG(kit_seed_max_dist, Match, "kit colour seed distance", 5.f, 80.f),
    TOUCHFC_CFG(roi_frames, Match, "scorebug search frames", 10, 500),
    TOUCHFC_CFG(roi_stable_max, Match, "scorebug max change", 0.02f, 5.f),
    TOUCHFC_CFG(roi_contrast_min, Match, "scorebug min detail", 2.f, 60.f),

    TOUCHFC_CFG(show_boxes, Overlay, "show boxes", 0, 1),
    TOUCHFC_CFG(delay_margin_ms, Overlay, "present margin ms", 0.f, 80.f),
    TOUCHFC_CFG(panel_side, Overlay, "panel side (auto/left/right)", 0, 2),
    TOUCHFC_CFG(flip_margin_px, Overlay, "flip margin px", 0.f, 200.f),
    TOUCHFC_CFG(flip_hold_ms, Overlay, "flip hold ms", 100, 10000),
    TOUCHFC_CFG(flip_react_ms, Overlay, "flip react ms", 0, 2000),
    TOUCHFC_CFG(minimap_dead_mode, Overlay, "dead ball minimap (freeze/hide)", 0, 1),
    TOUCHFC_CFG(mini_replay, Overlay, "mini replay", 0, 1),
    TOUCHFC_CFG(replay_budget_pct, Overlay, "mini replay budget % (0 off)", 0, 100),

    TOUCHFC_CFG(version, Hidden, "version", 0, 0),
    TOUCHFC_CFG(layers, Hidden, "layers", 0, 0),
    TOUCHFC_CFG(attack_dir, Hidden, "attack direction", 0, 0),
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
