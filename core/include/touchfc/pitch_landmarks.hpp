#pragma once

#include <cstdint>

namespace touchfc {

// 32 pitch landmarks in the keypoint order of Roboflow's SoccerPitchConfiguration
// (sports/configs/soccer.py, MIT), checked against its vertex list. That template
// is 120 x 70 with markings not to scale, so the positions here are the real
// IFAB markings on a 105 x 68 pitch instead: origin at the centre spot, +x
// toward the right-hand goal, +y toward the top touchline (template y = 0).
inline constexpr int kPitchLandmarks = 32;

struct PitchM {
  float x;
  float y;
};

namespace detail {
inline constexpr float kHx = 52.5f;   // half length
inline constexpr float kHy = 34.f;    // half width
inline constexpr float kPbx = 36.f;   // penalty area line, 16.5 m from goal
inline constexpr float kPby = 20.16f; // penalty area half width (40.32 m)
inline constexpr float kGbx = 47.f;   // goal area line, 5.5 m from goal
inline constexpr float kGby = 9.16f;  // goal area half width (18.32 m)
inline constexpr float kSpot = 41.5f; // penalty spot, 11 m from goal
inline constexpr float kCc = 9.15f;   // centre circle radius
}  // namespace detail

inline constexpr PitchM kPitchLandmarkM[kPitchLandmarks] = {
    {-detail::kHx, detail::kHy},    {-detail::kHx, detail::kPby},   {-detail::kHx, detail::kGby},
    {-detail::kHx, -detail::kGby},  {-detail::kHx, -detail::kPby},  {-detail::kHx, -detail::kHy},
    {-detail::kGbx, detail::kGby},  {-detail::kGbx, -detail::kGby}, {-detail::kSpot, 0.f},
    {-detail::kPbx, detail::kPby},  {-detail::kPbx, detail::kGby},  {-detail::kPbx, -detail::kGby},
    {-detail::kPbx, -detail::kPby}, {0.f, detail::kHy},             {0.f, detail::kCc},
    {0.f, -detail::kCc},            {0.f, -detail::kHy},            {detail::kPbx, detail::kPby},
    {detail::kPbx, detail::kGby},   {detail::kPbx, -detail::kGby},  {detail::kPbx, -detail::kPby},
    {detail::kSpot, 0.f},           {detail::kGbx, detail::kGby},   {detail::kGbx, -detail::kGby},
    {detail::kHx, detail::kHy},     {detail::kHx, detail::kPby},    {detail::kHx, detail::kGby},
    {detail::kHx, -detail::kGby},   {detail::kHx, -detail::kPby},   {detail::kHx, -detail::kHy},
    {-detail::kCc, 0.f},            {detail::kCc, 0.f},
};

// Short names for the manual calibration list. L/R = left/right goal, T/B = top/bottom.
inline constexpr const char* kPitchLandmarkName[kPitchLandmarks] = {
    "L corner T",      "L goal line, box T",  "L goal line, 6yd T", "L goal line, 6yd B",
    "L goal line, box B", "L corner B",       "L 6yd corner T",     "L 6yd corner B",
    "L penalty spot",  "L box corner T",      "L box edge, 6yd T",  "L box edge, 6yd B",
    "L box corner B",  "halfway T",           "centre circle T",    "centre circle B",
    "halfway B",       "R box corner T",      "R box edge, 6yd T",  "R box edge, 6yd B",
    "R box corner B",  "R penalty spot",      "R 6yd corner T",     "R 6yd corner B",
    "R corner T",      "R goal line, box T",  "R goal line, 6yd T", "R goal line, 6yd B",
    "R goal line, box B", "R corner B",       "centre circle L",    "centre circle R",
};

inline void pitch_landmark_m(int index, float& X, float& Y) noexcept {
  const PitchM p = kPitchLandmarkM[index];
  X = p.x;
  Y = p.y;
}

}  // namespace touchfc
