#pragma once

#include <cstdint>

namespace touchfc {

// 32 pitch landmarks in the order used by Roboflow's SoccerPitchConfiguration
// (sports/configs/soccer.py, MIT). Their template is 120 m by 70 m with the
// origin at one corner. These values are that template, converted into the
// internal frame: origin at the centre spot, +x toward the right-hand goal,
// +y toward the top touchline, scaled onto 105 x 68.
inline constexpr int kPitchLandmarks = 32;

struct PitchCm {
  float x;
  float y;
};

inline constexpr PitchCm kPitchLandmarkCm[kPitchLandmarks] = {
    {0, 0},       {0, 1450},    {0, 2584},    {0, 4416},    {0, 5550},    {0, 7000},
    {550, 2584},  {550, 4416},  {1100, 3500}, {2015, 1450}, {2015, 2584}, {2015, 4416},
    {2015, 5550}, {6000, 0},    {6000, 2585}, {6000, 4415}, {6000, 7000}, {9985, 1450},
    {9985, 2584}, {9985, 4416}, {9985, 5550}, {10900, 3500},{11450, 2584},{11450, 4416},
    {12000, 0},   {12000, 1450},{12000, 2584},{12000, 4416},{12000, 5550},{12000, 7000},
    {5085, 3500}, {6915, 3500},
};

inline void pitch_landmark_m(int index, float& X, float& Y) noexcept {
  const PitchCm p = kPitchLandmarkCm[index];
  X = (p.x / 12000.f - 0.5f) * 105.f;
  Y = (0.5f - p.y / 7000.f) * 68.f;
}

}  // namespace touchfc
