#include "touchfc/arena.hpp"
#include "touchfc/blocking_spsc.hpp"
#include "touchfc/config_json.hpp"
#include "touchfc/cpu.hpp"
#include "touchfc/detection.hpp"
#include "touchfc/frame_check.hpp"
#include "touchfc/gmc.hpp"
#include "touchfc/hdr_sink.hpp"
#include "touchfc/homography.hpp"
#include "touchfc/image_gmc.hpp"
#include "touchfc/pitch_calib.hpp"
#include "touchfc/kalman.hpp"
#include "touchfc/line_fit.hpp"
#include "touchfc/mailbox.hpp"
#include "touchfc/nms.hpp"
#include "touchfc/pitch.hpp"
#include "touchfc/pitch_control.hpp"
#include "touchfc/possession.hpp"
#include "touchfc/pitch_landmarks.hpp"
#include "touchfc/pitch_pose.hpp"
#include "touchfc/play_state.hpp"
#include "touchfc/preprocess.hpp"
#include "touchfc/seqlock.hpp"
#include "touchfc/scene.hpp"
#include "touchfc/slot_pool.hpp"
#include "touchfc/spsc_ring.hpp"
#include "touchfc/team.hpp"
#include "touchfc/telemetry.hpp"
#include "touchfc/time.hpp"
#include "touchfc/tracker.hpp"
#include "touchfc/triple_buffer.hpp"
#include "touchfc/types.hpp"
#include "touchfc/wake.hpp"
#include "touchfc/yolo.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstring>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace touchfc;

TEST_CASE("steady clock is monotonic") {
  const TimeNs a = now_ns();
  const TimeNs b = now_ns();
  REQUIRE(b >= a);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const TimeNs c = now_ns();
  REQUIRE(c - a >= 1'000'000);
}

TEST_CASE("spsc ring rejects overflow and preserves order") {
  SpscRing<std::uint32_t, 8> ring;
  for (std::uint32_t i = 0; i < 8; ++i) REQUIRE(ring.try_push(i));
  REQUIRE_FALSE(ring.try_push(99));
  std::uint32_t v = 0;
  REQUIRE(ring.try_pop(v));
  REQUIRE(v == 0);
  REQUIRE(ring.try_push(8));
  for (std::uint32_t expect = 1; expect <= 8; ++expect) {
    REQUIRE(ring.try_pop(v));
    REQUIRE(v == expect);
  }
  REQUIRE_FALSE(ring.try_pop(v));
}

TEST_CASE("spsc try_push_with writes in place") {
  SpscRing<std::uint64_t, 4> ring;
  REQUIRE(ring.try_push_with([](std::uint64_t& slot) noexcept { slot = 42; }));
  std::uint64_t v = 0;
  REQUIRE(ring.try_pop(v));
  REQUIRE(v == 42);
}

TEST_CASE("spsc concurrent order") {
  constexpr std::uint32_t kN = 200000;
  auto ring = std::make_unique<SpscRing<std::uint32_t, 1024>>();
  std::atomic<int> bad{0};
  std::thread producer([&] {
    for (std::uint32_t i = 0; i < kN; ++i) {
      while (!ring->try_push(i)) cpu_pause();
    }
  });
  std::uint32_t expect = 0;
  while (expect < kN) {
    std::uint32_t v = 0;
    if (!ring->try_pop(v)) {
      cpu_pause();
      continue;
    }
    if (v != expect) bad.fetch_add(1);
    ++expect;
  }
  producer.join();
  REQUIRE(bad.load() == 0);
}

TEST_CASE("mailbox drops the displaced index") {
  Mailbox box;
  REQUIRE(box.publish(1) == Mailbox::kEmpty);
  REQUIRE(box.publish(2) == 1);
  REQUIRE(box.take() == 2);
  REQUIRE(box.take() == Mailbox::kEmpty);
}

TEST_CASE("mailbox concurrent tokens are unique") {
  constexpr std::uint32_t kN = 100000;
  Mailbox box;
  std::vector<std::uint8_t> got(kN);
  std::atomic<int> dups{0};
  auto claim = [&](std::uint32_t token) {
    if (token == Mailbox::kEmpty) return;
    if (token >= kN) {
      dups.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const std::uint8_t prev = std::atomic_ref<std::uint8_t>(got[token]).exchange(1);
    if (prev != 0) dups.fetch_add(1, std::memory_order_relaxed);
  };
  std::atomic<bool> producer_done{false};
  std::thread producer([&] {
    for (std::uint32_t i = 0; i < kN; ++i) claim(box.publish(i));
    producer_done.store(true, std::memory_order_release);
  });
  std::thread consumer([&] {
    for (;;) {
      std::uint32_t v = box.take();
      if (v == Mailbox::kEmpty) {
        if (!producer_done.load(std::memory_order_acquire)) {
          cpu_pause();
          continue;
        }
        v = box.take();
        if (v == Mailbox::kEmpty) break;
      }
      claim(v);
    }
  });
  producer.join();
  consumer.join();
  REQUIRE(dups.load() == 0);
  std::uint32_t missing = 0;
  for (std::uint32_t i = 0; i < kN; ++i) {
    if (std::atomic_ref<std::uint8_t>(got[i]).load() == 0) ++missing;
  }
  REQUIRE(missing == 0);
}

TEST_CASE("triple buffer does not tear and keeps three owners apart") {
  struct Rec {
    std::uint64_t a, b, c, d;
  };
  TripleBuffer<Rec> buf;
  std::atomic<bool> start{false};
  std::atomic<bool> writer_done{false};
  std::atomic<int> bad{0};
  std::atomic<int> reads{0};
  std::thread writer([&] {
    while (!start.load()) cpu_pause();
    for (std::uint64_t i = 1; i <= 150000; ++i) {
      Rec& slot = buf.write_slot();
      slot.a = slot.b = slot.c = slot.d = i;
      buf.publish();
    }
    writer_done.store(true);
  });
  std::thread reader([&] {
    while (!start.load()) cpu_pause();
    while (!writer_done.load()) {
      Rec* p = buf.try_read();
      if (!p) {
        cpu_pause();
        continue;
      }
      if (!(p->a == p->b && p->b == p->c && p->c == p->d)) bad.fetch_add(1);
      reads.fetch_add(1);
    }
    if (Rec* p = buf.try_read()) {
      if (!(p->a == p->b && p->b == p->c && p->c == p->d)) bad.fetch_add(1);
      reads.fetch_add(1);
    }
  });
  start.store(true);
  writer.join();
  reader.join();
  REQUIRE(bad.load() == 0);
  REQUIRE(reads.load() > 0);
}

TEST_CASE("seqlock snapshot is consistent under a writer") {
  struct Rec {
    std::uint64_t w[8];
  };
  static_assert(sizeof(Rec) % 8 == 0);
  SeqLock<Rec> seq;
  std::atomic<bool> start{false};
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0};
  std::thread writer([&] {
    while (!start.load()) cpu_pause();
    Rec rec{};
    for (std::uint64_t i = 1; i <= 100000; ++i) {
      for (auto& word : rec.w) word = i;
      seq.store(rec);
    }
    stop.store(true, std::memory_order_release);
  });
  std::thread reader([&] {
    while (!start.load()) cpu_pause();
    while (!stop.load(std::memory_order_acquire)) {
      const Rec rec = seq.load();
      const std::uint64_t v = rec.w[0];
      for (int i = 1; i < 8; ++i) {
        if (rec.w[i] != v) bad.fetch_add(1);
      }
    }
  });
  start.store(true);
  writer.join();
  reader.join();
  REQUIRE(bad.load() == 0);
  Rec last{};
  for (auto& word : last.w) word = 7;
  seq.store(last);
  REQUIRE(seq.load().w[3] == 7);
}

