#pragma once

#include <string>

namespace touchfc {

struct AppArgs {
  enum class Mode { SelfTest, Replay, Overlay };
  Mode mode = Mode::SelfTest;
  std::string replay_path;
  std::string model_path;
  double seconds = 0;
  bool pin = false;
  std::string telemetry_path = "telemetry.csv";
};

int run_app(const AppArgs& args);

}  // namespace touchfc
