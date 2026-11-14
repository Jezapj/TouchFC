#include "jpeg_win.hpp"

#include "win_prelude.hpp"

#include <objbase.h>
#include <objidl.h>
#include <ocidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstring>

namespace touchfc {

using Microsoft::WRL::ComPtr;

struct Jpeg::Impl {
  ComPtr<IWICImagingFactory> factory;
};

Jpeg::Jpeg() : impl_(std::make_unique<Impl>()) {
  // The caller's thread is already in a COM apartment (T7 calls init_apartment).
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(impl_->factory.GetAddressOf()))))
    impl_->factory.Reset();
}

Jpeg::~Jpeg() = default;

bool Jpeg::ready() const noexcept { return impl_->factory != nullptr; }

bool Jpeg::encode(const std::uint8_t* bgra, int w, int h, float quality, std::vector<std::uint8_t>& out) {
  out.clear();
  if (!ready() || w <= 0 || h <= 0) return false;
  IWICImagingFactory* f = impl_->factory.Get();
  ComPtr<IStream> stream;
  ComPtr<IWICBitmapEncoder> enc;
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> props;
  ComPtr<IWICBitmap> bmp;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf()))) return false;
  if (FAILED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, enc.GetAddressOf()))) return false;
  if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
  if (FAILED(enc->CreateNewFrame(frame.GetAddressOf(), props.GetAddressOf()))) return false;
  if (props) {
    PROPBAG2 opt{};
    wchar_t name[] = L"ImageQuality";
    opt.pstrName = name;
    VARIANT v{};
    v.vt = VT_R4;
    v.fltVal = quality;
    props->Write(1, &opt, &v);
  }
  if (FAILED(frame->Initialize(props.Get()))) return false;
  if (FAILED(frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h)))) return false;
  WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
  if (FAILED(frame->SetPixelFormat(&fmt))) return false;
  // 32bppBGR: the fourth byte is ignored. WriteSource converts to the frame's format.
  if (FAILED(f->CreateBitmapFromMemory(static_cast<UINT>(w), static_cast<UINT>(h), GUID_WICPixelFormat32bppBGR,
                                       static_cast<UINT>(w) * 4u, static_cast<UINT>(w) * 4u * static_cast<UINT>(h),
                                       const_cast<BYTE*>(bgra), bmp.GetAddressOf())))
    return false;
  if (FAILED(frame->WriteSource(bmp.Get(), nullptr))) return false;
  if (FAILED(frame->Commit()) || FAILED(enc->Commit())) return false;
  STATSTG st{};
  HGLOBAL mem = nullptr;
  if (FAILED(stream->Stat(&st, STATFLAG_NONAME)) || FAILED(GetHGlobalFromStream(stream.Get(), &mem))) return false;
  const std::size_t n = static_cast<std::size_t>(st.cbSize.QuadPart);
  const void* p = GlobalLock(mem);
  if (!p) return false;
  out.resize(n);
  std::memcpy(out.data(), p, n);
  GlobalUnlock(mem);
  return n > 0;
}

bool Jpeg::decode(const std::uint8_t* data, std::size_t n, int w, int h, std::uint8_t* dst) {
  if (!ready() || !data || n == 0) return false;
  IWICImagingFactory* f = impl_->factory.Get();
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapDecoder> dec;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(f->CreateStream(stream.GetAddressOf()))) return false;
  if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(n)))) return false;
  if (FAILED(f->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.GetAddressOf())))
    return false;
  if (FAILED(dec->GetFrame(0, frame.GetAddressOf()))) return false;
  UINT fw = 0, fh = 0;
  if (FAILED(frame->GetSize(&fw, &fh)) || fw != static_cast<UINT>(w) || fh != static_cast<UINT>(h)) return false;
  if (FAILED(f->CreateFormatConverter(conv.GetAddressOf()))) return false;
  if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom)))
    return false;
  return SUCCEEDED(conv->CopyPixels(nullptr, static_cast<UINT>(w) * 4u, static_cast<UINT>(w) * 4u * static_cast<UINT>(h),
                                    dst));
}

}  // namespace touchfc
