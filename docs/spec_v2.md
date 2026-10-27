# TouchFC spec v2

Corrects [spec_v1.md](spec_v1.md). The reasoning is in [design_review.md](design_review.md). Where this file and the code disagree, the code in `core/` and `app/` is the authority for M0–M1b, and this file is the authority for milestones not built yet.

## 1. Goals

Unchanged from spec v1, with two renames:

- S1 is **capture-to-present-call**, p99 under 100 ms on a mid-range NVIDIA GPU, reported at p50 / p99 / p99.9. Glass latency is a PresentMon (or photodiode) measurement and is not claimed by the HUD.
- Replay of a recorded file is the primary demo. A browser overlay is the second path and uses the same `FrameSource`.

Non-goals are unchanged: no training, no DRM circumvention, no live redistribution, no claim of broadcast-grade accuracy, no real-time xG from vision.

## 2. Build

- C++20. MSVC `/O2 /arch:AVX2` for the app. `core/` also builds with clang.
- Default configure fetches Catch2 3.7.1, Google Benchmark 1.9.1 and HdrHistogram_c 0.11.8, and Dear ImGui 1.91.8. FFmpeg is the official **LGPL** shared build from `scripts/fetch_ffmpeg.ps1` (not the GPL build).
- `vcpkg.json` is an optional manifest. Verified port names: `catch2`, `benchmark`, `hdr-histogram`, `imgui` (features `dx11-binding`, `win32-binding`), `ffmpeg`. Later milestones, not in the manifest yet: `implot`, `nlohmann-json`, `simdjson`, `opencv4`, `cppwinrt`, `wil`. ONNX Runtime GPU and DirectML builds come from the official release or NuGet, not from an assumed vcpkg port.
- Presets: `windows-msvc`, `windows-vcpkg`, `linux-clang-tsan`.
- Tests: `touchfc_tests`. Benchmarks: `touchfc_bench`. App: `touchfc`.

## 3. Threads

| Thread | Role | Waits |
|---|---|---|
| T1 | Pull `FrameSource`, hash, black-frame test, enqueue | Source, and the pool if it is empty |
| T2 | M1 is a timestamp. Later: ONNX Runtime | Input queue |
| T3 | Marker / later tracker, events, publish snapshots | Input queue |
| Present | Video or overlay window, control window, ImGui | Vsync (`Present(1)` on the view; the control window presents without vsync when the view is visible) |
| T6 | Drain telemetry rings into HdrHistogram, below-normal priority | 50 ms sleep |
| T7 | Services (from M12): OCR, HTTP, disk cache, replay-clip copy. Below-normal. May allocate. | Request SPSC from T3 / present |

T1–T3 are above-normal. `--pin` selects one logical processor per core on the largest last-level cache via `SetThreadSelectedCpuSets`. Power throttling (execution speed) is off. The present thread joins MMCSS `Playback`.

There is one D3D11 device on T1 for capture and a different device on the present thread. No shared immediate context.

## 4. Memory orders

These are the rules the tests cover.

### SPSC ring

`head_` is stored `release` after the slot is written and loaded `acquire` before the slot is read. `tail_` is stored `release` after the consumer is done with the slot and loaded `acquire` before the producer reuses it. The cached remote index is private. A stale cache only causes a spurious empty or full, which the refresh corrects. Capacity is the power-of-two `N` (full when `head - tail == N`).

`BlockingSpsc` adds a generation. `pop_until(done)` returns false when `done` is set and the ring is empty. The producer stores `done` with `release`, then `wake()`. The waiter's acquire on the generation sees `done`.

### Mailbox

`publish` and `take` are `exchange` with `acq_rel`. `publish` returns the displaced index. `kEmpty` is `0xffffffff`. The producer recycles a displaced index and counts a drop.

### Triple buffer

See the review. Freshness is an acquire load. Both exchanges are `acq_rel`. One writer, one reader.

### Seqlock

Atomic `uint64_t` words, single writer. Odd sequence, release fence, relaxed words, release store of the even sequence. Reader acquire-loads, skips the copy when odd, relaxed-loads the words, acquire fence, relaxed-loads the sequence, retries on mismatch. `sizeof(T) % 8 == 0` and `sizeof(T) <= 4096`.

