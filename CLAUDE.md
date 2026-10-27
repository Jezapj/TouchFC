# TouchFC: notes for Claude Code

- Plan: `docs/spec_v2.md`. Section 9 is the milestone order, section 11 is the phase 2 detail (M9 to M15, then M8). `docs/review_gate.md` records what was verified. Do not reread `spec_v1.md` or `design_review.md` unless spec_v2 points to a section.
- Code is the authority for done milestones; spec_v2 for the rest. Update the milestone row and `review_gate.md` when a milestone lands.
- Build and test (PowerShell, from repo root):
  - `cmake --preset windows-msvc`
  - `cmake --build build/windows-msvc --config Release`
  - `build\windows-msvc\Release\touchfc_tests.exe`
  - `build\windows-msvc\Release\touchfc.exe --selftest --seconds 3`
- `core/` is portable C++20, no Windows headers. Windows code lives in `app/`.
- T1 to T3 hot path: no `new`, no `std::string`, no container growth after `Pipeline::start`. Slow work goes to T7 (services thread).
- Thresholds are `Config` fields (seqlock, trivially copyable, size % 8 == 0) with a slider in the Tuning tab.
- Never commit weights, broadcast footage, API keys, or `cache/`. Keys come from env vars.
- Style: terse comments, match surrounding naming. Tests in `core/tests/test_core.cpp` (Catch2), benches in `core/bench/bench_main.cpp`, results in `docs/bench/`.
