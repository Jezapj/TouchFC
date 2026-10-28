#pragma once

#include <string>

namespace touchfc {

struct AppArgs {
  enum class Mode { SelfTest, Replay, Overlay };
  Mode mode = Mode::SelfTest;
  std::string replay_path;
  std::string model_path;
  std::string keypoints_path;
  double seconds = 0;
  bool pin = false;
  std::string telemetry_path = "telemetry.csv";
  std::string pitch_dump_path;
  std::string match_spec;  // league:YYYYMMDD:home_id:away_id, as if confirmed in the Match tab  // per-frame homography CSV, for offline checks
};

int run_app(const AppArgs& args);

}  // namespace touchfc
