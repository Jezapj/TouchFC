# Final performance report (M8)

Release build, MSVC 19.50, CMake 4.2. Dev box: 4 cores at 2695 MHz, L2 512 KiB per core, L3 4 MiB. This is a low-spec machine; ONNX Runtime on its CPU runs at about 3 fps, so pipeline telemetry below uses the stub detector, which leaves our own code on the critical path.

Telemetry method: `scripts\telemetry_runs.ps1`, `--selftest --seconds 20 --mini-replay 0`, three runs per build, medians reported. Raw rows: [m8_runs.csv](m8_runs.csv). Present is capture-to-present-**call** and is set by the delayed-present pacing; it moved 15 to 67 ms between runs of the same binary, so read it as a range, not a point.

## Every optimisation applied, before and after

| Optimisation | Before | After | Mattered? |
|---|---|---|---|
| SPSC ring instead of a mutex queue (M0) | 122 ns per push/pop | 8.38 ns | Yes, 15x. Every stage handoff uses it. |
| Seqlock `Config` instead of a mutex (M0) | 64.9 ns load (mutex) | 685 ns under a writer that never pauses | Not in that benchmark. Kept because the real writer stores a few times a second and readers never write a shared line. |
| Triple buffer for snapshots (M0) | (mutex copy) | 5.48 ns `try_read` | Yes, for the present thread. |
| Frame arena instead of malloc (M0) | 17.9 us / 256 blocks | 1.42 us | Yes, 12x; scratch only. |
| Spin-then-sleep wake (M0) | 1751 ns round trip (sleep at once) | 175 ns (64 spins) | Yes, close to a pure spin (127 ns) without holding a core. |
| 64 / 128 B padding of hot atomics (M0) | 11.3 ms / 100k ping-pong, same line | 14.6 / 15.1 ms | **No, worse on this CPU.** Kept for other CPUs; reported, not adjusted. |
| AVX2 letterbox preprocess (M2) | 1.76 ms scalar | 1.02 ms | Yes, 1.7x, bounded by the 4.7 MiB of output writes. |
| AVX2 SAD image GMC on a 160x90 reduce (M9) | (no image GMC) | 0.86 ms per frame | Fits: under 5% of the 20 ms frame. |
| Line refit on ~300 samples (M9) | (keypoint fit only) | 0.10 ms per call | Fits; runs every 2nd wide frame. |
| AVX2 thumbnail box filter (M15) | (none) | 1.2 ms at 1080p, 0.63 ms at 720p | Fits on the present thread. |
| Thumbnail moved T3 -> present thread (M15) | track p50 1.0 ms, p99 2.3 ms | p50 0.54 ms, p99 1.1 ms | Yes; on T3 it doubled the track stage. |
| LTCG (`/GL /LTCG`, M8) | see below | see below | No measurable change. |
| PGO (`/GENPROFILE` -> `/USEPROFILE`, M8) | see below | see below | Within noise on latency; exe 12% smaller. |
| `--pin` to one L3 (M8) | see below | see below | Lower and tighter present latency; stages unchanged. |

## LTCG, PGO and `--pin`

Stage p50 / p99 in ms, median of three runs.

| Build | preprocess (T1) | inference (T2, stub) | track (T3) | present (call) | exe size |
|---|---|---|---|---|---|
| default | 0.250 / 1.40 | 0.060 / 0.17 | 0.507 / 1.02 | 48.4 / 63.1 | 932 KB |
| default `--pin` | 0.241 / 1.44 | 0.062 / 0.19 | 0.514 / 1.01 | 27.1 / 51.1 | |
| LTCG | 0.238 / 1.35 | 0.063 / 0.17 | 0.521 / 1.01 | 31.9 / 48.8 | 910 KB |
| PGO | 0.251 / 1.26 | 0.062 / 0.19 | 0.488 / 1.01 | 36.3 / 57.6 | 818 KB |
| PGO `--pin` | 0.242 / 1.31 | 0.063 / 0.21 | 0.509 / 1.02 | 24.9 / 40.7 | |

PGO training (`scripts\pgo.ps1`): 30 s of `--selftest` and 30 s of `--replay` on each of the three test clips, stub detector, mini replay on. Four `.pgc` files merged, 0% of counts dropped.

What this says:

- **LTCG and PGO do not move the hot stages.** Track p50 is 4% lower under PGO (0.488 vs 0.507 ms), which is inside the spread between runs of one binary (0.496 to 0.516). Preprocess and track are dominated by memory traffic on the 8 MiB frame and by SIMD loops that are already inlined, which is where PGO has little left to do. The real gain is size: 932 KB to 818 KB.
- **Single runs have stalls.** One LTCG run had a 41.6 ms track p99 and one pinned PGO run a 3.0 ms one. Both are OS pre-emption on a 4-core box with a normal desktop running; the other two runs of each build were at 1.0 ms. Medians hide them; the CSV keeps them.
- **`--pin` lowers present latency in both builds** (median 48 -> 27 ms default, 36 -> 25 ms PGO; all six pinned runs were 15 to 28 ms p50, unpinned runs 24 to 67 ms). The stage times do not change, so the effect is on scheduling between threads, not on the code. With four cores and five busy threads, the sample is small; it is reported as a likely win, not a measured constant.

## Decisions

- Default build stays `TOUCHFC_PGO=OFF`: one-step build, same latency. `scripts\pgo.ps1` builds the PGO binary in `build\pgo` for a smaller exe.
- `--pin` stays opt-in (it changes thread placement for the whole session) and is recommended on machines with more than one L3 or few cores.
- No optimisation was reverted. The padding result is the one that went the wrong way and is kept for portability.

## Not measured

- Glass-to-photon latency: needs PresentMon or a photodiode (`scripts\presentmon_check.ps1`).
- A GPU ONNX Runtime build and a mid-range NVIDIA GPU (the S1 target hardware). On this box ORT inference is the bottleneck by two orders of magnitude, which no CPU-side change here addresses.
