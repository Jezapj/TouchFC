# TouchFC

A Windows soccer-broadcast overlay. The point of the project is measurable C++: cache layout, queues, allocators, SIMD (from M2), atomics, false sharing, and tail latency. The product is a way to force those into one program.

The original design is [docs/spec_v1.md](docs/spec_v1.md). The review is [docs/design_review.md](docs/design_review.md). The corrected design is [docs/spec_v2.md](docs/spec_v2.md).

## What runs today

- `core/` is a portable C++20 library: SPSC ring, mailbox, triple buffer, seqlock, slot pool, wait/wake, arena, telemetry. No Windows headers.
- `touchfc --selftest` plays a synthetic 50 fps clip. A marker is drawn from the frame id by both the source and the track thread, so the box sits on the rectangle of the same frame.
- `touchfc --replay file.mp4` does the same for a video file. Requires FFmpeg (below).
- `touchfc --overlay` lists top-level windows and captures the one you pick with Windows Graphics Capture. The overlay is click-through, topmost, and excluded from capture when the OS allows it.
- The HUD reports capture-to-present-**call**. That is not glass-to-photon time. See `scripts/presentmon_check.ps1`.

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

## Layout

- `core/include/touchfc/` primitives
- `core/tests` stress tests
- `core/bench` the comparisons in spec v2 section 8
- `app/` the Windows pipeline, replay, capture and present
- `docs/bench/` benchmark output

## Licence

AGPL-3.0-only. See [LICENSE](LICENSE). Do not commit weights or broadcast footage.
