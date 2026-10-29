#include "present.hpp"

#include "d3d_util.hpp"
#include "log.hpp"
#include "pipeline.hpp"
#include "services.hpp"
#include "threads_win.hpp"
#include "win_time.hpp"

#include "touchfc/config_json.hpp"
#include "touchfc/homography.hpp"
#include "touchfc/mini_replay.hpp"
#include "touchfc/panel_flip.hpp"
#include "touchfc/pitch_calib.hpp"
#include "touchfc/pitch_landmarks.hpp"
#include "touchfc/play_state.hpp"
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_2.h>

#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace touchfc {
namespace {

class App;

// Pitch markings as polylines in metres, for the minimap and the reprojection view.
constexpr float kLineX[] = {-52.5f, 52.5f, 52.5f, -52.5f, -52.5f};
constexpr float kLineY[] = {34.f, 34.f, -34.f, -34.f, 34.f};
constexpr float kHalfX[] = {0.f, 0.f};
constexpr float kHalfY[] = {34.f, -34.f};
constexpr float kBoxX[] = {-52.5f, -36.f, -36.f, -52.5f};
constexpr float kBoxY[] = {20.16f, 20.16f, -20.16f, -20.16f};
constexpr float kSixX[] = {-52.5f, -47.f, -47.f, -52.5f};
constexpr float kSixY[] = {9.16f, 9.16f, -9.16f, -9.16f};

// to_screen(X, Y, out) is false for a point that cannot be drawn (above the horizon).
template <class F>
void draw_pitch_lines(ImDrawList* dl, F&& to_screen, ImU32 col, float thick) {
  auto poly = [&](const float* xs, const float* ys, int n, float mirror) {
    ImVec2 prev;
    bool have = false;
    for (int k = 0; k + 1 < n; ++k) {
      for (int t = 0; t <= 8; ++t) {
        if (t == 0 && k > 0) continue;
        const float a = t / 8.f;
        ImVec2 p;
        const bool ok = to_screen(mirror * (xs[k] + a * (xs[k + 1] - xs[k])), ys[k] + a * (ys[k + 1] - ys[k]), p);
        if (ok && have) dl->AddLine(prev, p, col, thick);
        prev = p;
        have = ok;
      }
    }
  };
  poly(kLineX, kLineY, 5, 1.f);
  poly(kHalfX, kHalfY, 2, 1.f);
  for (const float m : {1.f, -1.f}) {
    poly(kBoxX, kBoxY, 4, m);
    poly(kSixX, kSixY, 4, m);
  }
  ImVec2 prev;
  bool have = false;
  for (int i = 0; i <= 48; ++i) {
    const float a = i * 6.2831853f / 48.f;
    ImVec2 p;
    const bool ok = to_screen(9.15f * std::cos(a), 9.15f * std::sin(a), p);
    if (ok && have) dl->AddLine(prev, p, col, thick);
    prev = p;
    have = ok;
  }
}

struct Binding {
  ImGuiContext* imgui = nullptr;
  App* app = nullptr;
  bool control = false;
};

struct ListedWindow {
  HWND hwnd = nullptr;
  std::string title;
};

class App {
 public:
  explicit App(const AppArgs& args) : args_(args) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::strftime(date_buf_, sizeof(date_buf_), "%Y%m%d", &tm);
  }
  int run();

  void on_view_resize() { view_dirty_ = true; }
  void on_control_resize() { control_dirty_ = true; }

 private:
  bool create_window(HWND& out, const wchar_t* title, DWORD style, DWORD ex, int w, int h, Binding& binding);
  bool create_hwnd_swap(HWND hwnd, Microsoft::WRL::ComPtr<IDXGISwapChain1>& swap, bool premultiplied);
  void recreate_rtv(IDXGISwapChain1* swap, Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& rtv);
  void ensure_video_texture(std::uint32_t w, std::uint32_t h);
  void ensure_offscreen(std::uint32_t w, std::uint32_t h);
  void upload(const FrameSlot& slot);
  void remember(const FrameSlot& slot);
  void draw_view();
  void draw_control();
  void draw_tuning(Config& cfg);
  void draw_match();
  void write_validation();
  void start_services();
  bool live_value(const char* key, char* buf, int n) const;
  bool load_profile(const char* name);
  void load_last_profile();
  void pump_pipeline();
  void track_target();
  void refresh_windows();
  bool start_overlay_capture();
  std::unique_ptr<Detector> make_detector();
  std::unique_ptr<PitchModel> make_pitch();
  void imgui_begin(Binding& binding);
  void imgui_end(ID3D11RenderTargetView* rtv);

  AppArgs args_;
  Gpu gpu_{};
  FullscreenPass copy_pass_{};
  FullscreenPass premul_pass_{};
  HWND control_hwnd_ = nullptr;
  HWND view_hwnd_ = nullptr;
  Binding control_binding_{};
  Binding view_binding_{};
  Microsoft::WRL::ComPtr<IDXGISwapChain1> control_swap_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> view_swap_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> control_rtv_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view_rtv_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> video_tex_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> video_srv_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> offscreen_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> offscreen_rtv_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> offscreen_srv_;
  Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp_;
  Microsoft::WRL::ComPtr<IDCompositionTarget> dcomp_target_;
  Microsoft::WRL::ComPtr<IDCompositionVisual> dcomp_visual_;
  std::uint32_t video_w_ = 0;
  std::uint32_t video_h_ = 0;
  std::uint32_t off_w_ = 0;
  std::uint32_t off_h_ = 0;
  bool view_dirty_ = false;
  bool control_dirty_ = false;
  bool have_frame_ = false;
  bool anchored_ = false;
  bool affinity_ok_ = false;
  OverlayDraw last_draw_{};
  std::array<Detection, kMaxDetections> last_dets_{};
  std::uint32_t last_det_count_ = 0;
  std::uint32_t last_w_ = 0;
  std::uint32_t last_h_ = 0;
  FrameId last_frame_ = 0;
  TimeNs last_capture_ = 0;
  std::uint8_t last_flags_ = 0;
  std::uint8_t last_scene_ = 0;
  std::uint8_t last_pitch_valid_ = 0;
  float last_reproj_m_ = 0.f;
  std::array<float, 9> last_pitch_h_{};
  std::array<std::uint8_t, kMaxDetections> last_pitch_ok_{};
  std::uint16_t last_pitch_age_ = 0;
  std::uint8_t last_pitch_reject_ = 0;
  std::uint8_t last_pitch_manual_ = 0;
  float last_gmc_cost_ = 0.f;
  float last_line_support_ = 0.f;
  std::uint8_t last_gmc_ok_ = 0;
  std::uint8_t last_kp_n_ = 0;
  std::array<std::uint8_t, 32> last_kp_id_{};
  std::array<float, 32> last_kp_x_{};
  std::array<float, 32> last_kp_y_{};
  bool debug_reproj_ = false;
  // M14 panels. Sizes are last frame's, for the flip test.
  void layout_panels(float ox, float oy, float scale, float cw, float ch);
  void draw_stats_panel();
  void draw_card();
  void draw_replay();
  PanelFlip flip_stats_, flip_card_, flip_map_;
  ImVec2 stats_sz_{300.f, 120.f};
  ImVec2 card_sz_{220.f, 90.f};
  ImVec2 stats_pos_{}, card_pos_{}, map_pos_{}, replay_pos_{};
  float hud_h_ = 0.f;
  TimeNs flip_t_ = 0;
  TimeNs dead_since_ = 0;
  float last_poss_spell_s_ = 0.f;
  std::uint16_t last_holder_passes_ = 0, last_holder_passes_ok_ = 0, last_holder_shots_ = 0;
  struct TrackLabel {
    std::uint16_t track = 0;
    std::uint8_t team = 0;  // 1 A (home), 2 B
    std::uint8_t idx = 0;   // squad index
  };
  TrackLabel labels_[32]{};
  const TrackLabel* label_of(std::uint16_t track) const {
    for (const auto& l : labels_)
      if (l.track == track && track != 0) return &l;
    return nullptr;
  }
  // M15 playback.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> replay_tex_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> replay_srv_;
  int replay_clip_ = -1;
  int replay_frame_ = -1;
  TimeNs replay_t0_ = 0;
  ImVec2 replay_sz_{};
  MatchInfo match_{};
  int league_idx_ = 0;
  char date_buf_[9] = "";
  char team_filter_[2][32]{};
  std::uint32_t pick_id_[2]{};
  int sub_off_[2] = {-1, -1};
  int sub_on_[2] = {-1, -1};
  bool roi_drag_ = false;
  bool roi_manual_ = false;
  bool dragging_ = false;
  ImVec2 drag_a_{};
  std::int16_t last_bug_roi_[4]{};
  TeamStats last_team_stats_{};
  float last_holder_rating_ = 0.f;
  char profile_name_[48] = "default";
  std::string profile_msg_;
  float last_green_frac_ = 0.f;
  float last_view_angle_ = 0.f;
  float last_height_frac_ = 0.f;
  std::uint8_t last_play_dead_ = 0;
  std::uint8_t last_play_reasons_ = 0;
  // Minimap positions, refreshed only in play so a dead ball can freeze them.
  bool mm_valid_ = false;
  std::uint32_t mm_n_ = 0;
  std::array<float, kMaxDetections> mm_x_{};
  std::array<float, kMaxDetections> mm_y_{};
  std::array<ImU32, kMaxDetections> mm_col_{};
  bool mm_ball_ = false;
  float mm_bx_ = 0.f;
  float mm_by_ = 0.f;
  std::uint8_t minimap_dead_mode_ = 0;
  std::FILE* pitch_dump_ = nullptr;
  int calib_pick_ = 0;
  bool sent_anchor_[32]{};
  float sent_ax_[32]{};
  float sent_ay_[32]{};
  std::array<float, kMaxDetections> last_pitch_x_{};
  std::array<float, kMaxDetections> last_pitch_y_{};
  std::array<std::uint8_t, kMaxDetections> last_team_{};
  std::uint8_t last_pc_valid_ = 0;
  float last_territory_ = 0.5f;
  std::array<std::uint8_t, 160> last_pc_ds_{};
  std::uint8_t last_ball_ok_ = 0;
  std::uint8_t last_poss_team_ = 0;
  std::uint16_t last_poss_track_ = 0;
  float last_ball_x_ = 0.f;
  float last_ball_y_ = 0.f;
  TimeNs anchor_wall_ = 0;
  TimeNs anchor_pts_ = 0;
  int held_ = -1;
  float fps_ = 0;
  int fps_frames_ = 0;
  TimeNs fps_t_ = 0;
  std::vector<ListedWindow> windows_;
  HWND selected_ = nullptr;
  TimeNs window_refresh_ = 0;
  std::string status_;
  bool pending_cmd_ = false;
  UiCommand pending_{};

