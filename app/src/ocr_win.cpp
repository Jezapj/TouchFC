#include "ocr_win.hpp"

#include "win_prelude.hpp"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <cstring>

namespace touchfc {

namespace ocr = winrt::Windows::Media::Ocr;
namespace img = winrt::Windows::Graphics::Imaging;

struct Ocr::Impl {
  ocr::OcrEngine engine{nullptr};
};

Ocr::Ocr() : impl_(std::make_unique<Impl>()) {
  try {
    impl_->engine = ocr::OcrEngine::TryCreateFromUserProfileLanguages();
  } catch (...) {
    impl_->engine = nullptr;
  }
}

Ocr::~Ocr() = default;

bool Ocr::ready() const noexcept { return impl_->engine != nullptr; }

bool Ocr::read(const std::uint8_t* bgra, int w, int h, int stride, std::string& text) {
  text.clear();
  if (!ready() || w <= 0 || h <= 0) return false;
  // Scorebug glyphs are small; the engine reads them better at 2x or more.
  const int k = h < 40 ? 3 : h < 80 ? 2 : 1;
  const int ow = w * k, oh = h * k;
  try {
    winrt::Windows::Storage::Streams::Buffer buf(static_cast<std::uint32_t>(ow * oh * 4));
    buf.Length(buf.Capacity());
    std::uint8_t* dst = buf.data();
    for (int y = 0; y < oh; ++y) {
      const std::uint8_t* src = bgra + static_cast<std::size_t>(y / k) * static_cast<std::size_t>(stride);
      std::uint8_t* row = dst + static_cast<std::size_t>(y) * ow * 4;
      for (int x = 0; x < ow; ++x) {
        std::memcpy(row + x * 4, src + (x / k) * 4, 3);
        row[x * 4 + 3] = 255;
      }
    }
    const auto bmp = img::SoftwareBitmap::CreateCopyFromBuffer(buf, img::BitmapPixelFormat::Bgra8, ow, oh,
                                                               img::BitmapAlphaMode::Premultiplied);
    const auto result = impl_->engine.RecognizeAsync(bmp).get();
    for (const auto& line : result.Lines()) {
      if (!text.empty()) text += ' ';
      text += winrt::to_string(line.Text());
    }
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace touchfc
