#include "ocr_win.hpp"

#include "win_prelude.hpp"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

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

bool Ocr::read(const std::uint8_t* bgra, int w, int h, int stride, std::string& text, int mode) {
  text.clear();
  if (!ready() || w <= 0 || h <= 0) return false;
  // Scorebug glyphs are small (a stacked bug's rows more so); the engine reads
  // them better at 2x or more, smoothed.
  const int k = h <= 160 ? 3 : 2;
  const int ow = w * k, oh = h * k;
  try {
    // Grey range of the crop, for the stretch.
    int lo = 255, hi = 0;
    if (mode == 1) {
      for (int y = 0; y < h; ++y) {
        const std::uint8_t* p = bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        for (int x = 0; x < w; ++x, p += 4) {
          const int l = (p[0] + 2 * p[1] + p[2]) >> 2;
          lo = l < lo ? l : lo;
          hi = l > hi ? l : hi;
        }
      }
      if (hi - lo < 16) return false;  // a flat crop
    }
    winrt::Windows::Storage::Streams::Buffer buf(static_cast<std::uint32_t>(ow * oh * 4));
    buf.Length(buf.Capacity());
    std::uint8_t* dst = buf.data();
    for (int y = 0; y < oh; ++y) {
      const float fy = std::clamp((static_cast<float>(y) + 0.5f) / static_cast<float>(k) - 0.5f, 0.f,
                                  static_cast<float>(h - 1));
      const int y0 = static_cast<int>(fy), y1 = y0 + 1 < h ? y0 + 1 : y0;
      const int wy = static_cast<int>((fy - static_cast<float>(y0)) * 256.f);
      const std::uint8_t* r0 = bgra + static_cast<std::size_t>(y0) * static_cast<std::size_t>(stride);
      const std::uint8_t* r1 = bgra + static_cast<std::size_t>(y1) * static_cast<std::size_t>(stride);
      std::uint8_t* row = dst + static_cast<std::size_t>(y) * ow * 4;
      for (int x = 0; x < ow; ++x) {
        const float fx = std::clamp((static_cast<float>(x) + 0.5f) / static_cast<float>(k) - 0.5f, 0.f,
                                    static_cast<float>(w - 1));
        const int x0 = static_cast<int>(fx), x1 = x0 + 1 < w ? x0 + 1 : x0;
        const int wx = static_cast<int>((fx - static_cast<float>(x0)) * 256.f);
        int c[3];
        for (int q = 0; q < 3; ++q) {
          const int top = r0[x0 * 4 + q] * (256 - wx) + r0[x1 * 4 + q] * wx;
          const int bot = r1[x0 * 4 + q] * (256 - wx) + r1[x1 * 4 + q] * wx;
          c[q] = (top * (256 - wy) + bot * wy) >> 16;
        }
        if (mode == 1) {
          const int l = std::clamp(((c[0] + 2 * c[1] + c[2]) / 4 - lo) * 255 / (hi - lo), 0, 255);
          c[0] = c[1] = c[2] = l;
        }
        row[x * 4] = static_cast<std::uint8_t>(c[0]);
        row[x * 4 + 1] = static_cast<std::uint8_t>(c[1]);
        row[x * 4 + 2] = static_cast<std::uint8_t>(c[2]);
        row[x * 4 + 3] = 255;
      }
    }
    const auto bmp = img::SoftwareBitmap::CreateCopyFromBuffer(buf, img::BitmapPixelFormat::Bgra8, ow, oh,
                                                               img::BitmapAlphaMode::Premultiplied);
    const auto result = impl_->engine.RecognizeAsync(bmp).get();
    // The engine's line order is not always top to bottom on a stacked bug:
    // order the words by row, then left to right.
    struct Word {
      float x, cy, h;
      std::string s;
    };
    std::vector<Word> words;
    for (const auto& line : result.Lines())
      for (const auto& word : line.Words()) {
        const auto r = word.BoundingRect();
        words.push_back({r.X, r.Y + 0.5f * r.Height, r.Height, winrt::to_string(word.Text())});
      }
    std::sort(words.begin(), words.end(), [](const Word& a, const Word& b) { return a.cy < b.cy; });
    for (std::size_t i = 0; i < words.size();) {
      std::size_t j = i + 1;
      while (j < words.size() && words[j].cy - words[i].cy <= 0.6f * std::max(words[i].h, words[j].h)) ++j;
      std::sort(words.begin() + static_cast<std::ptrdiff_t>(i), words.begin() + static_cast<std::ptrdiff_t>(j),
                [](const Word& a, const Word& b) { return a.x < b.x; });
      for (; i < j; ++i) {
        if (!text.empty()) text += ' ';
        text += words[i].s;
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace touchfc
