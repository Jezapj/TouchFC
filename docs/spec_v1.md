# Soccer Broadcast Overlay: System Specification (C++20, Windows)

Audience: an advanced LLM (or engineer) asked to review, critique and refine this design, then help implement it.
Author context: a recent Monash graduate (Electrical & Computer Systems Engineering, Advanced Computer Science) who knows low-latency concepts in theory (cache hierarchy, branch prediction, SIMD, allocators, atomics, false sharing, PGO, CPU affinity, tail latency) but has only used them in small C examples. The project's primary purpose is to build **employable, demonstrable C++ performance skills**. Product value is secondary. AU English throughout.

---

## 0. How to use this document

1. Section 1 to 3 give goals, constraints and the external interfaces.
2. Section 4 to 12 define the concrete data structures and threads. Treat the code as a design sketch, not final code.
3. Section 13 to 16 cover persistence, telemetry, testing and milestones.
4. Section 17 lists open questions and known weak points. **Please challenge these first.**
5. Section 18 states what is being asked of the reviewing model.

---

## 1. Goals and non-goals

### 1.1 Goals
- G1. Capture a **browser window** playing a historical full soccer match (fullscreen allowed) on Windows 10/11.
- G2. Run **pretrained** detection and pitch-keypoint models (no offline training by the author) in real time.
- G3. Track players, referees, goalkeepers and the ball; map screen positions to **pitch coordinates** via per-frame homography.
- G4. Derive **tentative event data** (possession, pass, loss of possession, shot) with a state machine, and let the user confirm, reject, edit or add events by hand.
- G5. Display a **transparent, click-through overlay** on top of the browser window: boxes, track IDs, team colours, minimap, event log, latency HUD, optional pitch control.
- G6. Expose **live tuning controls** (sensitivity thresholds, detection interval, tracker noise, possession hysteresis) and **manual player number labelling**.
- G7. Be **measurable**: p50/p99/p99.9 per stage and glass-to-overlay latency, with naive-vs-optimised comparisons for each optimisation.
- G8. Export events in a SPADL-compatible CSV so Python tools (socceraction / VAEP / xG) can consume them offline.

### 1.2 Non-goals
- No model training or fine-tuning by the author.
- No circumvention of DRM. If the capture is black, the source is unsuitable.
- No camera, microphone or hardware inputs.
- No live scraping or redistribution of broadcasts.
- No claim of broadcast-grade accuracy. Output is *semi-automatic*: the system proposes, the user corrects.
- Real-time xG/VAEP is **not** computed from vision. If used, it is a precomputed timeline (e.g. StatsBomb open data for the same match) played back against the match clock (section 12.4).

### 1.3 Success criteria (measurable)
| ID | Metric | Target (initial) |
|---|---|---|
| S1 | Capture-to-overlay-present latency, p99 | under 100 ms on a mid-range NVIDIA GPU, reported at p50/p99/p99.9 |
| S2 | Frame pacing of the overlay | 60 Hz present with under 1% late frames |
| S3 | Dropped capture frames | reported, not hidden; under 2% in steady state |
| S4 | Non-inference CPU time per frame (all stages excluding ORT) | under 3 ms p99 |
| S5 | Event precision/recall vs StatsBomb ground truth (where available) | reported per event type and per sensitivity setting |
| S6 | Each optimisation (SIMD, allocator, layout, PGO, affinity) | has a before/after benchmark with a committed chart |

---

## 2. Platform and toolchain

- OS: Windows 10/11 x64. GPU: NVIDIA preferred (CUDA EP); fall back to DirectML EP; CPU-only is a degraded mode.
- Compiler: MSVC (latest) with `/O2 /arch:AVX2`, optional clang-cl for comparison. C++20.
- Build: CMake + vcpkg (manifest mode). Presets: `Debug`, `RelWithDebInfo`, `Release`, `Release-PGO` (MSVC `/GL` + `/GENPROFILE` / `/USEPROFILE`).
- Libraries:
  - Capture: Windows Graphics Capture via C++/WinRT (`Windows.Graphics.Capture`, `Direct3D11CaptureFramePool`).
  - Graphics: Direct3D 11 (shared device for capture, preprocess compute shaders, overlay render).
  - Inference: ONNX Runtime (CUDA EP, DirectML EP, CPU EP) with IO binding to avoid host copies where possible.
  - UI: Dear ImGui (Win32 + DX11 backends), ImPlot for plots.
  - JSON: simdjson (read) and a small writer, or nlohmann/json if simplicity wins.
  - Benchmarking: Google Benchmark (microbenchmarks), HdrHistogram_c (latency distributions), Tracy (optional profiler).
  - Tests: Catch2 or GoogleTest.
- Third-party model sources to evaluate (licences and current repo state **must be verified**): roboflow/sports (player/ball/referee detection, pitch keypoints). Ultralytics YOLO is AGPL: relevant if code or weights are published. Unverified: ahmed-el-zainly/soccer.

---

## 3. System inputs and outputs

