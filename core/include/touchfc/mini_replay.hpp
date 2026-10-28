#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>

#if defined(__AVX2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)))
#  ifndef TOUCHFC_HAS_AVX2
#    define TOUCHFC_HAS_AVX2 1
#  endif
#  include <immintrin.h>
#endif

namespace touchfc {

// M15 mini replay: thumbnails after T3 into a ring, clips copied out by T7,
// played by present. Everything is allocated before Pipeline::start.
inline constexpr int kThumbW = 480;
inline constexpr int kThumbH = 270;
inline constexpr std::size_t kThumbBytes = static_cast<std::size_t>(kThumbW) * kThumbH * 4;
inline constexpr int kThumbRing = 150;  // 6 s at 25 fps
inline constexpr int kClipFrames = 140;  // 5.5 s at 25 fps

// Box filter BGRA -> 480x270 BGRA. Rows of a box are summed into 16-bit lanes
// (AVX2), then each output pixel sums its columns as four packed 16-bit lanes
// in one 64-bit add. Boxes up to 16x16 (7680 wide). Alpha is forced opaque.
class ThumbScaler {
 public:
  static constexpr int kMaxW = 7680;

  bool prepare(int w, int h) noexcept {
    if (w < kThumbW || h < kThumbH || w > kMaxW || h > 4320) return false;
    w_ = w;
    h_ = h;
    for (int i = 0; i <= kThumbW; ++i) x0_[i] = static_cast<std::uint16_t>(static_cast<std::int64_t>(i) * w / kThumbW);
    for (int j = 0; j <= kThumbH; ++j) y0_[j] = static_cast<std::uint16_t>(static_cast<std::int64_t>(j) * h / kThumbH);
    for (int a = 1; a <= 256; ++a) recip_[a] = (65536u + static_cast<std::uint32_t>(a) / 2) / static_cast<std::uint32_t>(a);
    return true;
  }
  bool ready() const noexcept { return w_ > 0; }

  void run(const std::uint8_t* src, std::size_t stride, std::uint8_t* dst) noexcept {
    const int n = w_ * 4;
    for (int j = 0; j < kThumbH; ++j) {
      const int ya = y0_[j], yb = y0_[j + 1];
      std::memset(acc_, 0, static_cast<std::size_t>(n) * sizeof(std::uint16_t));
      for (int y = ya; y < yb; ++y) add_row(src + static_cast<std::size_t>(y) * stride, n);
      const std::uint32_t bh = static_cast<std::uint32_t>(yb - ya);
      std::uint8_t* out = dst + static_cast<std::size_t>(j) * kThumbW * 4;
      for (int i = 0; i < kThumbW; ++i) {
        const int xa = x0_[i], xb = x0_[i + 1];
        std::uint64_t s = 0;
        for (int x = xa; x < xb; ++x) {
          std::uint64_t v;
          std::memcpy(&v, acc_ + x * 4, 8);
          s += v;
        }
        const std::uint32_t r = recip_[bh * static_cast<std::uint32_t>(xb - xa)];
        out[i * 4 + 0] = static_cast<std::uint8_t>(((s & 0xffff) * r) >> 16);
        out[i * 4 + 1] = static_cast<std::uint8_t>((((s >> 16) & 0xffff) * r) >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>((((s >> 32) & 0xffff) * r) >> 16);
        out[i * 4 + 3] = 255;
      }
    }
  }

 private:
  void add_row(const std::uint8_t* row, int n) noexcept {
    int k = 0;
#if TOUCHFC_HAS_AVX2
    for (; k + 16 <= n; k += 16) {
      const __m256i v = _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(row + k)));
      __m256i* a = reinterpret_cast<__m256i*>(acc_ + k);
      _mm256_storeu_si256(a, _mm256_add_epi16(_mm256_loadu_si256(a), v));
    }
#endif
    for (; k < n; ++k) acc_[k] = static_cast<std::uint16_t>(acc_[k] + row[k]);
  }

  int w_ = 0, h_ = 0;
  std::uint16_t x0_[kThumbW + 1]{};
  std::uint16_t y0_[kThumbH + 1]{};
  std::uint32_t recip_[257]{};
  alignas(32) std::uint16_t acc_[kMaxW * 4]{};
};

// Per-frame data kept beside a thumbnail.
struct ThumbMeta {
  float t_s = 0.f;          // play clock (event time)
  float h[9]{};             // image -> pitch, valid when pitch_valid
  std::uint16_t src_w = 0, src_h = 0;
  std::uint8_t pitch_valid = 0;
  std::uint8_t dead = 0;
  std::uint8_t pad[2]{};
};

