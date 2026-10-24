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
- **Full ORT bring-up.** The code compiles against `third_party/onnxruntime` when present and the user runs with `--model`. Session options, output allocation and preprocess tensor are already there. It has not been exercised against a model this session because weights are not committed (YOLOv8 is claimed AGPL by Ultralytics; a licence check must land before the first run).
- **Execution-provider selection in the HUD.** Only "stub" or "ort" is reported today.

Hot-path status:
- `StubDetector::run` touches four slots in a fixed-size array. No allocation.
- `OrtDetector::run` reuses pre-sized `input_tensor_` and `output_buffer_`. ORT's own `Run` is the only path that could allocate internally, which is outside this process's control; the Session is created once in `prepare`.
- `FrameSlot` grew from ~24 B of metadata plus a BGRA buffer to also carry `std::array<Detection, 128>` (3 KiB). The slot is 16 x per pool, so this adds ~48 KiB on top of pixel memory.

## Licence and weights

The repo licence is AGPL-3.0-only (`LICENSE`). FFmpeg is the BtbN LGPL shared build, not the GPL one. `scripts/fetch_models.ps1` refuses to download weights. No broadcast footage is in the tree.