### 3.1 Inputs
| ID | Source | Content | Rate | Notes |
|---|---|---|---|---|
| IN1 | Windows Graphics Capture of a browser HWND (or monitor) | BGRA8 D3D11 textures + `SystemRelativeTime` | 30 or 60 fps | GPU-resident. Fullscreen supported. DRM content appears black: detect and warn. |
| IN2 | User UI (mouse, keyboard, ImGui widgets) | Config edits, label assignments, event confirm/reject/add, track merges, ball re-seed clicks | Event-driven | Must never block the pipeline. |
| IN3 | Model files (`.onnx`) | Detector (players/goalkeepers/referees/ball), pitch keypoint model | Load at start | Pretrained only. |
| IN4 | Lineup file (CSV/JSON) | Team, shirt number, name, position | Load per match | Optional; StatsBomb lineup JSON importer. |
| IN5 | Session file (JSON) | Saved config presets, labels, track-to-player map, confirmed events, sync anchor | Load/save | Resumable sessions. |
| IN6 | Optional event timeline (binary/JSON) | Precomputed xG/VAEP per event keyed by match clock | Load per match | Produced offline in Python. Not computed at runtime. |
| IN7 | Optional replay mode: recorded video file decoded via FFmpeg | Same `FrameSlot` interface as IN1 | Faster than real time | Gives repeatable benchmarks. Added after the capture path works. |

### 3.2 Outputs
| ID | Destination | Content |
|---|---|---|
| OUT1 | Overlay window (transparent, click-through, topmost) | Boxes, IDs, labels, team colours, minimap, event log, pitch control heatmap, latency HUD |
| OUT2 | Control window (normal ImGui window, separate HWND) | Sliders, label editor, event confirm UI, plots, session controls |
| OUT3 | `events.spadl.csv` | Confirmed events in SPADL-compatible columns |
| OUT4 | `tracks.parquet` or binary dump (optional) | Per-frame pitch-space tracks for offline analysis |
| OUT5 | `telemetry/*.hdr` and `*.csv` | Per-stage latency histograms, drop counters, frame pacing |
| OUT6 | `session.json` | Config, labels, anchors, event edits |

---

## 4. Conventions

### 4.1 Time
- Single monotonic clock: `QueryPerformanceCounter` converted to **int64 nanoseconds** (`TimeNs`). Capture timestamps from WGC (`SystemRelativeTime`) are converted into the same domain on arrival.
- `FrameId` is a monotonically increasing `uint64_t` assigned at capture.
- **Match clock** is separate: `MatchTimeMs` derived from a user-set anchor (kickoff frame + offset). Replays, ad breaks and halftime break the mapping, so the anchor can be re-set mid-session; every anchor change is logged with its `FrameId`.

```cpp
using TimeNs   = int64_t;
using FrameId  = uint64_t;
using TrackId  = uint32_t;     // 0 = invalid
using PlayerId = uint16_t;     // index into lineup; 0 = unassigned
using EventId  = uint64_t;

struct MatchAnchor { FrameId frame; TimeNs wall_ns; int32_t match_ms; uint8_t period; };
```

### 4.2 Coordinate systems
- **Image space:** pixels, origin top-left, in the captured frame resolution.
- **Model space:** letterboxed model input (e.g. 640x640 or 1280x1280). Keep the scale and padding in the frame metadata so detections map back to image space exactly.
- **Pitch space (internal):** metres, origin at the centre spot, `+x` toward the right-hand goal of the attacking direction defined for the period, `+y` toward the top touchline. Range `x in [-52.5, 52.5]`, `y in [-34, 34]`. Pitch size is configurable (default 105 x 68).
- **SPADL space (export only):** 105 x 68, origin bottom-left, with the playing direction normalised so each team attacks left to right. Conversion lives in one function and is unit-tested.
- Pitch space values are only valid for points on the ground plane. **Airborne ball positions are not reliable** and carry a flag (section 8).

### 4.3 Numeric types
- `float` for all per-frame geometry; `double` only for timestamps in analysis and the homography solver.
- Fixed-capacity containers everywhere on the hot path. No `std::vector` growth, `new`, `malloc`, `std::string` formatting or exceptions on pipeline threads after warm-up.

---

## 5. Threads and data flow

### 5.1 Overview
```
 [WGC callback]
      | latest-wins mailbox (slot index)
      v
 T1 Capture/Preprocess  --(slot idx)-->  SPSC  --> T2 Inference (ORT)
      ^                                                |
      | frame pool (FREE ring)                          | SPSC<DetectionBatch>
      |                                                 v
      |                              T3 Tracking / Homography / Events
      |                                  |            ^
      |                                  |            | SPSC<UiCommand>
      |                                  v            |
      |                         triple-buffered     T4 UI / Control window
      |                         OverlayState             (ImGui, command producer)
      |                                  |
      |                                  v
      +-------------------------------- T5 Overlay Render (present)
                          Telemetry: per-thread SPSC rings -> T6 Telemetry writer (low priority)
```

### 5.2 Thread table
| Thread | Role | Priority / affinity (initial) | Blocking allowed? |
|---|---|---|---|
| T0 WGC callback thread (system) | Receives capture frames, hands off immediately | system | No work beyond timestamp + mailbox publish |
| T1 Capture/Preprocess | Copy/convert frame to model input tensor on GPU (compute shader) or CPU SIMD path (baseline) | high, pinned to P-core A | No |
| T2 Inference | Runs ORT for detector and (at interval) keypoint model | high, pinned to P-core B | Waits on GPU only |
| T3 Track/Events | NMS-post, tracking, homography smoothing, team assignment, possession/event FSM, builds `OverlayState` | high, pinned to P-core C | No |
| T4 UI/Control | ImGui control window, command production | normal | Yes |
| T5 Overlay Render | Draws overlay at display rate, reads latest `OverlayState` | above normal, pinned to core D | Waits on vsync only |
| T6 Telemetry | Drains telemetry rings, writes files | below normal | Yes |