  std::unique_ptr<FrameSource> source_;
  std::unique_ptr<FramePool> pool_;
  std::unique_ptr<HdrSink> sink_;
  std::unique_ptr<Detector> detector_;
  std::unique_ptr<PitchModel> pitch_;
  std::unique_ptr<Pipeline> pipe_;
  std::unique_ptr<Services> services_;
};

App* g_app = nullptr;

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto* binding = reinterpret_cast<Binding*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (binding && binding->imgui && msg != WM_CREATE) {
    ImGui::SetCurrentContext(binding->imgui);
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return 1;
  }
  switch (msg) {
    case WM_SIZE:
      if (binding && binding->app) {
        if (binding->control) binding->app->on_control_resize();
        else binding->app->on_view_resize();
      }
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool App::create_window(HWND& out, const wchar_t* title, DWORD style, DWORD ex, int w, int h,
                        Binding& binding) {
  out = CreateWindowExW(ex, L"TouchFCWindow", title, style, CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr,
                        nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!out) return false;
  binding.app = this;
  SetWindowLongPtrW(out, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&binding));
  return true;
}

bool App::create_hwnd_swap(HWND hwnd, Microsoft::WRL::ComPtr<IDXGISwapChain1>& swap, bool premultiplied) {
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
  if (FAILED(gpu_.device.As(&dxgi))) return false;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  if (FAILED(dxgi->GetAdapter(adapter.GetAddressOf()))) return false;
  Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
  if (FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())))) return false;
  RECT rc{};
  GetClientRect(hwnd, &rc);
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = std::max(1L, rc.right - rc.left);
  desc.Height = std::max(1L, rc.bottom - rc.top);
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  if (premultiplied) {
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(factory->CreateSwapChainForComposition(gpu_.device.Get(), &desc, nullptr,
                                                      swap.GetAddressOf()))) {
      return false;
    }
  } else {
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (FAILED(factory->CreateSwapChainForHwnd(gpu_.device.Get(), hwnd, &desc, nullptr, nullptr,
                                               swap.GetAddressOf()))) {
      return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
  }
  return true;
}

void App::recreate_rtv(IDXGISwapChain1* swap, Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& rtv) {
  rtv.Reset();
  Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
  if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf())))) return;
  gpu_.device->CreateRenderTargetView(back.Get(), nullptr, rtv.GetAddressOf());
}

void App::ensure_video_texture(std::uint32_t w, std::uint32_t h) {
  if (video_tex_ && video_w_ == w && video_h_ == h) return;
  video_srv_.Reset();
  video_tex_.Reset();
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = w;
  desc.Height = h;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(gpu_.device->CreateTexture2D(&desc, nullptr, video_tex_.GetAddressOf()))) return;
  gpu_.device->CreateShaderResourceView(video_tex_.Get(), nullptr, video_srv_.GetAddressOf());
  video_w_ = w;
  video_h_ = h;
}

void App::ensure_offscreen(std::uint32_t w, std::uint32_t h) {
  if (w == 0 || h == 0) return;
  if (offscreen_ && off_w_ == w && off_h_ == h) return;
  offscreen_srv_.Reset();
  offscreen_rtv_.Reset();
  offscreen_.Reset();
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = w;
  desc.Height = h;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(gpu_.device->CreateTexture2D(&desc, nullptr, offscreen_.GetAddressOf()))) return;
  gpu_.device->CreateRenderTargetView(offscreen_.Get(), nullptr, offscreen_rtv_.GetAddressOf());
  gpu_.device->CreateShaderResourceView(offscreen_.Get(), nullptr, offscreen_srv_.GetAddressOf());
  off_w_ = w;
  off_h_ = h;
}

void App::upload(const FrameSlot& slot) {
  ensure_video_texture(slot.meta.width, slot.meta.height);
  if (!video_tex_) return;
  gpu_.context->UpdateSubresource(video_tex_.Get(), 0, nullptr, slot.bgra.get(), slot.stride, 0);
}

void App::remember(const FrameSlot& slot) {
  upload(slot);
  last_draw_ = slot.draw;
  last_det_count_ = slot.det_count;
  // Bounded memcpy; last_dets_ is a fixed-size array (kMaxDetections).
  for (std::uint32_t i = 0; i < slot.det_count && i < last_dets_.size(); ++i) {
    last_dets_[i] = slot.dets[i];
  }
  last_w_ = slot.meta.width;
  last_h_ = slot.meta.height;
  last_frame_ = slot.meta.id;
  last_capture_ = slot.meta.t_capture;
  last_flags_ = slot.meta.flags;
  last_scene_ = slot.scene;
  last_pitch_valid_ = slot.pitch_valid;
  last_reproj_m_ = slot.reproj_m;
  last_pitch_h_ = slot.pitch_h;
  last_pitch_age_ = slot.pitch_age;
  last_pitch_reject_ = slot.pitch_reject;
  last_pitch_manual_ = slot.pitch_manual;
  last_gmc_cost_ = slot.gmc_cost;
  last_line_support_ = slot.line_support;
  last_green_frac_ = slot.green_frac;
  last_view_angle_ = slot.view_angle_deg;
  last_height_frac_ = slot.player_height_frac;
  if (slot.play_dead && !last_play_dead_) dead_since_ = now_ns();
  last_play_dead_ = slot.play_dead;
  last_play_reasons_ = slot.play_reasons;
  if (!slot.play_dead) {
    mm_valid_ = slot.pitch_valid != 0;
    mm_n_ = 0;
    for (std::uint32_t i = 0; mm_valid_ && i < slot.det_count; ++i) {
      if (slot.dets[i].class_id > 1 || !slot.pitch_ok[i]) continue;
      const std::uint8_t t = slot.det_team[i];
      mm_x_[mm_n_] = slot.pitch_x[i];
      mm_y_[mm_n_] = slot.pitch_y[i];
      mm_col_[mm_n_] = t == 1 ? IM_COL32(40, 140, 255, 255)
                       : t == 2 ? IM_COL32(255, 70, 70, 255)
                       : slot.dets[i].class_id == 1 ? IM_COL32(255, 220, 32, 255)
                                                    : IM_COL32(0, 220, 220, 255);
      ++mm_n_;
    }
    mm_ball_ = slot.ball_ok != 0;
    mm_bx_ = slot.ball_x;
    mm_by_ = slot.ball_y;
  }
  last_gmc_ok_ = slot.gmc_ok;
  if (pitch_dump_) {
    // Present thread, not the hot path. One row per frame.
    std::fprintf(pitch_dump_, "%llu,%lld,%u,%u,%u,%u,%u,%.2f,%u", static_cast<unsigned long long>(slot.meta.id),
                 static_cast<long long>(slot.meta.pts_ns), slot.pitch_valid, slot.pitch_manual, slot.pitch_age,
                 slot.pitch_reject, slot.gmc_ok, slot.gmc_cost, slot.scene);
    for (const float v : slot.pitch_h) std::fprintf(pitch_dump_, ",%.9g", v);
    std::fprintf(pitch_dump_, ",");
    for (std::uint32_t k = 0; slot.kp_fresh && k < slot.kp_n; ++k)
      std::fprintf(pitch_dump_, "%u:%.1f:%.1f;", slot.kp_id[k], slot.kp_x[k], slot.kp_y[k]);
    std::fprintf(pitch_dump_, ",%.3f,%u,%u,%.1f,%.3f\n", slot.line_support, slot.play_dead, slot.play_reasons,
                 slot.view_angle_deg, slot.player_height_frac);
  }
  if (slot.kp_fresh) {
    last_kp_n_ = slot.kp_n;
    last_kp_id_ = slot.kp_id;
    last_kp_x_ = slot.kp_x;
    last_kp_y_ = slot.kp_y;
  }
  for (std::uint32_t i = 0; i < slot.det_count && i < last_pitch_x_.size(); ++i) {
    last_pitch_x_[i] = slot.pitch_x[i];
    last_pitch_y_[i] = slot.pitch_y[i];
    last_team_[i] = slot.det_team[i];
    last_pitch_ok_[i] = slot.pitch_ok[i];
  }
  last_pc_valid_ = slot.pc_valid;
  last_territory_ = slot.territory;
  last_pc_ds_ = slot.pc_ds;
  last_ball_ok_ = slot.ball_ok;
  last_ball_x_ = slot.ball_x;
  last_ball_y_ = slot.ball_y;
  last_poss_team_ = slot.poss_team;
  last_poss_track_ = slot.poss_track;
  for (int k = 0; k < 4; ++k) last_bug_roi_[k] = slot.bug_roi[k];
  last_team_stats_ = slot.team_stats;
  last_holder_rating_ = slot.holder_rating;
  last_poss_spell_s_ = slot.poss_spell_s;
  last_holder_passes_ = slot.holder_passes;
  last_holder_passes_ok_ = slot.holder_passes_ok;
  last_holder_shots_ = slot.holder_shots;
  have_frame_ = true;
  const TimeNs now = now_ns();
  if (pipe_) pipe_->note_present(slot.meta.id, slot.meta.t_capture, now);
  if (pipe_) pipe_->write_thumb(slot);  // after the latency stamp
}

void App::imgui_begin(Binding& binding) {
  ImGui::SetCurrentContext(binding.imgui);
  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
}

