#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace touchfc {

// FNV-1a over every `step` byte. Identical broadcast frames hash equal.
// A one-pixel change can be missed when it falls between samples; that is
// acceptable for duplicate-frame detection, not for a checksum.
inline std::uint64_t hash_bytes_stride(const std::uint8_t* data, std::size_t bytes,
                                       std::size_t step) noexcept {
  std::uint64_t h = 14695981039346656037ull;
  if (data == nullptr || bytes == 0) return h;
  if (step == 0) step = 1;
  for (std::size_t i = 0; i < bytes; i += step) {
    h ^= data[i];
    h *= 1099511628211ull;
  }
  return h;
}

// True when a coarse grid of BGRA pixels is near black. Used to flag a
// capture that came back black (DRM or a minimised source).
inline bool looks_black_bgra(const std::uint8_t* bgra, std::uint32_t width, std::uint32_t height,
                             std::uint32_t stride) noexcept {
  if (bgra == nullptr || width < 8 || height < 8 || stride < width * 4) return false;
  const std::uint32_t step_x = std::max(1u, width / 8);
  const std::uint32_t step_y = std::max(1u, height / 8);
  std::uint64_t sum = 0;
  std::uint32_t max_v = 0;
  std::uint32_t n = 0;
  for (std::uint32_t y = 0; y < height; y += step_y) {
    const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * stride;
    for (std::uint32_t x = 0; x < width; x += step_x) {
      const std::uint8_t* px = row + static_cast<std::size_t>(x) * 4;
      const std::uint32_t v = static_cast<std::uint32_t>(px[0]) + px[1] + px[2];
      max_v = std::max(max_v, v);
      sum += v;
      ++n;
    }
  }
  return n > 0 && max_v < 24 && sum / n < 12;
}

}  // namespace touchfc
