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
  Other
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
  std::uint8_t pad[7]{};
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
  ApplyAttackDir
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
struct Config {
  std::uint32_t version = 1;
  float conf_player = 0.35f;
  float conf_ball = 0.25f;
  float conf_keypoint = 0.40f;
  float iou_match_thresh = 0.30f;
  float poss_dist_m = 1.5f;
  float delay_margin_ms = 0.f;
  std::uint8_t det_interval = 1;
  std::uint8_t show_boxes = 1;
  std::uint8_t attack_dir = 1;
  std::uint8_t ball_tiling = 0;
  std::uint32_t layers = 0xffffffffu;
  float shot_speed_mps = 8.f;
  float h_max_rms_m = 1.5f;     // reject a pitch fit above this error
  float h_jump_m = 3.f;         // a fit this far from the prediction waits one more fit
  float h_smooth = 0.4f;        // weight of a new fit against the prediction
  float gmc_max_cost = 18.f;    // image motion worse than this is a cut
  std::uint16_t h_holdover = 50;  // frames a fit lives on camera motion alone
  std::uint8_t kp_interval = 4;   // keypoint model every Nth wide frame
  std::uint8_t minimap_dead_mode = 0;  // MinimapDeadMode, used from M11
  std::uint8_t green_margin = 8;   // grass: green over red and blue by this much
  std::uint8_t line_interval = 2;  // line refit every Nth wide frame
  std::uint16_t pad0 = 0;
  float line_search_px = 12.f;     // line search +- px, at 1920 wide
  float line_contrast = 18.f;      // painted line over grass, grey levels
  float line_min_support = 0.35f;  // share of in-view markings on painted lines
  float pad1 = 0.f;
};

enum class MinimapDeadMode : std::uint8_t { Freeze = 0, Hide = 1 };

static_assert(std::is_trivially_copyable_v<Config>);
static_assert(sizeof(Config) % 8 == 0, "SeqLock payload must be a multiple of 8");
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
