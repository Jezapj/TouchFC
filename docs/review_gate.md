# Review gate (before M2)

Checked against Part A of [design_review.md](design_review.md) and section 4 of [spec_v2.md](spec_v2.md). M2 has not been started.

## What was run

| Check | Result |
|---|---|
| `touchfc_tests.exe` (Release, MSVC 19.50) | 94 assertions, 17 cases, passed |
| `touchfc_bench.exe` | [bench/README.md](bench/README.md) |
| `touchfc.exe --selftest --seconds 3` | exit 0. 148 preprocess samples, 147 present samples. Capture-to-present-call p50 17.2 ms, p99 88.1 ms, max 91.6 ms |
| `touchfc.exe --overlay --seconds 2` | exit 0. Composition swap chain, DirectComposition target and the overlay window were created. No capture target was selected. |
| ThreadSanitizer | Not run on this machine. WSL has only `docker-desktop` and no clang. The `linux-tsan` and `linux-arm64` jobs in `.github/workflows/ci.yml` are the run. |

The self-test present tail is one vsync (the present thread blocks in `Present(1)`) plus a couple of slow frames at startup. It is not glass-to-photon latency. `scripts/presentmon_check.ps1` is the check for that, and it was not run: PresentMon is an external tool and there was no live display session to attach it to.

## Part A, blocking items

1. **Kalman covariance.** Not implemented. Spec v2 says a later Kalman stores `Pxx`, `Pxv`, `Pvv` per axis, and that a diagonal `P` is not a baseline.
2. **Seqlock.** `SeqLock` stores the payload as relaxed atomic `uint64_t` words (P1478 / Boehm). Odd sequence, release fence, relaxed words, release store of the even sequence. The reader acquire-loads the sequence, skips the words when it is odd, relaxed-loads the words, acquire-fences, then relaxed-loads the sequence again. `sizeof(T) % 8 == 0` and `sizeof(T) <= 4096` are `static_assert`s. `Config` is padded to a multiple of 8.
3. **Frame pool.** `SlotPool` is an atomic bitmask, not a multi-producer ring. Acquire is a CAS that clears the lowest set bit (`acq_rel`). Release is `fetch_or` (`release`). The app pool is 16 slots. Twelve deadlocks once three rings of depth 4 each hold a slot.
4. **Mailbox.** `publish` / `take` are `acq_rel` exchanges. `kEmpty` is `0xffffffff`. The producer releases the displaced index and counts a drop.
5. **Triple buffer.** Writer and reader both `exchange` with `acq_rel`. The freshness load is **acquire**, not relaxed. Initial indices are writer 0, reader 1, published slot 2 with the fresh bit clear.
6. **Wait.** `WakeSeq` spins, then `std::atomic::wait`. The spin count is a parameter. Benchmark 5 is the 0 / 64 / pure-spin comparison.
7. **D3D11 contexts.** T1 creates the capture device when WGC opens. The present thread creates the draw device. Nothing is shared across devices.
8. **WGC.** `CreateFreeThreaded`. `FrameArrived` only notifies. T1 drains `TryGetNextFrame` to the newest frame and counts the rest as drops, then copies through a staging texture. The surface is obtained with `QueryInterface` for `Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess` (it is not in the global namespace).
9. **ImGui.** One present thread, two contexts, switched with `SetCurrentContext`. The pipeline threads do not touch ImGui.
10. **UI snapshot.** T3 publishes `OverlayState` and `UiSnapshot` on separate triple buffers. The control window sends `UiCommand` on a blocking SPSC and retries when it is full.

## Smaller corrections

- `SpscRing` has `try_push_with`. Padding is a template argument (default 64). The ring is a member, not a stack temporary of a large object.
- `model_in_cpu` does not exist yet. The arena allocates 64-byte aligned blocks for when it does.
- User commands are not dropped silently. A failed push is kept and retried, and `command_warns` increments.
- `LabelMap` and the event log are not built yet. Spec v2 keeps the preallocated event buffer and the eviction rule for M6/M7.
- Dedupe hashes every 64th byte, sets `kFlagDeduped`, and still presents the frame. The present thread is vsync-paced; the source is 50 fps in the self-test.
- The HUD label is capture-to-present-call.

## Replay versus live

Replay and `--selftest` use blocking SPSC rings so a slow stage applies backpressure and the recording is reproducible. Live capture uses a mailbox into T2 and another into present, so a late consumer keeps the newest frame. That split is intentional and is in spec v2 section 5.