### Slot pool

Bitmask. Successful CAS is `acq_rel`. `release` is `fetch_or` with `release`. At most 64 slots. The pool does not, by itself, publish the frame bytes; the queue or mailbox does.

### WakeSeq

`notify` is `fetch_add` release plus `notify_one`. `wait_from` spins, then `atomic::wait`. A notify that landed first is visible because the generation already differs.

## 5. Frame path

`FramePool` holds 16 slots, sized once at startup.

Replay and the synthetic source (`--selftest`, 1280x720 at 50 fps):

`T1 -> blocking SPSC -> T2 -> blocking SPSC -> T3 -> blocking SPSC -> present`

The present thread holds a frame until `anchor_wall + (pts - anchor_pts) + margin`. The anchor is the arrival of the first frame at present. The overlay rectangle is stored on that slot by T3 (`marker_for(frame_id)`), and the synthetic source paints the same rectangle. They match because they are the same frame, not because of a prediction.

Live capture (`--overlay`):

`T1 -> mailbox -> T2 -> try-push SPSC (drop if full) -> T3 -> mailbox -> present`

T1 drains WGC to the newest frame. Present takes the mailbox once per vsync.

`t_capture` is `now_ns()` when the source did not set it. WGC sets it from `SystemRelativeTime`. The present sample is `Stage::Present` with `t_begin = t_capture` and `t_end` at the present call. T6 records it in an HdrHistogram (1 ns to 60 s, 3 significant figures).

Duplicate frames set `kFlagDeduped` and increment a counter. They are still shown. Near-black frames set `kFlagBlack`.

## 6. Data that later milestones must not break

`Config` is a trivially copyable multiple of 8 bytes and lives in a `SeqLock`. M1 uses `show_boxes`, `delay_margin_ms`, `version`.

`UiCommand` is the spec v1 command POD. M1 handles `ApplyAttackDir` (a round trip onto an atomic the control window reads) and retries a full queue. `SetAnchor`, labels and event edits are defined and ignored until their milestone.

Kalman state, when it is added, stores `Pxx`, `Pxv`, `Pvv` per axis. A diagonal `P` is not an acceptable baseline.

Internal pitch space: metres, origin at the centre spot, `+x` to the right-hand goal, `+y` to the top touchline. SPADL export: origin bottom-left, 105x68 by default, each team rotated so it attacks `+x`. `internal_to_spadl` / `spadl_to_internal` are the only conversion and are unit tested. A 180 degree flip negates both axes.

Global motion `G` is one component used three ways: tracker prediction, homography propagation (`H_t = H_fit * G`), and any residual image-space compensation. Replay present does not need image-space prediction for alignment.

## 7. Windows capture and the overlay

- Free-threaded frame pool. Callback notifies only.
- T1 owns the capture device, staging texture and `CopyResource` / `Map`.
- `IsCursorCaptureEnabled = false`. `IsBorderRequired = false` when the property exists.
- Overlay styles are the set in the review. Composition swap chain is premultiplied, flip-sequential. ImGui renders straight alpha to an offscreen target; `ps_premul` writes the swap chain.
- `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`. The HUD reports whether the call succeeded.
- The control window is a normal HWND. Click-through is never required for labelling.
- Black frames are a HUD warning, not a crash. DRM content is expected to be black.

## 8. Telemetry

Per-producer SPSC of `TelemetrySample`. A full ring drops the sample. T6 never runs on the hot path's critical section. CSV columns: `stage,count,p50_ns,p99_ns,p999_ns,max_ns`. The app writes the path given by `--telemetry` (default `telemetry.csv`) on exit.

Benchmarks in `core/bench/bench_main.cpp`:

1. SPSC versus mutex queue throughput
2. False sharing, adjacent atomics versus 64-byte versus 128-byte padding
3. Config publication: seqlock versus mutex versus triple buffer, with a writer running
4. `malloc`/`free` of 64-byte blocks versus an arena
5. `WakeSeq` round trip at 0, 64 and 1 000 000 spin iterations

