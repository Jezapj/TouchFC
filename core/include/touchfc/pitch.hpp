#pragma once

namespace touchfc {

// Internal pitch space: metres, origin at the centre spot, +x toward the
// right-hand goal, +y toward the top touchline.
// SPADL space: [0, length] x [0, width], origin bottom-left, and each team
// rotated so it attacks toward +x.
struct PitchPoint {
  float x = 0;
  float y = 0;
};

inline PitchPoint internal_to_spadl(PitchPoint internal, int attack_dir, float length = 105.f,
                                    float width = 68.f) noexcept {
  const float dir = attack_dir >= 0 ? 1.f : -1.f;
  PitchPoint out;
  out.x = dir * internal.x + length * 0.5f;
  out.y = dir * internal.y + width * 0.5f;
  return out;
}

// StatsBomb locations live on a 120 x 80 pitch, origin at the left-bottom corner.
inline PitchPoint statsbomb_to_internal(float sb_x, float sb_y) noexcept {
  PitchPoint out;
  out.x = (sb_x / 120.f - 0.5f) * 105.f;
  out.y = (sb_y / 80.f - 0.5f) * 68.f;
  return out;
}

inline PitchPoint spadl_to_internal(PitchPoint spadl, int attack_dir, float length = 105.f,
                                    float width = 68.f) noexcept {
  const float dir = attack_dir >= 0 ? 1.f : -1.f;
  PitchPoint out;
  out.x = dir * (spadl.x - length * 0.5f);
  out.y = dir * (spadl.y - width * 0.5f);
  return out;
}

}  // namespace touchfc
