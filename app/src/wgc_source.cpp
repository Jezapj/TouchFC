#include "frame_source.hpp"
#include "log.hpp"
#include "win_prelude.hpp"
#include "win_time.hpp"

#include "touchfc/wake.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <cstring>
#include <string>

namespace touchfc {
namespace {

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace dx = winrt::Windows::Graphics::DirectX::Direct3D11;

class WgcSource final : public FrameSource {
 public:
  explicit WgcSource(HWND hwnd) : hwnd_(hwnd) {}
  ~WgcSource() override { close(); }

  bool open(std::string& error) override {
    if (!hwnd_ || !IsWindow(hwnd_)) {
      error = "capture target is not a window";
      return false;
    }
    if (!wgc::GraphicsCaptureSession::IsSupported()) {
      error = "Windows Graphics Capture is not supported on this machine";
      return false;
    }
    RECT rc{};
    if (!GetClientRect(hwnd_, &rc) || rc.right <= 16 || rc.bottom <= 16) {
      error = "capture target has no client area";
      return false;
    }
    width_ = static_cast<std::uint32_t>(rc.right - rc.left);
    height_ = static_cast<std::uint32_t>(rc.bottom - rc.top);
    return true;
  }

  void close() override {
    request_stop();
    if (session_) {
      session_.Close();
      session_ = nullptr;
    }
    if (pool_) {
      pool_.Close();
      pool_ = nullptr;
    }
    staging_.Reset();
    context_.Reset();
    device_.Reset();
    started_ = false;
  }

  void request_stop() override {
    stop_.store(true, std::memory_order_release);
    arrived_.notify();
  }

  bool is_live() const noexcept override { return true; }
  std::uint32_t width() const noexcept override { return width_; }
  std::uint32_t height() const noexcept override { return height_; }
  std::uint64_t capture_drops() const noexcept override {
    return drops_.load(std::memory_order_relaxed);
  }

  bool decode_into(FrameSlot& slot) override {
    if (stop_.load(std::memory_order_acquire)) return false;
    try {
      if (!started_ && !start(slot)) return false;
      return pull(slot);
    } catch (const winrt::hresult_error& err) {
      log_line("WGC error: " + winrt::to_string(err.message()));
      return false;
    }
  }

 private:
  bool start(const FrameSlot& slot) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                          D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                          D3D11_SDK_VERSION, device_.GetAddressOf(), nullptr,
                                          context_.GetAddressOf());
    if (FAILED(hr)) {
      log_line("capture device creation failed");
      return false;
    }
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(device_.As(&dxgi))) return false;
    winrt::com_ptr<IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
    auto winrt_device = inspectable.as<dx::IDirect3DDevice>();

    auto factory = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(factory->CreateForWindow(
        hwnd_, winrt::guid_of<wgc::GraphicsCaptureItem>(), reinterpret_cast<void**>(winrt::put_abi(item))));

    const winrt::Windows::Graphics::SizeInt32 size{static_cast<int32_t>(width_),
                                                    static_cast<int32_t>(height_)};
    pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        winrt_device, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
        size);
    session_ = pool_.CreateCaptureSession(item);
    session_.IsCursorCaptureEnabled(false);
    if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
      session_.IsBorderRequired(false);
    }
    pool_.FrameArrived([this](auto&&, auto&&) { arrived_.notify(); });
    session_.StartCapture();
    ensure_staging(width_, height_);
    if (slot.capacity < width_ * height_ * 4) {
      log_line("frame pool is smaller than the capture");
      return false;
    }
    started_ = true;
    log_line("WGC session started");
    return true;
  }

  void ensure_staging(std::uint32_t width, std::uint32_t height) {
    if (staging_ && staging_w_ == width && staging_h_ == height) return;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_.Reset();
    winrt::check_hresult(device_->CreateTexture2D(&desc, nullptr, staging_.GetAddressOf()));
    staging_w_ = width;
    staging_h_ = height;
  }

  bool pull(FrameSlot& slot) {
    for (;;) {
      if (stop_.load(std::memory_order_acquire)) return false;
      wgc::Direct3D11CaptureFrame newest{nullptr};
      int extra = 0;
      for (;;) {
        auto frame = pool_.TryGetNextFrame();
        if (!frame) break;
        if (newest) {
          newest.Close();
          ++extra;
        }
        newest = frame;
      }
      if (extra > 0) drops_.fetch_add(static_cast<std::uint64_t>(extra), std::memory_order_relaxed);
      if (newest) return copy_frame(slot, newest);
      const std::uint32_t seen = arrived_.load();
      auto late = pool_.TryGetNextFrame();
      if (late) return copy_frame(slot, late);
      arrived_.wait_from(seen, 32);
    }
  }

  bool copy_frame(FrameSlot& slot, wgc::Direct3D11CaptureFrame frame) {
    // The interop interface lives in the Windows SDK namespace, not in the
    // C++/WinRT projection. QI the surface for it.
    ComPtr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
    winrt::check_hresult(static_cast<IUnknown*>(winrt::get_abi(frame.Surface()))
                             ->QueryInterface(IID_PPV_ARGS(access.GetAddressOf())));
    ComPtr<ID3D11Texture2D> tex;
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(tex.GetAddressOf())));
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    if (desc.Width > slot.meta.width || desc.Height > slot.meta.height || desc.Width == 0) {
      log_line("capture frame is larger than the slot; ignoring");
      frame.Close();
      return false;
    }
    ensure_staging(desc.Width, desc.Height);
    context_->CopyResource(staging_.Get(), tex.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    const std::uint32_t row_bytes = desc.Width * 4;
    for (UINT y = 0; y < desc.Height; ++y) {
      std::memcpy(slot.bgra.get() + static_cast<std::size_t>(y) * slot.stride,
                  static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch,
                  row_bytes);
    }
    context_->Unmap(staging_.Get(), 0);
    const auto rel = frame.SystemRelativeTime();
    slot.meta.t_capture = qpc_100ns_to_app_ns(rel.count());
    slot.meta.width = desc.Width;
    slot.meta.height = desc.Height;
    slot.meta.pts_ns = 0;
    slot.meta.flags = 0;
    frame.Close();
    return true;
  }

  HWND hwnd_ = nullptr;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> drops_{0};
  WakeSeq arrived_{};
  bool started_ = false;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11Texture2D> staging_;
  std::uint32_t staging_w_ = 0;
  std::uint32_t staging_h_ = 0;
  wgc::Direct3D11CaptureFramePool pool_{nullptr};
  wgc::GraphicsCaptureSession session_{nullptr};
};

}  // namespace

std::unique_ptr<FrameSource> make_wgc_source(void* hwnd, std::string& error) {
  auto source = std::make_unique<WgcSource>(reinterpret_cast<HWND>(hwnd));
  if (!source->open(error)) return nullptr;
  return source;
}

}  // namespace touchfc
