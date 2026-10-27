#pragma once

#include "detector.hpp"
#include "frame_source.hpp"
#include "pitch_model.hpp"

#include "touchfc/blocking_spsc.hpp"
#include "touchfc/gmc.hpp"
#include "touchfc/homography.hpp"
#include "touchfc/image_gmc.hpp"
#include "touchfc/line_fit.hpp"
#include "touchfc/pitch_calib.hpp"
#include "touchfc/play_state.hpp"
#include "touchfc/pitch_control.hpp"
#include "touchfc/possession.hpp"
#include "touchfc/scene.hpp"
#include "touchfc/tracker.hpp"
#include "touchfc/hdr_sink.hpp"
#include "touchfc/time.hpp"
#include "touchfc/mailbox.hpp"
#include "touchfc/seqlock.hpp"
#include "touchfc/telemetry.hpp"
#include "touchfc/triple_buffer.hpp"
#include "touchfc/wake.hpp"

#include <atomic>
#include <cstdint>
#include <thread>

namespace touchfc {

// T1 preprocess, T2 inference (pass-through in M1), T3 track/overlay, T6 telemetry.
// The present/UI thread is the caller, not owned here.
// Replay uses blocking SPSC so frames are not dropped. Live capture uses a
// mailbox (latest wins) at the capture edge and again before present.
class Pipeline {
 public:
  static constexpr std::size_t kDepth = 4;

  explicit Pipeline(FramePool& pool);
  ~Pipeline();

  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;

  void start(FrameSource& source, HdrSink& sink, Detector& detector, bool pin,
             PitchModel* pitch = nullptr);
  void request_stop();
  void join();

  bool live() const noexcept { return live_; }
  FramePool& pool() noexcept { return pool_; }
  SeqLock<Config>& config() noexcept { return config_; }
  TripleBuffer<OverlayState>& overlay() noexcept { return overlay_; }
  TripleBuffer<UiSnapshot>& ui() noexcept { return ui_; }
  BlockingSpsc<UiCommand, 64>& commands() noexcept { return commands_; }

  void note_present(FrameId frame, TimeNs capture, TimeNs present) noexcept {
    note(3, frame, Stage::Present, capture, present);
  }
  void write_spadl(const char* path) const;

  bool pop_present(std::uint32_t& slot) noexcept { return to_present_.try_pop(slot); }
  std::uint32_t take_live_present() noexcept { return live_present_.take(); }
  void release_presented(std::uint32_t slot) noexcept;

  std::atomic<std::uint64_t> drops{0};
  std::atomic<std::uint64_t> dedupes{0};
  std::atomic<std::uint64_t> black_frames{0};
  std::atomic<std::uint64_t> late_frames{0};
  std::atomic<std::uint64_t> command_warns{0};
  std::atomic<int> attack_dir{1};
  std::atomic<std::int64_t> present_p50_ns{0};
  std::atomic<std::int64_t> present_p99_ns{0};

 private:
  void run_t1();
  void run_t2();
  void run_t3();
  void run_t6();
  void note(int producer, FrameId frame, Stage stage, TimeNs begin, TimeNs end) noexcept;
  void drain_ring(BlockingSpsc<std::uint32_t, kDepth>& ring);
  void assign_teams(FrameSlot& slot, const Config& cfg);
  void update_possession(FrameSlot& slot);

  FramePool& pool_;
  FrameSource* source_ = nullptr;
  HdrSink* sink_ = nullptr;
  Detector* detector_ = nullptr;
  PitchModel* pitch_ = nullptr;
  bool live_ = false;
  bool pin_ = false;
  std::atomic<bool> stop_{false};
  std::atomic<bool> t1_done_{false};
  std::atomic<bool> t2_done_{false};
  std::atomic<bool> t3_done_{false};

  BlockingSpsc<std::uint32_t, kDepth> to_infer_{};
  BlockingSpsc<std::uint32_t, kDepth> to_track_{};
  BlockingSpsc<std::uint32_t, kDepth> to_present_{};
  Mailbox live_in_{};
  WakeSeq live_in_wake_{};
  Mailbox live_present_{};
  SeqLock<Config> config_{};
  TripleBuffer<OverlayState> overlay_{};
  TripleBuffer<UiSnapshot> ui_{};
  BlockingSpsc<UiCommand, 64> commands_{};
  TelemetryHub telemetry_{};
  WakeSeq pool_wake_{};

  std::thread t1_;
  std::thread t2_;
  std::thread t3_;
  std::thread t6_;
  std::uint64_t last_hash_ = 0;
  bool have_hash_ = false;
  FrameId next_id_ = 1;
  Tracker tracker_{};
  TrackerParams tracker_params_{};
  TimeNs last_track_t_ns_ = 0;
  GmcTranslation last_gmc_{};
  float gmc_px_[64]{};
  float gmc_py_[64]{};
  std::uint16_t gmc_id_[64]{};
  std::uint32_t gmc_n_ = 0;
  void update_pitch(FrameSlot& slot, const Config& cfg);
  void update_play(FrameSlot& slot, const Config& cfg);
  void log_event(const Event& e) noexcept;
  PlayGate play_{};
  bool play_dead_ = false;
  std::int64_t dead_start_ns_ = 0;
  void set_anchor(const UiCommand& command);
  PitchCalib calib_{};
  GmcImage gmc_img_[2]{};
  int gmc_cur_ = 0;
  FitReject last_reject_ = FitReject::None;
  float line_support_ = 0.f;
  // Manual calibration clicks, moved with the camera until the next cut.
  bool anchor_set_[32]{};
  float anchor_x_[32]{};
  float anchor_y_[32]{};
  int pitch_logs_ = 0;
  Lab seed_[2]{};
  bool seed_ok_[2]{};
  std::uint16_t seed_req_id_ = 0;
  std::uint8_t seed_req_team_ = 0;
  struct Mem {
    std::uint16_t id = 0;
    float x = 0.f;
    float y = 0.f;
    Lab lab{};
    std::uint8_t team = 0;
  };
  Mem prev_[64]{};
  int prev_n_ = 0;
  LostLabel lost_[16]{};
  int lost_n_ = 0;
  TimeNs last_pc_ns_ = 0;
  std::uint8_t pc_valid_ = 0;
  float territory_ = 0.5f;
  std::uint8_t pc_ds_[160]{};
  Possession poss_{};
  static constexpr int kMaxEvents = 4096;
  Event poss_events_[kMaxEvents]{};
  int poss_n_ = 0;
  std::int64_t poss_t0_ = 0;
};

}  // namespace touchfc