void App::imgui_end(ID3D11RenderTargetView* rtv) {
  ImGui::Render();
  gpu_.context->OMSetRenderTargets(1, &rtv, nullptr);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void App::pump_pipeline() {
  if (!pipe_) return;
  if (pending_cmd_) {
    if (pipe_->commands().try_push(pending_)) pending_cmd_ = false;
  }
  if (pipe_->live()) {
    const std::uint32_t slot = pipe_->take_live_present();
    if (slot != Mailbox::kEmpty) {
      remember(pipe_->pool().slot(slot));
      pipe_->release_presented(slot);
    }
    return;
  }
  if (held_ < 0) {
    std::uint32_t slot = 0;
    if (pipe_->pop_present(slot)) held_ = static_cast<int>(slot);
  }
  if (held_ < 0) return;
  const FrameSlot& slot = pipe_->pool().slot(static_cast<std::uint32_t>(held_));
  const Config cfg = pipe_->config().load();
  const TimeNs margin = static_cast<TimeNs>(cfg.delay_margin_ms * 1000000.0);
  if (!anchored_) {
    anchor_wall_ = now_ns();
    anchor_pts_ = slot.meta.pts_ns;
    anchored_ = true;
  }
  const TimeNs due = anchor_wall_ + (slot.meta.pts_ns - anchor_pts_) + margin;
  const TimeNs now = now_ns();
  if (now + 1000000 < due) return;
  if (now > due + 20000000) pipe_->late_frames.fetch_add(1, std::memory_order_relaxed);
  remember(slot);
  pipe_->release_presented(static_cast<std::uint32_t>(held_));
  held_ = -1;
}

void App::draw_view() {
  if (!view_hwnd_ || !view_swap_) return;
  if (view_dirty_) {
    // A composition swap chain rejects 0,0. Pass the client size, and unbind
    // the old target first or the device is removed.
    ID3D11RenderTargetView* none = nullptr;
    gpu_.context->OMSetRenderTargets(1, &none, nullptr);
    view_rtv_.Reset();
    RECT crc{};
    GetClientRect(view_hwnd_, &crc);
    const UINT rw = static_cast<UINT>(std::max(1L, crc.right - crc.left));
    const UINT rh = static_cast<UINT>(std::max(1L, crc.bottom - crc.top));
    if (FAILED(view_swap_->ResizeBuffers(0, rw, rh, DXGI_FORMAT_UNKNOWN, 0)))
      log_line("view ResizeBuffers failed");
    recreate_rtv(view_swap_.Get(), view_rtv_);
    view_dirty_ = false;
    if (args_.mode == AppArgs::Mode::Overlay) {
      RECT rc{};
      GetClientRect(view_hwnd_, &rc);
      ensure_offscreen(static_cast<std::uint32_t>(std::max(1L, rc.right)),
                       static_cast<std::uint32_t>(std::max(1L, rc.bottom)));
    }
  }
  RECT rc{};
  GetClientRect(view_hwnd_, &rc);
  const float cw = static_cast<float>(std::max(1L, rc.right - rc.left));
  const float ch = static_cast<float>(std::max(1L, rc.bottom - rc.top));
  ++fps_frames_;
  const TimeNs now = now_ns();
  if (fps_t_ == 0) fps_t_ = now;
  if (now - fps_t_ >= 500000000) {
    fps_ = static_cast<float>(fps_frames_ * 1.0e9 / static_cast<double>(now - fps_t_));
    fps_frames_ = 0;
    fps_t_ = now;
  }

  float ox = 0, oy = 0, scale = 1;
  D3D11_VIEWPORT fitted{0, 0, cw, ch, 0, 1};
  if (have_frame_ && last_w_ > 0 && last_h_ > 0) {
    scale = std::min(cw / static_cast<float>(last_w_), ch / static_cast<float>(last_h_));
    const float dw = static_cast<float>(last_w_) * scale;
    const float dh = static_cast<float>(last_h_) * scale;
    ox = (cw - dw) * 0.5f;
    oy = (ch - dh) * 0.5f;
    fitted = {ox, oy, dw, dh, 0.f, 1.f};
  }

  const bool overlay = args_.mode == AppArgs::Mode::Overlay;
  ID3D11RenderTargetView* imgui_target = view_rtv_.Get();
  if (overlay) {
    ensure_offscreen(static_cast<std::uint32_t>(cw), static_cast<std::uint32_t>(ch));
    imgui_target = offscreen_rtv_.Get();
  }
  const float clear[4] = {0, 0, 0, 0};
  const float video_clear[4] = {0.05f, 0.05f, 0.06f, 1.f};
  if (!overlay && view_rtv_) {
    gpu_.context->OMSetRenderTargets(1, view_rtv_.GetAddressOf(), nullptr);
    gpu_.context->ClearRenderTargetView(view_rtv_.Get(), video_clear);
    if (have_frame_ && video_srv_) draw_fullscreen(gpu_.context.Get(), copy_pass_, video_srv_.Get(), fitted);
  } else if (offscreen_rtv_) {
    gpu_.context->OMSetRenderTargets(1, offscreen_rtv_.GetAddressOf(), nullptr);
    gpu_.context->ClearRenderTargetView(offscreen_rtv_.Get(), clear);
  }

  imgui_begin(view_binding_);
  if (have_frame_ && last_draw_.box_w > 1.f) {
    const ImVec2 a(ox + last_draw_.box_x * scale, oy + last_draw_.box_y * scale);
    const ImVec2 b(ox + (last_draw_.box_x + last_draw_.box_w) * scale,
                   oy + (last_draw_.box_y + last_draw_.box_h) * scale);
    ImGui::GetForegroundDrawList()->AddRect(a, b, IM_COL32(255, 176, 32, 255), 0.f, 0, 2.f);
  }
  if (have_frame_) {
    // Class 0 (player) is grey, 1 (goalkeeper) yellow, 2 (referee) magenta,
    // 3 (ball) green, everything else grey. These classes match the roboflow
    // sports model's label order; the stub uses them too.
    static constexpr ImU32 kClassColor[] = {
        IM_COL32(200, 200, 200, 255), IM_COL32(255, 220, 32, 255),
        IM_COL32(220, 32, 220, 255),  IM_COL32(32, 220, 32, 255),
        IM_COL32(200, 200, 200, 255),
    };
    auto* dl = ImGui::GetForegroundDrawList();
    const std::uint32_t shown = pipe_ && !pipe_->config().load().show_boxes ? 0 : last_det_count_;
    for (std::uint32_t i = 0; i < shown; ++i) {
      const Detection& d = last_dets_[i];
      const bool teamed = last_team_[i] == 1 || last_team_[i] == 2;
      const ImU32 col = teamed ? (last_team_[i] == 1 ? IM_COL32(40, 140, 255, 255) : IM_COL32(255, 70, 70, 255))
                               : kClassColor[d.class_id < 4 ? d.class_id : 4];
      const ImVec2 a(ox + d.x * scale, oy + d.y * scale);
      const ImVec2 b(ox + (d.x + d.w) * scale, oy + (d.y + d.h) * scale);
      dl->AddRect(a, b, col, 0.f, 0, 1.5f);
      if (d.track_id != 0) {
        char label[16];
        const char* tag = last_team_[i] == 1 ? "A" : last_team_[i] == 2 ? "B" : "";
        std::snprintf(label, sizeof(label), "#%u%s%s", static_cast<unsigned>(d.track_id), tag[0] ? " " : "", tag);
        dl->AddText(ImVec2(a.x + 2.f, a.y + 2.f), col, label);
      }
    }
    if (debug_reproj_ && last_pitch_valid_) {
      // Pitch lines and landmarks warped by H^-1 onto the video.
      Homography inv;
      if (invert3(last_pitch_h_.data(), inv.h)) {
        auto to_img = [&](float X, float Y, ImVec2& out) {
          float x, y;
          if (!apply_homography(inv, X, Y, x, y)) return false;
          if (std::fabs(x) > 4.f * last_w_ || std::fabs(y) > 4.f * last_h_) return false;
          out = ImVec2(ox + x * scale, oy + y * scale);
          return true;
        };
        draw_pitch_lines(dl, to_img, IM_COL32(255, 0, 255, 220), 2.f);
        for (int k = 0; k < kPitchLandmarks; ++k) {
          ImVec2 p;
          if (!to_img(kPitchLandmarkM[k].x, kPitchLandmarkM[k].y, p)) continue;
          char id[4];
          std::snprintf(id, sizeof(id), "%d", k);
          dl->AddLine(ImVec2(p.x - 4, p.y), ImVec2(p.x + 4, p.y), IM_COL32(255, 0, 255, 255));
          dl->AddLine(ImVec2(p.x, p.y - 4), ImVec2(p.x, p.y + 4), IM_COL32(255, 0, 255, 255));
          dl->AddText(ImVec2(p.x + 4, p.y - 16), IM_COL32(255, 0, 255, 255), id);
        }
      }
    }
    if (debug_reproj_) {
      // Raw keypoints with the model's ids. Each should sit on its magenta cross.
      for (int k = 0; k < last_kp_n_; ++k) {
        const ImVec2 p(ox + last_kp_x_[static_cast<std::uint32_t>(k)] * scale,
                       oy + last_kp_y_[static_cast<std::uint32_t>(k)] * scale);
        char id[4];
        std::snprintf(id, sizeof(id), "%d", last_kp_id_[static_cast<std::uint32_t>(k)]);
        dl->AddCircle(p, 5.f, IM_COL32(255, 230, 0, 255), 0, 2.f);
        dl->AddText(ImVec2(p.x + 6, p.y + 2), IM_COL32(255, 230, 0, 255), id);
      }
    }
    layout_panels(ox, oy, scale, cw, ch);
    if (pipe_) minimap_dead_mode_ = pipe_->config().load().minimap_dead_mode;
    const bool dead = last_play_dead_ != 0;
    if (!(dead && minimap_dead_mode_ == static_cast<std::uint8_t>(MinimapDeadMode::Hide))) {
      // 105:68, scales with the window, clipped so nothing lands outside it.
      // Dead ball + Freeze: the last in-play positions, dimmed.
      const float pad = 8.f;
      const float mw = std::clamp(cw * 0.22f, 160.f, 420.f);
      const float iw = mw - 2.f * pad;
      const float ih = iw * 68.f / 105.f;
      const float mh = ih + 2.f * pad;
      const float mx = map_pos_.x, my = map_pos_.y;
      const float ix = mx + pad, iy = my + pad;
      dl->PushClipRect(ImVec2(mx, my), ImVec2(mx + mw, my + mh), true);
      dl->AddRectFilled(ImVec2(mx, my), ImVec2(mx + mw, my + mh), IM_COL32(12, 48, 18, 180));
      if (!dead && last_pitch_valid_ && last_pc_valid_) {
        for (int j = 0; j < 10; ++j) {
          for (int i = 0; i < 16; ++i) {
            const int p = last_pc_ds_[static_cast<std::uint32_t>((9 - j) * 16 + i)];
            const ImU32 col = IM_COL32(200 - p * 160 / 255, 40, 40 + p * 180 / 255, 110);
            dl->AddRectFilled(ImVec2(ix + i * iw / 16.f, iy + j * ih / 10.f),
                              ImVec2(ix + (i + 1) * iw / 16.f, iy + (j + 1) * ih / 10.f), col);
          }
        }
      }
      auto map = [&](float px, float py) {
        return ImVec2(ix + (px + 52.5f) / 105.f * iw, iy + (34.f - py) / 68.f * ih);
      };
      draw_pitch_lines(dl, [&](float X, float Y, ImVec2& out) { out = map(X, Y); return true; },
                       IM_COL32(220, 220, 220, 150), 1.f);
      const ImU32 dim = dead ? 0x60FFFFFFu : 0xFFFFFFFFu;  // alpha mask
      if (mm_valid_) {
        for (std::uint32_t i = 0; i < mm_n_; ++i) dl->AddCircleFilled(map(mm_x_[i], mm_y_[i]), 3.f, mm_col_[i] & dim);
        if (mm_ball_) dl->AddCircleFilled(map(mm_bx_, mm_by_), 4.f, IM_COL32(255, 255, 255, 255) & dim);
      }
      const char* msg = dead ? "paused" : mm_valid_ ? nullptr : "not calibrated";
      if (msg) {
        const ImVec2 sz = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(mx + 0.5f * (mw - sz.x), my + 0.5f * (mh - sz.y)), IM_COL32(255, 255, 255, 230), msg);
      }
      dl->PopClipRect();
    }
  }
  ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.55f);
  ImGui::Begin("HUD", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);
  const double p50 = pipe_ ? static_cast<double>(pipe_->present_p50_ns.load()) / 1.0e6 : 0;
  const double p99 = pipe_ ? static_cast<double>(pipe_->present_p99_ns.load()) / 1.0e6 : 0;
  ImGui::Text("capture-to-present-call  p50 %.2f ms   p99 %.2f ms", p50, p99);
  ImGui::Text("present %.1f fps   frame %llu", fps_, static_cast<unsigned long long>(last_frame_));
  if (pipe_)
    if (const MatchInfo* m = pipe_->match_ui().try_read()) match_ = *m;
  if (pipe_) {
    if (pipe_->replay_over_budget.load(std::memory_order_relaxed))
      ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "mini replay paused: over the frame budget");
    else if (pipe_->config().load().mini_replay)
      ImGui::Text("mini replay %.2f ms/frame", static_cast<double>(pipe_->thumb_ns.load()) / 1.0e6);
  }
  if (last_play_dead_) {
    char why[64] = "";
    for (int k = 0; k < kPlaySignals; ++k)
      if (last_play_reasons_ & (1u << k)) {
        std::strncat(why, " ", sizeof(why) - std::strlen(why) - 1);
        std::strncat(why, kPlaySignalName[k], sizeof(why) - std::strlen(why) - 1);
      }
    ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "dead ball:%s", why);
  }
  {
    const char* scene = last_scene_ == 1 ? "wide" : last_scene_ == 2 ? "close" : "other";
    if (last_pitch_valid_)
      ImGui::Text("pitch %s   %s fit error %.2f m   age %u", scene, last_pitch_manual_ ? "manual" : "kp",
                  last_reproj_m_, static_cast<unsigned>(last_pitch_age_));
    else
      ImGui::Text("pitch %s   not calibrated", scene);
  }
  if (last_pitch_valid_) {
    if (last_pc_valid_) ImGui::Text("territory A %.0f%%", last_territory_ * 100.f);
    if (last_poss_team_ != 0)
      ImGui::Text("possession %s #%u", last_poss_team_ == 1 ? "A" : "B", last_poss_track_);
    else if (last_ball_ok_) ImGui::TextUnformatted("ball seen, no holder");
  }
  if (pipe_) {
    const auto drops = pipe_->drops.load() + (source_ ? source_->capture_drops() : 0);
    ImGui::Text("drops %llu   deduped %llu   late %llu   free slots %d",
                static_cast<unsigned long long>(drops),
                static_cast<unsigned long long>(pipe_->dedupes.load()),
                static_cast<unsigned long long>(pipe_->late_frames.load()), pool_->free_approx());
  }
  if (gpu_.warp) ImGui::TextUnformatted("WARP software device");
  hud_h_ = ImGui::GetWindowSize().y;
  if (last_flags_ & kFlagBlack) ImGui::TextUnformatted("BLACK FRAME (DRM or an empty source)");
  if (overlay) {
    ImGui::TextUnformatted(affinity_ok_ ? "excluded from capture" : "display affinity failed");
    ImGui::TextUnformatted("click-through overlay");
  }
  ImGui::End();
  if (have_frame_) {
    draw_stats_panel();
    draw_card();
    draw_replay();
  }
  imgui_end(imgui_target);

  if (overlay && view_rtv_ && offscreen_srv_) {
    gpu_.context->OMSetRenderTargets(1, view_rtv_.GetAddressOf(), nullptr);
    gpu_.context->ClearRenderTargetView(view_rtv_.Get(), clear);
    D3D11_VIEWPORT full{0, 0, cw, ch, 0, 1};
    draw_fullscreen(gpu_.context.Get(), premul_pass_, offscreen_srv_.Get(), full);
  }
  view_swap_->Present(1, 0);
}

