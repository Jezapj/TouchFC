#include "touchfc/arena.hpp"
#include "touchfc/cpu.hpp"
#include "touchfc/image_gmc.hpp"
#include "touchfc/line_fit.hpp"
#include "touchfc/pitch_landmarks.hpp"
#include "touchfc/preprocess.hpp"
#include "touchfc/seqlock.hpp"
#include "touchfc/spsc_ring.hpp"
#include "touchfc/time.hpp"
#include "touchfc/triple_buffer.hpp"
#include "touchfc/types.hpp"
#include "touchfc/wake.hpp"

#include <vector>

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <queue>
#include <thread>

using namespace touchfc;

namespace {

template <class Pair>
std::int64_t ping_pong_ns(int iters) {
  auto* pair = new Pair();
  std::atomic<bool> ready{false};
  std::thread other([&] {
    ready.store(true, std::memory_order_release);
    for (int i = 1; i <= iters; ++i) {
      while (pair->a.load(std::memory_order_acquire) != static_cast<std::uint32_t>(i)) cpu_pause();
      pair->b.store(static_cast<std::uint32_t>(i), std::memory_order_release);
    }
  });
  while (!ready.load(std::memory_order_acquire)) cpu_pause();
  const TimeNs t0 = now_ns();
  for (int i = 1; i <= iters; ++i) {
    pair->a.store(static_cast<std::uint32_t>(i), std::memory_order_release);
    while (pair->b.load(std::memory_order_acquire) != static_cast<std::uint32_t>(i)) cpu_pause();
  }
  const TimeNs t1 = now_ns();
  other.join();
  delete pair;
  return t1 - t0;
}

struct TightPair {
  std::atomic<std::uint32_t> a{0};
  std::atomic<std::uint32_t> b{0};
};
struct Pad64Pair {
  alignas(64) std::atomic<std::uint32_t> a{0};
  alignas(64) std::atomic<std::uint32_t> b{0};
};
struct Pad128Pair {
  alignas(128) std::atomic<std::uint32_t> a{0};
  alignas(128) std::atomic<std::uint32_t> b{0};
};

}  // namespace

static void BM_SpscPushPop(benchmark::State& state) {
  auto ring = std::make_unique<SpscRing<std::uint32_t, 1024>>();
  std::atomic<bool> run{true};
  std::thread consumer([&] {
    std::uint32_t v = 0;
    while (run.load(std::memory_order_acquire)) {
      if (!ring->try_pop(v)) cpu_pause();
    }
    while (ring->try_pop(v)) {
    }
  });
  std::uint32_t i = 0;
  for (auto _ : state) {
    while (!ring->try_push(i)) cpu_pause();
    ++i;
  }
  run.store(false, std::memory_order_release);
  consumer.join();
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_SpscPushPop)->MinTime(0.15);

static void BM_MutexQueue(benchmark::State& state) {
  std::mutex mu;
  std::queue<std::uint32_t> q;
  constexpr std::size_t kCap = 1024;
  std::atomic<bool> run{true};
  std::thread consumer([&] {
    for (;;) {
      bool empty_and_stop = false;
      {
        std::lock_guard lock(mu);
        if (!q.empty()) {
          q.pop();
          continue;
        }
        empty_and_stop = !run.load(std::memory_order_relaxed);
      }
      if (empty_and_stop) return;
      cpu_pause();
    }
  });
  std::uint32_t i = 0;
  for (auto _ : state) {
    for (;;) {
      bool pushed = false;
      {
        std::lock_guard lock(mu);
        if (q.size() < kCap) {
          q.push(i);
          pushed = true;
        }
      }
      if (pushed) break;
      cpu_pause();
    }
    ++i;
  }
  run.store(false, std::memory_order_release);
  consumer.join();
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_MutexQueue)->MinTime(0.15);

static void BM_SeqlockLoad(benchmark::State& state) {
  SeqLock<Config> seq(Config{});
  std::atomic<bool> run{true};
  std::thread writer([&] {
    Config cfg{};
    while (run.load(std::memory_order_relaxed)) {
      ++cfg.version;
      seq.store(cfg);
    }
  });
  for (auto _ : state) {
    Config cfg = seq.load();
    benchmark::DoNotOptimize(cfg.version);
  }
  run.store(false, std::memory_order_relaxed);
  writer.join();
}
BENCHMARK(BM_SeqlockLoad)->MinTime(0.15);

static void BM_MutexConfigLoad(benchmark::State& state) {
  std::mutex mu;
  Config cfg{};
  std::atomic<bool> run{true};
  std::thread writer([&] {
    while (run.load(std::memory_order_relaxed)) {
      std::lock_guard lock(mu);
      ++cfg.version;
    }
  });
  for (auto _ : state) {
    std::lock_guard lock(mu);
    Config copy = cfg;
    benchmark::DoNotOptimize(copy.version);
  }
  run.store(false, std::memory_order_relaxed);
  writer.join();
}
BENCHMARK(BM_MutexConfigLoad)->MinTime(0.15);

