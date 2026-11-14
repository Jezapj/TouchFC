#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace touchfc {

// JPEG through WIC, for the replay log (T7 only: allocates, COM). A logged
// clip is 140 thumbnails; raw that is 73 MB, as JPEG about 4 MB.
class Jpeg {
 public:
  Jpeg();
  ~Jpeg();
  bool ready() const noexcept;
  // bgra: w x h, 4 bytes per pixel, alpha ignored.
  bool encode(const std::uint8_t* bgra, int w, int h, float quality, std::vector<std::uint8_t>& out);
  // dst: w x h BGRA, alpha opaque. False when the image is another size.
  bool decode(const std::uint8_t* data, std::size_t n, int w, int h, std::uint8_t* dst);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace touchfc
