#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace touchfc {

// Windows.Media.Ocr on a BGRA crop (offline, no new dependency). T7 only: the
// call blocks for tens of milliseconds and allocates.
class Ocr {
 public:
  Ocr();
  ~Ocr();
  bool ready() const noexcept;
  // Lines joined by spaces. False when the engine is missing or the read fails.
  bool read(const std::uint8_t* bgra, int w, int h, int stride, std::string& text);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace touchfc
