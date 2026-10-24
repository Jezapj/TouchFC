#include "pipeline.hpp"

#include "log.hpp"
#include "marker.hpp"
#include "threads_win.hpp"

#include "touchfc/pitch_control.hpp"
#include "touchfc/pitch_landmarks.hpp"
#include "touchfc/possession.hpp"

#include "touchfc/frame_check.hpp"
#include "touchfc/time.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>

namespace touchfc {

Pipeline::Pipeline(FramePool& pool) : pool_(pool) {}

Pipeline::~Pipeline() { join(); }

void Pipeline::start(FrameSource& source, HdrSink& sink, Detector& detector, bool pin,
                      PitchModel* pitch) {
  source_ = &source;
  sink_ = &sink;
  detector_ = &detector;
  pitch_ = pitch;
  live_ = source.is_live();
  pin_ = pin;
  stop_.store(false, std::memory_order_relaxed);
  t1_done_.store(false, std::memory_order_relaxed);
  t2_done_.store(false, std::memory_order_relaxed);
  t3_done_.store(false, std::memory_order_relaxed);
  t1_ = std::thread([this] { run_t1(); });
  t2_ = std::thread([this] { run_t2(); });
  t3_ = std::thread([this] { run_t3(); });
  t6_ = std::thread([this] { run_t6(); });
}

void Pipeline::request_stop() {
  stop_.store(true, std::memory_order_release);
  if (source_) source_->request_stop();
  to_infer_.wake();
  to_track_.wake();
  to_present_.wake();
  commands_.wake();
  live_in_wake_.notify();
  pool_wake_.notify();
}

void Pipeline::join() {
  if (!t1_.joinable() && !t2_.joinable() && !t3_.joinable() && !t6_.joinable()) return;
  request_stop();
  if (t1_.joinable()) t1_.join();
  if (t2_.joinable()) t2_.join();
  if (t3_.joinable()) t3_.join();
  if (t6_.joinable()) t6_.join();
  drain_ring(to_infer_);
  drain_ring(to_track_);
  drain_ring(to_present_);
  for (;;) {
    const std::uint32_t slot = live_in_.take();
    if (slot == Mailbox::kEmpty) break;
    pool_.release(slot);
  }
  for (;;) {
    const std::uint32_t slot = live_present_.take();
    if (slot == Mailbox::kEmpty) break;
    pool_.release(slot);
  }
}

void Pipeline::release_presented(std::uint32_t slot) noexcept {
  pool_.release(slot);
  pool_wake_.notify();
}

void Pipeline::drain_ring(BlockingSpsc<std::uint32_t, kDepth>& ring) {
  std::uint32_t slot = 0;
  while (ring.try_pop(slot)) pool_.release(slot);
}

void Pipeline::note(int producer, FrameId frame, Stage stage, TimeNs begin, TimeNs end) noexcept {
  if (begin <= 0 || end < begin) return;
  TelemetrySample sample;
  sample.frame = frame;
  sample.stage = static_cast<std::uint8_t>(stage);
  sample.t_begin = begin;
  sample.t_end = end;
  telemetry_.push(producer, sample);
}

void Pipeline::run_t1() {
  if (pin_) pin_current_thread_to_one_ccd();
  set_pipeline_thread();
  while (!stop_.load(std::memory_order_acquire)) {
    int index = -1;
    while (index < 0 && !stop_.load(std::memory_order_acquire)) {
      index = pool_.try_acquire();
      if (index >= 0) break;
      const std::uint32_t seen = pool_wake_.load();
      index = pool_.try_acquire();
      if (index >= 0) break;
      pool_wake_.wait_from(seen, 64);
    }
    if (index < 0) break;

    FrameSlot& slot = pool_.slot(static_cast<std::uint32_t>(index));
    slot.meta = {};
    slot.det_count = 0;
    slot.meta.width = pool_.width();
    slot.meta.height = pool_.height();
    if (!source_->decode_into(slot)) {
      pool_.release(static_cast<std::uint32_t>(index));
      pool_wake_.notify();
      break;
    }
    if (slot.meta.t_capture == 0) slot.meta.t_capture = now_ns();
    slot.meta.id = next_id_++;
    const std::uint64_t hash =
        hash_bytes_stride(slot.bgra.get(), static_cast<std::size_t>(slot.meta.height) * slot.stride, 64);
    if (have_hash_ && hash == last_hash_) {
      slot.meta.flags = static_cast<std::uint8_t>(slot.meta.flags | kFlagDeduped);
      dedupes.fetch_add(1, std::memory_order_relaxed);
    }
    have_hash_ = true;
    last_hash_ = hash;
    if (looks_black_bgra(slot.bgra.get(), slot.meta.width, slot.meta.height, slot.stride)) {
      slot.meta.flags = static_cast<std::uint8_t>(slot.meta.flags | kFlagBlack);
      black_frames.fetch_add(1, std::memory_order_relaxed);
    }
    slot.meta.t_pre_done = now_ns();
    note(0, slot.meta.id, Stage::Preprocess, slot.meta.t_capture, slot.meta.t_pre_done);

    if (live_) {
      const std::uint32_t displaced = live_in_.publish(static_cast<std::uint32_t>(index));
      live_in_wake_.notify();
      if (displaced != Mailbox::kEmpty) {
        pool_.release(displaced);
        drops.fetch_add(1, std::memory_order_relaxed);
        pool_wake_.notify();
      }
    } else if (!to_infer_.push_wait(static_cast<std::uint32_t>(index), stop_)) {
      pool_.release(static_cast<std::uint32_t>(index));
      break;
    }
  }
  t1_done_.store(true, std::memory_order_release);
  to_infer_.wake();
  live_in_wake_.notify();
}

void Pipeline::run_t2() {
  if (pin_) pin_current_thread_to_one_ccd();
  set_pipeline_thread();
  for (;;) {
    std::uint32_t index = 0;
    if (live_) {
      index = live_in_.take();
      if (index == Mailbox::kEmpty) {
        if (t1_done_.load(std::memory_order_acquire) || stop_.load(std::memory_order_acquire)) {
          index = live_in_.take();
          if (index == Mailbox::kEmpty) break;
        } else {
          const std::uint32_t seen = live_in_wake_.load();
          index = live_in_.take();
          if (index == Mailbox::kEmpty) {
            live_in_wake_.wait_from(seen, 32);
            continue;
          }
        }
      }
    } else if (!to_infer_.pop_until(index, t1_done_)) {
      break;
    }

    FrameSlot& slot = pool_.slot(index);
    const TimeNs begin = slot.meta.t_pre_done;
    slot.kp_fresh = 0;
    slot.kp_n = 0;
    if (detector_) detector_->run(slot);
    if (pitch_ && (slot.meta.id % 8u) == 1u) {
      const Config cfg = config_.load();
      const float kp_conf = cfg.conf_keypoint > 0.f ? cfg.conf_keypoint : 0.40f;
      pitch_->run(slot, kp_conf);
    }
    slot.meta.t_inf_done = now_ns();
    note(1, slot.meta.id, Stage::Inference, begin, slot.meta.t_inf_done);
    if (live_) {
      if (!to_track_.try_push(index)) {
        pool_.release(index);
        drops.fetch_add(1, std::memory_order_relaxed);
        pool_wake_.notify();
      }
    } else if (!to_track_.push_wait(index, stop_)) {
      pool_.release(index);
      break;
    }
  }
  std::uint32_t leftover = 0;
  while (to_infer_.try_pop(leftover)) pool_.release(leftover);
  t2_done_.store(true, std::memory_order_release);
  to_track_.wake();
}

void Pipeline::update_possession(FrameSlot& slot) {
  const Config cfg = config_.load();
  const float limit = cfg.poss_dist_m > 0.f ? cfg.poss_dist_m : 1.5f;
  int ball = -1;
  float best_score = -1.f;
  for (std::uint32_t i = 0; i < slot.det_count; ++i) {
    if (slot.dets[i].class_id != 3 || slot.dets[i].score <= best_score) continue;
    best_score = slot.dets[i].score;
    ball = static_cast<int>(i);
  }
  const bool have = ball >= 0 && slot.pitch_valid;
  float bx = 0, by = 0;
  if (have) {
    bx = slot.pitch_x[static_cast<std::uint32_t>(ball)];
    by = slot.pitch_y[static_cast<std::uint32_t>(ball)];
  }
  float best_d = 1e9f;
  std::uint16_t track = 0;
  std::uint8_t team = 0;
  if (have) {
    for (std::uint32_t i = 0; i < slot.det_count; ++i) {
      if (slot.dets[i].class_id == 3 || slot.det_team[i] == 0) continue;
      const float dx = slot.pitch_x[i] - bx;
      const float dy = slot.pitch_y[i] - by;
      const float d = dx * dx + dy * dy;
      if (d < best_d) {
        best_d = d;
        track = slot.dets[i].track_id;
        team = slot.det_team[i];
      }
    }
  }
  if (poss_t0_ == 0) poss_t0_ = last_track_t_ns_;
  const float dist = track != 0 ? std::sqrt(best_d) : 99.f;
  const PossOut out = step_possession(poss_, have, bx, by, track, team, dist, limit, last_track_t_ns_, poss_t0_);
  if (out.emitted && poss_n_ < 64) {
    poss_events_[poss_n_] = out.event;
    poss_events_[poss_n_].id = static_cast<EventId>(poss_n_ + 1);
    ++poss_n_;
  }
  slot.ball_ok = have ? 1 : 0;
  slot.ball_x = bx;
  slot.ball_y = by;
  slot.poss_team = poss_.held ? poss_.team : 0;
  slot.poss_track = poss_.held ? poss_.track : 0;
}

void Pipeline::write_spadl(const char* path) const {
  if (path == nullptr || path[0] == '\0') return;
  std::FILE* f = std::fopen(path, "wb");
  if (f == nullptr) return;
  std::fputs(kSpadlHeader, f);
  const int dir = attack_dir.load(std::memory_order_relaxed) >= 0 ? 1 : -1;
  for (int i = 0; i < poss_n_; ++i) {
    char row[256];
    format_spadl_row(row, static_cast<int>(sizeof(row)), poss_events_[i], dir);
    std::fputs(row, f);
  }
  std::fclose(f);
}

void Pipeline::assign_teams(FrameSlot& slot) {
  for (std::uint32_t i = 0; i < slot.det_count; ++i) slot.det_team[i] = 0;
  for (std::uint32_t i = 0; i < slot.det_count; ++i) {
    const Detection& d = slot.dets[i];
    if (d.track_id == 0 || d.class_id == 3) continue;
    Lab sample;
    if (!torso_lab(slot.bgra.get(), slot.stride, slot.meta.width, slot.meta.height, d.x, d.y, d.w, d.h,
                   sample))
      continue;
    for (auto& t : tracker_.tracks) {
      if (t.id != d.track_id) continue;
      if (t.hits <= 1) t.lab = sample;
      else {
        t.lab.L = 0.8f * t.lab.L + 0.2f * sample.L;
        t.lab.a = 0.8f * t.lab.a + 0.2f * sample.a;
        t.lab.b = 0.8f * t.lab.b + 0.2f * sample.b;
      }
      break;
    }
  }
  if (seed_req_id_ != 0) {
    for (auto& t : tracker_.tracks) {
      if (t.id != seed_req_id_) continue;
      const int s = seed_req_team_ - 1;
      seed_[s] = t.lab;
      seed_ok_[s] = true;
      t.team = seed_req_team_;
      t.team_lock = true;
      break;
    }
    seed_req_id_ = 0;
  }
  if (seed_ok_[0] || seed_ok_[1]) {
    for (auto& t : tracker_.tracks) {
      if (t.id == 0 || t.team_lock || t.class_id == 3) continue;
      t.team = static_cast<std::uint8_t>(nearest_team(t.lab, seed_[0], seed_ok_[0], seed_[1], seed_ok_[1]));
    }
  }
  for (auto& t : tracker_.tracks) {
    if (t.id == 0 || t.hits != 1 || t.team != 0) continue;
    float px = 0, py = 0;
    bool have = false;
    for (std::uint32_t i = 0; i < slot.det_count; ++i) {
      if (slot.dets[i].track_id != t.id || !slot.pitch_valid) continue;
      px = slot.pitch_x[i];
      py = slot.pitch_y[i];
      have = true;
      break;
    }
    if (!have) continue;
    const int inherited = inherit_team(lost_, lost_n_, px, py, t.lab, last_track_t_ns_);
    if (inherited != 0) {
      t.team = static_cast<std::uint8_t>(inherited);
      t.team_lock = true;
    }
  }
  Mem next[64]{};
  int next_n = 0;
  for (const auto& t : tracker_.tracks) {
    if (t.id == 0 || next_n >= 64) continue;
    Mem m;
    m.id = t.id;
    m.lab = t.lab;
    m.team = t.team;
    bool have = false;
    for (std::uint32_t i = 0; i < slot.det_count; ++i) {
      if (slot.dets[i].track_id != t.id) continue;
      m.x = slot.pitch_x[i];
      m.y = slot.pitch_y[i];
      have = slot.pitch_valid != 0;
      break;
    }
    if (!have) {
      for (int k = 0; k < prev_n_; ++k) {
        if (prev_[k].id != t.id) continue;
        m.x = prev_[k].x;
        m.y = prev_[k].y;
        break;
      }
    }
    next[next_n++] = m;
  }
  const TimeNs now = last_track_t_ns_;
  for (int k = 0; k < prev_n_; ++k) {
    if (prev_[k].team == 0) continue;
    bool alive = false;
    for (int i = 0; i < next_n; ++i)
      if (next[i].id == prev_[k].id) alive = true;
    if (alive) continue;
    if (lost_n_ < 16) {
      lost_[lost_n_++] = {prev_[k].x, prev_[k].y, prev_[k].lab, prev_[k].team, now};
    } else {
      lost_[0] = {prev_[k].x, prev_[k].y, prev_[k].lab, prev_[k].team, now};
    }
  }
  prev_n_ = next_n;
  for (int i = 0; i < next_n; ++i) prev_[i] = next[i];

  int n0 = 0, n1 = 0;
  for (const auto& t : tracker_.tracks) {
    if (t.id == 0 || t.class_id == 3) continue;
    if (t.team == 1) ++n0;
    else if (t.team == 2) ++n1;
  }
  if (n0 > 0 && n1 > 0 && slot.pitch_valid && now - last_pc_ns_ > 100000000LL) {
    PcPlayer players[kPcMaxPlayers]{};
    int n = 0;
    for (std::uint32_t i = 0; i < slot.det_count && n < kPcMaxPlayers; ++i) {
      const auto id = slot.dets[i].track_id;
      if (id == 0 || slot.dets[i].class_id == 3) continue;
      std::uint8_t team = 0;
      for (const auto& t : tracker_.tracks)
        if (t.id == id) team = t.team;
      if (team != 1 && team != 2) continue;
      players[n++] = {slot.pitch_x[i], slot.pitch_y[i], 0.f, 0.f, static_cast<std::uint8_t>(team - 1)};
    }
    if (n >= 2) {
      float grid[kPcCells];
      territory_ = pitch_control(players, n, 0.f, 0.f, false, grid);
      for (int j = 0; j < 10; ++j) {
        for (int i = 0; i < 16; ++i) {
          const int si = i * kPcNx / 16;
          const int sj = j * kPcNy / 10;
          const float p = grid[sj * kPcNx + si];
          pc_ds_[j * 16 + i] = static_cast<std::uint8_t>(p < 0.f ? 0 : p > 1.f ? 255 : p * 255.f);
        }
      }
      pc_valid_ = 1;
      last_pc_ns_ = now;
    }
  }
  slot.pc_valid = pc_valid_;
  slot.territory = territory_;
  for (int i = 0; i < 160; ++i) slot.pc_ds[static_cast<std::uint32_t>(i)] = pc_ds_[i];
  for (std::uint32_t i = 0; i < slot.det_count; ++i) {
    const auto id = slot.dets[i].track_id;
    if (id == 0) continue;
    for (const auto& t : tracker_.tracks)
      if (t.id == id) slot.det_team[i] = t.class_id == 3 ? 0 : t.team;
  }
  update_possession(slot);
}

void Pipeline::run_t3() {
  if (pin_) pin_current_thread_to_one_ccd();
  set_pipeline_thread();
  for (;;) {
    UiCommand command;
    while (commands_.try_pop(command)) {
      if (command.type == CmdType::ApplyAttackDir) {
        attack_dir.store(command.u8a == 0 ? -1 : 1, std::memory_order_relaxed);
      } else if (command.type == CmdType::SetTeamSeed && (command.u8a == 1 || command.u8a == 2)) {
        seed_req_id_ = command.u16a;
        seed_req_team_ = command.u8a;
      }
    }
    std::uint32_t index = 0;
    if (!to_track_.pop_until(index, t2_done_)) break;

    FrameSlot& slot = pool_.slot(index);
    const Config cfg = config_.load();
    Marker marker = marker_for(slot.meta.id, slot.meta.width);
    if (!cfg.show_boxes) {
      marker.w = 0;
      marker.h = 0;
      slot.det_count = 0;
    }
    const TimeNs now = now_ns();
    float dt = tracker_params_.dt_default;
    if (last_track_t_ns_ != 0) {
      const float measured = static_cast<float>(now - last_track_t_ns_) * 1.0e-9f;
      if (measured > 0.f && measured < 0.2f) dt = measured;
    }
    last_track_t_ns_ = now;
    if (cfg.iou_match_thresh > 0.f) tracker_params_.iou_hi = cfg.iou_match_thresh;
    tracker_params_.conf_new_track = cfg.conf_player;
    tracker_.update(slot.dets.data(), slot.det_count, dt, last_gmc_, tracker_params_);
    float pcx[64], pcy[64], ccx[64], ccy[64];
    std::uint32_t pairs = 0;
    for (std::uint32_t i = 0; i < slot.det_count && pairs < 64; ++i) {
      const auto id = slot.dets[i].track_id;
      if (id == 0) continue;
      for (std::uint32_t j = 0; j < gmc_n_; ++j) {
        if (gmc_id_[j] != id) continue;
        pcx[pairs] = gmc_px_[j];
        pcy[pairs] = gmc_py_[j];
        ccx[pairs] = slot.dets[i].x + 0.5f * slot.dets[i].w;
        ccy[pairs] = slot.dets[i].y + 0.5f * slot.dets[i].h;
        ++pairs;
        break;
      }
    }
    last_gmc_ = pairs >= 3 ? estimate_translation(pcx, pcy, ccx, ccy, pairs) : GmcTranslation{};
    gmc_n_ = 0;
    for (std::uint32_t i = 0; i < slot.det_count && gmc_n_ < 64; ++i) {
      if (slot.dets[i].track_id == 0) continue;
      gmc_id_[gmc_n_] = slot.dets[i].track_id;
      gmc_px_[gmc_n_] = slot.dets[i].x + 0.5f * slot.dets[i].w;
      gmc_py_[gmc_n_] = slot.dets[i].y + 0.5f * slot.dets[i].h;
      ++gmc_n_;
    }
    slot.scene = static_cast<std::uint8_t>(
        classify_scene_bgra(slot.bgra.get(), slot.meta.width, slot.meta.height, slot.stride));
    if (pitch_) {
      if (slot.kp_fresh && slot.kp_n >= 6) {
        float sx[32], sy[32], dx[32], dy[32];
        const int n = slot.kp_n;
        for (int i = 0; i < n; ++i) {
          sx[i] = slot.kp_x[static_cast<std::uint32_t>(i)];
          sy[i] = slot.kp_y[static_cast<std::uint32_t>(i)];
          pitch_landmark_m(slot.kp_id[static_cast<std::uint32_t>(i)], dx[i], dy[i]);
        }
        Homography fitted;
        if (fit_homography_ransac(sx, sy, dx, dy, n, 2.f, fitted) && fitted.rms_m <= 2.5f) {
          pitch_h_ = fitted;
          pitch_from_kp_ = true;
          pan_x_ = 0.f;
          pan_y_ = 0.f;
          if (pitch_logs_ < 3) {
            ++pitch_logs_;
            log_line("pitch fit: " + std::to_string(n) + " pts, error " +
                     std::to_string(fitted.rms_m) + " m");
          }
        }
      } else if (pitch_from_kp_ && last_gmc_.confidence >= 0.5f) {
        pan_x_ += last_gmc_.dx;
        pan_y_ += last_gmc_.dy;
      }
    } else if (!pitch_h_.valid || slot.scene == static_cast<std::uint8_t>(Scene::Wide)) {
      const float W = static_cast<float>(slot.meta.width);
      const float H = static_cast<float>(slot.meta.height);
      const float sx[5] = {0.12f * W, 0.88f * W, 0.30f * W, 0.70f * W, 0.50f * W};
      const float sy[5] = {0.90f * H, 0.90f * H, 0.48f * H, 0.48f * H, 0.66f * H};
      const float dx[5] = {-52.5f, 52.5f, -52.5f, 52.5f, 0.f};
      const float dy[5] = {-34.f, -34.f, 34.f, 34.f, 0.f};
      fit_homography(sx, sy, dx, dy, 5, pitch_h_);
    }
    slot.pitch_valid = pitch_h_.valid ? 1 : 0;
    slot.reproj_m = pitch_h_.rms_m;
    for (std::uint32_t i = 0; i < slot.det_count; ++i) {
      const float fx = slot.dets[i].x + 0.5f * slot.dets[i].w - pan_x_;
      const float fy = slot.dets[i].y + slot.dets[i].h - pan_y_;
      if (!pitch_h_.valid) {
        slot.pitch_x[i] = 0;
        slot.pitch_y[i] = 0;
        continue;
      }
      apply_homography(pitch_h_, fx, fy, slot.pitch_x[i], slot.pitch_y[i]);
    }
    assign_teams(slot);
    slot.draw.box_x = marker.x;
    slot.draw.box_y = marker.y;
    slot.draw.box_w = marker.w;
    slot.draw.box_h = marker.h;
    slot.draw.flags = slot.meta.flags;
    slot.meta.t_track_done = now_ns();
    note(2, slot.meta.id, Stage::Track, slot.meta.t_inf_done, slot.meta.t_track_done);

    OverlayState& state = overlay_.write_slot();
    state = {};
    state.frame = slot.meta.id;
    state.t_capture = slot.meta.t_capture;
    state.t_publish = slot.meta.t_track_done;
    state.box_x = marker.x;
    state.box_y = marker.y;
    state.box_w = marker.w;
    state.box_h = marker.h;
    state.width = slot.meta.width;
    state.height = slot.meta.height;
    state.flags = slot.meta.flags;
    state.drops = static_cast<std::uint32_t>(drops.load(std::memory_order_relaxed));
    state.dedupes = static_cast<std::uint32_t>(dedupes.load(std::memory_order_relaxed));
    state.black_frames = static_cast<std::uint32_t>(black_frames.load(std::memory_order_relaxed));
    overlay_.publish();

    UiSnapshot& snap = ui_.write_slot();
    snap = {};
    snap.frame = slot.meta.id;
    snap.drops = state.drops;
    snap.pending_command_warn = static_cast<std::uint32_t>(command_warns.load(std::memory_order_relaxed));
    ui_.publish();

    if (live_) {
      const std::uint32_t displaced = live_present_.publish(index);
      if (displaced != Mailbox::kEmpty) {
        pool_.release(displaced);
        drops.fetch_add(1, std::memory_order_relaxed);
        pool_wake_.notify();
      }
    } else if (!to_present_.push_wait(index, stop_)) {
      pool_.release(index);
      break;
    }
  }
  std::uint32_t leftover = 0;
  while (to_track_.try_pop(leftover)) pool_.release(leftover);
  t3_done_.store(true, std::memory_order_release);
  to_present_.wake();
}

void Pipeline::run_t6() {
  set_telemetry_thread();
  while (!stop_.load(std::memory_order_acquire)) {
    telemetry_.drain([&](const TelemetrySample& sample) { sink_->record(sample); });
    const LatencySnapshot snap = sink_->snapshot();
    const int present = static_cast<int>(Stage::Present);
    present_p50_ns.store(snap.p50_ns[present], std::memory_order_relaxed);
    present_p99_ns.store(snap.p99_ns[present], std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  telemetry_.drain([&](const TelemetrySample& sample) { sink_->record(sample); });
  const LatencySnapshot snap = sink_->snapshot();
  const int present = static_cast<int>(Stage::Present);
  present_p50_ns.store(snap.p50_ns[present], std::memory_order_relaxed);
  present_p99_ns.store(snap.p99_ns[present], std::memory_order_relaxed);
}

}  // namespace touchfc