Pinning: use `SetThreadAffinityMask` and `SetThreadPriority`; make the layout configurable and report it in telemetry. Avoid hyperthread siblings and E-cores for T1 to T3 on hybrid CPUs (check with `GetLogicalProcessorInformationEx` and `EfficiencyClass`).

### 5.3 Handoff primitives
- **Latest-wins mailbox** (capture to T1, T3 to T5): a single `std::atomic<uint32_t>` slot index swapped with `exchange`. The consumer takes the newest item; stale ones are recycled to the FREE ring and counted as drops. This implements the "drop oldest, never build latency" policy.
- **SPSC ring** (T1 to T2, T2 to T3, T4 to T3, telemetry): bounded power-of-two ring, cache-line-separated head/tail, cached remote index to cut cross-core traffic.
- **Config snapshot** (T4 to all): seqlock or triple buffer, section 9.
- **No mutexes on T1 to T3 or T5.**

```cpp
inline constexpr size_t kCacheLine = 64;   // std::hardware_destructive_interference_size where available

template <class T, size_t N>   // N power of two
class SpscRing {
  static_assert((N & (N - 1)) == 0);
public:
  bool try_push(const T& v) noexcept {
    const size_t h = head_.load(std::memory_order_relaxed);
    if (h - tail_cache_ == N) {                       // looks full; refresh cache
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (h - tail_cache_ == N) return false;
    }
    buf_[h & (N - 1)] = v;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  bool try_pop(T& out) noexcept {
    const size_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_cache_) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (t == head_cache_) return false;
    }
    out = buf_[t & (N - 1)];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }
private:
  alignas(kCacheLine) std::atomic<size_t> head_{0};
  alignas(kCacheLine) size_t tail_cache_{0};          // producer-local
  alignas(kCacheLine) std::atomic<size_t> tail_{0};
  alignas(kCacheLine) size_t head_cache_{0};          // consumer-local
  alignas(kCacheLine) T buf_[N];
};
```

---

## 6. Frame pool and per-frame metadata

### 6.1 FrameSlot
A fixed pool (default 6 slots) allocated at start. A slot is owned by exactly one stage at a time; ownership moves by passing the slot index.

```cpp
struct FrameMeta {
  FrameId  id;
  TimeNs   t_capture;        // WGC system-relative time, converted
  TimeNs   t_pre_done, t_inf_done, t_track_done, t_present;   // stage stamps
  uint32_t width, height;    // captured size
  float    scale;            // image -> model scale
  float    pad_x, pad_y;     // letterbox padding in model pixels
  uint8_t  slot;             // pool index
  uint8_t  flags;            // bit0: black-frame suspected (DRM), bit1: dropped-ahead, bit2: replay/closeup (shot class)
};

struct FrameSlot {
  FrameMeta meta;
  ID3D11Texture2D*          capture_tex;    // BGRA8, GPU
  ID3D11Texture2D*          model_in_tex;   // optional GPU preprocess target
  alignas(64) float*        model_in_cpu;   // CHW float32 or fp16, CPU baseline path (preallocated, 64B aligned)
};
```

### 6.2 Preprocess paths (benchmarked against each other)
1. **Baseline:** staging copy to CPU, scalar BGRA to RGB, bilinear resize, normalise, HWC to CHW.
2. **SIMD CPU:** AVX2 intrinsics for deinterleave + convert + normalise; tiled to fit L1/L2.
3. **GPU compute shader:** resize + normalise + CHW directly into a texture/buffer bound to ORT.

Each path implements the same function signature so benchmarks swap them.

---

## 7. Detection stage (T2) outputs

```cpp
enum class ObjClass : uint8_t { Player, Goalkeeper, Referee, Ball, Other };

struct Detection {
  float x1, y1, x2, y2;     // image space after unletterbox
  float conf;
  ObjClass cls;
  uint8_t  from_tile;       // 1 if from sliced/tiled ball pass
  uint16_t _pad;
};

inline constexpr int kMaxDet = 128;
struct DetectionBatch {
  FrameMeta meta;
  uint16_t  n;
  Detection det[kMaxDet];          // players etc., sorted by conf descending
  // Pitch keypoints (present only on frames where the keypoint model ran)
  bool      has_keypoints;
  uint8_t   n_kp;                  // up to 32
  float     kp_x[32], kp_y[32], kp_conf[32];
  uint32_t  kp_mask;               // bit i set if keypoint i is valid
};
```

Rules:
- Detector runs every `det_interval` frames (config). On skipped frames, T3 predicts from the tracker.
- Keypoint model runs every `kp_interval` frames (config), or on demand after a cut or homography failure.
- Ball detection: optional tiled pass (SAHI-style) around the last known ball position plus a coarse full-frame pass. Toggled by config; cost is reported separately.
- Pre-NMS vs post-NMS: if the ONNX export includes NMS, use it; otherwise implement NMS in C++ (branchy, a benchmark target for branch prediction / branchless variants).

