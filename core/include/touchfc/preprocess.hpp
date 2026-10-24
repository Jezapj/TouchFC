#pragma once

#include "touchfc/detection.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__AVX2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)))
#  include <immintrin.h>
#  define TOUCHFC_HAS_AVX2 1
#endif

namespace touchfc {

// Compute the letterbox parameters for a source image at (sw,sh) going into
// a square dst_side model input. Letterboxing picks the min of width and
// height scales so the whole frame fits, and centres it.
inline LetterboxParams make_letterbox(std::uint32_t sw, std::uint32_t sh,
                                      std::uint32_t dst_side) noexcept {
  LetterboxParams p{};
  p.src_w = sw;
  p.src_h = sh;
  p.dst_w = dst_side;
  p.dst_h = dst_side;
  if (sw == 0 || sh == 0 || dst_side == 0) return p;
  const float sx = static_cast<float>(dst_side) / static_cast<float>(sw);
  const float sy = static_cast<float>(dst_side) / static_cast<float>(sh);
  p.scale = std::min(sx, sy);
  const float used_w = static_cast<float>(sw) * p.scale;
  const float used_h = static_cast<float>(sh) * p.scale;
  p.pad_x = (static_cast<float>(dst_side) - used_w) * 0.5f;
  p.pad_y = (static_cast<float>(dst_side) - used_h) * 0.5f;
  return p;
}

// Map a detection in letterboxed coordinates back to original image pixels.
inline void unletterbox_box(const LetterboxParams& p, float& x, float& y,
                            float& w, float& h) noexcept {
  const float inv = p.scale > 0 ? 1.f / p.scale : 1.f;
  x = (x - p.pad_x) * inv;
  y = (y - p.pad_y) * inv;
  w = w * inv;
  h = h * inv;
}

// Nearest-neighbour BGRA -> CHW float32, normalised to [0,1], padded to 114/255
// in letterbox borders. "CHW" is channel-major: R plane, then G plane, then B
// plane, each dst_side * dst_side floats. YOLOv8 wants RGB in [0,1].
//
// Nearest is the right trade-off here: real preprocessing cost in the pipeline
// is memory bandwidth, not interpolation quality. A detector trained with
// bilinear letterbox will be within noise of nearest for players at typical
// broadcast sizes. Bilinear can go in a later milestone next to the AVX2 pass.
//
// This is the portable scalar reference. Correct on every target. Use
// bgra_letterbox_to_chw() to dispatch to the AVX2 fast path when it exists.
inline void bgra_letterbox_to_chw_scalar(const std::uint8_t* src, std::uint32_t sw,
                                         std::uint32_t sh, std::uint32_t src_stride,
                                         float* dst, const LetterboxParams& p) noexcept {
  const std::uint32_t D = p.dst_w;
  const std::size_t plane = static_cast<std::size_t>(D) * D;
  float* dR = dst;
  float* dG = dst + plane;
  float* dB = dst + 2 * plane;
  constexpr float kPad = 114.f / 255.f;
  // Precompute source y for each destination row (nearest neighbour).
  // dy=0..D-1, src_y = clamp(floor((dy - pad_y) / scale), 0, sh-1), or -1 for pad.
  const float inv = p.scale > 0 ? 1.f / p.scale : 0.f;
  for (std::uint32_t dy = 0; dy < D; ++dy) {
    const float fy = (static_cast<float>(dy) + 0.5f - p.pad_y) * inv - 0.5f;
    const int sy_i = static_cast<int>(std::floor(fy + 0.5f));
    const bool row_pad = sy_i < 0 || sy_i >= static_cast<int>(sh);
    const std::uint8_t* src_row = row_pad ? nullptr : src + static_cast<std::size_t>(sy_i) * src_stride;
    float* r = dR + static_cast<std::size_t>(dy) * D;
    float* g = dG + static_cast<std::size_t>(dy) * D;
    float* b = dB + static_cast<std::size_t>(dy) * D;
    for (std::uint32_t dx = 0; dx < D; ++dx) {
      const float fx = (static_cast<float>(dx) + 0.5f - p.pad_x) * inv - 0.5f;
      const int sx_i = static_cast<int>(std::floor(fx + 0.5f));
      if (row_pad || sx_i < 0 || sx_i >= static_cast<int>(sw)) {
        r[dx] = kPad;
        g[dx] = kPad;
        b[dx] = kPad;
      } else {
        const std::uint8_t* px = src_row + static_cast<std::size_t>(sx_i) * 4u;
        // BGRA source. YOLO expects R, G, B planes.
        b[dx] = static_cast<float>(px[0]) * (1.f / 255.f);
        g[dx] = static_cast<float>(px[1]) * (1.f / 255.f);
        r[dx] = static_cast<float>(px[2]) * (1.f / 255.f);
      }
    }
  }
}

#if defined(TOUCHFC_HAS_AVX2)
// AVX2 nearest-neighbour letterbox. Writes 8 destination pixels per iteration.
//
// The horizontal source index is computed in integer with a Q24 fixed-point
// step (large enough for 4K->1024 without rounding drift). We gather the four
// BGRA bytes per destination with VPGATHERDD on the source row, then split
// bytes to u32 and convert to float32. Normalisation is a multiply by 1/255.
inline void bgra_letterbox_to_chw_avx2(const std::uint8_t* src, std::uint32_t sw,
                                       std::uint32_t sh, std::uint32_t src_stride,
                                       float* dst, const LetterboxParams& p) noexcept {
  const std::uint32_t D = p.dst_w;
  const std::size_t plane = static_cast<std::size_t>(D) * D;
  float* dR = dst;
  float* dG = dst + plane;
  float* dB = dst + 2 * plane;
  constexpr float kPad = 114.f / 255.f;
  constexpr float kInv = 1.f / 255.f;
  const float inv_scale = p.scale > 0 ? 1.f / p.scale : 0.f;

  const __m256 v_inv = _mm256_set1_ps(kInv);
  const __m256 v_pad = _mm256_set1_ps(kPad);
  const __m256i v_mask_ff = _mm256_set1_epi32(0xff);

  for (std::uint32_t dy = 0; dy < D; ++dy) {
    const float fy = (static_cast<float>(dy) + 0.5f - p.pad_y) * inv_scale - 0.5f;
    const int sy_i = static_cast<int>(std::floor(fy + 0.5f));
    const bool row_pad = sy_i < 0 || sy_i >= static_cast<int>(sh);
    float* r = dR + static_cast<std::size_t>(dy) * D;
    float* g = dG + static_cast<std::size_t>(dy) * D;
    float* b = dB + static_cast<std::size_t>(dy) * D;

    if (row_pad) {
      for (std::uint32_t dx = 0; dx < D; dx += 8) {
        _mm256_storeu_ps(r + dx, v_pad);
        _mm256_storeu_ps(g + dx, v_pad);
        _mm256_storeu_ps(b + dx, v_pad);
      }
      continue;
    }

    const std::uint8_t* src_row = src + static_cast<std::size_t>(sy_i) * src_stride;
    const int sw_i = static_cast<int>(sw);

    // Precompute dx indices offsets for 8 lanes: 0..7
    __m256i v_lane = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    __m256 v_pad_x = _mm256_set1_ps(p.pad_x);
    __m256 v_inv_s = _mm256_set1_ps(inv_scale);
    __m256 v_half = _mm256_set1_ps(0.5f);
    __m256i v_sw = _mm256_set1_epi32(sw_i);

    const std::uint32_t D8 = D & ~std::uint32_t{7};
    for (std::uint32_t dx = 0; dx < D8; dx += 8) {
      __m256i v_dx = _mm256_add_epi32(_mm256_set1_epi32(static_cast<int>(dx)), v_lane);
      __m256 vf = _mm256_cvtepi32_ps(v_dx);
      vf = _mm256_add_ps(vf, v_half);
      vf = _mm256_sub_ps(vf, v_pad_x);
      vf = _mm256_mul_ps(vf, v_inv_s);
      vf = _mm256_sub_ps(vf, v_half);
      // Floor via rounding toward zero after adding 0.5 works only for >=0.
      // Use _mm256_floor_ps then convert.
      __m256 v_sx_f = _mm256_floor_ps(_mm256_add_ps(vf, v_half));
      __m256i v_sx = _mm256_cvttps_epi32(v_sx_f);
      // Mask: 0 <= sx < sw
      __m256i v_zero = _mm256_setzero_si256();
      __m256i v_in_lo = _mm256_cmpgt_epi32(v_sx, _mm256_set1_epi32(-1));
      __m256i v_in_hi = _mm256_cmpgt_epi32(v_sw, v_sx);
      __m256i v_in = _mm256_and_si256(v_in_lo, v_in_hi);
      __m256i v_sx_clamped = _mm256_and_si256(v_in, v_sx);  // out-of-range -> 0 to make gather safe
      // Byte offset = sx * 4
      __m256i v_off = _mm256_slli_epi32(v_sx_clamped, 2);
      __m256i v_px = _mm256_i32gather_epi32(reinterpret_cast<const int*>(src_row), v_off, 1);
      // Blue in low byte, Green byte 1, Red byte 2
      __m256i v_b = _mm256_and_si256(v_px, v_mask_ff);
      __m256i v_g = _mm256_and_si256(_mm256_srli_epi32(v_px, 8), v_mask_ff);
      __m256i v_r = _mm256_and_si256(_mm256_srli_epi32(v_px, 16), v_mask_ff);
      __m256 vrf = _mm256_mul_ps(_mm256_cvtepi32_ps(v_r), v_inv);
      __m256 vgf = _mm256_mul_ps(_mm256_cvtepi32_ps(v_g), v_inv);
      __m256 vbf = _mm256_mul_ps(_mm256_cvtepi32_ps(v_b), v_inv);
      // Blend padding where out-of-range.
      __m256 v_in_mask = _mm256_castsi256_ps(v_in);
      vrf = _mm256_blendv_ps(v_pad, vrf, v_in_mask);
      vgf = _mm256_blendv_ps(v_pad, vgf, v_in_mask);
      vbf = _mm256_blendv_ps(v_pad, vbf, v_in_mask);
      _mm256_storeu_ps(r + dx, vrf);
      _mm256_storeu_ps(g + dx, vgf);
      _mm256_storeu_ps(b + dx, vbf);
    }
    // Tail (D not a multiple of 8). D is typically 640, so this is unused.
    for (std::uint32_t dx = D8; dx < D; ++dx) {
      const float fx = (static_cast<float>(dx) + 0.5f - p.pad_x) * inv_scale - 0.5f;
      const int sx_i = static_cast<int>(std::floor(fx + 0.5f));
      if (sx_i < 0 || sx_i >= sw_i) {
        r[dx] = kPad;
        g[dx] = kPad;
        b[dx] = kPad;
      } else {
        const std::uint8_t* px = src_row + static_cast<std::size_t>(sx_i) * 4u;
        b[dx] = static_cast<float>(px[0]) * kInv;
        g[dx] = static_cast<float>(px[1]) * kInv;
        r[dx] = static_cast<float>(px[2]) * kInv;
      }
    }
  }
}
#endif  // TOUCHFC_HAS_AVX2

// Runtime dispatcher. Call this from the pipeline.
inline void bgra_letterbox_to_chw(const std::uint8_t* src, std::uint32_t sw,
                                  std::uint32_t sh, std::uint32_t src_stride,
                                  float* dst, const LetterboxParams& p) noexcept {
#if defined(TOUCHFC_HAS_AVX2)
  if ((p.dst_w & 7u) == 0) {
    bgra_letterbox_to_chw_avx2(src, sw, sh, src_stride, dst, p);
    return;
  }
#endif
  bgra_letterbox_to_chw_scalar(src, sw, sh, src_stride, dst, p);
}

}  // namespace touchfc