namespace {
constexpr ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
                                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings;

ImU32 team_color(const MatchInfo& m, int t) {
  if (m.teams_ok && m.team[t].has_color) {
    const std::uint32_t c = m.team[t].color;
    return IM_COL32((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, 255);
  }
  return t == 0 ? IM_COL32(40, 140, 255, 255) : IM_COL32(255, 70, 70, 255);
}
}  // namespace

// M14: each panel has a home side and flips to the mirror when the ball, the
// on-ball player or the scorebug sits on it. Top: stats (home right) with the
// mini replay under it. Bottom: minimap (home left), on-ball card (home right,
// stacked above the minimap when both share a side).
void App::layout_panels(float ox, float oy, float scale, float cw, float ch) {
  const Config cfg = pipe_ ? pipe_->config().load() : Config{};
  const TimeNs now = now_ns();
  const float dt = flip_t_ ? static_cast<float>(now - flip_t_) * 1e-9f : 0.f;
  flip_t_ = now;
  const float m = 16.f;
  const float mw = std::clamp(cw * 0.22f, 160.f, 420.f);
  const float mh = (mw - 16.f) * 68.f / 105.f + 16.f;
  replay_sz_ = ImVec2(std::clamp(cw * 0.2f, 240.f, 480.f), 0.f);
  replay_sz_.y = replay_sz_.x * 9.f / 16.f;
  const bool replay_on = replay_clip_ >= 0;
  const ImVec2 top(std::max(stats_sz_.x, replay_on ? replay_sz_.x + 16.f : 0.f),
                   stats_sz_.y + (replay_on ? replay_sz_.y + 40.f : 0.f));

  PanelRect obs[3];
  int no = 0;
  auto add = [&](float x, float y, float w, float h) {
    obs[no++] = {ox + x * scale, oy + y * scale, ox + (x + w) * scale, oy + (y + h) * scale};
  };
  int ball = -1;
  for (std::uint32_t i = 0; i < last_det_count_; ++i)
    if (last_dets_[i].class_id == 3 && (ball < 0 || last_dets_[i].score > last_dets_[ball].score))
      ball = static_cast<int>(i);
  if (ball >= 0) add(last_dets_[ball].x, last_dets_[ball].y, last_dets_[ball].w, last_dets_[ball].h);
  for (std::uint32_t i = 0; i < last_det_count_ && last_poss_track_; ++i)
    if (last_dets_[i].track_id == last_poss_track_) {
      add(last_dets_[i].x, last_dets_[i].y, last_dets_[i].w, last_dets_[i].h);
      break;
    }
  if (last_bug_roi_[2] > 0) add(last_bug_roi_[0], last_bug_roi_[1], last_bug_roi_[2], last_bug_roi_[3]);
  auto hit = [&](const PanelRect& r) {
    for (int k = 0; k < no; ++k)
      if (r.hits(obs[k], cfg.flip_margin_px)) return true;
    return false;
  };

  // left: 1 places the panel on the left edge.
  auto top_rect = [&](int left) {
    const float x = left ? m : cw - m - top.x;
    const float y = left ? 8.f + hud_h_ + 8.f : m;
    return PanelRect{x, y, x + top.x, y + top.y};
  };
  auto map_rect = [&](int left) {
    const float x = left ? m : cw - m - mw;
    return PanelRect{x, ch - m - mh, x + mw, ch - m};
  };
  const int map_left = flip_map_.side() == 0 ? 1 : 0;
  auto card_rect = [&](int left) {
    const float x = left ? m : cw - m - card_sz_.x;
    const float y1 = ch - m - (left == map_left ? mh + 8.f : 0.f);
    return PanelRect{x, y1 - card_sz_.y, x + card_sz_.x, y1};
  };

  // home_left: the panel's home is the left edge. Side 0 = home.
  auto step = [&](PanelFlip& f, int home_left, auto rect, bool active) {
    if (cfg.panel_side == static_cast<std::uint8_t>(PanelSide::Left)) return f.force(home_left ? 0 : 1);
    if (cfg.panel_side == static_cast<std::uint8_t>(PanelSide::Right)) return f.force(home_left ? 1 : 0);
    const bool h[2] = {active && hit(rect(home_left)), active && hit(rect(1 - home_left))};
    f.update(h, now / 1000000, cfg.flip_hold_ms, cfg.flip_react_ms, dt);
  };
  step(flip_stats_, 0, top_rect, true);
  step(flip_map_, 1, map_rect, true);
  step(flip_card_, 0, card_rect, last_poss_track_ != 0);

  auto place = [](const PanelFlip& f, int home_left, auto rect) {
    const PanelRect a = rect(home_left), b = rect(1 - home_left);
    const float t = f.pos();
    return ImVec2(a.x0 + (b.x0 - a.x0) * t, a.y0 + (b.y0 - a.y0) * t);
  };
  stats_pos_ = place(flip_stats_, 0, top_rect);
  replay_pos_ = ImVec2(stats_pos_.x, stats_pos_.y + stats_sz_.y + 6.f);
  map_pos_ = place(flip_map_, 1, map_rect);
  card_pos_ = place(flip_card_, 0, card_rect);
}

void App::draw_stats_panel() {
  const TeamStats& ts = last_team_stats_;
  ImGui::SetNextWindowPos(stats_pos_, ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.6f);
  ImGui::Begin("Stats", nullptr, kPanelFlags);
  const char* code[2] = {match_.teams_ok ? match_.team[0].code : "A", match_.teams_ok ? match_.team[1].code : "B"};
  if (match_.teams_ok) {
    ImGui::Text("%s %d - %d %s", code[0], std::max<int>(0, match_.team[0].score), std::max<int>(0, match_.team[1].score),
                code[1]);
    if (match_.clock_ms >= 0) {
      ImGui::SameLine();
      ImGui::TextDisabled("  %d'", match_.clock_ms / 60000 + 1);
    }
  }
  ImGui::TextDisabled("vision estimate");
  if (ImGui::BeginTable("stats", 3, ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    for (int t = 0; t < 2; ++t) {
      ImGui::TableNextColumn();
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(team_color(match_, t)), "%s", code[t]);
    }
    auto row = [&](const char* name, auto fmt) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(name);
      for (int t = 0; t < 2; ++t) {
        ImGui::TableNextColumn();
        fmt(t);
      }
    };
    row("possession", [&](int t) { ImGui::Text("%.0f%%", ts.poss_pct(t)); });
    row("passes", [&](int t) { ImGui::Text("%u/%u", ts.passes_ok[t], ts.passes[t]); });
    row("completion", [&](int t) { ImGui::Text("%.0f%%", ts.pass_pct(t)); });
    row("shots", [&](int t) { ImGui::Text("%u", ts.shots[t]); });
    row("key passes", [&](int t) { ImGui::Text("%u", ts.key_passes[t]); });
    row("xG", [&](int t) { ImGui::Text("%.2f", static_cast<double>(ts.xg[t])); });
    ImGui::EndTable();
  }
  // Team ratings, 0..10 (5 level).
  const TeamRatings tr[2] = {team_ratings(ts, 0), team_ratings(ts, 1)};
  ImGui::Separator();
  if (ImGui::BeginTable("ratings", 3, ImGuiTableFlags_SizingFixedFit)) {
    auto rate = [&](const char* name, float TeamRatings::*m) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(name);
      for (int t = 0; t < 2; ++t) {
        ImGui::TableNextColumn();
        const float v = tr[t].*m;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = 40.f, h = ImGui::GetTextLineHeight();
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 30));
        dl->AddRectFilled(p, ImVec2(p.x + w * std::clamp(v / 10.f, 0.f, 1.f), p.y + h), team_color(match_, t));
        ImGui::Dummy(ImVec2(w, h));
        ImGui::SameLine();
        ImGui::Text("%.1f", static_cast<double>(v));
      }
    };
    rate("attack", &TeamRatings::attack);
    rate("build-up", &TeamRatings::build);
    rate("defence", &TeamRatings::defence);
    rate("control", &TeamRatings::control);
    rate("overall", &TeamRatings::overall);
    ImGui::EndTable();
  }
  // Before kick-off and in a long break (half time): form and absentees.
  const bool idle = ts.poss_s[0] + ts.poss_s[1] < 1.f || (last_play_dead_ && now_ns() - dead_since_ > 30000000000LL);
  if (idle && match_.teams_ok) {
    ImGui::Separator();
    for (int t = 0; t < 2; ++t) {
      const TeamInfo& team = match_.team[t];
      char out[96] = "";
      int n = 0;
      for (int i = 0; i < team.n && n < 3; ++i) {
        if (!team.squad[i].out) continue;
        std::strncat(out, n ? ", " : "", sizeof(out) - std::strlen(out) - 1);
        std::strncat(out, team.squad[i].name, sizeof(out) - std::strlen(out) - 1);
        ++n;
      }
      ImGui::Text("%s  form %s", code[t], team.form[0] ? team.form : "-");
      if (n) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.45f, 1.f), "   out: %s", out);
    }
  }
  stats_sz_ = ImGui::GetWindowSize();
  ImGui::End();
}