---

## 8. Tracking state (T3): structure-of-arrays

### 8.1 Design rules
- **SoA with hot/cold split.** Per-frame hot loops (predict, associate, update, team assign, pitch mapping) touch only the hot arrays.
- Fixed capacity `kMaxTracks = 64` (22 players + referees + extras + headroom), power of two.
- Each array 64-byte aligned; the SIMD path processes in blocks of 8 floats (AVX2).
- Track slots are recycled with a free-list. `TrackId` is a monotonically increasing id; the slot index is separate.

```cpp
inline constexpr int kMaxTracks = 64;

struct alignas(64) TrackHot {
  // Image-space state (Kalman: x, y, w, h, vx, vy, vw, vh), stored per component for SIMD
  alignas(64) float cx[kMaxTracks], cy[kMaxTracks], w[kMaxTracks], h[kMaxTracks];
  alignas(64) float vx[kMaxTracks], vy[kMaxTracks], vw[kMaxTracks], vh[kMaxTracks];
  // Diagonal covariance approximation (full 8x8 is optional and slower)
  alignas(64) float Pcx[kMaxTracks], Pcy[kMaxTracks], Pw[kMaxTracks], Ph[kMaxTracks];
  alignas(64) float Pvx[kMaxTracks], Pvy[kMaxTracks];
  // Pitch-space position (metres), derived through the current homography
  alignas(64) float px[kMaxTracks], py[kMaxTracks];
  alignas(64) float pvx[kMaxTracks], pvy[kMaxTracks];   // pitch-space velocity, smoothed
  // Scalars used every frame
  alignas(64) float conf[kMaxTracks];
  alignas(64) uint16_t age[kMaxTracks], time_since_update[kMaxTracks], hits[kMaxTracks];
  alignas(64) uint8_t  state[kMaxTracks];   // 0 free, 1 tentative, 2 confirmed, 3 lost
  alignas(64) uint8_t  cls[kMaxTracks];     // ObjClass
  alignas(64) uint8_t  team[kMaxTracks];    // 0 unknown, 1 team A, 2 team B, 3 referee
};

struct TrackCold {
  TrackId   id[kMaxTracks];
  PlayerId  player[kMaxTracks];        // from label store
  uint8_t   label_source[kMaxTracks];  // 0 none, 1 manual, 2 inherited(tentative)
  float     appearance[kMaxTracks][8]; // torso colour histogram signature (L*a*b* bins), for team + label inheritance
  float     last_seen_px[kMaxTracks], last_seen_py[kMaxTracks]; // for label inheritance after loss
  TimeNs    last_update_ns[kMaxTracks];
};
```

### 8.2 Association
- ByteTrack-style two-stage matching: high-confidence detections first (IoU + appearance cost), then low-confidence with remaining tracks.
- Assignment: greedy for the baseline, Hungarian (Jonker-Volgenant) as the optimised/accurate variant. Both behind the same interface for benchmarking.
- Cost matrix stored row-major, padded to a multiple of 8; IoU computed with AVX2 over blocks.
- Parameters (config): `iou_match_thresh`, `appearance_weight`, `max_age_frames`, `min_hits_confirm`.

### 8.3 Label inheritance (identity continuity without training)
When a labelled track is lost and a new track appears within `inherit_radius_m` of its last pitch position within `inherit_window_ms` and with appearance distance below `inherit_app_thresh`, the new track inherits the label with `label_source = inherited` (shown as tentative in the UI). The user confirms, or merges/reassigns by command (section 10).

### 8.4 Ball track
Separate, smaller structure (single object, history needed):

```cpp
struct BallSample {
  TimeNs t;
  float  ix, iy;      // image space
  float  px, py;      // pitch space (ground-plane projection)
  float  conf;
  uint8_t src;        // 0 predicted, 1 detected full-frame, 2 detected tiled, 3 user-seeded
  uint8_t airborne_suspected;   // heuristic: speed/height cue; pitch coords unreliable
  uint16_t _pad;
};

struct BallState {
  // Kalman state in pitch space (x, y, vx, vy) with process noise from config
  float x, y, vx, vy;
  float P[4];                     // diagonal approximation
  uint16_t frames_since_detect;
  uint8_t  mode;                  // 0 lost, 1 tracking, 2 coasting(predict only)
  BallSample history[256];        // ring, last ~4 s at 60 fps
  uint32_t head;
};
```

Rules: coast on prediction for up to `ball_max_coast_ms`; after that mode becomes `lost` and the UI shows a "re-seed ball" affordance (user clicks the ball on the overlay; a `UiCommand::SeedBall` places a 3 (user-seeded) sample).