TEST_CASE("slot pool hands out each index once") {
  constexpr int kSlots = 32;
  SlotPool pool(kSlots);
  std::atomic<int> owner[kSlots];
  for (auto& o : owner) o.store(0);
  std::atomic<int> bad{0};
  std::atomic<int> ops{0};
  auto worker = [&] {
    for (int n = 0; n < 20000; ++n) {
      const int slot = pool.try_acquire();
      if (slot < 0) {
        cpu_pause();
        continue;
      }
      int expected = 0;
      if (!owner[slot].compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        bad.fetch_add(1);
      }
      owner[slot].store(0, std::memory_order_release);
      pool.release(static_cast<std::uint32_t>(slot));
      ops.fetch_add(1);
    }
  };
  std::thread threads[4];
  for (auto& t : threads) t = std::thread(worker);
  for (auto& t : threads) t.join();
  REQUIRE(bad.load() == 0);
  REQUIRE(ops.load() > 0);
  REQUIRE(pool.free_approx() == kSlots);

  int got = 0;
  int slots[kSlots];
  for (;;) {
    const int s = pool.try_acquire();
    if (s < 0) break;
    slots[got++] = s;
  }
  REQUIRE(got == kSlots);
  REQUIRE(pool.try_acquire() < 0);
  for (int i = 0; i < got; ++i) pool.release(static_cast<std::uint32_t>(slots[i]));
}

TEST_CASE("wake is not lost if notify happens first") {
  WakeSeq wake;
  const std::uint32_t seen = wake.load();
  wake.notify();
  const std::uint32_t now = wake.wait_from(seen, 0);
  REQUIRE(now != seen);
}

TEST_CASE("wake unblocks a sleeper") {
  WakeSeq wake;
  std::atomic<bool> entered{false};
  std::atomic<bool> done{false};
  const std::uint32_t seen = wake.load();
  std::thread waiter([&] {
    entered.store(true);
    wake.wait_from(seen, 0);
    done.store(true);
  });
  while (!entered.load()) cpu_pause();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  REQUIRE_FALSE(done.load());
  wake.notify();
  waiter.join();
  REQUIRE(done.load());
}

