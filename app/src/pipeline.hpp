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
#include "touchfc/events.hpp"
#include "touchfc/match_info.hpp"
#include "touchfc/mini_replay.hpp"
#include "touchfc/scorebug.hpp"
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

// A scorebug crop handed from T3 to T7 for OCR. T3 fills it only when not
// busy; T7 clears busy when done. Preallocated.
struct OcrCrop {
  static constexpr int kMaxW = 960;
  static constexpr int kMaxH = 240;
  std::atomic<bool> busy{false};
  int w = 0;
  int h = 0;
  std::int32_t roi[4]{};
  std::unique_ptr<std::uint8_t[]> px{new std::uint8_t[kMaxW * kMaxH * 4]};
};

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
  BlockingSpsc<std::uint32_t, 4>& ocr_requests() noexcept { return ocr_req_; }
  OcrCrop& ocr_crop(std::uint32_t i) noexcept { return crops_[i & 1]; }
  TripleBuffer<MatchInfo>& match_t3() noexcept { return match_t3_; }
  TripleBuffer<MatchInfo>& match_ui() noexcept { return match_ui_; }
  // M15: T3 writes the ring and posts requests; T7 fills clips; present plays.
  const ThumbRing& thumb_ring() const noexcept { return thumb_ring_; }
  BlockingSpsc<ClipReq, 8>& clip_requests() noexcept { return clip_req_; }
  ClipBuffer& clip(int i) noexcept { return clips_[i & 1]; }
  std::atomic<std::int64_t> thumb_ns{0};          // EMA of the thumbnail cost on T3
  std::atomic<bool> replay_over_budget{false};    // set once; the mini replay stays off
  void write_thumb(const FrameSlot& slot);         // present thread only
  // After join(): the final vision stats.
  const TeamStats& team_stats() const noexcept { return events_.team_stats(); }

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
  void request_clip(float t_s, const Event* e, std::uint8_t kind) noexcept;

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
  void update_match(FrameSlot& slot, const Config& cfg);
  void seed_kits(const Config& cfg);
  OcrCrop crops_[2]{};
  BlockingSpsc<std::uint32_t, 4> ocr_req_{};
  TripleBuffer<MatchInfo> match_t3_{};
  TripleBuffer<MatchInfo> match_ui_{};
  MatchInfo match_{};
  ScorebugRoiFinder roi_finder_{};
  bool roi_logged_ = false;
  std::int32_t manual_roi_[4]{};
  TimeNs last_ocr_ns_ = 0;
  std::int16_t last_score_[2] = {-1, -1};
  EventDetector events_{};
  float play_t_s_ = 0.f;
  float last_event_t_s_ = -1.f;
  std::int64_t t_gate0_ = -1;
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
  ThumbScaler thumb_scaler_{};
  ThumbRing thumb_ring_{};
  ClipBuffer clips_[2]{};
  BlockingSpsc<ClipReq, 8> clip_req_{};
  float last_thumb_t_ = -1e9f;
  TimeNs budget_t_ = 0;
  std::uint64_t budget_frames_ = 0;
  std::uint64_t budget_late_ = 0;
  std::uint64_t frames_ = 0;
};

}  // namespace touchfc