Results go in `docs/bench/` with the compiler and the logical CPU count. Scaled microbenchmarks for IoU, NMS and the tracker are added with the stages that have something to measure, and they say when the result is noise at 22 players.

## 9. Milestones

Estimates are one developer, full time, with an assistant. M0–M1b are in this tree.

| M | What | Done when |
|---|---|---|
| M0 | `core/` primitives, stress tests, benchmarks | `touchfc_tests` passes. TSan job exists. |
| M1 | Replay and `--selftest`, delayed present, HUD, telemetry | A moving marker sits on the rectangle of the same frame. HUD shows capture-to-present-call. |
| M1b | WGC, click-through DComp overlay, display affinity, dedupe, black frame | Overlay tracks a chosen window. Clicks pass through. Affinity is reported. |
| M2 | ONNX detector, AVX2 preprocess from BGRA, YOLO decode and NMS, Detector interface, stub for the default build | Boxes on screen. Preprocess benchmark committed. **Done** with the stub and the preprocess fast path; ORT is wired behind `TOUCHFC_HAS_ORT` and takes a model via `--model`. AVX2 letterbox is 1.7x scalar on the dev box. YUV-direct preprocess is deferred: FFmpeg already converts to BGRA via `sws_scale`, and the BGRA fast path is in budget. The YUV path returns if replay dominates the profile. |
| M3 | ByteTrack-lite, 2x2 Kalman, centroid GMC | **Done.** Stable ids on the stub boxes. Track stage stays on the stack. Image-based GMC is later. |
| M4 | Homography, scene gate, minimap | **Done.** `--keypoints` runs a 32-point YOLOv8 pose model every 8th frame, fits with RANSAC, and holds that homography between fits by the centroid pan. Without the file, the minimap still uses the wide-shot template. |
| M5 | SIMD pitch control on a 2 m grid, also runnable on StatsBomb 360 alone | **Done.** 53×34 Spearman time-to-intercept, AVX2 over each row. A 360 frame is just positions and a team bit; no video. The minimap waits for team labels (M6) before painting it. |
| M6 | Team histogram, label UI, lineup import, session save/load | **Done** without lineup import or session save/load. A control-window click sets team A or B from the torso colour; other tracks follow the nearer colour. A new id within 8 m and the same colour keeps the team for 2 s. |
| M7 | Ball, possession, then pass/shot only if recall allows, SPADL export | **Done** for the ball and possession. COCO class 32 is kept as the ball. A player within 1.5 m holds it; a change closes a spell. Pass and shot are not emitted. `spadl.csv` uses the SPADL columns, with type_id 21 for a possession spell. |
| M9 | Minimap and homography fix, reprojection debug view | Section 11.1. |
| M10 | Config v2, tuning panel, profiles | Section 11.2. |
| M11 | Play-state gate (in play / dead) | Section 11.3. |
| M12 | Match setup: scorebug OCR or manual entry, stats API, lineups, kit colours | Section 11.4. |
| M13 | Pass / shot events, team stats accumulators | Section 11.5. |
| M14 | Stats overlay, ball-avoid flip, on-ball player card | Section 11.6. |
| M15 | Mini replay of key passes and shots | Section 11.7. |
| M8 | PGO, `--pin` on versus off, final report. **Runs last, after M15.** | Before/after for every optimisation that was actually applied, including the ones that did not matter. |

Order is M9 → M10 → M11 → M12 → M13 → M14 → M15 → M8. M9 first because every positional feature after it reads the homography. M10 before M11 so every later threshold lands on a slider the day it is added.

Cut in this order if time runs out: xG timeline (already cut), jersey-number OCR (11.6), M15 mini replay, on-ball rating (keep the card without a rating), label inheritance, shots (keep passes), M1b polish. Do not cut M0, M1, M2, M3, M5, M8, M9, M11.

## 10. Tests already required

