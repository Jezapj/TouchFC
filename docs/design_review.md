# Design review of the soccer overlay spec

Reviewed document: [spec_v1.md](spec_v1.md) (the original system specification).
The corrected design is [spec_v2.md](spec_v2.md). This note is the critique. It is written for the person implementing the later milestones, not as a status report.

The original spec is a strong skeleton. The thread split, the "drop oldest" live policy, the POD config, the SPADL boundary, and the decision to measure every optimisation are the right shape for a performance portfolio. Several of the concurrency sketches are undefined behaviour or will deadlock, and the Kalman layout as drawn cannot estimate velocity. Those have to be fixed before any of the later milestones are worth building. A few of the planned microbenchmarks will not show up in a 22-player frame, and the spec should say so instead of pretending they are the bottleneck.

## Blocking flaws

### 1. A diagonal Kalman covariance does not learn velocity

`TrackHot` stores a diagonal `P`. The Kalman gain on velocity is `P_xv / (P_xx + R)`. If the position-velocity covariance is forced to zero, that gain is zero, and velocity stays at its initial value forever. Latency compensation, pass speed and the shot trigger all read that velocity.

Keep a 2x2 block per axis: `Pxx`, `Pxv`, `Pvv` for x, y, w and h, and the same for the ball. It is still a structure of arrays and it still vectorises. Do not ship the diagonal form even as the "naive" baseline, because the baseline would be silently wrong rather than merely slow.

### 2. The seqlock, as sketched, is a data race

Copying a non-atomic `Config` while the writer is in `store` is undefined in C++. Fences do not make a non-atomic memcpy safe. The payload has to be relaxed atomic words (this is the point of P1478, which exists because the usual seqlock is UB).

`sizeof(Config)` must be a multiple of 8. The padding is explicit in [spec_v2.md](spec_v2.md) and `static_assert`ed. The implementation is `touchfc::SeqLock`:

- writer publishes an odd sequence with a relaxed store, a release fence, relaxed word stores, then a **release** store of the even sequence
- reader acquire-loads the sequence, skips the words when it is odd, relaxed-loads the words, an acquire fence, then a relaxed load of the sequence, and retries unless the two sequence values match

The odd-sequence check happens before the word loads. That is equivalent to the spec sketch and does less wasted traffic. A single writer is a hard rule. Readers do not write shared lines.

### 3. The free-slot ring is not SPSC, and 12 slots deadlock

Slots return to the pool from the capture thread (dropped mailboxes), the track thread (live drops) and the present thread (after display). That is multi-producer. A SPSC free ring will lose or double-issue indices.

The pool is an `std::atomic<uint64_t>` bitmask, 64 slots maximum. Acquire is a CAS that clears the lowest set bit (`acq_rel`). Release is `fetch_or` (`release`). The acquire synchronises with the previous owner's release, so the new owner sees that owner's writes to the slot **only for the pool handshake**. The bytes inside the slot still need the queue or mailbox that carries the index.

Twelve slots are not enough. Three queues of depth 4, plus the slot each stage is holding, need 16. With 12 the pipeline can fill every queue and then wait for a slot that will never be released. The code uses 16.

### 4. The mailbox must name who recycles

`Mailbox::publish` exchanges the new index and returns the displaced one. `kEmpty` is `0xffffffff`. If the displaced value is not empty, the producer releases it and counts a drop. The consumer `take()` exchanges empty. Both exchanges are `acq_rel`, so the slot payload written before publish is visible after take.

### 5. Triple buffer

Indices 0, 1 and 2 start as writer, reader and middle. The packed word is the middle index plus bit 2 (`kFresh = 4`).

- Writer: `prev = idx.exchange(w | kFresh, acq_rel); w = prev & 3`
- Reader: acquire-load, and if `kFresh` is set, `r = idx.exchange(r, acq_rel) & 3`

The freshness load is **acquire**, not relaxed. A relaxed load may observe a stale set bit after this thread has already cleared it, and the exchange would then give away the buffer the reader is still drawing. `acq_rel` on both exchanges is what publishes the slot bytes and what stops the writer reusing a slot the reader has not finished.

`try_read()`'s pointer is valid until the next successful `try_read()`. `write_slot()` is valid until `publish()`.

### 6. "No mutexes" is not a wait strategy

