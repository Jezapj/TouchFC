#include "frame_source.hpp"
#include "marker.hpp"
#include "win_time.hpp"

#include <cstring>
#include <memory>

namespace touchfc {
namespace {

class SelfTestSource final : public FrameSource {
 public:
  bool open(std::string&) override {
    width_ = 1280;
    height_ = 720;
    return true;
  }
  void close() override {}
  void request_stop() override { stop_.store(true, std::memory_order_release); }
  bool is_live() const noexcept override { return false; }
  std::uint32_t width() const noexcept override { return width_; }
  std::uint32_t height() const noexcept override { return height_; }

  bool decode_into(FrameSlot& slot) override {
    if (stop_.load(std::memory_order_acquire)) return false;
    if (!have_anchor_) {
      anchor_wall_ = now_ns();
      have_anchor_ = true;
    }
    const TimeNs pts = static_cast<TimeNs>(index_) * 20000000;  // 50 fps
    wait_.until(anchor_wall_ + pts, stop_);
    if (stop_.load(std::memory_order_acquire)) return false;

    std::memset(slot.bgra.get(), 32, slot.capacity);
    const Marker m = marker_for(index_, width_);
    const int x0 = static_cast<int>(m.x);
    const int y0 = static_cast<int>(m.y);
    for (int y = y0; y < y0 + static_cast<int>(m.h) && y < static_cast<int>(height_); ++y) {
      std::uint8_t* row = slot.bgra.get() + static_cast<std::size_t>(y) * slot.stride;
      for (int x = x0; x < x0 + static_cast<int>(m.w) && x < static_cast<int>(width_); ++x) {
        std::uint8_t* px = row + x * 4;
        px[0] = 32;
        px[1] = 160;
        px[2] = 255;
        px[3] = 255;
      }
    }
    slot.meta.width = width_;
    slot.meta.height = height_;
    slot.meta.pts_ns = pts;
    slot.meta.t_capture = 0;
    slot.meta.flags = kFlagReplay;
    ++index_;
    return true;
  }

 private:
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  // Starts at 1 so the painted rectangle matches Pipeline's frame id, which also starts at 1.
  std::uint64_t index_ = 1;
  bool have_anchor_ = false;
  TimeNs anchor_wall_ = 0;
  std::atomic<bool> stop_{false};
  PreciseWait wait_{};
};

}  // namespace

std::unique_ptr<FrameSource> make_selftest_source() {
  return std::make_unique<SelfTestSource>();
}

}  // namespace touchfc
