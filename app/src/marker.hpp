#pragma once

#include <cstdint>

namespace touchfc {

struct Marker {
  float x;
  float y;
  float w;
  float h;
};

// Shared by the synthetic source and the track thread so the box lands on the
// rectangle of the same frame. Both sides read the frame id, nothing else.
inline Marker marker_for(std::uint64_t frame, std::uint32_t width) noexcept {
  Marker m;
  m.w = 48.f;
  m.h = 48.f;
  m.y = 96.f;
  const std::uint32_t span = width > 64 ? width - 64 : 1;
  m.x = static_cast<float>((frame * 7ull) % span);
  return m;
}

}  // namespace touchfc