Four pinned threads spinning on empty queues will heat the CPU and drop GPU clocks, which makes the inference tail worse. The queues spin a bounded number of pauses, then `std::atomic::wait` / `notify_one` (`WaitOnAddress` on Windows). A notify that happens before the wait is not lost, because the waiter reloads the generation. Shutdown sets the done flag **then** calls `wake()`, so a sleeper rechecks. The spin-versus-sleep comparison is a benchmark, not the production default.

### 7. One D3D11 immediate context, one thread

`ID3D11DeviceContext` is not thread-safe. Sharing it between capture and present either corrupts the device or hides a lock inside `ID3D11Multithread`. Capture (T1) creates its own device and copies to a staging texture. Present/UI (one thread) creates the device that draws. Frames cross the boundary as CPU bytes in the slot pool. There is no cross-device sharing in M1. NT handles and `ID3D11Fence` are out of scope.

### 8. WGC: the callback does not own the frame

The frame pool is `Direct3D11CaptureFramePool::CreateFreeThreaded`. `FrameArrived` only notifies a `WakeSeq`. T1 is the only thread that calls `TryGetNextFrame`, and it drains to the newest frame, counting the rest as drops. It copies into a staging texture and then into the slot. `SystemRelativeTime` is a 100 ns QPC value and is converted into `steady_clock` nanoseconds at that edge (`qpc_100ns_to_app_ns`). The session disables the cursor and, when the property exists, the capture border.

### 9. ImGui is on the present thread only

Dear ImGui's context is not shared across threads. The control window and the view window are both owned by the present thread, each with its own `ImGuiContext`, switched with `SetCurrentContext`. Pipeline threads never call ImGui. Input cannot stall T1–T3.

### 10. The UI needs more than sixteen recent events

`OverlayState` is the draw snapshot. A second triple buffer carries `UiSnapshot` (frame id, drop count, command-queue warning). The full event log for later milestones belongs in that snapshot, not in the draw state. User commands travel the other way on an SPSC of `UiCommand`. A failed push is retried next frame and counted. Commands are never dropped silently.

## Smaller corrections

- The original SPSC orderings (release head, acquire tail, private caches) are right. `try_push_with` fills the slot in place. Padding is a template argument so 64 versus 128 can be measured. Rings of large `T` are heap allocated; the ring object contains the buffer.
- `alignas(64) float*` aligns the pointer, not the allocation. The frame buffer comes from `std::make_unique` at startup. Scratch that needs alignment uses `Arena`, which allocates with `align_val_t{64}`.
- `LabelMap` of 256 entries will overflow a 90-minute match if every `TrackId` is appended. Later milestones compact it when a track dies. The event log is a fixed buffer, not an allocating `vector`, on the track thread.
- Historical broadcasts are 25 or 50 fps. The pipeline runs at the source rate. Duplicate captured frames are hashed (every 64th byte) and counted. They are still presented, so a replay does not stutter, but the counter is what a later inference stage will use to skip work. The present thread still runs at the display rate.
- The success metric is capture-to-present-**call**. The HUD says so. PresentMon measures present-to-photon. `scripts/presentmon_check.ps1` prints the command. A photodiode is the only way to get true glass latency; we do not pretend otherwise.
- MSVC has no ThreadSanitizer. `core/` has no Windows headers. The `linux-clang-tsan` preset and the CI job are how the rings are checked on a weak memory model. This machine's only WSL distro is docker-desktop, which has no compiler, so TSan is CI rather than a local run.

## Where the time actually goes

At 22 to 64 tracks, `TrackHot` is a few kilobytes and lives in L1. A structure-of-arrays track update, an AVX2 IoU kernel and a profile-guided state machine will not move the frame time. They stay as **scaled** microbenchmarks (sweep N) with a written note that they are noise at match scale. That note is part of the portfolio: knowing when not to optimise.

The work that moves bytes or FLOPs:

- 1080p colour conversion into a letterboxed CHW tensor. In replay, start from YUV420 and do not round-trip through BGRA. M1 still produces BGRA because that is what the swap chain samples. The YUV path is M2.
- YOLO decode and NMS over the channel-major `[1, 4+nc, 8400]` tensor.
- The scene classifier on a downscale.
- Pitch control, which can be developed on StatsBomb 360 freeze frames with no vision at all.
- Profile-guided optimisation of the preprocess and of T3 as a whole, not of the state machine in isolation.

## Positions on the original open questions

