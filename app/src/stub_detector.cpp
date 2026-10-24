#include "detector.hpp"

#include "touchfc/detection.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace touchfc {
namespace {

// Boxes-on-screen stub. Produces four moving rectangles so the full pipeline
// (T2 writes dets, T3 copies, present draws) is exercised without a model.
// This is NOT training data or anything close; it is a smoke test fixture.
class StubDetector final : public Detector {
 public:
  bool prepare(std::string&) override { return true; }

  void run(FrameSlot& slot) noexcept override {
    const float W = static_cast<float>(slot.meta.width);
    const float H = static_cast<float>(slot.meta.height);
    const float t = static_cast<float>(slot.meta.id & 0xffu) / 255.0f;  // 0..1
    const float cx = 160.f + t * (W - 320.f);
    const float cy = 180.f + t * (H - 360.f);
    const struct {
      float dx, dy;
      std::uint16_t cls;
      float score;
    } samples[] = {
        {-120.f, 0.f, 0, 0.90f},
        {120.f, 0.f, 0, 0.85f},
        {0.f, 60.f, 2, 0.60f},
        {0.f, -60.f, 3, 0.50f},
    };
    std::uint32_t n = 0;
    for (const auto& s : samples) {
      if (n >= slot.dets.size()) break;
      Detection d{};
      d.w = 64.f;
      d.h = 128.f;
      d.x = std::clamp(cx + s.dx - 0.5f * d.w, 0.f, std::max(0.f, W - d.w));
      d.y = std::clamp(cy + s.dy - 0.5f * d.h, 0.f, std::max(0.f, H - d.h));
      d.score = s.score;
      d.class_id = s.cls;
      slot.dets[n++] = d;
    }
    slot.det_count = n;
  }

  const char* label() const noexcept override { return "stub"; }
};

}  // namespace

std::unique_ptr<Detector> make_stub_detector() {
  return std::make_unique<StubDetector>();
}

#if !defined(TOUCHFC_HAS_ORT)
std::unique_ptr<Detector> make_ort_detector(const std::string&, std::string& error) {
  error = "ONNX Runtime not compiled in; set TOUCHFC_ORT_ROOT and rebuild";
  return nullptr;
}
#endif

}  // namespace touchfc
