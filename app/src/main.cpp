#include "present.hpp"

#include <cstdlib>
#include <cstdio>
#include <string>

namespace {

void print_usage() {
  std::fputs(
      "touchfc [--selftest] [--replay file] [--overlay] [--model path.onnx] [--keypoints path.onnx] [--seconds N] [--pin] [--telemetry path] [--pitch-dump path.csv] [--match league:YYYYMMDD:home:away]\n"
      "  --selftest   synthetic 50 fps clip (default)\n"
      "  --replay     pace a video file through the same pipeline\n"
      "  --overlay    pick a window and draw a click-through capture overlay\n"
      "  --model      YOLOv8 ONNX model; without this a deterministic stub paints boxes\n"
      "  --keypoints  pitch-keypoint ONNX; without it the minimap needs a manual calibration\n"
      "  --seconds    exit after N seconds (useful for a smoke run)\n"
      "  --pin        pin pipeline threads to one L3 cache\n",
      stderr);
}

}  // namespace

int main(int argc, char** argv) {
  touchfc::AppArgs args;
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
    } else if (a == "--replay" && i + 1 < argc) {
      args.mode = touchfc::AppArgs::Mode::Replay;
      args.replay_path = argv[++i];
    } else if (a == "--seconds" && i + 1 < argc) {
      args.seconds = std::atof(argv[++i]);
    } else if (a == "--model" && i + 1 < argc) {
      args.model_path = argv[++i];
    } else if (a == "--keypoints" && i + 1 < argc) {
      args.keypoints_path = argv[++i];
    } else if (a == "--match" && i + 1 < argc) {
      args.match_spec = argv[++i];
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
  return touchfc::run_app(args);
}
