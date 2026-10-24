#pragma once

#include <cstdint>

namespace touchfc {

enum class Scene : std::uint8_t { Other = 0, Wide = 1, Close = 2 };

// Coarse green-pitch fraction. Wide when a lot of the frame is grass and it
// reaches the middle of the picture. Close when grass is only along the bottom.
inline Scene classify_scene_bgra(const std::uint8_t* bgra, std::uint32_t w, std::uint32_t h,
                                 std::uint32_t stride) noexcept {
  if (!bgra || w < 16 || h < 16 || stride < w * 4) return Scene::Other;
  const std::uint32_t step = 16;
  std::uint32_t green = 0, total = 0, mid = 0, mid_n = 0;
  for (std::uint32_t y = 0; y < h; y += step) {
    const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * stride;
    for (std::uint32_t x = 0; x < w; x += step) {
      const std::uint8_t* p = row + x * 4;
      const bool g = p[1] > p[2] + 12 && p[1] > p[0] + 12 && p[1] > 40;
      ++total;
      if (g) ++green;
      if (y > h / 3 && y < 2 * h / 3) {
        ++mid_n;
        if (g) ++mid;
      }
    }
  }
  if (total == 0) return Scene::Other;
  const float frac = static_cast<float>(green) / static_cast<float>(total);
  const float mid_frac = mid_n ? static_cast<float>(mid) / static_cast<float>(mid_n) : 0.f;
  if (frac > 0.28f && mid_frac > 0.15f) return Scene::Wide;
  if (frac > 0.12f) return Scene::Close;
  return Scene::Other;
}

}  // namespace touchfc
