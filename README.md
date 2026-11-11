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
   - A player and ball detector. The presets in `config/detectors.json` list the expected files: a D-FINE or RF-DETR model fine-tuned on football data (Apache 2.0 code; how to train and export one: `docs/models.md`), or, for testing, a YOLO model (Ultralytics, AGPL-3.0). This script sets up Python with Ultralytics in `touchfc_train\`, downloads or copies a YOLO11n soccer `best.pt`, and exports it to `yolo11n_soccer_960.onnx` and `yolo11n_soccer_1280.onnx`; `-Coco` also makes `yolov8n.onnx`:
     ```
     powershell -ExecutionPolicy Bypass -File scripts\fetch_yolo_models.ps1 -Source <url or path to best.pt>
     powershell -ExecutionPolicy Bypass -File scripts\fetch_yolo_models.ps1 -Coco
     ```
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

The app finds the repo root itself (it looks up from the exe for `config\detectors.json`). It reads `data\` and writes `profiles\`, `cache\` and `telemetry.csv` there.

On a live stream, open `build\windows-msvc\app\Release\touchfc.exe` (double-click, or run it with no arguments). There is no main window; TouchFC works like a game overlay:
1. A card opens over your screen. Pick what you're watching (the likeliest window is preselected and the last one is remembered), optionally a settings profile and the model, then **Start**. Play the stream full screen or large.
2. The card goes away and the stats draw over the stream. Teams come from the shirts after a few seconds of wide play and the attack direction after about 15 s; a short message on the overlay says when each is found. The change of ends at half time is followed too.
3. **Alt+T** (or the tray icon) opens the menu, docked top right of the stream:
   - **Home:** what has been found so far, **Swap teams**, **Flip direction**, **Find again**. While the menu is open the video takes clicks: left-click a player for team A, right-click for team B, and the rest follow that colour.
   - **Match:** league, date, home and away teams. This fetches lineups, form and kit colours, and makes A the home team. `--match eng.1:YYYYMMDD:home:away` does it from the command line.
   - **Advanced:** pitch calibration by clicking the video, a performance overlay, and every threshold with profile save and load.
   - **Hide** (or Esc), **Change window**, **Quit**. Quit is also on the tray icon's right-click menu.

The pitch keypoint model is found at `third_party\models\football-pitch-detection.onnx`; without it the pitch is never calibrated, so the minimap, possession and every event stay empty. `--keypoints`, `--detector "<name>"` and `--model file.onnx` override from the command line.

On a recorded match (developer view, with the old control window):
```
build\windows-msvc\app\Release\touchfc.exe --replay match.mp4 --detector "Light: D-FINE-N" --keypoints third_party\models\football-pitch-detection.onnx
```

Useful flags:
- `--pin`: puts pipeline threads on one L3 cache. Lower present latency on the dev box.
- `--mini-replay 0|1`: overrides the profile.
- `--telemetry file.csv`: writes per-stage latency on exit.
- `--seconds N`: quits after N seconds.
- `--recordable`: the overlay and menu show in screen recordings and screenshots (hidden by default).

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