### 8.5 Homography
```cpp
struct Homography {
  double H[9];                 // image -> pitch (row-major)
  double H_inv[9];
  TimeNs t_valid;              // time of the keypoint fit
  float  reproj_err_px;        // RMS over inliers
  uint8_t n_inliers;
  uint8_t quality;             // 0 invalid, 1 poor, 2 ok, 3 good
};
```
- Fit: DLT / RANSAC over valid keypoint correspondences (model keypoint id to known pitch landmark coordinates).
- Between keypoint frames: propagate by tracking feature points or by low-rate smoothing of H (e.g. decompose and filter, or fit inter-frame homography via sparse optical flow on pitch-line pixels). Start with "reuse last H until next keypoint fit" and measure the error.
- Reject fits with `reproj_err_px` above threshold, or whose implied pitch region is implausible (e.g. area outside the pitch). Mark `quality` and expose in the HUD.
- If `quality < ok`, pitch-space outputs for that frame are flagged `invalid` and downstream analysis pauses.

### 8.6 Shot/scene classifier (gating)
Cheap, fully self-written stage on a downscaled luma/HSV image (e.g. 160x90), AVX2:
- Green pitch ratio, line-pixel density, colour histogram change vs previous frame (cut detection), edge density.
- Output `SceneClass { Wide, Medium, CloseUp, Crowd, Replay?, Unknown }` and `cut_flag`.
- Gating: on `CloseUp`, `Crowd` or `cut_flag`, freeze event emission and suspend ball/possession logic; tracker continues in coasting mode up to a limit. Replays are hard to detect reliably (scoreboard/graphic wipes); provide a manual "mark as replay" hotkey in addition to automatic heuristics.

### 8.7 Team assignment
- Torso crop median/histogram in L*a*b*; online 2- or 3-cluster k-means (teams A/B + referee) with per-session initial seeds the user can click to set ("this is team A").
- Goalkeepers separated by class from the detector plus colour outlier test.
- Update cluster centres slowly (EMA) to cope with lighting changes. Cost target: under 0.3 ms per frame for 22 tracks (SIMD histograms).

---

## 9. Configuration and live tuning

### 9.1 Config struct (trivially copyable, POD)
```cpp
struct Config {
  uint32_t version;                // incremented by T4 on every change
  // Detection
  float    conf_player, conf_ball, conf_keypoint;
  uint8_t  det_interval, kp_interval;      // frames
  uint8_t  ball_tiling;                    // 0 off, 1 around last pos, 2 full-frame tiles
  uint16_t tile_size_px;
  // Tracking
  float    iou_match_thresh, appearance_weight;
  uint16_t max_age_frames, min_hits_confirm;
  float    kf_q_pos, kf_q_vel, kf_r_meas;            // player Kalman noise
  float    ball_q, ball_r;  uint16_t ball_max_coast_ms;
  // Homography
  float    reproj_err_max_px;  uint8_t min_keypoints;
  // Possession and events
  float    poss_dist_m;          // max player-ball distance to own the ball
  uint16_t poss_min_ms;          // minimum hold time to count
  uint16_t poss_gap_ms;          // max ball-missing gap before closing a sequence
  float    pass_min_len_m;
  float    shot_speed_mps;       // ball speed trigger for shot candidate
  float    shot_goal_cone_deg;
  // Labelling and display
  float    inherit_radius_m;  uint16_t inherit_window_ms;  float inherit_app_thresh;
  uint32_t layers;               // bitmask: boxes, ids, names, minimap, pitchctrl, events, hud
  uint8_t  attack_dir_period1;   // +1 or -1
  // Pipeline
  uint8_t  max_queue_depth;      // reserved
};
static_assert(std::is_trivially_copyable_v<Config>);
```

### 9.2 Publication mechanism
- **Seqlock** (single writer T4, many readers): readers copy the whole `Config` once per frame (about 200 bytes), retry if the sequence number changed. No locks, no allocation, no reader-side writes to shared cache lines.
- Alternative for comparison: triple buffer with an atomic index. Implement both and benchmark reader overhead; document the memory orderings (`acquire`/`release`/`seq_cst`) and why each is used.
- Presets (JSON) can be saved/loaded; loading creates a new snapshot via T4.

---

## 10. User commands (T4 to T3 via SPSC)

```cpp
enum class CmdType : uint8_t {
  AssignLabel,      // track_id -> player_id (manual)
  ClearLabel,       // track_id
  MergeTracks,      // track_a absorbs track_b
  SeedBall,         // image x,y at frame_id
  SetTeamSeed,      // image point -> team A/B/Referee cluster seed
  SetAnchor,        // frame_id -> match_ms, period
  ConfirmEvent, RejectEvent, EditEvent, AddEvent,
  MarkReplay,       // toggle replay gating from frame_id
  ResetTracker, ResetHomography,
  ApplyAttackDir
};

struct UiCommand {
  CmdType  type;
  uint8_t  u8a, u8b;  uint16_t u16a;
  uint32_t u32a, u32b;
  uint64_t u64a;               // frame_id / event_id / track ids
  float    f[4];               // coordinates
};
```
- Commands reference `FrameId` and `TrackId` (not slot indices), so T3 can validate that the target still exists and ignore stale commands.
- Commands are applied by T3 at the **start of its frame loop**, preserving determinism for replay mode (commands are logged with the frame id they applied on).

---

## 11. Identity and label store