- SPSC order under a concurrent producer, including a full ring and `try_push_with`.
- Mailbox: every published token is taken or returned as a drop, never both, never neither.
- Triple buffer and seqlock: a concurrent writer cannot tear a multi-word record.
- Slot pool: an index is never owned twice. Acquiring `N` times then fails.
- Wake: a notify that happens first is not lost. A sleeper wakes.
- Blocking SPSC: a full queue unblocks on pop. `pop_wait` returns when stop is set and the ring is empty.
- Arena alignment, reset, overflow.
- SPADL round trip and attack-direction flip.
- Black-frame and stride hash.
- HdrHistogram percentile of a known ramp.

Hot path after `Pipeline::start`: no `new`, no `std::string`, no `std::vector` growth. Startup allocations (the pool, the rings, the D3D device, the decoder) are allowed.

## 11. Phase 2: live match analysis (M9 to M15)

Applies to everything after M7. Executed in Claude Code, one milestone per session or commit series. Read [CLAUDE.md](../CLAUDE.md) first.

### 11.0 Rules for all of phase 2

- **Analysis stays continuous and online**, as it is now: every frame goes through T1 to T3, every stat updates incrementally on T3. Nothing waits for the end of the match. Post-match work (export, API comparison) is extra, never a substitute.
- The hot-path rule (section 10) still holds for T1 to T3. Anything that allocates, blocks or is slow (OCR, HTTP, JSON, disk, clip copy) runs on T7. T3 talks to T7 through a request SPSC; T7 answers through a result SPSC or a seqlock / triple buffer, never by T3 waiting.
- Every new threshold is a `Config` field with a slider (M10). No magic numbers in T3 code that a user might need to move for a different kit or pitch.
- Every new state machine gets a Catch2 test driven by a synthetic signal sequence. Every new hot loop that might matter gets a bench entry in `docs/bench/`.
- New shared data: `MatchInfo` (team names, 3-letter codes, kit colours, score, clock, lineup) and `TeamStats` are trivially copyable PODs. `MatchInfo` is written by T7 and read by T3 and present through a triple buffer. `TeamStats` rides in `OverlayState` / `UiSnapshot`.
- API keys come from environment variables. Never in the tree, the log, a URL query string, or `session.json`.

### 11.1 M9: minimap and homography fix

Observed: player icons land far outside the minimap and positions do not match the picture. Causes in the current code (`pipeline.cpp` around lines 470 to 517, `present.cpp` around 445 to 474):

1. **The template fallback is fiction.** Without `--keypoints`, five fixed image points are mapped to the whole pitch (frame bottom corners = near corner flags). A broadcast wide shot shows a third to a half of the pitch, so every position is wrong.
2. **Pan compensation is not camera motion.** `pan_x_ / pan_y_` accumulate the player-centroid shift and are subtracted in pixel space. Players move as well as the camera, zoom is ignored, and if keypoint fits keep failing the pan accumulates forever.
3. **No projection guard.** `apply_homography` does not reject points where the homogeneous `w <= 0` (above the horizon) or that land far off the pitch, so a point near the vanishing line goes to hundreds of metres.
4. **No drawing clip.** `map()` in present has no clamp or clip rect.
5. **Fits are not checked beyond rms <= 2.5 m**, and there is no temporal smoothing, so one bad fit replaces a good one.
6. **Keypoint id order is unverified.** If the 32-point model's order differs from `pitch_landmarks.hpp`, every fit is wrong in a way RANSAC can still accept. Check this first.

Work, in order:

