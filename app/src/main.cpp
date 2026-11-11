#include "present.hpp"
#include "win_prelude.hpp"

#include <cstdlib>
#include <cstdio>
#include <string>

namespace {

void print_usage() {
  std::fputs(
      "touchfc [--selftest] [--replay file] [--overlay] [--model path.onnx] [--detector name] [--keypoints path.onnx] [--seconds N] [--pin] [--recordable] [--telemetry path] [--pitch-dump path.csv] [--match league:YYYYMMDD:home:away] [--mini-replay 0|1]\n"
      "  --selftest   synthetic 50 fps clip\n"
      "  --replay     pace a video file through the same pipeline\n"
      "  --overlay    the app: pick a window, stats over it (default)\n"
      "  --model      detector ONNX (YOLOv8 or DETR family); without it or --detector a stub paints boxes\n"
      "  --detector   a preset name from config/detectors.json (overlay mode remembers the last pick)\n"
      "  --keypoints  pitch-keypoint ONNX; overlay mode finds third_party/models/football-pitch-detection.onnx\n"
      "  --seconds    exit after N seconds (useful for a smoke run)\n"
      "  --pin        pin pipeline threads to one L3 cache\n"
      "  --recordable  overlay and menu show in screen recordings (hidden by default)\n"
      "  --mini-replay  0 or 1, over the profile (before/after telemetry)\n",
      stderr);
}

}  // namespace

int main(int argc, char** argv) {
  touchfc::AppArgs args;
  args.mode = touchfc::AppArgs::Mode::Overlay;  // a double-click opens the app
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      print_usage();
      return 0;
    }
    if (a == "--selftest") {
      args.mode = touchfc::AppArgs::Mode::SelfTest;
    } else if (a == "--overlay") {
      args.mode = touchfc::AppArgs::Mode::Overlay;
    } else if (a == "--pin") {
      args.pin = true;
    } else if (a == "--recordable") {
      args.recordable = true;
    } else if (a == "--replay" && i + 1 < argc) {
      args.mode = touchfc::AppArgs::Mode::Replay;
      args.replay_path = argv[++i];
    } else if (a == "--seconds" && i + 1 < argc) {
      args.seconds = std::atof(argv[++i]);
    } else if (a == "--model" && i + 1 < argc) {
      args.model_path = argv[++i];
    } else if (a == "--detector" && i + 1 < argc) {
      args.detector_name = argv[++i];
    } else if (a == "--keypoints" && i + 1 < argc) {
      args.keypoints_path = argv[++i];
    } else if (a == "--match" && i + 1 < argc) {
      args.match_spec = argv[++i];
    } else if (a == "--mini-replay" && i + 1 < argc) {
      args.mini_replay = std::atoi(argv[++i]);
    } else if (a == "--pitch-dump" && i + 1 < argc) {
      args.pitch_dump_path = argv[++i];
    } else if (a == "--telemetry" && i + 1 < argc) {
      args.telemetry_path = argv[++i];
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      print_usage();
      return 2;
    }
  }
  // config\, data\ and profiles\ are relative. A double-click starts in the
  // exe's folder, so walk up from it to the folder holding config\detectors.json.
  if (GetFileAttributesW(L"config\\detectors.json") == INVALID_FILE_ATTRIBUTES) {
    wchar_t dir[MAX_PATH]{};
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    for (int up = 0; up < 6; ++up) {
      wchar_t* slash = wcsrchr(dir, L'\\');
      if (!slash) break;
      *slash = 0;
      if (GetFileAttributesW((std::wstring(dir) + L"\\config\\detectors.json").c_str()) != INVALID_FILE_ATTRIBUTES) {
        SetCurrentDirectoryW(dir);
        break;
      }
    }
  }
  // Started from Explorer: the console is ours alone, so drop it and log to a file.
  DWORD procs[2];
  if (args.mode == touchfc::AppArgs::Mode::Overlay && GetConsoleProcessList(procs, 2) <= 1) {
    FreeConsole();
    std::FILE* f = nullptr;
    freopen_s(&f, "touchfc.log", "w", stderr);
  }
  return touchfc::run_app(args);
}