void App::draw_card() {
  if (last_poss_track_ == 0 || (last_poss_team_ != 1 && last_poss_team_ != 2)) return;
  const int t = last_poss_team_ - 1;
  ImGui::SetNextWindowPos(card_pos_, ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.6f);
  ImGui::Begin("On ball", nullptr, kPanelFlags);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float sz = ImGui::GetTextLineHeight();
  ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), team_color(match_, t), 2.f);
  ImGui::Dummy(ImVec2(sz, sz));
  ImGui::SameLine();
  const TrackLabel* lb = label_of(last_poss_track_);
  const PlayerInfo* pl = lb && lb->team == last_poss_team_ ? &match_.team[t].squad[lb->idx] : nullptr;
  if (pl)
    ImGui::Text("%u %s", pl->jersey, pl->name);
  else
    ImGui::Text("#%u %s", last_poss_track_, match_.teams_ok ? match_.team[t].code : (t == 0 ? "A" : "B"));
  ImGui::Text("on the ball %.1f s", last_poss_spell_s_);
  if (pl) {
    ImGui::Text("passes %u/%u   shots %u", last_holder_passes_ok_, last_holder_passes_, last_holder_shots_);
    ImGui::TextDisabled("season: %u apps  %u G  %u A", pl->apps, pl->goals, pl->assists);
  }
  card_sz_ = ImGui::GetWindowSize();
  ImGui::End();
}

// M15: play the oldest ready clip at half speed, twice, then fade. A newer
// clip takes over once the current one has played through once.
void App::draw_replay() {
  if (!pipe_) return;
  constexpr float kFadeS = 0.5f;
  const bool on = pipe_->config().load().mini_replay != 0;
  const TimeNs now = now_ns();
  int next = -1;
  for (int i = 0; i < 2; ++i)
    if (i != replay_clip_ && pipe_->clip(i).state.load(std::memory_order_acquire) == ClipState::Ready &&
        (next < 0 || pipe_->clip(i).seq < pipe_->clip(next).seq))
      next = i;
  auto length = [&](const ClipBuffer& c) {
    return std::max(0.5f, 2.f * (c.meta[c.n - 1].t_s - c.meta[0].t_s));
  };
  if (replay_clip_ >= 0) {
    ClipBuffer& c = pipe_->clip(replay_clip_);
    const float loops = static_cast<float>(now - replay_t0_) * 1e-9f / length(c);
    if (!on || (next >= 0 && loops >= 1.f) || loops * length(c) >= 2.f * length(c) + kFadeS) {
      c.state.store(ClipState::Free, std::memory_order_release);
      replay_clip_ = -1;
    }
  }
  if (on && replay_clip_ < 0 && next >= 0) {
    ClipState s = ClipState::Ready;
    if (pipe_->clip(next).state.compare_exchange_strong(s, ClipState::Playing)) {
      replay_clip_ = next;
      replay_t0_ = now;
      replay_frame_ = -1;
    }
  }
  if (replay_clip_ < 0) return;
  const ClipBuffer& c = pipe_->clip(replay_clip_);
  const float len = length(c);
  const float el = static_cast<float>(now - replay_t0_) * 1e-9f;
  const float alpha = el < 2.f * len ? 1.f : std::max(0.f, 1.f - (el - 2.f * len) / kFadeS);
  const float t = c.meta[0].t_s + (el < 2.f * len ? std::fmod(el, len) : len) * 0.5f;
  int k = 0;
  while (k + 1 < c.n && c.meta[k + 1].t_s <= t) ++k;

  if (!replay_tex_) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kThumbW;
    desc.Height = kThumbH;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(gpu_.device->CreateTexture2D(&desc, nullptr, replay_tex_.GetAddressOf()))) return;
    gpu_.device->CreateShaderResourceView(replay_tex_.Get(), nullptr, replay_srv_.GetAddressOf());
  }
  if (k != replay_frame_) {
    gpu_.context->UpdateSubresource(replay_tex_.Get(), 0, nullptr, c.frame(k), kThumbW * 4, 0);
    replay_frame_ = k;
  }

  ImGui::SetNextWindowPos(replay_pos_, ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.6f * alpha);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
  ImGui::Begin("Mini replay", nullptr, kPanelFlags);
  const char* what = c.req.kind == 1 ? "shot" : c.req.kind == 0 ? "key pass" : "replay";
  ImGui::Text("%s   x0.5", what);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(replay_srv_.Get())), replay_sz_,
               ImVec2(0, 0), ImVec2(1, 1), ImVec4(1, 1, 1, alpha), ImVec4(0, 0, 0, 0));
  const ThumbMeta& fm = c.meta[k];
  Homography inv;
  if (c.req.kind != 2 && fm.pitch_valid && fm.src_w && invert3(fm.h, inv.h)) {
    const float sx = replay_sz_.x / fm.src_w, sy = replay_sz_.y / fm.src_h;
    float ax, ay, bx, by;
    if (apply_homography(inv, c.req.sx, c.req.sy, ax, ay) && apply_homography(inv, c.req.ex, c.req.ey, bx, by)) {
      auto* dl = ImGui::GetWindowDrawList();
      dl->PushClipRect(p, ImVec2(p.x + replay_sz_.x, p.y + replay_sz_.y), true);
      const ImU32 col = (team_color(match_, c.req.team == 2 ? 1 : 0) & 0x00FFFFFFu) |
                        (static_cast<ImU32>(alpha * 230.f) << 24);
      const ImVec2 a(p.x + ax * sx, p.y + ay * sy), b(p.x + bx * sx, p.y + by * sy);
      dl->AddLine(a, b, col, 2.5f);
      dl->AddCircleFilled(b, 4.f, col);
      dl->PopClipRect();
    }
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

void App::refresh_windows() {
  const TimeNs now = now_ns();
  if (now - window_refresh_ < 1000000000 && !windows_.empty()) return;
  window_refresh_ = now;
  windows_.clear();
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        if (!IsWindowVisible(hwnd)) return TRUE;
        wchar_t title[256]{};
        if (GetWindowTextW(hwnd, title, 256) <= 0) return TRUE;
        wchar_t klass[64]{};
        GetClassNameW(hwnd, klass, 64);
        if (wcscmp(klass, L"TouchFCWindow") == 0) return TRUE;
        char utf8[512]{};
        WideCharToMultiByte(CP_UTF8, 0, title, -1, utf8, sizeof(utf8), nullptr, nullptr);
        reinterpret_cast<std::vector<ListedWindow>*>(lp)->push_back(ListedWindow{hwnd, utf8});
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&windows_));
}

std::unique_ptr<Detector> App::make_detector() {
  if (!args_.model_path.empty()) {
    std::string error;
    auto det = make_ort_detector(args_.model_path, error);
    if (det) return det;
    log_line("ORT detector unavailable (" + error + "); using stub");
  }
  return make_stub_detector();
}

std::unique_ptr<PitchModel> App::make_pitch() {
  if (args_.keypoints_path.empty()) return nullptr;
  std::string error;
  auto model = make_pitch_model(args_.keypoints_path, error);
  if (!model) log_line("pitch model unavailable (" + error + "); template homography");
  return model;
}

bool App::start_overlay_capture() {
  if (!selected_ || !IsWindow(selected_)) {
    status_ = "pick a window first";
    return false;
  }
  std::string error;
  source_ = make_wgc_source(selected_, error);
  if (!source_) {
    status_ = error;
    log_line(error);
    return false;
  }
  pool_ = std::make_unique<FramePool>(source_->width(), source_->height());
  sink_ = std::make_unique<HdrSink>();
  detector_ = make_detector();
  if (!detector_) return false;
  services_.reset();
  pipe_.reset();
  pitch_ = make_pitch();
  pipe_ = std::make_unique<Pipeline>(*pool_);
  pipe_->start(*source_, *sink_, *detector_, args_.pin, pitch_.get());
  start_services();
  load_last_profile();
  status_ = std::string("capturing: ") + detector_->label();
  ShowWindow(view_hwnd_, SW_SHOWNOACTIVATE);
  return true;
}