```cpp
struct PlayerEntry {   // from lineup
  PlayerId id;
  uint8_t  team;       // 1 or 2
  uint8_t  shirt;
  uint8_t  position;   // GK, DF, MF, FW
  char     name[32];
};

struct LabelMap {      // T3-owned; written to session.json by T4 via snapshot
  // track_id -> player_id (manual) ; plus inherited-tentative entries with confidence
  struct Entry { TrackId track; PlayerId player; uint8_t source; float conf; FrameId set_at; };
  Entry entries[256];
  uint16_t n;
};
```
- Lineup import: CSV (`team,shirt,name,pos`) and StatsBomb lineup JSON.
- Label UI: click a box on the paused frame, type a shirt number or choose from the lineup; the label attaches to the `TrackId`. The pause is the browser's pause, which freezes captured frames naturally.
- Persistent session: all manual labels with their frame ids, so a session can be rebuilt deterministically.

---

## 12. Events, possession and analysis

### 12.1 Event record (fixed size POD)
```cpp
enum class EventType : uint8_t {
  PossessionStart, PossessionEnd, Pass, Interception, Loss, Recovery,
  Shot, Goal, Clearance, Dribble, Foul, Other
};
enum class EventStatus : uint8_t { Tentative, Confirmed, Rejected, Manual };
enum class EventResult : uint8_t { Unknown, Success, Fail, Goal, Saved, Blocked, OffTarget };

struct Event {
  EventId     id;
  FrameId     frame_start, frame_end;
  int32_t     match_ms_start, match_ms_end;   // via anchor
  EventType   type;  EventStatus status;  EventResult result;
  uint8_t     team;                           // acting team (1/2)
  PlayerId    player;                         // actor, 0 if unknown
  PlayerId    receiver;                       // for passes, 0 if unknown
  TrackId     track, track_recv;              // always known if detected
  float       sx, sy, ex, ey;                 // start/end pitch position (m), flagged invalid if homography poor
  float       confidence;                     // heuristic 0..1
  uint8_t     flags;                          // bit0: airborne suspected, bit1: homography poor, bit2: user-edited
};
```
- Stored in a fixed ring (e.g. 8192 events) plus an append-only log for export.

### 12.2 Possession and event state machine (T3)
States: `NoPossession`, `Candidate(track)`, `Possessed(track)`, `InFlight(from_track, since_frame)`, `Contested`, `Paused(gated)`.

Per-frame inputs: ball sample (pitch space, mode), nearest-player distances (SIMD min over tracks), gating flags, config.
- `NoPossession` to `Candidate`: nearest player within `poss_dist_m`.
- `Candidate` to `Possessed`: held for at least `poss_min_ms`, nearest by margin (hysteresis).
- `Possessed` to `InFlight`: ball leaves radius and ball speed exceeds a threshold.
- `InFlight` to `Possessed(new)`: a player (same team = **Pass**, other team = **Interception/Loss**) holds the ball per the rule above.
- `InFlight` with ball speed toward the goal and a goal-cone test = **Shot candidate**; result resolved by subsequent state (goal-line crossing, GK possession, out of play).
- Timeouts: if the ball is lost beyond `poss_gap_ms`, close the sequence as `Unknown`.
- Emitted events are **Tentative**; they become eligible for display as confirmed only if no contradiction arrives within about 0.3 to 0.5 s of video, or the user confirms.
- Implementation hint: encode the state machine as a table-driven or branchless-where-sensible design, and also implement the straightforward `switch` version; benchmark branch misprediction with PGO on/off.

### 12.3 Pitch control (optional, stretch)
- Grid over the pitch (e.g. 2 m cells, 53 x 34), SIMD over cells x players.
- Spearman-style time-to-intercept model using pitch-space positions and velocities; produce per-cell probability for team A, rendered as a heat overlay and used for a numerical "territory control" metric.
- Update rate lower than frame rate (e.g. 10 to 15 Hz) on its own cadence inside T3 or a helper thread; double-buffer the result.

### 12.4 Precomputed statistics timeline (IN6)
- Python (offline, not real time): socceraction/VAEP and an xG model run on **StatsBomb open data** for the same match, exported as a flat binary: `[match_ms, team, player, type, xg, vaep_off, vaep_def, x, y]`.
- Runtime: load into a sorted array; query by `match_ms` via binary search or a moving cursor (amortised O(1)); display on the overlay and in the event log. **This is displayed data, not computed from video.**
- Sync: manual anchor, with re-anchor hotkeys. Optional later: scoreboard clock OCR.

---

## 13. Overlay render state (T3 to T5)

```cpp
struct OverlayTrack {
  TrackId id; PlayerId player; uint8_t team, cls, label_source, flags;
  float ix, iy, iw, ih;     // image-space box (predicted forward to display time)
  float px, py;             // pitch position
  float speed_mps;
};
struct OverlayState {
  FrameId  frame;  TimeNs t_capture, t_publish;
  uint16_t n_tracks;  OverlayTrack tracks[kMaxTracks];
  BallSample ball;   uint8_t ball_mode;
  Homography H;      uint8_t scene;
  uint8_t  n_recent_events;  Event recent[16];
  float    possession_pct[2];
  float    pitch_control[53*34];    // optional, low-rate update
  struct Hud { float p50_ms, p99_ms; float fps; uint32_t drops; } hud;
};
```
- **Triple buffer** between T3 and T5: T3 writes to its private slot, publishes by atomic swap; T5 grabs the latest each vsync. No copying under lock.
- **Latency compensation:** at draw time, T5 advances each box by `(t_present - t_capture) * v` using Kalman velocity (clamped). Log both the compensated and uncompensated positions' error against a later detection to produce the "prediction helps by X px" graph.
- Window: `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE`, borderless, aligned to the captured window's client rect (track via `GetWindowRect`/`DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)` or an event hook). **Call `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)`** so WGC does not capture the overlay (feedback loop).
- Rendering: D3D11 swap chain with premultiplied alpha (DirectComposition or layered window with `UpdateLayeredWindow` as fallback); ImGui draw lists for text and shapes; instanced quads for boxes if counts grow.
- Control window is a **separate** normal window (T4) so interaction does not need to pass through the click-through overlay. Clicking-to-label happens on a mirrored preview in the control window, or via a toggled "interactive overlay" mode that removes `WS_EX_TRANSPARENT` while held.