- **Reprojection debug view (do first).** Toggle in the control window: draw the pitch lines warped by `H^-1` onto the video, plus the raw keypoints with ids. This is how item 6 is checked and how every later fix is judged.
- Verify the keypoint table against the model's dataset definition. Fix the table. Add a test that a synthetic `H` maps all 32 landmarks round-trip within 1 cm.
- **No model, no positions.** Without `--keypoints`, the minimap shows "not calibrated" unless the user does a manual calibration: click 4 or more known landmarks on the frame in the control window (`UiCommand::SetAnchor`, already defined). Delete the five-point template.
- **Image GMC.** Replace player-centroid pan for homography propagation with camera motion from the frame: phase correlation (or a coarse block match) on a downscaled grayscale grass mask, giving translation plus scale. AVX2, benchmarked. Compose as `H_t = H_fit * G_t^-1` with `G` a 3x3 similarity, per section 6. Holdover is capped (default 50 frames); past the cap `pitch_valid = 0`.
- **Run keypoints more often in wide shots**: every 4th frame by default (`Config.kp_interval`).
- **Fit validation.** Reject a fit if: rms > `Config.h_max_rms_m` (default 1.5), fewer than 4 inliers spread over at least two image quadrants, the determinant sign differs from the previous good fit, the horizon line (`w = 0`) is below the top 10% of the player boxes, or the image bottom-centre maps outside the pitch by more than 10 m.
- **Temporal smoothing.** Smooth the four projected corners of the visible pitch area (in image space) with an exponential filter and refit from them, so `H` cannot jump. A fit that moves the prediction by more than `Config.h_jump_m` is held for one more fit before it is accepted.
- **Projection guard.** `apply_homography` returns false for `w <= eps`. Points outside the pitch by more than 3 m are not drawn and are not used by pitch control or possession.
- **Minimap drawing.** `PushClipRect`, size scales with the window, aspect 105:68. On a dead ball (M11) the minimap follows `Config.minimap_dead_mode`: `Freeze` (default: keep the last in-play positions, dimmed, with a "paused" label) or `Hide`.

Done when: on a replay clip with `--keypoints`, the warped pitch lines sit on the painted lines within a few pixels across a full attack and through a pan, and no icon is ever drawn outside the minimap rectangle.

### 11.2 M10: config v2, tuning panel, profiles

Kit colours and pitches differ (white kit on a sunlit pitch, green kits, rain), so thresholds must be adjustable live.

- Extend `Config` (bump `version`, keep `% 8 == 0` and `<= 4096`). New fields, grouped as the panel shows them:
  - Detection: `conf_player`, `conf_ball`, `nms_iou`.
  - Pitch: `conf_keypoint`, `kp_interval`, `h_max_rms_m`, `h_jump_m`, green-mask margin and minimum value (used by the scene classifier and GMC).
  - Teams: colour-distance threshold, minimum torso pixels.
  - Play state (M11): `view_angle_max_deg`, `player_height_max_frac`, `dead_enter_ms`, `live_enter_ms`, `scorebug_match_min`.
  - Events (M13): `poss_dist_m`, `pass_min_dist_m`, `ball_flight_min_mps`, `shot_speed_mps`.
  - Overlay (M14, M15): panel side `Auto/Left/Right`, flip margin px, flip hold ms, `minimap_dead_mode`, mini replay on/off.
- Control-window "Tuning" tab: one slider per field, each with the **live value of the signal it thresholds** beside it (current view angle, median player height fraction, current fit rms, green fraction). The user moves the threshold against a number they can see.
- Reset-to-default per group.
- Profiles: save / load the whole `Config` as `profiles/<name>.json` (nlohmann-json, on the present thread at click time, not the hot path). The last used profile loads at start. This also covers M6's missing session save/load for config.

Done when: every threshold used by T2/T3 is in `Config`, changes take effect within one frame, and a saved profile reloads identically (test: JSON round trip equals the struct byte for byte).

### 11.3 M11: play-state gate

Stats stop when the ball is out of play, during substitutions, broadcast replays, close-ups and crowd shots, and resume when play restarts.

`PlayState { InPlay, Dead }` on T3. Several signals, because each fails somewhere:

| Signal | Dead when | Needs keypoint model | Notes |
|---|---|---|---|
| View angle | Angle of the pitch long axis in the image (from `H^-1` at the image centre) exceeds `view_angle_max_deg` | Yes | The main camera is side-on; behind-goal and corner angles are not. Also gives the reverse-angle sign flip, which would otherwise mirror attack direction. |
| Player size | Median player box height / frame height > `player_height_max_frac` | No | Close-ups, bench, subs board. Cheap and robust. |
| Scene class | `Scene::Close` or `Other` | No | Existing classifier, now with tunable thresholds. |
| Scorebug presence | Scorebug ROI correlation with its learned template < `scorebug_match_min` (M12 learns the ROI) | No | Most broadcasts hide the scorebug during replays. Normalised cross-correlation on a small grayscale crop, every frame, no OCR. |
| Ball off pitch | Ball pitch position outside the lines by more than 1 m | Yes | Only trusted when `pitch_valid`. |