The present thread holds one replay slot until `anchor_wall + (pts - anchor_pts) + margin`. The anchor is the arrival of the first frame at present, so the source's own pacing is not added a second time. The overlay rectangle is written onto that same slot.

The synthetic source paints its marker with `marker_for` of the same number T1 will store as the frame id. Both sequences start at 1. They stay aligned because T1 assigns the id once per successful `decode_into`, and the source advances its counter once per successful fill.

## Hot path

After `Pipeline::start`, T1–T3 do not call `new`, grow a container, or format a string. Slot buffers, rings and the histogram are allocated at startup. A full telemetry ring drops the sample. The present thread creates the video texture on the first frame and again only if the size changes. There is no global `operator new` hook. Catch2 and the process CRT make a process-wide hook a poor fit, so this is by inspection of T1–T3, not by a trap. A hook can be added in M3 when the tracker is the thing that might allocate.

## What is not proven

- ThreadSanitizer and the arm64 weak-memory run have not executed here. The CI jobs are the proof, and they have not been run either.
- Click-through was not checked with a real click on a browser. `WS_EX_TRANSPARENT` is set, and `WS_EX_LAYERED` together with `WS_EX_NOREDIRECTIONBITMAP` is the combination most likely to make the overlay invisible or clickable on some builds. Split those styles first if that happens.
- No replay file was available, so FFmpeg decode was compiled and linked (LGPL shared build under `third_party/ffmpeg`) but not exercised on a video.
- The seqlock-versus-mutex benchmark is a hostile writer that stores on every iteration. The seqlock lost that comparison (685 ns versus 65 ns). It is the right tool when the writer is rare, which is how config is used. The false-sharing benchmark also did not show the textbook win for padding on this 4-core CPU. Both results are written up in the bench note rather than dropped.
- A failure return after the control ImGui context is created (view window or swap chain fails) does not destroy that context. The success path does.

## Independent pass

Second-pass read of the primitives and the pipeline against Part A, done after the first pass wrote `review_gate.md`. Changed nothing. Three stress runs with fresh seeds (1792808823, 1792808824 twice): 94 assertions in 17 cases, all green.