---

## 14. Memory and allocation strategy
- All pipeline structures are preallocated in one startup arena (aligned, optionally with large pages if privilege allows). Sub-allocated by bump pointer; no frees in steady state.
- Per-frame scratch (cost matrices, NMS buffers, SIMD temporaries) from a frame-scoped linear arena reset every frame.
- ORT: use IO binding and preallocated input/output tensors; avoid per-run allocations; configure the arena strategy.
- Benchmarks to include: `new`/`malloc` baseline vs arena vs pool for detection/track structs, with allocation counts verified via a custom allocator counter or ETW heap tracing.
- Verify no allocations on hot threads with a debug-only allocation hook that asserts after warm-up.

---

## 15. Telemetry and benchmarking

### 15.1 Per-frame stamps
`t_capture` to `t_pre_done` to `t_inf_done` to `t_track_done` to `t_present`; stage time = difference. Each thread writes `TelemetrySample` into its own SPSC ring (no shared counters); T6 aggregates into HdrHistograms.

```cpp
struct TelemetrySample { FrameId frame; uint8_t stage; uint8_t flags; uint16_t _; TimeNs t_begin, t_end; };
```

### 15.2 Metrics reported
- Per-stage: p50, p99, p99.9, max; capture-to-present end to end.
- Counters: drops (per mailbox), late presents, queue high-water marks, config-change counts, cuts detected, homography quality histogram.
- Overlay prediction error (px and metres).
- Event accuracy vs ground truth when available.

### 15.3 Microbenchmarks (Google Benchmark)
Each with a naive baseline and optimised variant(s), same inputs, committed results and charts:
1. Preprocess: scalar vs AVX2 vs GPU compute.
2. IoU matrix: scalar vs AVX2.
3. NMS: branchy vs branchless.
4. Association: greedy vs Hungarian.
5. SoA vs AoS track update.
6. Queue: mutex queue vs SPSC; padded vs unpadded (false-sharing demo).
7. Config publication: mutex vs seqlock vs triple buffer.
8. Allocators: malloc vs arena vs pool.
9. Possession FSM: switch vs table-driven; PGO on/off.
10. Thread affinity on/off, priority on/off: tail-latency histograms.

### 15.4 Replay mode (IN7)
A deterministic mode: decode a recorded file with FFmpeg into the same `FrameSlot` interface, with an optional "faster than real time" flag; UI commands are replayed from a log. Gives reproducible end-to-end benchmarks that live capture cannot.

---

## 16. Persistence formats

### 16.1 `session.json`
```json
{
  "version": 1,
  "match": {"id": "optional", "pitch_m": [105, 68]},
  "anchors": [{"frame": 12345, "match_ms": 0, "period": 1}],
  "attack_dir": {"period1": 1, "period2": -1},
  "config": { "conf_player": 0.35, "...": "..." },
  "team_seeds": {"A": [12.1, 30.5, -10.2], "B": [55.0, -5.2, 20.0], "REF": [80.0, 0.0, 0.0]},
  "labels": [{"frame": 20010, "track": 17, "player": 9, "source": "manual"}],
  "merges": [{"frame": 30000, "keep": 17, "absorb": 42}],
  "events": [ { "...": "Event fields, status, user edits" } ],
  "commands_log": [ { "frame": 20010, "type": "AssignLabel", "args": [17, 9] } ]
}
```

### 16.2 `events.spadl.csv` export (SPADL-compatible)
Columns: `game_id, original_event_id, period_id, time_seconds, team_id, player_id, start_x, start_y, end_x, end_y, type_id, result_id, bodypart_id`.
- Coordinates converted to SPADL space (105 x 68, bottom-left origin, left-to-right attack per team).
- Mapping: `Pass` to `pass`; `Shot` to `shot`; `Interception` to `interception`; `Clearance` to `clearance`; etc. Unknown body part defaults to `foot`. Events without a player id are exported with a flag column or filtered by a setting.
- Only `Confirmed` and `Manual` events are exported by default; `Tentative` optionally.

### 16.3 `lineup.csv`
`team,shirt,name,position`

---

## 17. Open questions, assumptions and known weak points (challenge these)