Rule: enter `Dead` when any signal holds for `dead_enter_ms` (default 400). Return to `InPlay` only when **all** available signals are clear for `live_enter_ms` (default 700). The hysteresis stops flicker on a quick pan.

On the two options proposed (angle versus player size): use both, plus the scorebug. Player size and scorebug work on day one with no keypoint model. The angle is the best signal once M9 is solid, and it is the only one that catches "wide, but from behind the goal".

While `Dead`: the possession spell closes, possession time and event detection stop, pitch control stops, the tracker keeps running (so ids survive a short cut), the minimap follows `minimap_dead_mode`, and the HUD shows "dead ball". Each Dead interval is written to the event log as its own record so exports show the gaps.

Done when: tests drive the state machine with synthetic signal sequences (a short pan does not trigger, a 400 ms close-up does, return needs all-clear), and on a replay clip the state is visibly correct on throw-ins, corners, a substitution and a broadcast replay.

### 11.4 M12: match setup

At start, the app identifies the two teams so it can fetch lineups and kit colours.

**From the scorebug (default).**
- Find the ROI: over the first few seconds of wide frames, search the top 20% of the frame for a stable, high-contrast region that barely changes between frames. The user can drag the ROI in the control window instead.
- OCR on T7 with `Windows.Media.Ocr` (C++/WinRT, already in the build, offline, no new dependency). Once a second until confident, then once every 10 s for score and clock.
- Parse `([A-Z]{2,4})\s*(\d+)\s*[-:]?\s*(\d+)\s*([A-Z]{2,4})` (also accept an en dash). Left code is home, right is away: `ARS 1 EVE 0` is Arsenal at home to Everton. Also parse a `mm:ss` clock into `match_ms`.
- Confident = the same codes in 3 consecutive reads. A score change is logged as a goal event.
- Map codes to teams with `data/team_codes.json` (code to provider team id and name, per league). Ambiguous or unknown codes go to the user.

**Manual (fallback and override).** Control window: home and away search boxes backed by the provider's team search, a date (today by default) and a confirm button. Manual always wins over OCR.

**Stats provider.** Interface `StatsProvider` on T7, so the source can change:
- `ApiFootballProvider` (default; api-sports.io v3, key in `TOUCHFC_API_FOOTBALL_KEY`, sent as a header). Fixture by date and team, `fixtures/lineups`, `fixtures/statistics`, `fixtures/players` (ratings), `teams` search. Lineups are usually posted 20 to 40 minutes before kick-off. Check endpoint and field names against the live docs when implementing.
- `FileProvider` reading `lineup.csv` (spec v1 16.3) and an optional stats JSON, for offline replay and tests.
- HTTP with WinHTTP (no new dependency). JSON with nlohmann-json (add to `vcpkg.json` / FetchContent).
- Every response is cached under `cache/`, keyed by fixture and endpoint. A replay of a past match uses the cache and costs no quota. The free tier is about 100 requests a day, so the budget is 1 fixture lookup, 1 lineup, then live statistics and player ratings no more often than every 3 minutes (configurable), stopping at full time.

**What setup gives the pipeline.**
- `MatchInfo`: names, codes, starting XI and bench, shirt numbers, formation.
- **Kit colours.** If the lineup response carries team colours, seed the team A/B colour centres from them so team assignment works without the M6 click. The click still overrides.
- Lineup confirmation in the control window (XI per side, formation). Substitutions from the provider update the lineup.

Done when: on a replay with a visible scorebug the teams are identified without input; with a key the lineup and colours load and are cached; without a key or network the manual / file path works and nothing blocks T1 to T3.

### 11.5 M13: events and team stats

Builds on M7 possession. Online, on T3, only while `InPlay`.

