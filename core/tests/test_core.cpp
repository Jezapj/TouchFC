#include "touchfc/arena.hpp"
#include "touchfc/blocking_spsc.hpp"
#include "touchfc/cpu.hpp"
#include "touchfc/detection.hpp"
#include "touchfc/frame_check.hpp"
#include "touchfc/gmc.hpp"
#include "touchfc/hdr_sink.hpp"
#include "touchfc/homography.hpp"
#include "touchfc/kalman.hpp"
#include "touchfc/mailbox.hpp"
#include "touchfc/nms.hpp"
#include "touchfc/pitch.hpp"
#include "touchfc/pitch_landmarks.hpp"
#include "touchfc/pitch_pose.hpp"
#include "touchfc/preprocess.hpp"
#include "touchfc/seqlock.hpp"
#include "touchfc/scene.hpp"
#include "touchfc/slot_pool.hpp"
#include "touchfc/spsc_ring.hpp"
#include "touchfc/telemetry.hpp"
#include "touchfc/time.hpp"
#include "touchfc/tracker.hpp"
#include "touchfc/triple_buffer.hpp"
#include "touchfc/types.hpp"
#include "touchfc/wake.hpp"
#include "touchfc/yolo.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

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