// Single writer (T3), single reader (T7). Each entry has a sequence: odd while
// written. The reader copies, then checks the sequence again and drops a frame
// the writer reached meanwhile, so the writer never waits.
class ThumbRing {
 public:
  void allocate() {
    if (!px_) px_.reset(new std::uint8_t[kThumbBytes * kThumbRing]);
  }
  bool allocated() const noexcept { return px_ != nullptr; }

  std::uint8_t* begin_write(const ThumbMeta& m) noexcept {
    const int i = static_cast<int>(head_ % kThumbRing);
    seq_[i].store(seq_[i].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    meta_[i] = m;
    return px_.get() + kThumbBytes * static_cast<std::size_t>(i);
  }
  void end_write() noexcept {
    const int i = static_cast<int>(head_ % kThumbRing);
    seq_[i].store(seq_[i].load(std::memory_order_relaxed) + 1, std::memory_order_release);
    last_t_.store(meta_[i].t_s, std::memory_order_release);
    ++head_;
    written_.store(head_, std::memory_order_release);
  }

  std::uint64_t written() const noexcept { return written_.load(std::memory_order_acquire); }
  float last_t() const noexcept { return last_t_.load(std::memory_order_acquire); }

  // Reader: copy entry n (absolute index) if it is still in the ring.
  bool read(std::uint64_t n, ThumbMeta& m, std::uint8_t* px) const noexcept {
    const int i = static_cast<int>(n % kThumbRing);
    const std::uint32_t s0 = seq_[i].load(std::memory_order_acquire);
    if (s0 & 1u) return false;
    std::memcpy(&m, &meta_[i], sizeof(m));
    if (px) std::memcpy(px, px_.get() + kThumbBytes * static_cast<std::size_t>(i), kThumbBytes);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq_[i].load(std::memory_order_relaxed) != s0) return false;
    // s0 counts writes to this entry: 2 per lap. Entry n was written on lap n / kThumbRing.
    return s0 / 2u == static_cast<std::uint32_t>(n / kThumbRing) + 1u;
  }

 private:
  std::unique_ptr<std::uint8_t[]> px_;
  ThumbMeta meta_[kThumbRing]{};
  std::atomic<std::uint32_t> seq_[kThumbRing]{};
  std::uint64_t head_ = 0;
  std::atomic<std::uint64_t> written_{0};
  std::atomic<float> last_t_{-1e9f};
};

// A key pass or shot, T3 -> T7.
struct ClipReq {
  float t_s = 0.f;  // event time on the play clock
  float sx = 0.f, sy = 0.f, ex = 0.f, ey = 0.f;
  std::uint8_t kind = 0;  // 0 pass, 1 shot, 2 manual test
  std::uint8_t team = 0;
  std::uint8_t pad[2]{};
};

enum class ClipState : std::uint8_t { Free = 0, Filling, Ready, Playing };

struct ClipBuffer {
  std::atomic<ClipState> state{ClipState::Free};
  std::uint64_t seq = 0;  // order of completion
  ClipReq req{};
  int n = 0;
  ThumbMeta meta[kClipFrames]{};
  std::unique_ptr<std::uint8_t[]> px;
  void allocate() {
    if (!px) px.reset(new std::uint8_t[kThumbBytes * kClipFrames]);
  }
  std::uint8_t* frame(int k) noexcept { return px.get() + kThumbBytes * static_cast<std::size_t>(k); }
  const std::uint8_t* frame(int k) const noexcept { return px.get() + kThumbBytes * static_cast<std::size_t>(k); }
};

inline constexpr float kClipBeforeS = 4.f;
inline constexpr float kClipAfterS = 1.5f;

// T7: copy the ring range [t - 4 s, t + 1.5 s] into the clip. Returns frames
// copied; frames the writer reached during the copy are dropped. dead_before
// is set when a pre-event frame was a dead ball (a cut or a broadcast replay).
inline int copy_clip(const ThumbRing& ring, const ClipReq& req, ClipBuffer& clip, bool& dead_before) noexcept {
  dead_before = false;
  clip.n = 0;
  clip.req = req;
  const std::uint64_t w = ring.written();
  const std::uint64_t first = w > static_cast<std::uint64_t>(kThumbRing) ? w - kThumbRing : 0;
  const float t0 = req.t_s - kClipBeforeS, t1 = req.t_s + kClipAfterS;
  ThumbMeta m;
  for (std::uint64_t k = first; k < w && clip.n < kClipFrames; ++k) {
    if (!ring.read(k, m, nullptr) || m.t_s < t0 || m.t_s > t1) continue;
    if (!ring.read(k, clip.meta[clip.n], clip.frame(clip.n))) continue;
    if (clip.meta[clip.n].dead && clip.meta[clip.n].t_s < req.t_s) dead_before = true;
    ++clip.n;
  }
  return clip.n;
}

}  // namespace touchfc
