#pragma once

#include "touchfc/detection.hpp"
#include "touchfc/homography.hpp"
#include "touchfc/slot_pool.hpp"
#include "touchfc/types.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace touchfc {

// Detections are stored inline on the slot so T3 never allocates. 128 is
// bounded well above a soccer frame's worth of boxes (22 players, three
// officials, a ball, a few spurious crops).
struct FrameSlot {
  FrameMeta meta{};
  OverlayDraw draw{};
  std::uint32_t stride = 0;
  std::uint32_t capacity = 0;
  std::unique_ptr<std::uint8_t[]> bgra;
  std::uint32_t det_count = 0;
  std::array<Detection, kMaxDetections> dets{};
  std::uint8_t scene = 0;
  std::uint8_t pitch_valid = 0;
  float reproj_m = 0.f;
  std::array<float, kMaxDetections> pitch_x{};
  std::array<float, kMaxDetections> pitch_y{};
};

// Slots are allocated once. The index from SlotPool is the vector index.
// 16 slots, not 12: three rings of depth 4 plus the slot a stage is filling
// do not fit in 12, and a full pipeline would stall forever.
class FramePool {
 public:
  static constexpr int kSlots = 16;

  FramePool(std::uint32_t width, std::uint32_t height)
      : pool_(kSlots), width_(width), height_(height) {
    slots_.resize(kSlots);
    const std::uint32_t stride = width * 4u;
    const std::uint32_t bytes = stride * height;
    for (auto& slot : slots_) {
      slot.stride = stride;
      slot.capacity = bytes;
      slot.bgra = std::make_unique<std::uint8_t[]>(bytes);
      slot.meta.width = width;
      slot.meta.height = height;
    }
  }

  int try_acquire() noexcept { return pool_.try_acquire(); }
  void release(std::uint32_t index) noexcept { pool_.release(index); }

  FrameSlot& slot(std::uint32_t index) noexcept { return slots_[index]; }
  const FrameSlot& slot(std::uint32_t index) const noexcept { return slots_[index]; }

  std::uint32_t width() const noexcept { return width_; }
  std::uint32_t height() const noexcept { return height_; }
  int free_approx() const noexcept { return pool_.free_approx(); }

 private:
  SlotPool pool_;
  std::uint32_t width_;
  std::uint32_t height_;
  std::vector<FrameSlot> slots_;
};

}  // namespace touchfc
