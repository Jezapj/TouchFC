#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "touchfc/scorebug.hpp"

namespace touchfc {

// Windows.Media.Ocr on a BGRA crop (offline, no new dependency). T7 only: the
// call blocks for tens of milliseconds and allocates.
class Ocr {
 public:
  Ocr();
  ~Ocr();
  bool ready() const noexcept;
  // Words in reading order (rows top to bottom, each left to right), joined
  // by spaces. mode 0: colour; 1: grey with the contrast stretched. False when
  // the engine is missing or the read fails. words: each word with its box.
  bool read(const std::uint8_t* bgra, int w, int h, int stride, std::string& text, int mode = 0,
            std::vector<BugWord>* words = nullptr);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace touchfc
