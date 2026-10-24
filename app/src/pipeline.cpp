#include "pipeline.hpp"

#include "marker.hpp"
#include "threads_win.hpp"

#include "touchfc/frame_check.hpp"
#include "touchfc/time.hpp"

#include <chrono>

namespace touchfc {

Pipeline::Pipeline(FramePool& pool) : pool_(pool) {}

Pipeline::~Pipeline() { join(); }

void Pipeline::start(FrameSource& source, HdrSink& sink, Detector& detector, bool pin) {
  source_ = &source;
  sink_ = &sink;
  detector_ = &detector;
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
    if (detector_) detector_->run(slot);
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

void Pipeline::run_t3() {
  if (pin_) pin_current_thread_to_one_ccd();
  set_pipeline_thread();
  for (;;) {
    UiCommand command;
    while (commands_.try_pop(command)) {
      if (command.type == CmdType::ApplyAttackDir) {
        attack_dir.store(command.u8a == 0 ? -1 : 1, std::memory_order_relaxed);
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
