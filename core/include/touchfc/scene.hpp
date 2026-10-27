#pragma once

#include <cstdint>

namespace touchfc {

enum class Scene : std::uint8_t { Other = 0, Wide = 1, Close = 2 };

struct SceneParams {
  int margin = 12;          // green over red and blue
  int min_green = 40;
  float wide_frac = 0.28f;  // grass share of the frame
  float mid_frac = 0.15f;   // grass share of the middle third
  float close_frac = 0.12f;
};

// Coarse green-pitch fraction. Wide when a lot of the frame is grass and it
// reaches the middle of the picture. Close when grass is only along the bottom.
inline Scene classify_scene_bgra(const std::uint8_t* bgra, std::uint32_t w, std::uint32_t h,
                                 std::uint32_t stride, const SceneParams& sp = {},
                                 float* green_frac = nullptr) noexcept {
  if (!bgra || w < 16 || h < 16 || stride < w * 4) return Scene::Other;
  const std::uint32_t step = 16;
  std::uint32_t green = 0, total = 0, mid = 0, mid_n = 0;
  for (std::uint32_t y = 0; y < h; y += step) {
    const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * stride;
    for (std::uint32_t x = 0; x < w; x += step) {
      const std::uint8_t* p = row + x * 4;
      const bool g = p[1] > p[2] + sp.margin && p[1] > p[0] + sp.margin && p[1] > sp.min_green;
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
  if (green_frac) *green_frac = frac;
  if (frac > sp.wide_frac && mid_frac > sp.mid_frac) return Scene::Wide;
  if (frac > sp.close_frac) return Scene::Close;
  return Scene::Other;
}

}  // namespace touchfc
