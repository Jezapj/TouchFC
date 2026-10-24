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
| M4 | Keypoints, homography, minimap, scene gate, 360 check | Reprojection error on the HUD. Wide shots match by eye within 1–2 m. |
| M5 | SIMD pitch control on a 2 m grid, also runnable on StatsBomb 360 alone | 10–15 Hz inside the CPU budget. |
| M6 | Team histogram, label UI, lineup import, session save/load | A label survives an occlusion the user can see. Session reload matches. |
| M7 | Ball, possession, then pass/shot only if recall allows, SPADL export | Precision/recall table. Export columns match SPADL. |
| M8 | PGO, `--pin` on versus off, final report | Before/after for every optimisation that was actually applied, including the ones that did not matter. |

Cut in this order if time runs out: xG timeline (already cut), label inheritance, M7 pass/shot (keep possession), M1b polish, team clustering. Do not cut M0, M1, M2, M3, M5 or M8.

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
