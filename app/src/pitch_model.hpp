#pragma once

#include "frame_pool.hpp"

#include <memory>
#include <string>

namespace touchfc {

// Pitch-keypoint model. Called on T2, and only on T2. prepare() may allocate.
// run() must not.
class PitchModel {
 public:
  virtual ~PitchModel() = default;
  virtual bool prepare(std::string& error) = 0;
  virtual void run(FrameSlot& slot, float keypoint_conf) noexcept = 0;
};

// Null when ONNX Runtime is not compiled in, or the file cannot be loaded.
std::unique_ptr<PitchModel> make_pitch_model(const std::string& model_path, std::string& error);

}  // namespace touchfc
