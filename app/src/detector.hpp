#pragma once

#include "frame_pool.hpp"

#include "touchfc/detection.hpp"

#include <memory>
#include <string>

namespace touchfc {

// Detector runs on T2. It owns the per-thread scratch (letterbox tensor,
// decode scratch). Every method is called on T2 only.
class Detector {
 public:
  virtual ~Detector() = default;

  // Called once from T2 before the first run. May allocate; returns false on
  // failure with a human-readable message in `error`.
  virtual bool prepare(std::string& error) = 0;

  // Fill slot.dets / slot.det_count using slot.bgra and slot.meta.
  // Must not allocate and must not throw.
  virtual void run(FrameSlot& slot) noexcept = 0;

  // Short label for the HUD, e.g. "stub" or "ort-cpu".
  virtual const char* label() const noexcept = 0;
};

// Deterministic stub: paints boxes along a horizontal line driven by frame id.
// Used when no model is supplied so M2's "boxes on screen" works out of the
// box. Replaced by OrtDetector when TOUCHFC_HAS_ORT is defined and a model
// path is given.
std::unique_ptr<Detector> make_stub_detector();

// Optional ORT detector. Returns nullptr with `error` set when ORT is not
// compiled in or the model cannot be loaded.
std::unique_ptr<Detector> make_ort_detector(const std::string& model_path,
                                            std::string& error);

}  // namespace touchfc