static void BM_TripleBufferRead(benchmark::State& state) {
  TripleBuffer<Config> buf;
  std::atomic<bool> run{true};
  std::thread writer([&] {
    std::uint32_t n = 0;
    while (run.load(std::memory_order_relaxed)) {
      buf.write_slot().version = ++n;
      buf.publish();
    }
  });
  for (auto _ : state) {
    if (Config* cfg = buf.try_read()) benchmark::DoNotOptimize(cfg->version);
  }
  run.store(false, std::memory_order_relaxed);
  writer.join();
}
BENCHMARK(BM_TripleBufferRead)->MinTime(0.15);

static void BM_Malloc64(benchmark::State& state) {
  for (auto _ : state) {
    for (int i = 0; i < 256; ++i) {
      void* p = std::malloc(64);
      benchmark::DoNotOptimize(p);
      std::free(p);
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * 256);
}
BENCHMARK(BM_Malloc64)->MinTime(0.15);

static void BM_Arena64(benchmark::State& state) {
  for (auto _ : state) {
    Arena arena(64 * 256 + 4096);
    for (int i = 0; i < 256; ++i) {
      void* p = arena.allocate(64, 64);
      benchmark::DoNotOptimize(p);
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * 256);
}
BENCHMARK(BM_Arena64)->MinTime(0.15);

static void BM_WakeRoundTrip(benchmark::State& state) {
  const int spins = static_cast<int>(state.range(0));
  WakeSeq ping;
  WakeSeq pong;
  std::atomic<bool> run{true};
  std::atomic<bool> ready{false};
  std::uint32_t seen_ping = 0;
  std::thread other([&] {
    std::uint32_t seen = ping.load();
    ready.store(true, std::memory_order_release);
    while (run.load(std::memory_order_acquire)) {
      const std::uint32_t now = ping.wait_from(seen, spins);
      if (now == seen) continue;  // spurious wake
      seen = now;
      pong.notify();
    }
  });
  while (!ready.load(std::memory_order_acquire)) cpu_pause();
  std::uint32_t seen_pong = pong.load();
  for (auto _ : state) {
    ping.notify();
    seen_pong = pong.wait_from(seen_pong, spins);
  }
  run.store(false, std::memory_order_release);
  ping.notify();
  other.join();
  (void)seen_ping;
}
BENCHMARK(BM_WakeRoundTrip)->Arg(0)->Arg(64)->Arg(1000000)->MinTime(0.15);

static void BM_FalseSharing(benchmark::State& state) {
  const int kind = static_cast<int>(state.range(0));
  for (auto _ : state) {
    constexpr int kIters = 100000;
    std::int64_t ns = 0;
    if (kind == 0) ns = ping_pong_ns<TightPair>(kIters);
    else if (kind == 1) ns = ping_pong_ns<Pad64Pair>(kIters);
    else ns = ping_pong_ns<Pad128Pair>(kIters);
    benchmark::DoNotOptimize(ns);
  }
}
BENCHMARK(BM_FalseSharing)->Arg(0)->Arg(1)->Arg(2)->MinTime(0.2);

// Preprocess: 1280x720 BGRA -> 640x640 CHW, scalar vs AVX2.
// This is the one path that *is* memory-bandwidth bound at 25-50 fps,
// so it is the microbenchmark that actually predicts pipeline cost.
struct PreprocessFixture {
  static constexpr std::uint32_t sw = 1280;
  static constexpr std::uint32_t sh = 720;
  static constexpr std::uint32_t D = 640;
  std::vector<std::uint8_t> src;
  std::vector<float> dst;
  LetterboxParams p{};
  PreprocessFixture() : src(static_cast<std::size_t>(sw) * sh * 4), dst(3u * D * D) {
    for (std::size_t i = 0; i < src.size(); ++i) src[i] = static_cast<std::uint8_t>(i);
    p = make_letterbox(sw, sh, D);
  }
};

static void BM_PreprocessScalar(benchmark::State& state) {
  PreprocessFixture fx;
  for (auto _ : state) {
    bgra_letterbox_to_chw_scalar(fx.src.data(), fx.sw, fx.sh, fx.sw * 4u, fx.dst.data(), fx.p);
    benchmark::DoNotOptimize(fx.dst.data());
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(fx.sw) * fx.sh * 4);
}
BENCHMARK(BM_PreprocessScalar)->MinTime(0.3);

#if defined(TOUCHFC_HAS_AVX2)
static void BM_PreprocessAvx2(benchmark::State& state) {
  PreprocessFixture fx;
  for (auto _ : state) {
    bgra_letterbox_to_chw_avx2(fx.src.data(), fx.sw, fx.sh, fx.sw * 4u, fx.dst.data(), fx.p);
    benchmark::DoNotOptimize(fx.dst.data());
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(fx.sw) * fx.sh * 4);
}
BENCHMARK(BM_PreprocessAvx2)->MinTime(0.3);
#endif
// Image GMC runs on T3 every frame: reduce a 1920x1080 BGRA frame, then search.
static void BM_ImageGmc(benchmark::State& state) {
  std::vector<std::uint8_t> a(1920u * 1080u * 4u), b(a.size());
  for (std::size_t y = 0; y < 1080; ++y)
    for (std::size_t x = 0; x < 1920; ++x) {
      std::uint8_t* p = a.data() + (y * 1920 + x) * 4;
      const auto g = static_cast<std::uint8_t>(110 + ((x / 7 + y / 5) * 37) % 60);
      p[0] = static_cast<std::uint8_t>(g / 3);
      p[1] = g;
      p[2] = static_cast<std::uint8_t>(g / 2);
      std::uint8_t* q = b.data() + (y * 1920 + (x + 13) % 1920) * 4;
      q[0] = p[0];
      q[1] = p[1];
      q[2] = p[2];
    }
  static GmcImage ga, gb;
  gmc_reduce_bgra(a.data(), 1920, 1080, 1920 * 4, ga);
  for (auto _ : state) {
    gmc_reduce_bgra(b.data(), 1920, 1080, 1920 * 4, gb);
    benchmark::DoNotOptimize(estimate_gmc(ga, gb));
  }
}
BENCHMARK(BM_ImageGmc)->Unit(benchmark::kMicrosecond)->MinTime(0.3);


// Line refit on T3 every line_interval wide frames: 1920x1080, a third of the
// pitch around the right box, prediction off by a 9 px pan.
static void BM_LineRefine(benchmark::State& state) {
  const float P[9] = {30.f, 0.f, -360.f, 0.f, -26.f, 560.f, 0.0002f, 0.006f, 1.f};
  std::vector<std::uint8_t> px(1920u * 1080u * 4u);
  for (std::size_t i = 0; i < px.size(); i += 4) {
    px[i] = 40;
    px[i + 1] = 120;
    px[i + 2] = 50;
  }
  float sx[32], sy[32], dx[32], dy[32];
  for (int i = 0; i < kPitchLandmarks; ++i) {
    pitch_landmark_m(i, dx[i], dy[i]);
    const float w = P[6] * dx[i] + P[7] * dy[i] + P[8];
    sx[i] = (P[0] * dx[i] + P[1] * dy[i] + P[2]) / w;
    sy[i] = (P[3] * dx[i] + P[4] * dy[i] + P[5]) / w;
  }
  Homography truth, inv;
  fit_homography(sx, sy, dx, dy, kPitchLandmarks, truth);
  invert3(truth.h, inv.h);
  // Paint the markings as 3 px dots every 5 cm.
  detail::for_each_mark_sample([&](float X, float Y, float tX, float tY, int) {
    for (int k = 0; k < 20; ++k) {
      float x, y;
      if (!apply_homography(inv, X + 0.05f * k * tX, Y + 0.05f * k * tY, x, y)) continue;
      for (int oy = -1; oy <= 1; ++oy)
        for (int ox = -1; ox <= 1; ++ox) {
          const int xi = static_cast<int>(x) + ox, yi = static_cast<int>(y) + oy;
          if (xi < 0 || yi < 0 || xi >= 1920 || yi >= 1080) continue;
          std::uint8_t* p = px.data() + (static_cast<std::size_t>(yi) * 1920 + xi) * 4;
          p[0] = p[1] = p[2] = 225;
        }
    }
  });
  const float G[9] = {1, 0, 9, 0, 1, 0, 0, 0, 1};
  float Gi[9];
  invert3(G, Gi);
  Homography pred = truth;
  detail::mul3(truth.h, Gi, pred.h);
  LineFitParams p;
  for (auto _ : state) {
    Homography out;
    LineFitStats st;
    benchmark::DoNotOptimize(line_refine(px.data(), 1920, 1080, 1920 * 4, pred, p, out, st));
  }
}
BENCHMARK(BM_LineRefine)->Unit(benchmark::kMicrosecond)->MinTime(0.3);

int main(int argc, char** argv) {
  std::fprintf(stderr, "touchfc bench  clock=steady_clock\n");
#if defined(_MSC_VER)
  std::fprintf(stderr, "compiler=msvc %d\n", _MSC_VER);
#elif defined(__clang__)
  std::fprintf(stderr, "compiler=clang %d.%d\n", __clang_major__, __clang_minor__);
#elif defined(__GNUC__)
  std::fprintf(stderr, "compiler=gcc %d.%d\n", __GNUC__, __GNUC_MINOR__);
#endif
  std::fprintf(stderr, "logical_cpus=%u\n", std::thread::hardware_concurrency());
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
