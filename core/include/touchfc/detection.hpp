#pragma once

#include "touchfc/types.hpp"

#include <cstdint>
#include <type_traits>

namespace touchfc {

// Pixel-space detection in the original source frame (not the letterboxed
// model input). The decoder undoes the letterbox so downstream code never
// needs to know how the frame was fed to the model.
struct Detection {
  float x = 0;        // top-left in pixels
  float y = 0;
  float w = 0;
  float h = 0;
  float score = 0;
  std::uint16_t class_id = 0;
  std::uint16_t flags = 0;
};

static_assert(std::is_trivially_copyable_v<Detection>);
static_assert(sizeof(Detection) == 24, "keep Detection small so copies are cheap");

// Letterbox parameters: how a source image was placed inside a square model
// input. All numbers are in pixels of the model input (dst), except src_w/h
// which are the original frame.
struct LetterboxParams {
  std::uint32_t src_w = 0;
  std::uint32_t src_h = 0;
  std::uint32_t dst_w = 0;
  std::uint32_t dst_h = 0;
  float scale = 1.f;   // dst = round(src * scale)
  float pad_x = 0.f;   // offset of src inside dst
  float pad_y = 0.f;
};

// Max detections carried on a slot. 128 is comfortable for an association
// pass over 22 players, three officials, a ball and some stragglers.
inline constexpr std::uint32_t kMaxDetections = 128;

}  // namespace touchfc
