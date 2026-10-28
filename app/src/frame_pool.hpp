#pragma once

#include "touchfc/detection.hpp"
#include "touchfc/events.hpp"
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
  float green_frac = 0.f;  // grass share, the live value beside the scene sliders
  std::uint8_t pitch_valid = 0;
  std::uint8_t kp_fresh = 0;
  std::uint8_t kp_n = 0;
  std::array<std::uint8_t, 32> kp_id{};
  std::array<float, 32> kp_x{};
  std::array<float, 32> kp_y{};
  float reproj_m = 0.f;
  std::array<float, 9> pitch_h{};  // image -> pitch, valid when pitch_valid
  std::uint16_t pitch_age = 0;     // frames since the last accepted fit
  std::uint8_t pitch_reject = 0;   // FitReject of the last keypoint fit
  std::uint8_t pitch_manual = 0;
  float gmc_cost = 0.f;
  float line_support = 0.f;  // share of in-view markings on painted lines
  std::uint8_t gmc_ok = 0;
  std::array<std::uint8_t, kMaxDetections> pitch_ok{};  // on the pitch within 3 m
  std::array<float, kMaxDetections> pitch_x{};
  std::array<float, kMaxDetections> pitch_y{};
  std::array<std::uint8_t, kMaxDetections> det_team{};
  std::uint8_t pc_valid = 0;
  float territory = 0.5f;
  std::array<std::uint8_t, 160> pc_ds{};
  std::uint8_t play_dead = 0;
  std::uint8_t play_reasons = 0;  // PlaySignal bits behind the last Dead
  float view_angle_deg = 0.f;
  float player_height_frac = 0.f;
  std::int16_t bug_roi[4]{};       // scorebug x, y, w, h; w = 0 not found
  TeamStats team_stats{};          // vision estimate, M13
  float holder_rating = 0.f;       // TFC rating contributions of poss_track
  float play_t_s = 0.f;            // play clock, event time
  float poss_spell_s = 0.f;        // poss_track's time on the ball this spell
  std::uint16_t holder_passes = 0;
  std::uint16_t holder_passes_ok = 0;
  std::uint16_t holder_shots = 0;
  std::uint8_t ball_ok = 0;
  std::uint8_t poss_team = 0;
  std::uint16_t poss_track = 0;
  float ball_x = 0.f;
  float ball_y = 0.f;
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