- **Ball in flight**: ball speed (pitch space, smoothed over 3 to 5 frames) > `ball_flight_min_mps` and no player within `poss_dist_m`.
- **Pass**: a possession spell by player P ends, the ball is in flight, and the next spell starts with player Q after the ball travelled at least `pass_min_dist_m`. Same team = completed. Other team = incomplete (logged as an interception when the receiver was close to the ball's line). A dead ball after P's spell = incomplete (out of play).
- **Shot**: ball in flight toward the goal mouth (projected line crosses the goal line between the posts plus a margin) faster than `shot_speed_mps`, starting inside a shooting zone (default 35 m from goal). Outcome is unknown unless the score changes (goal) or play goes dead.
- **Key pass**: a completed pass followed by a shot by the same team within 5 s, or a completed pass into the penalty area.
- `TeamStats` per team: passes attempted, completed, completion %, possession % (in-play time with that team in possession over in-play time with any possession), shots, key passes. Per-track counters for the same, used by the on-ball card.
- Events go into the preallocated event log (spec v1 12.1) and the SPADL export: pass = type 0, shot = type 11, with result.
- **Validation**: after replaying a match that has an API cache, write our team stats beside the provider's `fixtures/statistics` (passes, pass %, possession, shots) to a CSV in `docs/bench/`. Report the error honestly; vision pass counts will be low because of camera cuts and replays.

Done when: tests cover pass, interception, out-of-play and shot on synthetic possession timelines, and the validation CSV exists for at least one match.

### 11.6 M14: stats overlay

On the overlay (and the replay view), three ImGui panels drawn on the present thread from `OverlayState`:

1. **Team stats panel**: team codes and score (from `MatchInfo`), possession %, passes, completion %, shots. Labelled as a vision estimate, to keep the non-goal of broadcast accuracy honest. Optional line with provider live stats when available.
2. **On-ball card**: the player in possession. Tiers:
   - Always: team colour, track id, time on the ball this spell.
   - If the track has a label (manual label, label inheritance, or a lineup pick in the control window): name, shirt number, our per-player counters (passes, completion), and the provider's live rating if it has one.
   - Stretch (cut first): jersey-number OCR on the torso crop of medium shots with `Windows.Media.Ocr` on T7, matched to the lineup, to label tracks automatically. Expect low recall.
   - "Rating" means the provider's rating. A homemade score from vision events is not shown as a rating; at most as "involvement".
3. **Minimap** (M9).

**Ball-avoid flip.** Each panel has a preferred side. If the ball's screen box (or the on-ball player's box) intersects the panel rectangle grown by the flip margin for more than 150 ms, the panel moves to the mirrored side. It stays there for at least the flip hold time (default 2 s) before it may return, and slides rather than jumps. The scorebug ROI is also an obstacle. The side can be forced in the tuning panel.

Done when: on replay, panels never cover the ball for longer than the hold time, do not flicker, and the stats update live.

### 11.7 M15: mini replay

- **Thumbnail ring.** After T3, each frame is downscaled to 480x270 BGRA with an AVX2 box filter into a preallocated ring: 6 s at 25 fps (every second frame at 50 fps) is 150 frames, about 78 MB. Benchmarked.
- **Clip capture.** On a key pass or shot, T3 sends T7 the ring range from `t - 4 s` to `t + 1.5 s`. T7 waits until `t + 1.5 s` has been written, then copies the range into one of two preallocated clip buffers. Copying, rather than pinning ring slots, keeps the ring's writer free of any reader.
- **Playback.** Present uploads clip frames to one texture and plays them in a small window at half speed, loops twice, then fades. A new clip replaces the old one only after the old one has played once. When `pitch_valid`, draw the pass or shot line on it from the stored pitch positions.
- Skip a clip whose window overlaps a Dead interval caused by a broadcast replay (the broadcaster is already showing it).
- Off by default when the machine misses the frame budget; the HUD shows its cost.

Done when: a shot on a replay clip produces a mini replay within 2 s of the end of its window, with no allocation and no added p99 on T1 to T3 (telemetry before and after in `docs/bench/`).

### 11.8 Open items for the user

- Stats provider: API-Football is assumed (free tier, lineups, ratings). A key is needed for live use in M12; without one, the file provider and manual entry still work.
- `minimap_dead_mode` defaults to `Freeze`. Switch to `Hide` if that reads better on screen.