1. **Ball detection in wide shots.** Tiny, blurred, frequently occluded. Is tiled inference plus Kalman coasting plus manual re-seed sufficient for useful possession logic? What minimum recall is needed before events are worth showing?
2. **Homography under broadcast panning/zoom.** Is "reuse last H until next keypoint fit" adequate, or is per-frame propagation (optical flow on lines) needed? What error budget in metres is acceptable for pass start/end positions?
3. **Airborne balls** violate the ground-plane assumption. Is there a cheap heuristic (speed, apparent size change, trajectory arc) that flags them reliably, and should pass length and receiver be suppressed in those cases?
4. **Player identity.** The design uses manual labelling + appearance/position inheritance. Is a simple L*a*b* torso histogram enough for reasonable inheritance, or is a small re-ID embedding model justified (while still avoiding training)?
5. **Real-time inference budget.** At 60 fps there are about 16.6 ms per frame; with detector + tiled ball pass + keypoints this is unlikely. Proposed policy: detector every 2nd frame, keypoints every 5th to 10th, ball tiles on demand. Review this scheduling and propose better ones (e.g. async inference with frame-skip policy vs fixed intervals).
6. **Capture format and copies.** Is GPU-resident capture to ORT via D3D11-CUDA interop worth the complexity versus a CPU SIMD path as the first implementation? Provide the recommended order of implementation.
7. **Latency compensation.** Is constant-velocity forward prediction at draw time adequate for sprinting players and camera pans, or should camera motion be modelled separately (global motion compensation)?
8. **Match clock sync** for the precomputed xG/VAEP timeline across replays, ad breaks and stoppages. Is manual re-anchoring acceptable, or should clock OCR be in scope?
9. **Concurrency design.** Check the memory orderings in the ring, mailbox, seqlock and triple buffer; check for false sharing between `TrackHot` arrays accessed by T3 and the copy made for T5; check seqlock reader correctness with respect to the C++ memory model (non-atomic data race concerns; consider atomic relaxed copies of the payload words or `std::atomic_ref`).
10. **Hybrid CPU scheduling.** On Intel hybrid CPUs, how should threads be pinned and what should be done about Windows Thread Director behaviour (EcoQoS, power throttling)? Include `SetProcessInformation(ProcessPowerThrottling)` opt-outs.
11. **Scope.** The project could easily sprawl into data science. Propose cut lines that preserve the C++ learning value if time runs short.
12. **Licensing.** Confirm model/weight/code licences (Ultralytics AGPL, Roboflow model terms) and what may be published in a portfolio repository.

---

## 18. Milestones and acceptance tests

| M | Deliverable | Acceptance |
|---|---|---|
| M1 | WGC capture of a browser window + transparent click-through overlay + latency HUD + telemetry + black-frame (DRM) detection | HUD shows capture-to-present p50/p99; overlay excluded from capture; works fullscreen |
| M2 | Scene classifier (SIMD pitch-green ratio, cut detection) + benchmarks scalar vs AVX2 | Correctly labels wide vs close-up on a recorded clip; benchmark chart committed |
| M3 | ONNX detection + SoA tracker + boxes and IDs on overlay + config seqlock + sliders | Tracks stable across 60 s of wide play; per-stage p99 reported; no allocations on hot threads after warm-up |
| M4 | Pitch keypoints + homography + minimap | Reprojection error and quality flag shown; minimap matches wide shots by eye |
| M5 | Team clustering + labelling UI + lineup import + label inheritance + session save/load | Labels survive a simulated occlusion; session restores identically |
| M6 | Ball tracker + possession/event FSM + tentative events + confirm/reject UI + SPADL export | Events validated against StatsBomb for a chosen match; precision/recall table |
| M7 | Pitch control (SIMD) + precomputed xG/VAEP timeline playback | Heatmap at 10 to 15 Hz within CPU budget; timeline in sync via anchor |
| M8 | Replay mode (FFmpeg), PGO build, affinity/priority experiments, final benchmark report | Reproducible end-to-end numbers; before/after charts for every listed optimisation |

Testing:
- Unit tests: coordinate conversions (image/pitch/SPADL), SPSC ring (stress with thread sanitiser equivalents, e.g. a long randomised test; also run on a weakly ordered model if possible), seqlock correctness, homography solver, Kalman filters, FSM scenarios from scripted ball/player tracks.
- Property tests: tracker never exceeds `kMaxTracks`; no `TrackId` reuse; command application is idempotent for stale ids.
- Golden tests: fixed input clip in replay mode yields identical events given the same config and command log.

---

## 19. Request to the reviewing model

1. Critique this design against the stated goals and the author's learning objectives. Identify flaws, risks and over-engineering. Be direct.
2. Review all concurrency code (SPSC ring, mailbox, seqlock, triple buffer) for correctness under the C++ memory model; propose corrected code where needed.
3. Answer or take a position on each item in section 17.
4. Propose a refined build order and cut lines, with an estimated effort per milestone for one developer with AI assistance.
5. Produce concrete starting code for M1: CMake/vcpkg layout, WGC capture setup, the overlay window with correct extended styles and display affinity, the frame pool and mailbox, and the telemetry/latency HUD.
6. Suggest what, if anything, should be simplified so that this finishes in a reasonable time while still demonstrating every item in the author's checklist: cache layout, branch prediction, instruction throughput, allocators, SIMD, atomics/contention, false sharing, PGO, CPU affinity/OS effects, tail-latency measurement.