1. **Ball.** Tiled detection plus coasting is not enough for automatic passes unless wide-shot ball recall is at least about 80% on a hand-labelled two-minute clip. Ship possession (nearest player, hysteresis) first. Gate pass and shot on that measurement.
2. **Homography.** Reusing the last homography fails because broadcast cameras pan. Estimate an inter-frame global motion `G`, set `H_t = H_fit * G(fit -> t)`, and refit on a cut, on low `G` confidence, or about every 0.5 s. The error budget is 1–2 m in a wide shot, checked against StatsBomb 360 freeze frames.
3. **Airborne ball.** A flag from the image-space vertical arc and from detection-size inconsistency is enough to suppress the end location. Time-box it at a day. Do not build a 3D ball model.
4. **Identity.** A torso L\*a\*b\* histogram separates teams. It does not identify players. Identity is a manual label plus pitch-space continuity. A pretrained re-identification network is a stretch goal and its licence has to be checked first.
5. **Scheduling.** Run the detector asynchronously on the latest frame. Keep `det_interval` as an override, not the default. Keypoints run on a cut, on global-motion failure, or on a timer. The ball tile runs only when the full-frame ball confidence is low.
6. **Capture to inference.** The CPU SIMD preprocess comes first. A host tensor works with CUDA, TensorRT, DirectML and the CPU provider, which is what "any Windows desktop" requires, and it is the SIMD showcase. GPU preprocess is a stretch goal. D3D11-CUDA interop is cut.
7. **Latency compensation.** Constant velocity in image space mixes player motion with the camera pan. Global motion is required, and it is the same `G` as the homography propagation. In replay the question mostly goes away: the present thread holds frame N until its PTS is due and draws the overlay stored on that same slot. Alignment is exact because the two did not come from different frames.
8. **Match clock.** Replay uses the video PTS plus one anchor, so pause and seek (once seek exists) do not drift. The browser path stops the clock when no new frame arrives, and the user can re-anchor. Scoreboard OCR is a stretch goal.
9. **Concurrency.** Covered above. The code, the tests and spec v2 section 4 are the same rules.
10. **CPU scheduling.** The development machine is Ryzen, not a hybrid Intel part. `SetThreadSelectedCpuSets` pins T1–T3 to one logical processor per core on the largest last-level cache when `--pin` is passed. It is off by default so a benchmark is not silently different from a normal run. `ProcessPowerThrottling` execution-speed throttling is opted out always. The present thread uses MMCSS `Playback`. Replay pacing uses `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`. E-core policy is not implemented; there is nothing to pin away from.
11. **Cut lines.** See spec v2. Do not cut the core library, the replay present path, the CPU preprocess, the corrected tracker, pitch control or the benchmark report. Those are the performance portfolio. Cut the xG timeline (already cut), label inheritance, pass/shot if ball recall is poor, overlay polish, then team clustering.
12. **Licensing.** roboflow/sports code is widely described as MIT. The weights that repository points at are Ultralytics YOLOv8, which Ultralytics distributes under AGPL-3.0. Both must be re-checked against the upstream repos before a weight file is fetched. `scripts/fetch_models.ps1` refuses to download. This repository is AGPL-3.0-only, which is compatible with that if the weights stay AGPL. Weights and broadcast footage are never committed. FFmpeg is linked as the official LGPL shared build, not the GPL one.

## What was over-built

- Hungarian assignment. Greedy matching is the implementation. Hungarian can be a later benchmark if the greedy matcher is actually wrong.
- Two config mechanisms in the app. The app uses the seqlock. The triple buffer is still implemented and benchmarked, because the present path needs it for `OverlayState`.
- Large pages, PGO on the state machine alone, the xG/VAEP timeline, clock OCR, D3D11-CUDA interop, GPU preprocess.
- OpenCV is allowed later for `findHomography` and optical flow as a baseline. A self-written DLT/RANSAC replaces it only as a measured swap. It is not a dependency of M1.

## Replay versus the browser

Replay is the primary path. It uses blocking SPSC queues so a benchmark file is not missing frames. Live capture uses the mailbox, because the browser will not wait. Both share `FramePool`, `FrameSource` and the present thread. The browser overlay is M1b: `WS_POPUP` with `WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW`, a premultiplied DirectComposition swap chain, and `WDA_EXCLUDEFROMCAPTURE`. ImGui draws straight alpha into an offscreen target; a pixel shader premultiplies into the composition chain.

`WS_EX_LAYERED` together with `WS_EX_NOREDIRECTIONBITMAP` is the combination most likely to fail on a given Windows build. If the overlay is invisible or clicks do not pass through, that pair is the first thing to split. The control window is a normal window, so labelling never depends on the click-through surface.
