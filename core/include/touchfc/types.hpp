#pragma once

#include <cstdint>
#include <type_traits>

namespace touchfc {

using FrameId = std::uint64_t;
using TrackId = std::uint32_t;
using PlayerId = std::uint16_t;
using EventId = std::uint64_t;

enum class ObjClass : std::uint8_t { Player = 0, Goalkeeper, Referee, Ball, Other };

enum class EventType : std::uint8_t {
  PossessionStart = 0,
  PossessionEnd,
  Pass,
  Interception,
  Loss,
  Recovery,
  Shot,
  Goal,
  Clearance,
  Dribble,
  Foul,
  Other,
  DeadBall  // a play-state gap, so exports show where stats stopped
};

enum class EventStatus : std::uint8_t { Tentative = 0, Confirmed, Rejected, Manual };
enum class EventResult : std::uint8_t { Unknown = 0, Success, Fail, Goal, Saved, Blocked, OffTarget };

enum FrameFlags : std::uint8_t {
  kFlagNone = 0,
  kFlagBlack = 1u << 0,
  kFlagDeduped = 1u << 1,
  kFlagReplay = 1u << 2,
};

// Image -> pitch homography is filled in later milestones. The slot here is the
// M1 overlay payload that travels with the frame so present draws frame N with
// the overlay for frame N.
struct OverlayDraw {
  float box_x = 0;
  float box_y = 0;
  float box_w = 0;
  float box_h = 0;
  std::uint8_t flags = 0;
  std::uint8_t pad[3]{};
};

struct FrameMeta {
  FrameId id = 0;
  std::int64_t t_capture = 0;
  std::int64_t t_pre_done = 0;
  std::int64_t t_inf_done = 0;
  std::int64_t t_track_done = 0;
  std::int64_t pts_ns = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint8_t flags = 0;
  std::uint8_t pad[1]{};
};

// Published by the track thread for the control window. The present thread
// does not use this for the pixels it draws; those live on the frame slot.
struct OverlayState {
  FrameId frame = 0;
  std::int64_t t_capture = 0;
  std::int64_t t_publish = 0;
  float box_x = 0, box_y = 0, box_w = 0, box_h = 0;
  float p50_ms = 0, p99_ms = 0, fps = 0;
  std::uint32_t drops = 0;
  std::uint32_t dedupes = 0;
  std::uint32_t black_frames = 0;
  std::uint32_t late_frames = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint8_t flags = 0;
  std::uint8_t scene = 0;
  std::uint8_t pad[6]{};
};

struct UiSnapshot {
  FrameId frame = 0;
  std::uint32_t pending_command_warn = 0;
  std::uint32_t drops = 0;
  char status[120]{};
};

enum class CmdType : std::uint8_t {
  AssignLabel = 0,
  ClearLabel,
  MergeTracks,
  SeedBall,
  SetTeamSeed,
  SetAnchor,
  ConfirmEvent,
  RejectEvent,
  EditEvent,
  AddEvent,
  MarkReplay,
  ResetTracker,
  ResetHomography,
  ApplyAttackDir,
  SetScorebugRoi,  // f[0..3] = x, y, w, h in frame pixels; w = 0 back to auto
  TestReplay       // mini replay of the last 5.5 s
};

struct UiCommand {
  CmdType type{};
  std::uint8_t u8a = 0;
  std::uint8_t u8b = 0;
  std::uint16_t u16a = 0;
  std::uint32_t u32a = 0;
  std::uint32_t u32b = 0;
  std::uint64_t u64a = 0;
  float f[4]{};
};

// Live-tuned POD. Multiple of 8 so it can sit in a SeqLock.
// version increments on every store. Readers copy it once per frame.
// Fields are grouped as the Tuning tab shows them; config_fields.hpp lists
// each with its range, and profiles save them by name.
struct Config {
  std::uint32_t version = 1;
  // Detection
  float conf_player = 0.35f;
  float conf_ball = 0.25f;
  float nms_iou = 0.45f;
  float iou_match_thresh = 0.30f;
  // Pitch
  float conf_keypoint = 0.40f;
  float h_max_rms_m = 1.5f;        // reject a pitch fit above this error
  float h_jump_m = 3.f;            // a fit this far from the prediction waits one more fit
  float h_smooth = 0.4f;           // weight of a new fit against the prediction
  float gmc_max_cost = 18.f;       // image motion worse than this is a cut
  float line_search_px = 12.f;     // line search +- px, at 1920 wide
  float line_contrast = 18.f;      // painted line over grass, grey levels
  float line_min_support = 0.35f;  // share of in-view markings on painted lines
  float scene_wide_frac = 0.28f;   // grass share of the frame for a wide shot
  float scene_mid_frac = 0.15f;    // ... and of the middle third
  float scene_close_frac = 0.12f;  // below this grass share: not a pitch view
  // Teams
  float team_max_dist = 45.f;      // Lab distance to the nearer kit seed; beyond it, no team
  // Play state
  float view_angle_max_deg = 40.f;     // pitch long axis tilt at the image centre
  float player_height_max_frac = 0.25f;  // median player box height / frame height
  float scorebug_match_min = 0.6f;
  // Events
  float poss_dist_m = 2.5f;         // ball to feet; homography error is ~1 m mid-pitch
  float pass_min_dist_m = 5.f;
  float ball_flight_min_mps = 6.f;
  float shot_speed_mps = 8.f;
  float shot_zone_m = 35.f;          // a shot starts this close to the goal
  float shot_end_m = 9.f;            // a loose ball this close to the goal line ends a shot
  float shot_blind_m = 22.f;         // release this close, unseen, then dead: a shot (0 off)
  float key_pass_window_s = 5.f;     // completed pass then shot by the same team
  // TFC rating (spec 11.5): xT-based, shown as rating_base + contributions
  float rating_pass_k = 3.f;
  float rating_press_mult = 1.25f;
  float rating_press_m = 2.f;
  float rating_pass_floor = 0.02f;
  float rating_loss_k = 3.f;
  float rating_loss_base = 0.05f;
  float rating_int = 0.05f;
  float rating_shot = 0.1f;
  float rating_goal = 1.f;
  float rating_base = 6.f;
  // Match setup
  float kit_seed_max_dist = 25.f;    // provider kit colour must be this close (Lab) to a player cluster
  float roi_stable_max = 0.2f;       // scorebug cells change less than this per frame (graphics ~0, slow pans 0.25+)
  float roi_contrast_min = 12.f;     // ... and have this much detail
  // Overlay
  float delay_margin_ms = 0.f;
  float flip_margin_px = 24.f;
  std::uint32_t layers = 0xffffffffu;
  std::uint16_t h_holdover = 50;     // frames a fit lives on camera motion alone
  std::uint16_t team_min_px = 8;     // non-grass torso samples for a kit colour
  std::uint16_t dead_enter_ms = 400;
  std::uint16_t live_enter_ms = 700;
  std::uint16_t flip_hold_ms = 2000;
  std::uint16_t roi_frames = 75;     // wide frames to find the scorebug
  std::uint16_t flip_react_ms = 150;  // a panel hit this long moves
  std::uint8_t det_interval = 1;
  std::uint8_t show_boxes = 1;
  std::uint8_t attack_dir = 1;
  std::uint8_t ball_tiling = 0;
  std::uint8_t kp_interval = 4;      // keypoint model every Nth wide frame
  std::uint8_t line_interval = 2;    // line refit every Nth wide frame
  std::uint8_t green_margin = 8;     // grass: green over red and blue by this much
  std::uint8_t green_min = 40;       // ... and green above this
  std::uint8_t minimap_dead_mode = 0;  // MinimapDeadMode
  std::uint8_t panel_side = 0;         // PanelSide
  std::uint8_t mini_replay = 1;
  std::uint8_t ball_off_on = 0;      // a lofted ball projects past the lines, so off by default
  std::uint8_t replay_budget_pct = 5;  // mini replay pauses above this % late/dropped; 0 never
  std::uint8_t team_force = 1;  // every box on a team but one (the referee)
  std::uint8_t pad[2]{};
};

enum class MinimapDeadMode : std::uint8_t { Freeze = 0, Hide = 1 };
enum class PanelSide : std::uint8_t { Auto = 0, Left = 1, Right = 2 };

static_assert(std::is_trivially_copyable_v<Config>);
static_assert(std::is_standard_layout_v<Config>);
static_assert(sizeof(Config) % 8 == 0, "SeqLock payload must be a multiple of 8");
static_assert(sizeof(Config) <= 4096);
static_assert(std::is_trivially_copyable_v<OverlayState>);
static_assert(std::is_trivially_copyable_v<UiCommand>);
static_assert(std::is_trivially_copyable_v<FrameMeta>);

struct Event {
  EventId id = 0;
  FrameId frame_start = 0;
  FrameId frame_end = 0;
  std::int32_t match_ms_start = 0;
  std::int32_t match_ms_end = 0;
  EventType type = EventType::Other;
  EventStatus status = EventStatus::Tentative;
  EventResult result = EventResult::Unknown;
  std::uint8_t team = 0;
  PlayerId player = 0;
  PlayerId receiver = 0;
  TrackId track = 0;
  TrackId track_recv = 0;
  float sx = 0, sy = 0, ex = 0, ey = 0;
  float confidence = 0;
  std::uint8_t flags = 0;
  std::uint8_t pad[3]{};
};

static_assert(std::is_trivially_copyable_v<Event>);

enum class Stage : std::uint8_t {
  Capture = 0,
  Preprocess,
  Inference,
  Track,
  Present,
  Count
};

struct TelemetrySample {
  FrameId frame = 0;
  std::uint8_t stage = 0;
  std::uint8_t flags = 0;
  std::uint16_t pad = 0;
  std::int64_t t_begin = 0;
  std::int64_t t_end = 0;
};

inline constexpr int kTelemetryProducers = 4;

}  // namespace touchfc