TEST_CASE("blocking spsc waits for space and for data") {
  BlockingSpsc<int, 2> q;
  REQUIRE(q.try_push(1));
  REQUIRE(q.try_push(2));
  std::atomic<bool> stop{false};
  std::atomic<bool> pushed{false};
  std::thread producer([&] {
    REQUIRE(q.push_wait(3, stop));
    pushed.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  REQUIRE_FALSE(pushed.load());
  int v = 0;
  REQUIRE(q.try_pop(v));
  REQUIRE(v == 1);
  producer.join();
  REQUIRE(pushed.load());
  while (q.try_pop(v)) {
  }

  std::atomic<int> popped_value{-1};
  std::thread consumer([&] {
    int out = -1;
    if (q.pop_wait(out, stop)) popped_value.store(out);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  REQUIRE(popped_value.load() == -1);
  REQUIRE(q.try_push(9));
  consumer.join();
  REQUIRE(popped_value.load() == 9);

  std::atomic<bool> blocked_done{false};
  std::thread blocked([&] {
    int out = 0;
    blocked_done.store(!q.pop_wait(out, stop));
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  stop.store(true);
  q.wake();
  blocked.join();
  REQUIRE(blocked_done.load());
}

TEST_CASE("arena aligns, resets and refuses overflow") {
  Arena arena(256);
  void* a = arena.allocate(16, 64);
  void* b = arena.allocate(32, 64);
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  REQUIRE(reinterpret_cast<std::uintptr_t>(a) % 64 == 0);
  REQUIRE(reinterpret_cast<std::uintptr_t>(b) % 64 == 0);
  REQUIRE(b > a);
  REQUIRE(arena.allocate(10'000, 64) == nullptr);
  arena.reset();
  void* c = arena.allocate(16, 64);
  REQUIRE(c == a);
}

TEST_CASE("spadl conversion round-trips and flips attack direction") {
  const PitchPoint centre{0.f, 0.f};
  const PitchPoint spadl = internal_to_spadl(centre, +1);
  REQUIRE(spadl.x == 52.5f);
  REQUIRE(spadl.y == 34.f);
  const PitchPoint back = spadl_to_internal(spadl, +1);
  REQUIRE(back.x == 0.f);
  REQUIRE(back.y == 0.f);

  const PitchPoint attacking_goal = internal_to_spadl(PitchPoint{52.5f, 0.f}, +1);
  REQUIRE(attacking_goal.x == 105.f);
  const PitchPoint other = internal_to_spadl(PitchPoint{-52.5f, 0.f}, -1);
  REQUIRE(other.x == 105.f);

  const PitchPoint sample{10.f, -4.f};
  const PitchPoint via = spadl_to_internal(internal_to_spadl(sample, -1), -1);
  REQUIRE(via.x == sample.x);
  REQUIRE(via.y == sample.y);
}

TEST_CASE("black-frame and stride hash") {
  std::vector<std::uint8_t> black(64 * 32 * 4, 0);
  REQUIRE(looks_black_bgra(black.data(), 64, 32, 64 * 4));
  // step_x is width/8 == 8, so x = 8 is on the sample grid.
  black[8 * 4 + 0] = 200;
  black[8 * 4 + 1] = 200;
  black[8 * 4 + 2] = 200;
  REQUIRE_FALSE(looks_black_bgra(black.data(), 64, 32, 64 * 4));

  std::vector<std::uint8_t> a(128, 1);
  std::vector<std::uint8_t> b = a;
  REQUIRE(hash_bytes_stride(a.data(), a.size(), 16) == hash_bytes_stride(b.data(), b.size(), 16));
  b[0] = 2;
  REQUIRE(hash_bytes_stride(a.data(), a.size(), 16) != hash_bytes_stride(b.data(), b.size(), 16));
}

TEST_CASE("telemetry hub and hdr percentiles") {
  TelemetryHub hub;
  TelemetrySample sample;
  sample.frame = 3;
  sample.stage = static_cast<std::uint8_t>(Stage::Track);
  sample.t_begin = 0;
  sample.t_end = 1000;
  REQUIRE(hub.push(2, sample));
  int got = 0;
  hub.drain([&](const TelemetrySample& s) {
    ++got;
    REQUIRE(s.frame == 3);
    REQUIRE(s.t_end - s.t_begin == 1000);
  });
  REQUIRE(got == 1);

  HdrSink sink;
  for (int i = 0; i < 1000; ++i) {
    TelemetrySample s;
    s.stage = static_cast<std::uint8_t>(Stage::Present);
    s.t_begin = 0;
    s.t_end = 1'000'000 + static_cast<std::int64_t>(i) * 1000;
    sink.record(s);
  }
  const LatencySnapshot snap = sink.snapshot();
  const int present = static_cast<int>(Stage::Present);
  REQUIRE(snap.count[present] == 1000);
  REQUIRE(snap.p50_ns[present] > 500'000);
  REQUIRE(snap.p50_ns[present] < 2'000'000);
  REQUIRE(sink.write_csv("telemetry_test.csv"));
  std::remove("telemetry_test.csv");
}

TEST_CASE("config fits the seqlock") {
  SeqLock<Config> cfg(Config{});
  Config next = cfg.load();
  next.version = 2;
  next.conf_player = 0.5f;
  next.show_boxes = 0;
  cfg.store(next);
  const Config got = cfg.load();
  REQUIRE(got.version == 2);
  REQUIRE(got.conf_player == 0.5f);
  REQUIRE(got.show_boxes == 0);
}

TEST_CASE("letterbox parameters round-trip a box") {
  const LetterboxParams p = make_letterbox(1920, 1080, 640);
  REQUIRE(p.scale > 0.33f);
  REQUIRE(p.scale < 0.34f);
  REQUIRE(p.pad_x == 0.f);          // 1920x scales to 640x
  REQUIRE(p.pad_y > 0);              // 1080y scales to 360y, 140y padding total
  float x = 320.f, y = 180.f, w = 64.f, h = 128.f;
  unletterbox_box(p, x, y, w, h);
  // 320 letterboxed <-> 960 src; 180 lb y-pad is 140, so (180 - 140) / scale = 120
  REQUIRE(std::abs(x - 960.f) < 1.f);
  REQUIRE(std::abs(y - 120.f) < 1.f);
}

TEST_CASE("preprocess scalar and avx2 agree where defined") {
  constexpr std::uint32_t sw = 320, sh = 180, D = 128;
  std::vector<std::uint8_t> src(static_cast<std::size_t>(sw) * sh * 4);
  for (std::size_t i = 0; i < src.size(); i += 4) {
    src[i + 0] = static_cast<std::uint8_t>((i / 4) & 0xff);        // B
    src[i + 1] = static_cast<std::uint8_t>(((i / 4) >> 8) & 0xff); // G
    src[i + 2] = static_cast<std::uint8_t>(((i / 4) >> 1) & 0xff); // R
    src[i + 3] = 255;
  }
  const LetterboxParams p = make_letterbox(sw, sh, D);
  std::vector<float> a(3 * D * D), b(3 * D * D);
  bgra_letterbox_to_chw_scalar(src.data(), sw, sh, sw * 4, a.data(), p);
#if defined(TOUCHFC_HAS_AVX2)
  bgra_letterbox_to_chw_avx2(src.data(), sw, sh, sw * 4, b.data(), p);
  // Nearest + fixed-point can disagree by one source pixel at a few lanes.
  // Count mismatches instead of requiring bitwise equality.
  int bad = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::abs(a[i] - b[i]) > 1.5f / 255.f) ++bad;
  }
  REQUIRE(bad * 1000 < static_cast<int>(a.size()));  // < 0.1 % of pixels disagree
#else
  REQUIRE(a == a);  // AVX2 not compiled; scalar path is the only one.
#endif
}

TEST_CASE("preprocess paints the letterbox borders") {
  constexpr std::uint32_t sw = 160, sh = 90, D = 64;
  std::vector<std::uint8_t> src(static_cast<std::size_t>(sw) * sh * 4, 50);
  const LetterboxParams p = make_letterbox(sw, sh, D);
  std::vector<float> dst(3 * D * D);
  bgra_letterbox_to_chw(src.data(), sw, sh, sw * 4, dst.data(), p);
  // Top row is pure padding for 160x90 -> 64x36 inside 64x64 (14 px pad).
  const float pad = 114.f / 255.f;
  REQUIRE(std::abs(dst[0] - pad) < 1e-3f);
  REQUIRE(std::abs(dst[D * D] - pad) < 1e-3f);         // G plane
  REQUIRE(std::abs(dst[2 * D * D] - pad) < 1e-3f);     // B plane
}

TEST_CASE("greedy nms keeps the top box per class and drops overlaps") {
  Detection d[6]{};
  d[0] = {100, 100, 50, 100, 0.95f, 0, 0};
  d[1] = {110, 100, 50, 100, 0.80f, 0, 0};  // overlaps d[0], same class -> drop
  d[2] = {400, 100, 50, 100, 0.70f, 0, 0};  // alone, same class -> keep
  d[3] = {100, 100, 50, 100, 0.60f, 1, 0};  // overlaps d[0] but different class -> keep
  d[4] = {800, 800,  8,   8, 0.90f, 3, 0};  // ball, alone -> keep
  d[5] = {802, 801,  8,   8, 0.50f, 3, 0};  // overlaps ball -> drop
  const std::uint32_t kept = greedy_nms(d, 6, 0.45f);
  REQUIRE(kept == 4);
  // The strongest four should be: 0.95 (cls 0), 0.90 (cls 3), 0.70 (cls 0), 0.60 (cls 1).
  float scores[4] = {d[0].score, d[1].score, d[2].score, d[3].score};
  for (float s : scores) REQUIRE(s >= 0.6f);
}

TEST_CASE("yolo decode drops below-threshold anchors") {
  constexpr std::uint32_t nc = 4;
  constexpr std::uint32_t na = 32;
  std::vector<float> output(static_cast<std::size_t>(4 + nc) * na, 0.f);
  auto set = [&](std::uint32_t ch, std::uint32_t a, float v) {
    output[static_cast<std::size_t>(ch) * na + a] = v;
  };
  // Anchor 0: a confident player at (100,100,64,128) in a 640x640 letterbox.
  set(0, 0, 100 + 32);  // cx
  set(1, 0, 100 + 64);  // cy
  set(2, 0, 64);
  set(3, 0, 128);
  set(4, 0, 0.9f);  // class 0 score
  // Anchor 1: a weak ball below threshold.
  set(0, 1, 300);
  set(1, 1, 300);
  set(2, 1, 8);
  set(3, 1, 8);
  set(7, 1, 0.2f);
  // Anchor 2: another high-score ball.
  set(0, 2, 500);
  set(1, 2, 500);
  set(2, 2, 10);
  set(3, 2, 10);
  set(7, 2, 0.95f);
  const LetterboxParams p = make_letterbox(1280, 720, 640);
  Detection out[16]{};
  std::uint32_t count = 0;
  bool trunc = false;
  decode_yolov8(output.data(), nc, na, 0.25f, p, out, 16, count, trunc);
  REQUIRE_FALSE(trunc);
  REQUIRE(count == 2);
  REQUIRE(out[0].score >= 0.9f);              // sorted by score, 0.95 first
  REQUIRE(out[0].class_id == 3);
  REQUIRE(out[1].class_id == 0);
}

TEST_CASE("1D kalman learns velocity") {
  Kalman1D k;
  k.Pxx = 100.f;
  k.Pvv = 100.f;
  const float dt = 1.f / 30.f;
  for (int i = 0; i < 90; ++i) {
    k.predict(dt, 0.5f, 0.5f);
    k.update(50.f * (i + 1) * dt, 2.f);
  }
  REQUIRE(std::abs(k.v - 50.f) < 5.f);
}

TEST_CASE("gmc ignores one bad pair") {
  float px[] = {100, 200, 300, 400, 500, 50};
  float py[] = {100, 100, 100, 100, 100, 100};
  float cx[] = {110, 210, 310, 410, 510, 400};
  float cy[] = {105, 105, 105, 105, 105, 300};
  const auto g = estimate_translation(px, py, cx, cy, 6);
  REQUIRE(std::abs(g.dx - 10.f) < 1.f);
  REQUIRE(std::abs(g.dy - 5.f) < 1.f);
}

TEST_CASE("tracker keeps ids across a pan") {
  Tracker tr;
  TrackerParams params;
  params.conf_new_track = 0.5f;
  Detection dets[2]{};
  dets[0] = {100, 300, 40, 80, 0.9f, 0, 0};
  dets[1] = {500, 300, 40, 80, 0.9f, 0, 0};
  tr.update(dets, 2, 0.02f, {}, params);
  const auto a = dets[0].track_id;
  const auto b = dets[1].track_id;
  REQUIRE(a != 0);
  REQUIRE(b != 0);
  REQUIRE(a != b);
  for (int i = 0; i < 40; ++i) {
    dets[0] = {100.f + 5.f * (i + 1), 300, 40, 80, 0.9f, 0, 0};
    dets[1] = {500.f + 5.f * (i + 1), 300, 40, 80, 0.9f, 0, 0};
    tr.update(dets, 2, 0.02f, {}, params);
    REQUIRE(dets[0].track_id == a);
    REQUIRE(dets[1].track_id == b);
  }
}

TEST_CASE("homography recovers a scale and shift") {
  const float sx[] = {10, 40, 10, 40, 25};
  const float sy[] = {10, 10, 40, 40, 25};
  float dx[5], dy[5];
  for (int i = 0; i < 5; ++i) {
    dx[i] = 2.f * sx[i] + 1.f;
    dy[i] = 3.f * sy[i] - 4.f;
  }
  Homography H;
  REQUIRE(fit_homography(sx, sy, dx, dy, 5, H));
  float X = 0, Y = 0;
  apply_homography(H, 15.f, 15.f, X, Y);
  REQUIRE(std::abs(X - 31.f) < 0.05f);
  REQUIRE(std::abs(Y - 41.f) < 0.05f);
  REQUIRE(H.rms_m < 0.05f);
}

TEST_CASE("green frame is a wide scene") {
  std::vector<std::uint8_t> px(64 * 64 * 4, 0);
  for (std::size_t i = 0; i < px.size(); i += 4) {
    px[i] = 30;
    px[i + 1] = 170;
    px[i + 2] = 30;
  }
  REQUIRE(classify_scene_bgra(px.data(), 64, 64, 64 * 4) == Scene::Wide);
  std::fill(px.begin(), px.end(), 0);
  REQUIRE(classify_scene_bgra(px.data(), 64, 64, 64 * 4) == Scene::Other);
}

TEST_CASE("pitch landmarks sit on the 105 by 68 frame") {
  float x = 0, y = 0;
  pitch_landmark_m(0, x, y);
  REQUIRE(std::abs(x - -52.5f) < 0.05f);
  REQUIRE(std::abs(y - 34.f) < 0.05f);
  pitch_landmark_m(13, x, y);
  REQUIRE(std::abs(x) < 0.05f);
  REQUIRE(std::abs(y - 34.f) < 0.05f);
}

TEST_CASE("ransac homography ignores one bad point") {
  const float sx[] = {10, 40, 10, 40, 25, 70, 70, 25};
  const float sy[] = {10, 10, 40, 40, 25, 15, 50, 55};
  float dx[8], dy[8];
  for (int i = 0; i < 8; ++i) {
    dx[i] = 2.f * sx[i] + 1.f;
    dy[i] = 3.f * sy[i] - 4.f;
  }
  dx[7] += 30.f;
  Homography H;
  REQUIRE(fit_homography_ransac(sx, sy, dx, dy, 8, 1.f, H));
  float X = 0, Y = 0;
  apply_homography(H, 15.f, 15.f, X, Y);
  REQUIRE(std::abs(X - 31.f) < 0.05f);
  REQUIRE(std::abs(Y - 41.f) < 0.05f);
}

TEST_CASE("pitch pose decode keeps the visible keypoints") {
  constexpr std::uint32_t anchors = 2;
  constexpr std::uint32_t kpts = 2;
  constexpr std::uint32_t channels = 5 + 3 * kpts;
  float out[channels * anchors]{};
  out[4 * anchors + 1] = 0.9f;
  const std::size_t k0 = static_cast<std::size_t>(5) * anchors;
  out[k0 + 1] = 100.f;
  out[k0 + anchors + 1] = 50.f;
  out[k0 + 2 * anchors + 1] = 0.8f;
  const std::size_t k1 = static_cast<std::size_t>(8) * anchors;
  out[k1 + 1] = 10.f;
  out[k1 + anchors + 1] = 10.f;
  out[k1 + 2 * anchors + 1] = 0.1f;
  const LetterboxParams lb = make_letterbox(640, 640, 640);
  PitchKeypoint found[4];
  const std::uint32_t n = decode_pitch_pose(out, kpts, anchors, 0.25f, 0.4f, lb, found, 4);
  REQUIRE(n == 1);
  REQUIRE(found[0].id == 0);
  REQUIRE(std::abs(found[0].x - 100.f) < 0.01f);
  REQUIRE(std::abs(found[0].y - 50.f) < 0.01f);
}

TEST_CASE("tti row matches the scalar intercept time") {
  const float tx[8] = {-40, -20, -5, 0, 8, 15, 30, 48};
  const float ty[8] = {0, 4, -10, 20, -20, 6, 12, -8};
  float row[8];
  tti_row8(-10.f, 2.f, 3.f, -1.f, tx, ty, row);
  for (int i = 0; i < 8; ++i) {
    const float s = tti_seconds(-10.f, 2.f, 3.f, -1.f, tx[i], ty[i]);
    REQUIRE(std::abs(row[i] - s) < 0.02f);
  }
}

TEST_CASE("pitch control favours the nearer team") {
  PcPlayer players[2] = {};
  players[0] = {-20.f, 0.f, 0.f, 0.f, 0};
  players[1] = {20.f, 0.f, 0.f, 0.f, 1};
  float grid[kPcCells];
  const float territory = pitch_control(players, 2, 0.f, 0.f, false, grid);
  int i0 = 0, i1 = 0;
  float best0 = 1e9f, best1 = 1e9f;
  for (int i = 0; i < kPcNx; ++i) {
    const float d0 = std::abs(pc_cell_x(i) - -20.f);
    const float d1 = std::abs(pc_cell_x(i) - 20.f);
    if (d0 < best0) { best0 = d0; i0 = i; }
    if (d1 < best1) { best1 = d1; i1 = i; }
  }
  const int row = kPcNy / 2;
  REQUIRE(grid[row * kPcNx + i0] > 0.65f);
  REQUIRE(grid[row * kPcNx + i1] < 0.35f);
  REQUIRE(territory > 0.35f);
  REQUIRE(territory < 0.65f);
}

TEST_CASE("statsbomb frame is a pitch-control input") {
  const PitchPoint centre = statsbomb_to_internal(60.f, 40.f);
  REQUIRE(std::abs(centre.x) < 0.01f);
  REQUIRE(std::abs(centre.y) < 0.01f);
  PcPlayer players[10]{};
  for (int i = 0; i < 5; ++i) {
    const PitchPoint h = statsbomb_to_internal(90.f + static_cast<float>(i), 30.f + static_cast<float>(i) * 4.f);
    players[i] = {h.x, h.y, 0.f, 0.f, 0};
    const PitchPoint a = statsbomb_to_internal(20.f + static_cast<float>(i), 28.f + static_cast<float>(i) * 4.f);
    players[5 + i] = {a.x, a.y, 0.f, 0.f, 1};
  }
  float grid[kPcCells];
  const auto t0 = std::chrono::steady_clock::now();
  pitch_control(players, 10, 0.f, 0.f, false, grid);
  const PitchPoint home = statsbomb_to_internal(92.f, 40.f);
  const PitchPoint away = statsbomb_to_internal(22.f, 40.f);
  int ih = 0, ia = 0;
  float dh = 1e9f, da = 1e9f;
  for (int i = 0; i < kPcNx; ++i) {
    const float dxh = std::abs(pc_cell_x(i) - home.x);
    const float dxa = std::abs(pc_cell_x(i) - away.x);
    if (dxh < dh) { dh = dxh; ih = i; }
    if (dxa < da) { da = dxa; ia = i; }
  }
  const int mid = kPcNy / 2;
  REQUIRE(grid[mid * kPcNx + ih] > 0.65f);
  REQUIRE(grid[mid * kPcNx + ia] < 0.35f);
  PcPlayer crowd[22]{};
  for (int i = 0; i < 22; ++i) {
    crowd[i].x = -40.f + static_cast<float>(i) * 4.f;
    crowd[i].y = (i % 2 == 0) ? -15.f : 15.f;
    crowd[i].team = static_cast<std::uint8_t>(i < 11 ? 0 : 1);
  }
  pitch_control(crowd, 22, 0.f, 0.f, true, grid);
  const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  REQUIRE(ms < 100.0);
}

TEST_CASE("team colour follows the seed and survives a gap") {
  const Lab red = rgb_to_lab(180, 30, 30);
  const Lab blue = rgb_to_lab(30, 60, 180);
  REQUIRE(nearest_team(rgb_to_lab(170, 40, 40), red, true, blue, true) == 1);
  REQUIRE(nearest_team(rgb_to_lab(40, 70, 190), red, true, blue, true) == 2);
  LostLabel lost[1] = {{10.f, 2.f, red, 1, 1000}};
  REQUIRE(inherit_team(lost, 1, 12.f, 3.f, red, 1500) == 1);
  REQUIRE(inherit_team(lost, 1, 40.f, 3.f, red, 1500) == 0);
  REQUIRE(inherit_team(lost, 1, 12.f, 3.f, blue, 1500) == 0);
}

TEST_CASE("possession closes a spell and spadl uses the spec columns") {
  Possession s;
  const PossOut start = step_possession(s, true, 0.f, 0.f, 4, 1, 1.f, 1.5f, 1000000000LL, 0);
  REQUIRE_FALSE(start.emitted);
  REQUIRE(s.held);
  const PossOut next = step_possession(s, true, 10.f, 0.f, 9, 2, 0.4f, 1.5f, 2000000000LL, 0);
  REQUIRE(next.emitted);
  REQUIRE(next.event.team == 1);
  REQUIRE(next.event.track == 4);
  REQUIRE(s.track == 9);
  Possession far;
  const PossOut none = step_possession(far, true, 0.f, 0.f, 4, 1, 5.f, 1.5f, 0, 0);
  REQUIRE_FALSE(none.emitted);
  REQUIRE_FALSE(far.held);
  char row[256];
  Event e = next.event;
  e.id = 1;
  e.match_ms_end = 2000;
  format_spadl_row(row, 256, e, 1);
  REQUIRE(std::strstr(kSpadlHeader, "type_id") != nullptr);
  REQUIRE(std::strstr(row, ",21,1,0") != nullptr);
  REQUIRE(std::strstr(row, "52.50,34.00") != nullptr);
}

namespace {

// Pitch metres -> 1920x1080 image, a broadcast-like perspective.
constexpr float kP[9] = {12.f, 0.f, 960.f, 0.f, -12.f, 540.f, 0.0005f, 0.004f, 1.f};

void project(const float* P, float X, float Y, float& x, float& y) {
  const float w = P[6] * X + P[7] * Y + P[8];
  x = (P[0] * X + P[1] * Y + P[2]) / w;
  y = (P[3] * X + P[4] * Y + P[5]) / w;
}

// Landmarks visible under P, as keypoint pairs.
int visible_landmarks(const float* P, float* sx, float* sy, float* dx, float* dy) {
  int n = 0;
  for (int i = 0; i < kPitchLandmarks; ++i) {
    float X, Y, x, y;
    pitch_landmark_m(i, X, Y);
    project(P, X, Y, x, y);
    if (x < 0 || y < 0 || x >= 1920 || y >= 1080) continue;
    sx[n] = x;
    sy[n] = y;
    dx[n] = X;
    dy[n] = Y;
    ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("synthetic homography round-trips all 32 landmarks within 1 cm") {
  float sx[32], sy[32], dx[32], dy[32];
  for (int i = 0; i < kPitchLandmarks; ++i) {
    pitch_landmark_m(i, dx[i], dy[i]);
    project(kP, dx[i], dy[i], sx[i], sy[i]);
  }
  Homography H;
  REQUIRE(fit_homography(sx, sy, dx, dy, kPitchLandmarks, H));
  Homography inv;
  REQUIRE(invert3(H.h, inv.h));
  for (int i = 0; i < kPitchLandmarks; ++i) {
    float X, Y, x, y;
    REQUIRE(apply_homography(H, sx[i], sy[i], X, Y));
    REQUIRE(std::hypot(X - dx[i], Y - dy[i]) < 0.01f);
    REQUIRE(apply_homography(inv, dx[i], dy[i], x, y));
    REQUIRE(std::hypot(x - sx[i], y - sy[i]) < 0.1f);
  }
}

TEST_CASE("landmarks are real pitch markings") {
  float x, y;
  pitch_landmark_m(9, x, y);  // left penalty-area corner, top
  REQUIRE(std::abs(x - -36.f) < 1e-4f);
  REQUIRE(std::abs(y - 20.16f) < 1e-4f);
  pitch_landmark_m(21, x, y);  // right penalty spot
  REQUIRE(std::abs(x - 41.5f) < 1e-4f);
  pitch_landmark_m(31, x, y);
  REQUIRE(std::abs(x - 9.15f) < 1e-4f);
}

TEST_CASE("projection guard rejects points above the horizon") {
  Homography H;
  const float h[9] = {1, 0, 0, 0, 1, 0, 0, -0.01f, 1};
  for (int i = 0; i < 9; ++i) H.h[i] = h[i];
  float X, Y;
  REQUIRE(apply_homography(H, 10.f, 50.f, X, Y));
  REQUIRE_FALSE(apply_homography(H, 10.f, 100.f, X, Y));
  REQUIRE_FALSE(apply_homography(H, 10.f, 200.f, X, Y));
  REQUIRE(on_pitch(55.f, 0.f, 3.f));
  REQUIRE_FALSE(on_pitch(56.f, 0.f, 3.f));
}

TEST_CASE("pitch calib validates, holds jumps and smooths fits") {
  float sx[32], sy[32], dx[32], dy[32];
  const int n = visible_landmarks(kP, sx, sy, dx, dy);
  REQUIRE(n >= 8);
  Homography fit;
  REQUIRE(fit_homography_ransac(sx, sy, dx, dy, n, 2.f, fit));
  PitchCalibParams p;
  PitchCalib c;
  const float head = 200.f;

  Homography bad = fit;
  bad.rms_m = 2.f;
  REQUIRE(c.offer(bad, sx, sy, dx, dy, n, 1920, 1080, head, p) == FitReject::Rms);
  REQUIRE_FALSE(c.valid());
  REQUIRE(c.offer(fit, sx, sy, dx, dy, n, 1920, 1080, head, p) == FitReject::None);
  REQUIRE(c.valid());

  // Mirrored ids flip the determinant.
  float mx[32];
  for (int i = 0; i < n; ++i) mx[i] = -dx[i];
  Homography mirror;
  REQUIRE(fit_homography(sx, sy, mx, dy, n, mirror));
  REQUIRE(c.offer(mirror, sx, sy, mx, dy, n, 1920, 1080, head, p) == FitReject::Orient);

  // All inliers in one quadrant.
  float qx[6], qy[6], qX[6], qY[6];
  for (int i = 0; i < 6; ++i) {
    qx[i] = 100.f + 120.f * i;
    qy[i] = 300.f + 30.f * (i % 3);
    apply_homography(fit, qx[i], qy[i], qX[i], qY[i]);
  }
  REQUIRE(c.offer(fit, qx, qy, qX, qY, 6, 1920, 1080, head, p) == FitReject::Spread);

  // A 1 m shift is blended by `smooth`.
  float X0, Y0, X1, Y1;
  apply_homography(c.current(), 960.f, 800.f, X0, Y0);
  float sdx[32];
  for (int i = 0; i < n; ++i) sdx[i] = dx[i] + 1.f;
  Homography near_fit;
  REQUIRE(fit_homography(sx, sy, sdx, dy, n, near_fit));
  REQUIRE(c.offer(near_fit, sx, sy, sdx, dy, n, 1920, 1080, head, p) == FitReject::None);
  apply_homography(c.current(), 960.f, 800.f, X1, Y1);
  REQUIRE(std::abs(X1 - X0 - p.smooth) < 0.02f);
  REQUIRE(std::abs(Y1 - Y0) < 0.02f);

  // A 6 m jump waits for a second fit that agrees.
  apply_homography(c.current(), 960.f, 800.f, X0, Y0);
  for (int i = 0; i < n; ++i) sdx[i] = dx[i] + 6.f;
  Homography far_fit;
  REQUIRE(fit_homography(sx, sy, sdx, dy, n, far_fit));
  float Xf, Yf;
  apply_homography(far_fit, 960.f, 800.f, Xf, Yf);
  REQUIRE(c.offer(far_fit, sx, sy, sdx, dy, n, 1920, 1080, head, p) == FitReject::Jump);
  apply_homography(c.current(), 960.f, 800.f, X1, Y1);
  REQUIRE(std::abs(X1 - X0) < 0.01f);
  REQUIRE(c.offer(far_fit, sx, sy, sdx, dy, n, 1920, 1080, head, p) == FitReject::None);
  apply_homography(c.current(), 960.f, 800.f, X1, Y1);
  REQUIRE(std::abs(X1 - Xf) < 0.01f);

  // Camera pans 10 px right: the same pitch point is 10 px further right.
  const float G[9] = {1, 0, 10, 0, 1, 0, 0, 0, 1};
  apply_homography(c.current(), 960.f, 800.f, X0, Y0);
  c.predict(G, true, p);
  apply_homography(c.current(), 970.f, 800.f, X1, Y1);
  REQUIRE(std::abs(X1 - X0) < 0.01f);
  REQUIRE(std::abs(Y1 - Y0) < 0.01f);

  // Holdover cap, manual has none, a cut ends both.
  const float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  for (int i = 1; i < p.holdover; ++i) c.predict(I, true, p);
  REQUIRE(c.valid());
  c.predict(I, true, p);
  REQUIRE_FALSE(c.valid());
  c.set_manual(fit);
  for (int i = 0; i < 2 * p.holdover; ++i) c.predict(I, true, p);
  REQUIRE(c.valid());
  c.predict(I, false, p);
  REQUIRE_FALSE(c.valid());
}

namespace {

std::uint8_t grass_g(float x, float y) {
  const float v = 110.f + 25.f * std::sin(x / 53.f) + 20.f * std::sin(y / 41.f + x / 97.f) +
                  15.f * std::sin((x - y) / 131.f);
  return static_cast<std::uint8_t>(v);
}

// grass_g moved so a point p lands at s * (p - c) + c + d.
void grass_frame(std::vector<std::uint8_t>& px, float s, float dx, float dy) {
  const float cx = 960.f, cy = 540.f;
  for (int y = 0; y < 1080; ++y) {
    for (int x = 0; x < 1920; ++x) {
      const std::uint8_t g = grass_g(cx + (x - cx - dx) / s, cy + (y - cy - dy) / s);
      std::uint8_t* p = px.data() + (static_cast<std::size_t>(y) * 1920 + x) * 4;
      p[0] = static_cast<std::uint8_t>(g * 0.35f);
      p[1] = g;
      p[2] = static_cast<std::uint8_t>(g * 0.45f);
      p[3] = 255;
    }
  }
}

}  // namespace

TEST_CASE("image gmc recovers pan and zoom") {
  std::vector<std::uint8_t> a(1920u * 1080u * 4u), b(a.size());
  grass_frame(a, 1.f, 0.f, 0.f);
  grass_frame(b, 1.01f, 37.f, -18.f);
  static GmcImage ga, gb;
  gmc_reduce_bgra(a.data(), 1920, 1080, 1920 * 4, ga);
  gmc_reduce_bgra(b.data(), 1920, 1080, 1920 * 4, gb);
  const GmcMotion m = estimate_gmc(ga, gb);
  REQUIRE(m.ok);
  REQUIRE(std::abs(m.tx - 37.f) < 4.f);
  REQUIRE(std::abs(m.ty - -18.f) < 4.f);
  REQUIRE(std::abs(m.s - 1.01f) < 0.004f);

  const GmcMotion still = estimate_gmc(ga, ga);
  REQUIRE(still.ok);
  REQUIRE(std::abs(still.tx) < 1.f);
  REQUIRE(std::abs(still.ty) < 1.f);

  std::fill(b.begin(), b.end(), std::uint8_t{90});  // grey: no grass, a cut to the crowd
  gmc_reduce_bgra(b.data(), 1920, 1080, 1920 * 4, gb);
  REQUIRE_FALSE(estimate_gmc(ga, gb).ok);
}

namespace {

// Distance in metres from a pitch point to the nearest marking line_fit models.
float mark_distance(float X, float Y) {
  float best = 1e9f;
  for (const auto& s : detail::kMarkSegs) {
    const float dx = s.x1 - s.x0, dy = s.y1 - s.y0;
    float t = ((X - s.x0) * dx + (Y - s.y0) * dy) / (dx * dx + dy * dy);
    t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
    best = std::fmin(best, std::hypot(X - s.x0 - t * dx, Y - s.y0 - t * dy));
  }
  best = std::fmin(best, std::fabs(std::hypot(X, Y) - 9.15f));
  for (const float cx : {-41.5f, 41.5f}) {
    if (std::fabs(X) < 36.f) best = std::fmin(best, std::fabs(std::hypot(X - cx, Y) - 9.15f));
  }
  return best;
}

// Broadcast-like frame: grass with mowing stripes, 12 cm white lines through H.
void painted_frame(std::vector<std::uint8_t>& px, const Homography& H) {
  for (int y = 0; y < 1080; ++y) {
    for (int x = 0; x < 1920; ++x) {
      float X, Y;
      std::uint8_t* p = px.data() + (static_cast<std::size_t>(y) * 1920 + x) * 4;
      const bool on = apply_homography(H, x + 0.5f, y + 0.5f, X, Y);
      const int stripe = on && (static_cast<int>(std::floor(X / 5.5f)) & 1) ? 12 : 0;
      p[0] = static_cast<std::uint8_t>(40 + stripe / 2);
      p[1] = static_cast<std::uint8_t>(120 + stripe);
      p[2] = static_cast<std::uint8_t>(50 + stripe / 2);
      p[3] = 255;
      if (on && mark_distance(X, Y) < 0.06f) {
        p[0] = 220;
        p[1] = 225;
        p[2] = 220;
      }
    }
  }
}

}  // namespace

TEST_CASE("line fit scores, rejects and snaps to painted lines") {
  // A camera on a third of the pitch: around the right penalty area.
  constexpr float kQ[9] = {30.f, 0.f, -360.f, 0.f, -26.f, 560.f, 0.0002f, 0.006f, 1.f};
  float sx[32], sy[32], dx[32], dy[32];
  int n = 0;
  for (int i = 0; i < kPitchLandmarks; ++i) {
    pitch_landmark_m(i, dx[n], dy[n]);
    project(kQ, dx[n], dy[n], sx[n], sy[n]);
    ++n;
  }
  Homography truth;
  REQUIRE(fit_homography(sx, sy, dx, dy, n, truth));
  std::vector<std::uint8_t> px(1920u * 1080u * 4u);
  painted_frame(px, truth);
  LineFitParams p;

  const LineFitStats good = line_support(px.data(), 1920, 1080, 1920 * 4, truth, p);
  REQUIRE(good.in_view > 40);
  REQUIRE(good.support > 0.8f);
  REQUIRE(good.constrained);

  // A fit 6 m and 4 m off: the shape is right, but nothing lies under it.
  float ox[32], oy[32];
  for (int i = 0; i < n; ++i) {
    ox[i] = dx[i] + 6.f;
    oy[i] = dy[i] + 4.f;
  }
  Homography wrong;
  REQUIRE(fit_homography(sx, sy, ox, oy, n, wrong));
  REQUIRE(line_support(px.data(), 1920, 1080, 1920 * 4, wrong, p).support < p.min_support);

  // The prediction drifted (a 9 px pan and a 1% zoom the motion step missed):
  // the line refit pulls it back within 15 cm across the visible area.
  const float G[9] = {1.01f, 0.f, 9.f - 0.01f * 960.f, 0.f, 1.01f, -0.01f * 540.f, 0.f, 0.f, 1.f};
  float Gi[9], drift[9];
  REQUIRE(invert3(G, Gi));
  detail::mul3(truth.h, Gi, drift);
  Homography pred = truth;
  for (int i = 0; i < 9; ++i) pred.h[i] = drift[i];
  Homography refined;
  LineFitStats st;
  REQUIRE(line_refine(px.data(), 1920, 1080, 1920 * 4, pred, p, refined, st));
  REQUIRE(st.support > 0.8f);
  float worst_pred = 0.f, worst = 0.f;
  for (const float y : {500.f, 800.f, 1050.f}) {
    for (const float x : {100.f, 960.f, 1800.f}) {
      float tX, tY, pX, pY, rX, rY;
      if (!apply_homography(truth, x, y, tX, tY)) continue;
      apply_homography(pred, x, y, pX, pY);
      apply_homography(refined, x, y, rX, rY);
      worst_pred = std::fmax(worst_pred, std::hypot(pX - tX, pY - tY));
      worst = std::fmax(worst, std::hypot(rX - tX, rY - tY));
    }
  }
  REQUIRE(worst_pred > 0.5f);
  REQUIRE(worst < 0.15f);

  // A plain grass frame: nothing to match, no refit.
  painted_frame(px, Homography{});
  REQUIRE_FALSE(line_refine(px.data(), 1920, 1080, 1920 * 4, truth, p, refined, st));
}

TEST_CASE("config profile round-trips byte for byte") {
  Config c{};
  c.version = 77;
  c.conf_player = 0.4123f;
  c.h_smooth = 0.123456789f;
  c.line_min_support = 0.5f;
  c.h_holdover = 123;
  c.green_margin = 11;
  c.layers = 0xdeadbeefu;
  c.attack_dir = 0;
  c.minimap_dead_mode = 1;
  Config back{};
  REQUIRE(config_from_json(config_to_json(c), back));
  REQUIRE(std::memcmp(&c, &back, sizeof(Config)) == 0);

  // Out-of-range values clamp; unknown keys and bad JSON do not break a load.
  REQUIRE(config_from_json(R"({"conf_player": 9, "nope": 1})", back));
  REQUIRE(back.conf_player == 0.95f);
  REQUIRE(back.h_smooth == Config{}.h_smooth);
  REQUIRE_FALSE(config_from_json("{not json", back));

  Config r = c;
  cfg_reset_group(r, CfgGroup::Pitch);
  REQUIRE(r.h_smooth == Config{}.h_smooth);
  REQUIRE(r.conf_player == c.conf_player);

  // Every visible field's default sits inside its slider range.
  const Config d{};
  for (const CfgField& f : kConfigFields) {
    if (f.group == CfgGroup::Hidden) continue;
    const double v = cfg_get(d, f);
    INFO(f.key);
    REQUIRE(v >= f.lo);
    REQUIRE(v <= f.hi);
  }
}

TEST_CASE("play gate: short pan ignored, close-up enters, return needs all-clear") {
  constexpr std::int64_t ms = 1000000;
  const std::uint8_t all = kSigAngle | kSigSize | kSigScene;
  PlayGate g;
  std::int64_t t = 0;
  auto run = [&](int dur_ms, std::uint8_t dead) {
    for (int k = 0; k < dur_ms; k += 40) {
      g.update(t, all, dead, 400, 700);
      t += 40 * ms;
    }
  };
  run(1000, 0);
  REQUIRE(g.state() == PlayState::InPlay);
  run(240, kSigAngle);  // a quick pan tilts the view for a moment
  run(400, 0);
  REQUIRE(g.state() == PlayState::InPlay);
  run(480, kSigSize);  // a close-up
  REQUIRE(g.state() == PlayState::Dead);
  REQUIRE(g.reasons() == kSigSize);
  run(400, 0);
  run(120, kSigScene);  // one signal back before 700 ms of all-clear
  run(400, 0);
  REQUIRE(g.state() == PlayState::Dead);
  run(400, 0);
  REQUIRE(g.state() == PlayState::InPlay);
  // A signal that is not available cannot hold the ball dead.
  for (int k = 0; k < 30; ++k, t += 40 * ms) g.update(t, kSigScene, kSigBallOff, 400, 700);
  REQUIRE(g.state() == PlayState::InPlay);
}

TEST_CASE("view angle: side-on is small, end-on is large, reverse is over 90") {
  float sx[32], sy[32], dx[32], dy[32];
  for (int i = 0; i < kPitchLandmarks; ++i) {
    pitch_landmark_m(i, dx[i], dy[i]);
    project(kP, dx[i], dy[i], sx[i], sy[i]);
  }
  Homography side;
  REQUIRE(fit_homography(sx, sy, dx, dy, kPitchLandmarks, side));
  float deg = 0;
  REQUIRE(view_angle_deg(side, 1920, 1080, deg));
  REQUIRE(deg < 15.f);
  // Behind the goal: pitch x runs up the image.
  const float E[9] = {0.f, 12.f, 960.f, -12.f, 0.f, 540.f, 0.004f, 0.f, 1.f};
  for (int i = 0; i < kPitchLandmarks; ++i) project(E, dx[i], dy[i], sx[i], sy[i]);
  Homography end;
  REQUIRE(fit_homography(sx, sy, dx, dy, kPitchLandmarks, end));
  REQUIRE(view_angle_deg(end, 1920, 1080, deg));
  REQUIRE(deg > 60.f);
  // Reverse angle: pitch x runs right to left.
  const float R[9] = {-12.f, 0.f, 960.f, 0.f, 12.f, 540.f, -0.0005f, -0.004f, 1.f};
  for (int i = 0; i < kPitchLandmarks; ++i) project(R, dx[i], dy[i], sx[i], sy[i]);
  Homography rev;
  REQUIRE(fit_homography(sx, sy, dx, dy, kPitchLandmarks, rev));
  REQUIRE(view_angle_deg(rev, 1920, 1080, deg));
  REQUIRE(deg > 90.f);
}

TEST_CASE("dead-ball records export as type 22") {
  Event e{};
  e.type = EventType::DeadBall;
  e.match_ms_end = 5000;
  char row[256];
  format_spadl_row(row, 256, e, 1);
  REQUIRE(std::strstr(row, ",22,1,0") != nullptr);
}