void App::track_target() {
  if (args_.mode != AppArgs::Mode::Overlay || !view_hwnd_ || !selected_ || !IsWindow(selected_)) return;
  RECT frame{};
  if (FAILED(DwmGetWindowAttribute(selected_, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
    GetWindowRect(selected_, &frame);
  }
  const int w = std::max(1L, frame.right - frame.left);
  const int h = std::max(1L, frame.bottom - frame.top);
  RECT now{};
  GetWindowRect(view_hwnd_, &now);
  auto gap = [](int a, int b) { return a > b ? a - b : b - a; };
  const int dw = gap(now.right - now.left, w);
  const int dh = gap(now.bottom - now.top, h);
  // A one-pixel mismatch would resize the swap chain every frame.
  if (gap(now.left, frame.left) <= 4 && gap(now.top, frame.top) <= 4 && dw <= 4 && dh <= 4) return;
  SetWindowPos(view_hwnd_, HWND_TOPMOST, frame.left, frame.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  if (dw > 4 || dh > 4) view_dirty_ = true;
}

// The live value each threshold is compared against, so a slider can be moved
// against a number on screen. False when the field has none.
bool App::live_value(const char* key, char* buf, int n) const {
  auto is = [&](const char* k) { return std::strcmp(key, k) == 0; };
  if (is("h_max_rms_m")) return std::snprintf(buf, n, "now %.2f", last_reproj_m_) > 0;
  if (is("h_holdover")) return std::snprintf(buf, n, "age %u", static_cast<unsigned>(last_pitch_age_)) > 0;
  if (is("gmc_max_cost"))
    return std::snprintf(buf, n, "now %.1f%s", last_gmc_cost_, last_gmc_ok_ ? "" : " lost") > 0;
  if (is("line_min_support")) return std::snprintf(buf, n, "now %.2f", last_line_support_) > 0;
  if (is("green_margin") || is("green_min") || is("scene_wide_frac") || is("scene_close_frac"))
    return std::snprintf(buf, n, "grass %.2f %s", last_green_frac_,
                         last_scene_ == 1 ? "wide" : last_scene_ == 2 ? "close" : "other") > 0;
  if (is("view_angle_max_deg")) return std::snprintf(buf, n, "now %.0f", last_view_angle_) > 0;
  if (is("player_height_max_frac")) return std::snprintf(buf, n, "now %.2f", last_height_frac_) > 0;
  if (is("dead_enter_ms") || is("live_enter_ms"))
    return std::snprintf(buf, n, "%s", last_play_dead_ ? "dead" : "in play") > 0;
  if (is("conf_player")) return std::snprintf(buf, n, "%u boxes", last_det_count_) > 0;
  return false;
}

namespace {
bool profile_name_ok(const char* s) {
  if (!s[0]) return false;
  for (const char* p = s; *p; ++p)
    if (!std::isalnum(static_cast<unsigned char>(*p)) && *p != '_' && *p != '-') return false;
  return true;
}
}  // namespace

bool App::load_profile(const char* name) {
  if (!pipe_ || !profile_name_ok(name)) return false;
  std::ifstream in(std::string("profiles/") + name + ".json");
  if (!in) return false;
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  Config cfg;
  if (!config_from_json(text, cfg)) return false;
  cfg.version = pipe_->config().load().version + 1;
  pipe_->config().store(cfg);
  std::ofstream("profiles/last.txt") << name;
  return true;
}

void App::load_last_profile() {
  std::ifstream in("profiles/last.txt");
  std::string name;
  if (in >> name && name.size() < sizeof(profile_name_)) {
    std::snprintf(profile_name_, sizeof(profile_name_), "%s", name.c_str());
    profile_msg_ = load_profile(profile_name_) ? "loaded " + name : "could not load " + name;
  }
  if (args_.mini_replay >= 0 && pipe_) {
    Config c = pipe_->config().load();
    c.mini_replay = args_.mini_replay ? 1 : 0;
    pipe_->config().store(c);
  }
}

// Tuning tab: every Config field by group, with its live value and a reset.
void App::draw_tuning(Config& cfg) {
  ImGui::SetNextItemWidth(160.f);
  ImGui::InputText("##profile", profile_name_, sizeof(profile_name_));
  ImGui::SameLine();
  if (ImGui::Button("save profile")) {
    if (!profile_name_ok(profile_name_)) {
      profile_msg_ = "name: letters, digits, _ and - only";
    } else {
      std::error_code ec;
      std::filesystem::create_directories("profiles", ec);
      std::ofstream(std::string("profiles/") + profile_name_ + ".json") << config_to_json(cfg);
      std::ofstream("profiles/last.txt") << profile_name_;
      profile_msg_ = std::string("saved ") + profile_name_;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("load profile")) {
    profile_msg_ = load_profile(profile_name_) ? std::string("loaded ") + profile_name_ : "no such profile";
    cfg = pipe_->config().load();
  }
  if (!profile_msg_.empty()) ImGui::TextDisabled("%s", profile_msg_.c_str());

  bool changed = false;
  for (int g = 0; g < static_cast<int>(CfgGroup::Hidden); ++g) {
    if (!ImGui::CollapsingHeader(kCfgGroupName[g])) continue;
    ImGui::PushID(g);
    if (ImGui::SmallButton("reset group")) {
      cfg_reset_group(cfg, static_cast<CfgGroup>(g));
      changed = true;
    }
    for (const CfgField& f : kConfigFields) {
      if (static_cast<int>(f.group) != g) continue;
      const double v = cfg_get(cfg, f);
      ImGui::SetNextItemWidth(220.f);
      if (f.type == CfgType::F32) {
        float x = static_cast<float>(v);
        if (ImGui::SliderFloat(f.label, &x, f.lo, f.hi)) {
          cfg_set(cfg, f, x);
          changed = true;
        }
      } else {
        int x = static_cast<int>(v);
        if (ImGui::SliderInt(f.label, &x, static_cast<int>(f.lo), static_cast<int>(f.hi))) {
          cfg_set(cfg, f, x);
          changed = true;
        }
      }
      char live[48];
      if (live_value(f.key, live, sizeof(live))) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", live);
      }
    }
    ImGui::PopID();
  }
  if (changed) {
    ++cfg.version;
    pipe_->config().store(cfg);
  }
}

namespace {
constexpr const char* kLeagues[] = {"eng.1", "esp.1", "uefa.champions", "ger.1", "ita.1", "fra.1", "usa.1"};

bool contains_ci(const std::string& hay, const char* needle) {
  if (!needle[0]) return true;
  const std::size_t n = std::strlen(needle);
  for (std::size_t i = 0; i + n <= hay.size(); ++i) {
    std::size_t k = 0;
    while (k < n && std::tolower(static_cast<unsigned char>(hay[i + k])) ==
                        std::tolower(static_cast<unsigned char>(needle[k])))
      ++k;
    if (k == n) return true;
  }
  return false;
}
}  // namespace

// Match tab: manual setup (always wins over the scorebug), the scorebug box,
// what the provider gave, and manual substitutions.
void App::draw_match() {
  if (!services_ || !pipe_) return;
  ImGui::TextWrapped("%s", match_.status);
  ImGui::SetNextItemWidth(160.f);
  if (ImGui::Combo("league", &league_idx_, kLeagues, IM_ARRAYSIZE(kLeagues))) {
    ServiceCmd c;
    c.kind = ServiceCmd::Kind::SetLeague;
    std::snprintf(c.league, sizeof(c.league), "%s", kLeagues[league_idx_]);
    std::snprintf(c.date, sizeof(c.date), "%s", date_buf_);
    services_->post(c);
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90.f);
  ImGui::InputText("date YYYYMMDD", date_buf_, sizeof(date_buf_), ImGuiInputTextFlags_CharsDecimal);
  const auto teams = services_->teams();
  for (int s = 0; s < 2; ++s) {
    ImGui::PushID(s);
    ImGui::SetNextItemWidth(110.f);
    ImGui::InputTextWithHint("##f", "search", team_filter_[s], sizeof(team_filter_[s]));
    ImGui::SameLine();
    const char* cur = "pick";
    for (const auto& t : teams)
      if (t.id == pick_id_[s]) cur = t.name.c_str();
    ImGui::SetNextItemWidth(200.f);
    if (ImGui::BeginCombo(s == 0 ? "home" : "away", cur)) {
      for (const auto& t : teams) {
        if (!contains_ci(t.name, team_filter_[s]) && !contains_ci(t.code, team_filter_[s])) continue;
        if (ImGui::Selectable((t.code + "  " + t.name).c_str(), t.id == pick_id_[s])) pick_id_[s] = t.id;
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }
  if (ImGui::Button("confirm teams") && pick_id_[0] && pick_id_[1]) {
    ServiceCmd c;
    c.kind = ServiceCmd::Kind::SetTeams;
    std::snprintf(c.date, sizeof(c.date), "%s", date_buf_);
    c.home = pick_id_[0];
    c.away = pick_id_[1];
    services_->post(c);
  }
  ImGui::SameLine();
  if (ImGui::Button("refresh lineup")) {
    ServiceCmd c;
    c.kind = ServiceCmd::Kind::Refresh;
    services_->post(c);
  }
  ImGui::SameLine();
  if (ImGui::Button("clear")) {
    ServiceCmd c;
    c.kind = ServiceCmd::Kind::ClearTeams;
    services_->post(c);
  }

  ImGui::Separator();
  if (last_bug_roi_[2] > 0)
    ImGui::Text("scorebug at %d,%d %dx%d%s", last_bug_roi_[0], last_bug_roi_[1], last_bug_roi_[2], last_bug_roi_[3],
                roi_manual_ ? " (manual)" : "");
  else
    ImGui::TextUnformatted("scorebug: searching wide frames");
  ImGui::Checkbox("draw scorebug box on the frame", &roi_drag_);
  ImGui::SameLine();
  if (ImGui::Button("auto")) {
    UiCommand cmd;
    cmd.type = CmdType::SetScorebugRoi;
    if (!pipe_->commands().try_push(cmd)) pipe_->command_warns.fetch_add(1, std::memory_order_relaxed);
    roi_manual_ = false;
  }
  if (roi_drag_ && have_frame_ && video_srv_ && last_w_ > 0) {
    const float tw = ImGui::GetContentRegionAvail().x;
    const float th = tw * static_cast<float>(last_h_) / static_cast<float>(last_w_);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::Image(reinterpret_cast<ImTextureID>(video_srv_.Get()), ImVec2(tw, th));
    const float k = static_cast<float>(last_w_) / tw;
    if (ImGui::IsItemClicked()) {
      drag_a_ = ImGui::GetMousePos();
      dragging_ = true;
    }
    auto* wdl = ImGui::GetWindowDrawList();
    if (dragging_) {
      const ImVec2 b = ImGui::GetMousePos();
      wdl->AddRect(drag_a_, b, IM_COL32(255, 200, 0, 255), 0.f, 0, 2.f);
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        dragging_ = false;
        UiCommand cmd;
        cmd.type = CmdType::SetScorebugRoi;
        cmd.f[0] = (std::min(drag_a_.x, b.x) - o.x) * k;
        cmd.f[1] = (std::min(drag_a_.y, b.y) - o.y) * k;
        cmd.f[2] = std::fabs(b.x - drag_a_.x) * k;
        cmd.f[3] = std::fabs(b.y - drag_a_.y) * k;
        if (cmd.f[2] > 8.f && cmd.f[3] > 4.f && pipe_->commands().try_push(cmd)) roi_manual_ = true;
      }
    } else if (last_bug_roi_[2] > 0) {
      wdl->AddRect(ImVec2(o.x + last_bug_roi_[0] / k, o.y + last_bug_roi_[1] / k),
                   ImVec2(o.x + (last_bug_roi_[0] + last_bug_roi_[2]) / k, o.y + (last_bug_roi_[1] + last_bug_roi_[3]) / k),
                   IM_COL32(255, 200, 0, 255), 0.f, 0, 2.f);
    }
  }

  ImGui::Separator();
  if (!match_.teams_ok) return;
  const TeamInfo& h = match_.team[0];
  const TeamInfo& a = match_.team[1];
  char clock[16] = "";
  if (match_.clock_ms >= 0) std::snprintf(clock, sizeof(clock), "  %d:%02d", match_.clock_ms / 60000, (match_.clock_ms / 1000) % 60);
  ImGui::Text("%s %d - %d %s%s", h.code, h.score < 0 ? 0 : h.score, a.score < 0 ? 0 : a.score, a.code, clock);
  for (int s = 0; s < 2; ++s) {
    const TeamInfo& t = match_.team[s];
    ImGui::PushID(100 + s);
    if (!ImGui::CollapsingHeader(t.name[0] ? t.name : "team", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::PopID();
      continue;
    }
    ImGui::Text("form %s   formation %s   %s", t.form[0] ? t.form : "-", t.formation[0] ? t.formation : "-",
                match_.lineup_ok ? "lineup" : "squad (lineup not out)");
    if (ImGui::BeginTable("squad", 6, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
      for (const char* c : {"#", "name", "pos", "apps", "G", "A"}) ImGui::TableSetupColumn(c);
      ImGui::TableHeadersRow();
      for (int i = 0; i < t.n; ++i) {
        const PlayerInfo& p = t.squad[i];
        if (match_.lineup_ok && !p.starter && p.subbed != 2) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%u", p.jersey);
        ImGui::TableNextColumn();
        const ImVec4 col = p.out ? ImVec4(1.f, 0.4f, 0.4f, 1.f) : p.subbed == 1 ? ImVec4(0.6f, 0.6f, 0.6f, 1.f)
                                                                                : ImVec4(1.f, 1.f, 1.f, 1.f);
        ImGui::TextColored(col, "%s%s%s", p.name, p.out ? " (out)" : "", p.subbed == 2 ? " (on)" : "");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p.pos);
        ImGui::TableNextColumn();
        ImGui::Text("%u", p.apps);
        ImGui::TableNextColumn();
        ImGui::Text("%u", p.goals);
        ImGui::TableNextColumn();
        ImGui::Text("%u", p.assists);
      }
      ImGui::EndTable();
    }
    if (match_.lineup_ok) {
      // Substitution: someone on the pitch off, a bench player on.
      auto pick = [&](const char* label, int& idx, bool on_pitch) {
        const char* cur = idx >= 0 && idx < t.n ? t.squad[idx].name : "-";
        ImGui::SetNextItemWidth(150.f);
        if (ImGui::BeginCombo(label, cur)) {
          for (int i = 0; i < t.n; ++i) {
            const PlayerInfo& p = t.squad[i];
            const bool playing = (p.starter && p.subbed != 1) || p.subbed == 2;
            if (playing != on_pitch) continue;
            if (ImGui::Selectable(p.name, idx == i)) idx = i;
          }
          ImGui::EndCombo();
        }
      };
      pick("off", sub_off_[s], true);
      ImGui::SameLine();
      pick("on", sub_on_[s], false);
      ImGui::SameLine();
      if (ImGui::Button("sub") && sub_off_[s] >= 0 && sub_on_[s] >= 0) {
        ServiceCmd c;
        c.kind = ServiceCmd::Kind::Sub;
        c.side = static_cast<std::uint8_t>(s);
        c.off = static_cast<std::uint8_t>(sub_off_[s]);
        c.on = static_cast<std::uint8_t>(sub_on_[s]);
        services_->post(c);
        sub_off_[s] = sub_on_[s] = -1;
      }
    }
    ImGui::PopID();
  }
  ImGui::SeparatorText("On-ball player");
  if (last_poss_track_ == 0 || (last_poss_team_ != 1 && last_poss_team_ != 2)) {
    ImGui::TextDisabled("nobody on the ball");
    return;
  }
  const TeamInfo& team = match_.team[last_poss_team_ - 1];
  const TrackLabel* cur = label_of(last_poss_track_);
  char preview[48] = "unlabelled";
  if (cur) std::snprintf(preview, sizeof(preview), "%u %s", team.squad[cur->idx].jersey, team.squad[cur->idx].name);
  ImGui::Text("#%u (%s)", last_poss_track_, team.code[0] ? team.code : (last_poss_team_ == 1 ? "A" : "B"));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(220.f);
  if (ImGui::BeginCombo("##label", preview)) {
    for (int i = 0; i < team.n; ++i) {
      char item[48];
      std::snprintf(item, sizeof(item), "%u %s", team.squad[i].jersey, team.squad[i].name);
      if (ImGui::Selectable(item, cur && cur->idx == i)) {
        TrackLabel* slot = nullptr;
        for (auto& l : labels_)
          if (l.track == last_poss_track_ || (!slot && l.track == 0)) slot = &l;
        if (!slot) slot = &labels_[last_poss_track_ % 32];
        *slot = {last_poss_track_, last_poss_team_, static_cast<std::uint8_t>(i)};
      }
    }
    ImGui::EndCombo();
  }
  if (cur) {
    ImGui::SameLine();
    if (ImGui::SmallButton("clear label"))
      for (auto& l : labels_)
        if (l.track == last_poss_track_) l = {};
  }
}

// M13 validation: our team stats beside the provider's post-match numbers.
void App::write_validation() {
  if (!pipe_ || !match_.event_id || match_.team[0].post.passes < 0) return;
  const TeamStats& ts = pipe_->team_stats();
  if (ts.passes[0] + ts.passes[1] == 0) return;  // nothing counted (selftest, no pitch fit)
  std::error_code ec;
  std::filesystem::create_directories("docs/bench", ec);
  const std::string path = "docs/bench/m13_validation_" + std::to_string(match_.event_id) + ".csv";
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return;
  std::fprintf(f, "team,metric,vision,provider\n");
  for (int s = 0; s < 2; ++s) {
    const TeamInfo& t = match_.team[s];
    std::fprintf(f, "%s,passes,%u,%d\n", t.code, ts.passes[s], t.post.passes);
    std::fprintf(f, "%s,pass_pct,%.1f,%.1f\n", t.code, ts.pass_pct(s),
                 t.post.passes > 0 ? 100.0 * t.post.passes_ok / t.post.passes : -1.0);
    std::fprintf(f, "%s,possession_pct,%.1f,%.1f\n", t.code, ts.poss_pct(s), t.post.possession_pct);
    std::fprintf(f, "%s,shots,%u,%d\n", t.code, ts.shots[s], t.post.shots);
  }
  std::fclose(f);
  log_line("validation written: " + path);
}

void App::start_services() {
  services_ = std::make_unique<Services>(*pipe_);
  services_->start();
  if (args_.match_spec.empty()) return;
  char league[16]{}, date[9]{};
  unsigned home = 0, away = 0;
  if (std::sscanf(args_.match_spec.c_str(), "%15[^:]:%8[0-9]:%u:%u", league, date, &home, &away) != 4) {
    log_line("--match: expected league:YYYYMMDD:home_id:away_id");
    return;
  }
  ServiceCmd c;
  c.kind = ServiceCmd::Kind::SetLeague;
  std::snprintf(c.league, sizeof(c.league), "%s", league);
  std::snprintf(c.date, sizeof(c.date), "%s", date);
  services_->post(c);
  c.kind = ServiceCmd::Kind::SetTeams;
  c.home = home;
  c.away = away;
  services_->post(c);
  std::snprintf(date_buf_, sizeof(date_buf_), "%s", date);
}

void App::draw_control() {
  if (control_dirty_ && control_swap_) {
    control_rtv_.Reset();
    control_swap_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    recreate_rtv(control_swap_.Get(), control_rtv_);
    control_dirty_ = false;
  }
  const float bg[4] = {0.1f, 0.1f, 0.12f, 1.f};
  gpu_.context->OMSetRenderTargets(1, control_rtv_.GetAddressOf(), nullptr);
  gpu_.context->ClearRenderTargetView(control_rtv_.Get(), bg);
  imgui_begin(control_binding_);
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
  ImGui::Begin("TouchFC", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
  ImGui::TextUnformatted("TouchFC");
  if (!status_.empty()) ImGui::TextWrapped("%s", status_.c_str());
  if (pipe_) {
    Config cfg = pipe_->config().load();
    bool main_tab = false;
    if (ImGui::BeginTabBar("tabs")) {
      if (ImGui::BeginTabItem("Main")) {
        main_tab = true;
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Match")) {
        draw_match();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Tuning")) {
        draw_tuning(cfg);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    if (main_tab) {
    if (ImGui::Button("attack direction")) {
      UiCommand cmd;
      cmd.type = CmdType::ApplyAttackDir;
      cmd.u8a = static_cast<std::uint8_t>(pipe_->attack_dir.load() > 0 ? 0 : 1);
      if (!pipe_->commands().try_push(cmd)) {
        pending_ = cmd;
        pending_cmd_ = true;
        pipe_->command_warns.fetch_add(1, std::memory_order_relaxed);
      }
    }
    ImGui::TextUnformatted("Click a track to set its team. Others follow that colour.");
    int shown = 0;
    for (std::uint32_t i = 0; i < last_det_count_ && shown < 12; ++i) {
      const auto id = last_dets_[i].track_id;
      if (id == 0) continue;
      bool dup = false;
      for (std::uint32_t k = 0; k < i; ++k)
        if (last_dets_[k].track_id == id) dup = true;
      if (dup) continue;
      ++shown;
      const char* tag = last_team_[i] == 1 ? "A" : last_team_[i] == 2 ? "B" : "-";
      ImGui::PushID(static_cast<int>(id));
      ImGui::Text("#%u  %s", static_cast<unsigned>(id), tag);
      ImGui::SameLine();
      auto send = [&](std::uint8_t team) {
        UiCommand cmd;
        cmd.type = CmdType::SetTeamSeed;
        cmd.u8a = team;
        cmd.u16a = id;
        if (!pipe_->commands().try_push(cmd)) {
          pending_ = cmd;
          pending_cmd_ = true;
          pipe_->command_warns.fetch_add(1, std::memory_order_relaxed);
        }
      };
      if (ImGui::SmallButton("A")) send(1);
      ImGui::SameLine();
      if (ImGui::SmallButton("B")) send(2);
      ImGui::PopID();
    }
    if (ImGui::CollapsingHeader("Pitch calibration", ImGuiTreeNodeFlags_DefaultOpen)) {
      auto push = [&](const UiCommand& cmd) {
        if (!pipe_->commands().try_push(cmd)) {
          pending_ = cmd;
          pending_cmd_ = true;
          pipe_->command_warns.fetch_add(1, std::memory_order_relaxed);
        }
      };
      if (ImGui::Button("mini replay: last 5 s")) {
        UiCommand c;
        c.type = CmdType::TestReplay;
        push(c);
      }
      ImGui::SameLine();
      ImGui::Checkbox("reprojection view", &debug_reproj_);
      ImGui::SameLine();
      ImGui::TextDisabled("magenta = H^-1 lines, yellow = raw keypoints");
      ImGui::Text("%s   last fit: %s",
                  last_pitch_valid_ ? (last_pitch_manual_ ? "manual" : "keypoints") : "not calibrated",
                  fit_reject_name(static_cast<FitReject>(last_pitch_reject_ < kFitRejectCount ? last_pitch_reject_ : 0)));
      ImGui::Text("fit error %.2f m   age %u   motion %.1f%s   lines %.2f", last_reproj_m_,
                  static_cast<unsigned>(last_pitch_age_), last_gmc_cost_, last_gmc_ok_ ? "" : " (lost)",
                  last_line_support_);
      ImGui::TextDisabled("Thresholds are in the Tuning tab.");

      ImGui::Separator();
      ImGui::TextWrapped("Manual: pick a landmark, click it on the frame. 4 or more fit. Pause first.");
      ImGui::Combo("landmark", &calib_pick_, kPitchLandmarkName, kPitchLandmarks);
      if (ImGui::Button("clear calibration")) {
        UiCommand cmd;
        cmd.type = CmdType::ResetHomography;
        push(cmd);
        for (bool& a : sent_anchor_) a = false;
      }
      if (have_frame_ && video_srv_ && last_w_ > 0) {
        const float tw = ImGui::GetContentRegionAvail().x;
        const float th = tw * static_cast<float>(last_h_) / static_cast<float>(last_w_);
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::Image(reinterpret_cast<ImTextureID>(video_srv_.Get()), ImVec2(tw, th));
        const float k = static_cast<float>(last_w_) / tw;
        if (ImGui::IsItemClicked()) {
          const ImVec2 m = ImGui::GetMousePos();
          UiCommand cmd;
          cmd.type = CmdType::SetAnchor;
          cmd.u8a = static_cast<std::uint8_t>(calib_pick_);
          cmd.f[0] = (m.x - o.x) * k;
          cmd.f[1] = (m.y - o.y) * k;
          push(cmd);
          sent_anchor_[calib_pick_] = true;
          sent_ax_[calib_pick_] = cmd.f[0];
          sent_ay_[calib_pick_] = cmd.f[1];
          calib_pick_ = (calib_pick_ + 1) % kPitchLandmarks;
        }
        auto* wdl = ImGui::GetWindowDrawList();
        for (int i = 0; i < kPitchLandmarks; ++i) {
          if (!sent_anchor_[i]) continue;
          const ImVec2 p(o.x + sent_ax_[i] / k, o.y + sent_ay_[i] / k);
          char id[4];
          std::snprintf(id, sizeof(id), "%d", i);
          wdl->AddCircle(p, 4.f, IM_COL32(0, 255, 255, 255), 0, 2.f);
          wdl->AddText(ImVec2(p.x + 5, p.y - 14), IM_COL32(0, 255, 255, 255), id);
        }
      }
    }
    ImGui::Text("dir %d   command warns %llu", pipe_->attack_dir.load(),
                static_cast<unsigned long long>(pipe_->command_warns.load()));
    if (pending_cmd_) ImGui::TextUnformatted("command queue full; will retry");
    const double p50 = static_cast<double>(pipe_->present_p50_ns.load()) / 1.0e6;
    const double p99 = static_cast<double>(pipe_->present_p99_ns.load()) / 1.0e6;
    ImGui::Text("capture-to-present-call p50 %.2f ms  p99 %.2f ms", p50, p99);
    ImGui::TextWrapped("Glass-to-photon latency needs PresentMon. See scripts/presentmon_check.ps1.");
    }
  }
  if (args_.mode == AppArgs::Mode::Overlay && !pipe_) {
    refresh_windows();
    if (ImGui::BeginListBox("windows", ImVec2(-1, 280))) {
      for (const auto& item : windows_) {
        const bool current = item.hwnd == selected_;
        const std::string label =
            item.title + "##" + std::to_string(reinterpret_cast<std::uintptr_t>(item.hwnd));
        if (ImGui::Selectable(label.c_str(), current)) selected_ = item.hwnd;
      }
      ImGui::EndListBox();
    }
    if (ImGui::Button("Start capture") && start_overlay_capture()) {
      status_ = "capturing " + std::to_string(reinterpret_cast<std::uintptr_t>(selected_));
    }
  }
  ImGui::End();
  imgui_end(control_rtv_.Get());
  const UINT sync = (view_hwnd_ && IsWindowVisible(view_hwnd_)) ? 0u : 1u;
  control_swap_->Present(sync, 0);
}

int App::run() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  opt_out_power_throttling();
  begin_present_mmcss();
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  if (!args_.pitch_dump_path.empty()) pitch_dump_ = std::fopen(args_.pitch_dump_path.c_str(), "w");
  if (!create_gpu(gpu_)) return 1;
  if (!compile_pass(gpu_.device.Get(), false, copy_pass_)) return 1;
  if (!compile_pass(gpu_.device.Get(), true, premul_pass_)) return 1;

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"TouchFCWindow";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  if (!RegisterClassExW(&wc)) return 1;

  control_binding_.control = true;
  if (!create_window(control_hwnd_, L"TouchFC control", WS_OVERLAPPEDWINDOW, 0, 440, 720, control_binding_))
    return 1;
  if (!create_hwnd_swap(control_hwnd_, control_swap_, false)) return 1;
  recreate_rtv(control_swap_.Get(), control_rtv_);
  control_binding_.imgui = ImGui::CreateContext();
  ImGui::SetCurrentContext(control_binding_.imgui);
  ImGui::GetIO().IniFilename = nullptr;
  ImGui_ImplWin32_Init(control_hwnd_);
  ImGui_ImplDX11_Init(gpu_.device.Get(), gpu_.context.Get());
  ShowWindow(control_hwnd_, SW_SHOW);

  if (args_.mode != AppArgs::Mode::Overlay) {
    std::string error;
    if (args_.mode == AppArgs::Mode::Replay) {
      source_ = make_replay_source(args_.replay_path, error);
    } else {
      source_ = make_selftest_source();
      if (source_ && !source_->open(error)) source_.reset();
    }
    if (!source_) {
      log_line(error.empty() ? "could not open the source" : error);
      status_ = error;
    } else {
      pool_ = std::make_unique<FramePool>(source_->width(), source_->height());
      sink_ = std::make_unique<HdrSink>();
      detector_ = make_detector();
      if (!detector_) {
        log_line("detector init failed; falling back to stub");
        detector_ = make_stub_detector();
      }
      pitch_ = make_pitch();
      pipe_ = std::make_unique<Pipeline>(*pool_);
      pipe_->start(*source_, *sink_, *detector_, args_.pin, pitch_.get());
      start_services();
      load_last_profile();
      if (!create_window(view_hwnd_, L"TouchFC", WS_OVERLAPPEDWINDOW, 0, 1280, 760, view_binding_)) return 1;
      if (!create_hwnd_swap(view_hwnd_, view_swap_, false)) return 1;
      recreate_rtv(view_swap_.Get(), view_rtv_);
      view_binding_.imgui = ImGui::CreateContext();
      ImGui::SetCurrentContext(view_binding_.imgui);
      ImGui::GetIO().IniFilename = nullptr;
      ImGui_ImplWin32_Init(view_hwnd_);
      ImGui_ImplDX11_Init(gpu_.device.Get(), gpu_.context.Get());
      ShowWindow(view_hwnd_, SW_SHOW);
    }
  } else {
    const DWORD ex = WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                     WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    if (!create_window(view_hwnd_, L"TouchFC overlay", WS_POPUP, ex, 640, 360, view_binding_)) return 1;
    if (!create_hwnd_swap(view_hwnd_, view_swap_, true)) {
      log_line("composition swap chain failed");
      return 1;
    }
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    gpu_.device.As(&dxgi);
    if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(dcomp_.GetAddressOf()))) ||
        FAILED(dcomp_->CreateTargetForHwnd(view_hwnd_, TRUE, dcomp_target_.GetAddressOf())) ||
        FAILED(dcomp_->CreateVisual(dcomp_visual_.GetAddressOf()))) {
      log_line("DirectComposition setup failed");
      return 1;
    }
    dcomp_visual_->SetContent(view_swap_.Get());
    dcomp_target_->SetRoot(dcomp_visual_.Get());
    dcomp_->Commit();
    recreate_rtv(view_swap_.Get(), view_rtv_);
    view_binding_.imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(view_binding_.imgui);
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplWin32_Init(view_hwnd_);
    ImGui_ImplDX11_Init(gpu_.device.Get(), gpu_.context.Get());
    affinity_ok_ = SetWindowDisplayAffinity(view_hwnd_, WDA_EXCLUDEFROMCAPTURE) == TRUE;
    if (!affinity_ok_) log_line("SetWindowDisplayAffinity failed");
    status_ = "pick a window, then Start capture";
  }

  const TimeNs t_end = args_.seconds > 0 ? now_ns() + static_cast<TimeNs>(args_.seconds * 1.0e9) : 0;
  bool quit = false;
  while (!quit) {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) quit = true;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (t_end != 0 && now_ns() >= t_end) quit = true;
    track_target();
    pump_pipeline();
    if (view_hwnd_ && view_swap_ && IsWindowVisible(view_hwnd_)) draw_view();
    draw_control();
  }

    if (held_ >= 0 && pipe_) {
      pipe_->release_presented(static_cast<std::uint32_t>(held_));
      held_ = -1;
    }
    if (services_) services_->stop();
    if (pipe_) pipe_->join();
    write_validation();
  if (sink_) sink_->write_csv(args_.telemetry_path);
  if (pitch_dump_) std::fclose(pitch_dump_);
  if (pipe_) pipe_->write_spadl("spadl.csv");
  if (source_) source_->close();
  services_.reset();
  pipe_.reset();
  detector_.reset();
  sink_.reset();
  pool_.reset();
  source_.reset();

  if (view_binding_.imgui) {
    ImGui::SetCurrentContext(view_binding_.imgui);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(view_binding_.imgui);
  }
  if (control_binding_.imgui) {
    ImGui::SetCurrentContext(control_binding_.imgui);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(control_binding_.imgui);
  }
  end_present_mmcss();
  return 0;
}

}  // namespace

int run_app(const AppArgs& args) {
  App app(args);
  g_app = &app;
  const int rc = app.run();
  g_app = nullptr;
  return rc;
}

}  // namespace touchfc
