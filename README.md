# TouchFC

A Windows soccer-broadcast overlay. The point of the project is measurable C++: cache layout, queues, allocators, SIMD (from M2), atomics, false sharing, and tail latency. The product is a way to force those into one program.

The original design is [docs/spec_v1.md](docs/spec_v1.md). The review is [docs/design_review.md](docs/design_review.md). The corrected design is [docs/spec_v2.md](docs/spec_v2.md).

## What runs today

- `core/` is a portable C++20 library: SPSC ring, mailbox, triple buffer, seqlock, slot pool, wait/wake, arena, telemetry. No Windows headers.
- `touchfc --selftest` plays a synthetic 50 fps clip. A marker is drawn from the frame id by both the source and the track thread, so the box sits on the rectangle of the same frame.
- `touchfc --replay file.mp4` does the same for a video file. Requires FFmpeg (below).
- `touchfc --overlay` lists top-level windows and captures the one you pick with Windows Graphics Capture. The overlay is click-through, topmost, and excluded from capture when the OS allows it.
- The HUD reports capture-to-present-**call**. That is not glass-to-photon time. See `scripts/presentmon_check.ps1`.
- With the models (below): player and ball detection, tracking, team colours, pitch calibration and minimap, pitch control, play state, scorebug OCR, match data, pass / shot / key pass events, team ratings, the stats overlay and the mini replay. Milestones and their state: `docs/spec_v2.md` section 9. Performance: `docs/bench/final_report.md`.

## Build

Requirements: Windows 10/11, CMake 3.24+, Visual Studio 2026 (or 2022, if you change the preset generator), the Windows 10 SDK (10.0.26100 or newer, for C++/WinRT).

```
cmake --preset windows-msvc
cmake --build build/windows-msvc --config Release
build\windows-msvc\Release\touchfc_tests.exe
build\windows-msvc\Release\touchfc_bench.exe --benchmark_min_time=0.1s
build\windows-msvc\Release\touchfc.exe --selftest --seconds 3
```

FFmpeg, only for `--replay`:

```
powershell -File scripts\fetch_ffmpeg.ps1
cmake --preset windows-msvc
```

The script fetches the official LGPL shared build. The GPL build is not used. DLLs are copied next to `touchfc.exe`.

An optional vcpkg manifest is `vcpkg.json` (ports checked against the registry: `catch2`, `benchmark`, `hdr-histogram`, `imgui`, `ffmpeg`). The default build does not require vcpkg. Preset `windows-vcpkg` does, and building FFmpeg from that manifest takes a long time.

Model weights are not downloaded. `scripts/fetch_models.ps1` explains why.

## Set up on a new PC

What you need:
- Windows 10 or 11, 64-bit, on a CPU with AVX2 (most CPUs from 2015 on).
- Visual Studio 2026 with the "Desktop development with C++" workload. With Visual Studio 2022, change `generator` in `CMakePresets.json` to `Visual Studio 17 2022`.
- The Windows SDK 10.0.26100 or newer, from the Visual Studio installer.
- CMake 3.24 or newer, and Git.
- Python with `pip`, only if you export the detector model yourself (step 4).

Steps, in PowerShell:

1. Clone the repo and open PowerShell in it.
   ```
   git clone <repo url> TouchFC
   cd TouchFC
   ```
2. Fetch FFmpeg, the official LGPL build. It is needed for `--replay`.
   ```
   powershell -ExecutionPolicy Bypass -File scripts\fetch_ffmpeg.ps1
   ```
3. Install ONNX Runtime. Download `onnxruntime-win-x64-1.20.1.zip` from https://github.com/microsoft/onnxruntime/releases. Extract it so that `third_party\onnxruntime\include\onnxruntime_cxx_api.h` exists. Without it the app builds with a stub detector that draws fake boxes.
4. Put the models in `third_party\models\`. Weights are never committed; read each licence first (see `scripts/fetch_models.ps1`).
   - `yolov8n.onnx`, the player and ball detector. Make it with `pip install ultralytics`, then `yolo export model=yolov8n.pt format=onnx imgsz=640`. A football-specific detector with player, goalkeeper, referee and ball classes, exported the same way, detects the ball much better.
   - `football-pitch-detection.onnx`, the 32-point pitch keypoint model from https://github.com/roboflow/sports, exported to ONNX.
5. Configure, build and test.
   ```
   cmake --preset windows-msvc
   cmake --build build/windows-msvc --config Release
   build\windows-msvc\Release\touchfc_tests.exe
   build\windows-msvc\app\Release\touchfc.exe --selftest --seconds 3
   ```
   The selftest should exit with code 0 and no errors.

## Run it

Run from the repo root. The app reads `data\` and writes `profiles\`, `cache\` and `telemetry.csv` there.

On a recorded match:
```
build\windows-msvc\app\Release\touchfc.exe --replay match.mp4 --model third_party\models\yolov8n.onnx --keypoints third_party\models\football-pitch-detection.onnx
```

On a live stream:
```
build\windows-msvc\app\Release\touchfc.exe --overlay --model third_party\models\yolov8n.onnx --keypoints third_party\models\football-pitch-detection.onnx
```
Without `--keypoints` the pitch is never calibrated, so the minimap, possession and every event stay empty.

First run on a match:
1. Choose the window showing the stream (a browser or player), then **Start capture**. Play the stream full screen or large.
2. **Match tab:** choose the league, date, home and away teams. This fetches the lineups, form and kit colours. You can also start with `--match eng.1:YYYYMMDD:home:away`.
3. **Main tab:** if the boxes are not blue and red yet, click one player of each team and press **A** or **B**. Everyone else then follows their kit colour. Use **attack direction** if team A attacks to the left.
4. Wait for a wide shot. The HUD should show `pitch wide ... kp` with a small fit error, and dots on the minimap.
5. **Tuning tab:** adjust thresholds, for example player and ball confidence, and the pass and shot sliders in Events. Then **save profile**. The last profile used loads on the next start.
6. The stats panel, the on-ball card and the mini replay appear on the overlay. **mini replay: last 5 s** in the Main tab tests the replay.

Useful flags:
- `--pin`: puts pipeline threads on one L3 cache. Lower present latency on the dev box.
- `--mini-replay 0|1`: overrides the profile.
- `--telemetry file.csv`: writes per-stage latency on exit.
- `--seconds N`: quits after N seconds.

Optional profile-guided build, which makes a smaller exe with the same latency (see `docs/bench/final_report.md`):
```
powershell -ExecutionPolicy Bypass -File scripts\pgo.ps1
build\pgo\app\Release\touchfc.exe ...
```

On a low-spec PC, ONNX Runtime on the CPU runs at only a few frames a second, which makes passes and shots harder to catch. A faster CPU, or a GPU build of ONNX Runtime, helps more than any slider.

## Layout

- `core/include/touchfc/` primitives
- `core/tests` stress tests
- `core/bench` the comparisons in spec v2 section 8
- `app/` the Windows pipeline, replay, capture and present
- `docs/bench/` benchmark output

## Licence

AGPL-3.0-only. See [LICENSE](LICENSE). Do not commit weights or broadcast footage.