- `SeqLock::store` writes the odd sequence with `relaxed`, then a release fence, then relaxed word stores, then an even release-store. A reader's acquire-load that observes the odd value synchronises with the release fence via the fence-store rule in `[atomics.fences]`. The reader retries without touching the words. An acquire-load that observes the even value synchronises with the release-store and the word reads that follow it are consistent until the second seq load differs. The acquire fence between the relaxed word loads and the relaxed second seq load prevents the compiler from hoisting the word loads past the second seq load. This is the Boehm construction; it is correct.
- `SpscRing::try_pop` is `out = buf_[t & (N - 1)]`, a copy assignment. For small `T` that is one cache line of load/store. For large payloads later milestones should use `try_push_with` and a matching pop-with form, but M1 only pushes slot indices (`uint32_t`) and `TelemetrySample` (32 B), so this is not a hot-path concern yet.
- `Mailbox::peek` is `acquire`, consistent with `take`. It is unused in M1 but defined for M3's HUD.
- `BlockingSpsc::pop_until` requires the producer to `wake()` after setting `done`. `Pipeline::run_t1` and `run_t2` call `to_infer_.wake()` / `to_track_.wake()` after storing `t1_done_` and `t2_done_`. `request_stop` also wakes every ring, so a stuck waiter cannot sleep past shutdown.
- `SlotPool::release` does not check that the bit was previously clear. A double release silently merges. Pipeline's slot ownership is linear (one owner at a time via rings and mailboxes) so this is not reachable from the current code, but a `debug_assert` could catch future regressions. Not added.
- `run_t2` live mode has a careful shutdown sequence: it takes the mailbox, if empty checks `t1_done_`/`stop_`, then re-takes once more before breaking. That second take handles a late publish that landed between the first take and the done check. `run_t3` only uses `to_track_.pop_until` which has the same race-free pattern.
- Pipeline live-mode drops are counted in two places: capture-side (T1's `live_in_.publish` displacement) and T3's `to_track_.try_push`-fails and `live_present_.publish` displacement. The `drops` counter aggregates them. The HUD also sums `source_->capture_drops()` from WGC's internal drain, which counts frames consumed inside T1 that never left WGC's pool.
- WGC: the capture device is created inside T1 (`WgcSource::start` called from `decode_into`), separate from the present thread's device. No `ID3D11Multithread`, no sharing. The surface's dxgi-interop interface is queried from `::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess` via `IUnknown::QueryInterface` on `winrt::get_abi(frame.Surface())`, which is the correct path: `winrt::IInspectable::as<T>` does not know this SDK-only interface.
- Present-thread ImGui contexts are leaked on an early-return failure after `ImGui::CreateContext`. The success path destroys both. The failure paths only fire if a window or swap chain cannot be created, which terminates the process. Left as-is.
- `Config` is 48 B. The compile-time `static_assert(sizeof(Config) % 8 == 0)` is the proof.

## M2 state

`touchfc_tests` after M2: 114 assertions in 22 cases, Release, MSVC 19.50, three fresh-seed runs all green. `touchfc.exe --selftest --seconds 3` exits 0 with four stub detections drawn on top of the marker box per frame.

| What | Where | Notes |
|---|---|---|
| Portable letterbox + CHW float32 | [`core/include/touchfc/preprocess.hpp`](../core/include/touchfc/preprocess.hpp) | Scalar reference, AVX2 fast path, runtime dispatcher. BGRA source, RGB plane order, 114/255 padding, nearest-neighbour. |
| YOLOv8 decode | [`core/include/touchfc/yolo.hpp`](../core/include/touchfc/yolo.hpp) | `[1, 4+nc, A]` channel-major, undoes letterbox, sorts by score. |
| Greedy NMS | [`core/include/touchfc/nms.hpp`](../core/include/touchfc/nms.hpp) | Per-class, in place, O(n^2). Fine at a few hundred boxes. Hungarian stays cut. |
| Detector interface | [`app/src/detector.hpp`](../app/src/detector.hpp) | `StubDetector` always available. `OrtDetector` compiles when `third_party/onnxruntime/include/onnxruntime_cxx_api.h` exists. |
| Pipeline wiring | T2 calls `Detector::run` on each slot; T3 clears dets when `show_boxes` is 0; present reads `slot.dets[0..det_count]` and draws per-class colours. |
| Benchmark | [`docs/bench/README.md`](bench/README.md) | AVX2 1.7x scalar on 1280x720 to 640x640. |

Not done, by choice:
- **YUV-direct preprocess.** FFmpeg already does YUV->BGRA through `sws_scale` on T1 of the replay path, and the BGRA letterbox is 1 ms. A direct `AV_PIX_FMT_YUV420P` reader skips one copy and saves bandwidth; it is the first thing to add if profiling shows preprocess as a tail-latency problem.
- **ORT bring-up.** ONNX Runtime 1.20.1 and Ultralytics YOLOv8n v8.4.0 (AGPL-3.0, confirmed on the v8.4.0 `LICENSE`) live under `third_party/` and are gitignored. `--replay` of the Ultralytics bus still through `yolov8n.onnx` logged `classes=80` and 4 person boxes on three frames. A live match window has not been captured in this run.
- **Execution-provider selection in the HUD.** Only "stub" or "ort" is reported today.

Hot-path status:
- `StubDetector::run` touches four slots in a fixed-size array. No allocation.
- `OrtDetector::run` reuses pre-sized `input_tensor_` and `output_buffer_`. ORT's own `Run` is the only path that could allocate internally, which is outside this process's control; the Session is created once in `prepare`.
- `FrameSlot` grew from ~24 B of metadata plus a BGRA buffer to also carry `std::array<Detection, 128>` (3 KiB). The slot is 16 x per pool, so this adds ~48 KiB on top of pixel memory.

## M9 state

`touchfc_tests`: 538 assertions in 45 cases after M9 to M11, Release, MSVC 19.50, all green. `touchfc.exe --selftest --seconds 3` exits 0. `BM_ImageGmc` 0.86 ms per 1080p frame, `BM_LineRefine` 0.10 ms ([bench](bench/README.md)).

| What | Where | Notes |
|---|---|---|
| Landmark table | [`pitch_landmarks.hpp`](../core/include/touchfc/pitch_landmarks.hpp) | Order checked against Roboflow's `SoccerPitchConfiguration` vertex list. Positions are the real IFAB markings (16.5 m box, 40.32 m wide, 9.15 m circle), not Roboflow's not-to-scale template squeezed onto 105 x 68 (box corners were up to 1.1 m off, the centre circle an ellipse). Test: a synthetic `H` maps all 32 landmarks round-trip within 1 cm. |
| Projection guard | [`homography.hpp`](../core/include/touchfc/homography.hpp) | Fits are unit-norm with `w > 0` on the fitted points; `apply_homography` returns false at or above the horizon. Detections more than 3 m off the pitch get `pitch_ok = 0` and are skipped by possession, team inheritance and pitch control. |
| Image GMC | [`image_gmc.hpp`](../core/include/touchfc/image_gmc.hpp) | Grass and line pixels only, so players and the scorebug do not pull the match. Scale + translation. Test: recovers a 1% zoom and a (37, -18) px pan; a grey frame is rejected. Locks on 346/347 and 222/223 frames of the two elevated test clips. |
| Line tracking | [`line_fit.hpp`](../core/include/touchfc/line_fit.hpp) | Markings projected through `H^-1`; each sample searches its image normal for a bright ridge between grass flanks; refit is point-to-line (one linear row per match) with a light prior so a direction the visible lines do not pin stays put. Keeps a fit alive on a third of the pitch, between keypoint runs. Test: on a painted synthetic frame of one penalty area, a 9 px + 1% drift comes back within 15 cm. |
| Line support | same | Share of in-view marking samples that sit on a painted line. A keypoint fit is snapped to the lines, then must score `line_min_support` (0.35). Test: true fit > 0.8, a fit 6 m / 4 m off < 0.35. |
| Fit validation and smoothing | [`pitch_calib.hpp`](../core/include/touchfc/pitch_calib.hpp) | Rejects on error, < 4 inliers, one quadrant, main-camera orientation (image right = +x, image up = far side: resolves left/right and near/far mirrors of the symmetric markings), horizon below the top 10% of player boxes, bottom centre > 10 m off, low line support. Blends four image-space corners by `h_smooth`; a jump over `h_jump_m` waits for a second agreeing fit. Holdover cap `h_holdover`; a lost GMC lock (cut) invalidates. |
| Keypoint cadence | `pipeline.cpp` T2 | Scene classified on T2; keypoints every `kp_interval` wide frames, none in other views (set pieces and close-ups pause the fit). |
| No model, no positions | `pipeline.cpp`, `present.cpp` | Five-point template deleted. Without `--keypoints` the minimap says "not calibrated" until 4+ landmarks are clicked on the frame thumbnail (`SetAnchor`; `ResetHomography` clears). Manual is a fallback only; line tracking then keeps it. |
| Minimap | `present.cpp` | 105:68, scales with the window, `PushClipRect`, pitch markings drawn. |
| Reprojection view | `present.cpp` | Toggle: magenta pitch lines and landmark crosses through `H^-1`, yellow raw keypoints with ids. |
| `--pitch-dump path.csv` | `present.cpp` | Per-frame H, keypoints, verdict, line support and play state, written on the present thread. For offline overlays. |

Found on the way: `SeqLock<Config>` default-constructs to zero words and nothing stored a `Config` at start, so every in-class default read as 0 until a UI change. `Pipeline` now stores `Config{}` in its constructor.

Test clips (`test_clips/`, gitignored): two elevated views of an amateur ground and one low pitch-side set piece. The Roboflow keypoint model is out of its training distribution there: it outputs a whole-pitch layout on bare grass. Before line support those fits were accepted; now 280 of 347 frames of `middle_pan_to_right` and every frame of `right_side_pitch` reject them (support 0.15 to 0.29). One fit is accepted where that ground really has a vertical line with a circle centred on it; its markings are non-standard (overlaid small-sided pitches), so it decays. The set-piece clip never runs keypoints (not a wide view).

Not verified:
- **The done-when check on broadcast footage.** Needs a Premier League / LaLiga style main-camera clip with standard markings, `--keypoints`, through an attack and a pan. The amateur clips cannot pass it with any standard-pitch model.
- Bench numbers on the target machine; this one runs CPU inference at about 2 fps.

## M10 state

| What | Where | Notes |
|---|---|---|
| Config v2 | [`types.hpp`](../core/include/touchfc/types.hpp) | 136 B, grouped Detection / Pitch / Teams / Play state / Events / Overlay. New: `nms_iou`, scene fractions, `green_margin`, `green_min`, `team_max_dist`, `team_min_px`, play-state and event thresholds, overlay flip settings, `panel_side`, `mini_replay`. |
| Field table | [`config_fields.hpp`](../core/include/touchfc/config_fields.hpp) | One row per field (key, label, group, type, range). Drives the Tuning tab, reset per group and profiles. A test checks every default sits inside its range. |
| Tuning tab | `present.cpp` | One slider per field, with the live signal beside it where one exists (fit error, holdover age, motion cost, line support, grass share and scene, view angle, player height, play state, box count). Changes store into the seqlock; T2/T3 read it every frame. |
| Profiles | [`config_json.hpp`](../core/include/touchfc/config_json.hpp) | `profiles/<name>.json` (gitignored) by key, on the present thread at click time. Unknown keys ignored, missing ones default, visible fields clamped. Last used loads at start (`profiles/last.txt`). Test: JSON round trip equals the struct byte for byte. nlohmann-json 3.11.3 by FetchContent (URL + SHA256). |
| Wired thresholds | `pipeline.cpp`, `ort_detector.cpp`, `scene.hpp`, `team.hpp` | Detector score floor and NMS IoU via `Detector::configure`; scene classifier, GMC, line search and torso colour use `green_margin` / `green_min`; `nearest_team` leaves a player unassigned beyond `team_max_dist` (referee, keeper). |

Still hard-coded, by choice (internal, not a kit or pitch difference): tracker gating (`iou_lo`, `conf_high`, miss counts), possession miss count (5), team inheritance radius and window, GMC search ranges, RANSAC inlier distance (2 m), pose checks (10 m bottom, 3 m `pitch_ok`). `pass_min_dist_m`, `ball_flight_min_mps`, panel flip and mini replay fields exist for M13 to M15.

## M11 state

| What | Where | Notes |
|---|---|---|
| Gate | [`play_state.hpp`](../core/include/touchfc/play_state.hpp) | `PlayGate`: Dead when any available signal holds `dead_enter_ms` (400); InPlay only after every available signal is clear for `live_enter_ms` (700). Test: a 240 ms pan does not trigger, a 480 ms close-up does, a signal returning inside the 700 ms keeps it dead, an unavailable signal cannot. Replay runs the hysteresis on video time. |
| Signals | `pipeline.cpp` `update_play` | View angle (pitch long axis at the image centre via `H^-1`; > 90 is a reverse angle), median player height, scene class, ball > 1 m off the lines. Scorebug presence is defined but unavailable until M12 learns the ROI. |
| While Dead | `pipeline.cpp`, `present.cpp` | Possession spell closes and none starts, pitch control stops, the tracker keeps running. HUD shows "dead ball" with the reasons. Minimap follows `minimap_dead_mode`: Freeze (last in-play positions, dimmed, "paused") or Hide. |
| Event log | `pipeline.cpp`, `possession.hpp` | Each Dead interval is an `EventType::DeadBall` record; SPADL export writes it as type 22 (outside the SPADL list, like possession's 21). Event log raised from 64 to 4096 records. |

Not verified: the done-when replay check on throw-ins, corners, a substitution and a broadcast replay. That needs broadcast footage; the test clips only cover a set-piece angle.

## M12 state

`touchfc_tests`: 647 assertions in 50 cases after M12 and M13, all green. `--selftest` exits 0. The Release app now writes a PDB (`/Zi /DEBUG`, no runtime cost) so a crash offset maps to a line.

| What | Where | Notes |
|---|---|---|
| `MatchInfo` | [`match_info.hpp`](../core/include/touchfc/match_info.hpp) | POD: names, codes, colours, form, formation, squad (jersey, position, starter, out, season apps / goals / assists / shots), score, clock, scorebug ROI, provider post-match stats. T7 publishes it to two triple buffers (T3 and the UI; each has one reader). |
| Scorebug | [`scorebug.hpp`](../core/include/touchfc/scorebug.hpp) | ROI finder: cells in the top 20% that are detailed and do not change (a burned-in graphic changes 0.00 per frame, a slow drone pan 0.25+, so `roi_stable_max` defaults to 0.2), bridged through stable cells across glyph gaps, largest wide cluster. Parser: `CODE n - n CODE`, en dash and `ARS1` forms, `mm:ss` clock. Confident after 3 identical reads. Tests for all three. |
| OCR | [`ocr_win.cpp`](../app/src/ocr_win.cpp) | `Windows.Media.Ocr`, user-profile languages, crop upscaled 2 to 3x when small. T3 copies the ROI into one of two preallocated crops (no allocation) at 1 Hz until the teams are known, then every 10 s; T7 reads it. |
| Provider | [`espn.hpp`](../core/include/touchfc/espn.hpp), [`services.cpp`](../app/src/services.cpp), [`http_win.cpp`](../app/src/http_win.cpp) | ESPN shapes checked against live responses (October 2026). Scoreboard (date, then a day either side) gives the fixture, colours and form; summary gives lineups, formation and post-match team stats; team roster gives season stats and injuries. WinHTTP, cached under `cache/espn/` (team list 7 days, scoreboard 10 min, rosters 12 h, summary polled every 2 min until the lineup is out, then kept). Parsers tested on hand-made fixtures in the live shapes. |
| Code mapping | `services.cpp`, [`data/team_codes.json`](../data/team_codes.json) | Scorebug codes to provider ids: overrides file first, then the league's abbreviations. Unknown codes go to the user (status line). |
| Match tab | `present.cpp` | League, date, home/away search over the team list, confirm (manual always wins), refresh lineup, clear; scorebug box shown and drawable on the frame, or back to auto; lineup tables with season stats, injured players in red; manual substitutions. |
| Kit colours | `pipeline.cpp` `seed_kits` | Provider colour (or alternate) seeds team A (home) / B (away) only when each is within `kit_seed_max_dist` of at least 3 tracked players and the two differ. A click seed still wins. |
| Goals | `pipeline.cpp` `update_match` | A score rise on the scorebug is logged as a `Goal` event and credits the last shot's TFC rating. |
| `--match league:YYYYMMDD:home:away` | `present.cpp` | Same as confirming in the Match tab; for replays of past matches. |

Checked end to end here:
- `--match eng.1:20251025:363:366`: fixture CHE v SUN found, lineups loaded, rosters and summary cached (`cache/espn/`, gitignored).
- A synthetic scorebug ("CHE 1 - 0 SUN 23:41", burned into a test clip with ffmpeg, kept out of the tree): ROI found at 64,16 400x96, OCR read, codes mapped, teams identified.

Not verified: a real broadcast scorebug (fonts, gradients, transparency) and its kit colours. The Match tab has not been driven by hand here.

## M13 state

| What | Where | Notes |
|---|---|---|
| Events | [`events.hpp`](../core/include/touchfc/events.hpp) | Spells from the possession holder; ball speed smoothed over 3 to 5 samples. Pass = release, flight, next holder after `pass_min_dist_m`; same team completed, other team incomplete (plus an interception when taken in flight); a kicked ball that goes dead is incomplete. A fast ball toward a goal mouth from inside `shot_zone_m` is only a shot candidate until the flight ends: a team-mate receiving it makes it a pass (crosses, through balls), anything else makes it a shot. Key pass: completed pass into the area, or followed by a shot within `key_pass_window_s` (counted once). |
| Team stats | same, HUD | Passes, completed, %, possession % (in-play time with possession), shots, key passes; labelled "vision estimate". |
| TFC rating | same | Spec 11.5: xT from Karun Singh's published `open_xt_12x8_v1.json` (fetched, not recalled). Weights are `Config` fields (Tuning > Rating). The holder's rating shows on the HUD. |
| Export | `possession.hpp` | SPADL type pass 0, interception 10, shot 11 with result (a shot succeeds only as a goal); outside the list: possession 21, dead ball 22, scorebug goal 23. |
| Validation | `present.cpp` `write_validation` | On exit, when the provider has post-match stats and vision counted something: `docs/bench/m13_validation_<event>.csv` (vision beside provider for passes, pass %, possession, shots). |

Tests drive passes, an interception, out of play, a shot gathered by the keeper, key passes, a goal credit and a short tap on synthetic timelines.

Not verified: the validation CSV for a real match. It needs a broadcast replay with `--keypoints --match ...` through enough play for passes to register; expect low pass counts (cuts, replays, ball misses).

## M14 state

| What | Where | Notes |
|---|---|---|
| Stats panel | `present.cpp` `draw_stats_panel` | Codes, score and clock from `MatchInfo`; possession, passes, completion, shots, key passes per team in team colours; "vision estimate". Before any in-play time, or after 30 s of dead ball (half time): last-5 form and up to three unavailable players per team. Moved out of the HUD. |
| On-ball card | `draw_card` | Team colour, track id, time on the ball this spell, TFC rating. With a label (Match tab > On-ball player, a lineup pick for the current holder): shirt number and name, our passes / completion / shots, one season line. Labels are by track id, so a re-acquired track starts unlabelled and its rating starts again. Jersey-number OCR is cut (11.6 stretch). |
| Ball-avoid flip | [`panel_flip.hpp`](../core/include/touchfc/panel_flip.hpp), `layout_panels` | Stats top (home right) with the mini replay under it; minimap bottom (home left); card bottom (home right, stacked above the minimap when they share a side). Obstacles: ball box, on-ball player box, scorebug ROI, grown by `flip_margin_px`. A hit for `flip_react_ms` (150) moves the panel if the mirror is clear; it holds for `flip_hold_ms` (2 s), then goes home once home has been clear for the react time. Slides over 0.25 s. `panel_side` forces Left / Right. Test drives react, slide, hold, return and the both-sides-covered case. |

Not verified here: the done-when on a broadcast replay (panels never cover the ball longer than the hold, no flicker). The test clips run but I cannot watch the panels; the state machine is covered by the test.

## M15 state

| What | Where | Notes |
|---|---|---|
| Thumbnail ring | [`mini_replay.hpp`](../core/include/touchfc/mini_replay.hpp) | 480x270 AVX2 box filter (1.2 ms at 1080p, 0.63 ms at 720p) into a 150-entry ring (6 s at 25 fps; every second frame above 30 fps). Per-entry sequence: the reader drops a frame the writer reached during the copy, so the writer never waits. Written on the present thread, not T3 (see `docs/bench/README.md`: on T3 it doubled the track stage's p99). |
| Clip capture | `pipeline.cpp` `request_clip`, `services.cpp` `serve_clips` | T3 posts a request for every shot and every key pass (SPSC, drops when full). T7 waits until the ring reaches t + 1.5 s, copies t - 4 s to t + 1.5 s (12 ms) into a free clip, or over one that has not started playing. Skipped when a pre-event frame was a dead ball (a cut or a broadcast replay inside the build-up; no replay classifier exists, so the whole dead state stands in for it), or when the frames cover under 2.5 s of the window (playback is by timestamp, so a 3 fps present rate still plays). |
| Playback | `present.cpp` `draw_replay` | Oldest ready clip, half speed, two loops, 0.5 s fade; a newer clip takes over after one full play. Pass / shot line drawn from the event's pitch positions through each frame's stored homography. Main tab button "mini replay: last 5 s" plays the last window for a check. |
| Budget | `write_thumb`, HUD | HUD shows ms per thumbnail. If drops plus late frames exceed `replay_budget_pct` (5%, Tuning > Overlay; 0 never) over a 10 s window, the mini replay pauses for the next window and the HUD says so; it resumes once a window is back under. `--mini-replay 0|1` overrides the profile. |

Telemetry before and after: `docs/bench/README.md` (selftest; no added p99 on T1 to T3). Not verified: a shot on a broadcast replay producing a clip within 2 s (the test clips have no shots); the copy (12 ms) and T7's 50 ms poll put it far inside the limit. The playback window has not been watched here.

## Team ratings and forced teams (after M15)

| What | Where | Notes |
|---|---|---|
| Team ratings | `events.hpp` `team_ratings` | Attack, build-up, defence, control, overall, 0 to 10 (5 level), in the stats panel with xG. Shares against the other team with priors, from new `TeamStats` counters: xG (distance and post angle), xT gained, progressive passes, recoveries and high recoveries, final-third possession time. Replaces the per-player TFC on the card and HUD (tracks do not survive re-acquisition). |
| Forced teams | `pipeline.cpp` `assign_teams` | `team_force` (Teams tab, on): once both kits are seeded, every visible box is put on the nearer kit except one spare, the model's referee or else the box least like both kits. Unteamed player boxes are grey, not cyan. |

Not verified on a broadcast: rating weights and xG fit are hand-set; the spare box choice with two assistant referees in shot (one is forced onto a team).

## Licence and weights

The repo licence is AGPL-3.0-only (`LICENSE`). FFmpeg is the BtbN LGPL shared build, not the GPL one. `scripts/fetch_models.ps1` refuses to download weights. No broadcast footage is in the tree.
