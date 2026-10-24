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

inline PitchPoint spadl_to_internal(PitchPoint spadl, int attack_dir, float length = 105.f,
                                    float width = 68.f) noexcept {
  const float dir = attack_dir >= 0 ? 1.f : -1.f;
  PitchPoint out;
  out.x = dir * (spadl.x - length * 0.5f);
  out.y = dir * (spadl.y - width * 0.5f);
  return out;
}

}  // namespace touchfc
