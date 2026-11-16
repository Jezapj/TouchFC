#include "present.hpp"

#include "d3d_util.hpp"
#include "log.hpp"
#include "pipeline.hpp"
#include "services.hpp"
#include "shell_win.hpp"
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
  // Overlay shell: Alt+T, tray, clicks on the video while the menu is open.
  bool shell() const { return args_.mode == AppArgs::Mode::Overlay; }
  void toggle_menu() { set_menu(!menu_open_); }
  void on_close();  // live: hide the menu; before capture: quit
  void on_tray(LPARAM lp);
  void on_shell_cmd(UINT id);
  bool view_interactive() const { return view_interactive_; }
  void on_view_click(int x, int y, bool right);

 private:
  void set_menu(bool open);
  void set_view_interactive(bool on);
  void place_menu();
  void draw_menu();
  void draw_start_card();
  void draw_live_card();
  void stop_capture(const char* why);
  void toast(std::string text, double seconds = 4.0);
  void push_cmd(const UiCommand& cmd);
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
  void load_presets();
  void draw_detector_pick();
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
  // A panel that has to move fades out where it is and back in at the new
  // spot. Small shifts (a panel that grew) follow at once.
  struct PanelFade {
    ImVec2 pos{};
    float a = 1.f;
    bool init = false;
    ImVec2 step(ImVec2 target, float dt) {
      constexpr float kFadeS = 0.18f, kJumpPx = 90.f;
      if (!init) {
        pos = target;
        init = true;
      }
      if (std::fabs(target.x - pos.x) + std::fabs(target.y - pos.y) > kJumpPx) {
        a -= dt / kFadeS;
        if (a <= 0.f) {
          a = 0.f;
          pos = target;
        }
      } else {
        pos = target;
        a = std::min(1.f, a + dt / kFadeS);
      }
      return pos;
    }
  };
  PanelFade fade_stats_, fade_card_, fade_map_;
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
  // M18 match flow: pop-ups, goal banner, flames, dead-ball panels, replay log.
  void pump_flow();
  void draw_heat(float cw, float ch);
  void draw_news(float cw);
  void draw_dead_panel(float cw, float ch);
  void draw_lineup(float w);
  void draw_lines(float w);
  void draw_momentum(float w);
  void draw_passes(float w);
  void draw_layer_toggles(Config& cfg);
  void draw_replays_tab(Config& cfg);
  void demo_flow();
  TimeNs demo_t0_ = 0;
  int demo_step_ = -1;
  bool layer(std::uint32_t bit) const { return (layers_ & bit) != 0; }
  const char* code(int t) const { return match_.teams_ok ? match_.team[t].code : (t == 0 ? "A" : "B"); }
  ImU32 kit_color(int t) const;
  FlowSnapshot flow_{};
  std::uint32_t layers_ = 0xffffffffu;
  float popup_s_ = 3.f, goal_banner_s_ = 90.f, dead_panel_s_ = 3.f, line_high_m_ = 40.f;
  struct Popup {
    char text[64]{};
    std::uint8_t team = 0;
    FeedKind kind = FeedKind::Shot;
    TimeNs until = 0;
  };
  Popup popups_[3]{};
  std::uint32_t seen_goal_n_ = 0;
  int banner_goal_ = -1;
  TimeNs banner_t0_ = 0;
  float heat_ = 0.f;
  std::uint8_t heat_team_ = 0;
  TimeNs heat_t_ = 0;
  int dead_rot_ = 0, dead_panel_ = -1;
  TimeNs dead_panel_t0_ = 0;
  bool stop_log_ = false;
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
  std::vector<ShellWindow> windows_;
  HWND selected_ = nullptr;
  std::string last_window_title_;
  // Overlay shell state.
  bool menu_open_ = true;
  bool view_interactive_ = false;
  bool show_diag_ = true;  // the HUD; off in overlay mode
  bool calib_mode_ = false;
  bool raise_menu_ = false;
  bool hotkey_ok_ = false;
  bool tray_ok_ = false;
  bool quit_ = false;
  ImFont* title_font_ = nullptr;
  ImVec2 menu_sz_{460.f, 300.f};
  float dpi_ = 1.f;
  float view_ox_ = 0.f, view_oy_ = 0.f, view_scale_ = 1.f;
  std::string toast_;
  TimeNs toast_until_ = 0;
  std::uint8_t seen_team_source_ = 0;
  bool seen_dir_ = false;
  std::vector<std::string> profiles_;
  TimeNs window_refresh_ = 0;
  std::string status_;
  bool pending_cmd_ = false;
  UiCommand pending_{};

  std::unique_ptr<FrameSource> source_;
  std::unique_ptr<FramePool> pool_;
  std::unique_ptr<HdrSink> sink_;
  std::unique_ptr<Detector> detector_;
  std::vector<DetectorPreset> presets_;
  std::vector<char> preset_ok_;  // file present
  int preset_ = -1;              // -1: --model or the stub
  std::unique_ptr<PitchModel> pitch_;
  std::unique_ptr<Pipeline> pipe_;
  std::unique_ptr<Services> services_;
};

App* g_app = nullptr;

// Scales the alpha of everything a draw list has received since vertex `from`.
void fade_vtx(ImDrawList* dl, int from, float a) {
  if (a >= 1.f) return;
  a = std::max(0.f, a);
  for (int i = from; i < dl->VtxBuffer.Size; ++i) {
    ImU32& c = dl->VtxBuffer[i].col;
    const ImU32 al = static_cast<ImU32>(static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xff) * a);
    c = (c & ~IM_COL32_A_MASK) | (al << IM_COL32_A_SHIFT);
  }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto* binding = reinterpret_cast<Binding*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (binding && binding->imgui && msg != WM_CREATE) {
    ImGui::SetCurrentContext(binding->imgui);
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return 1;
  }
  App* app = binding ? binding->app : nullptr;
  switch (msg) {
    case WM_SIZE:
      if (app) {
        if (binding->control) app->on_control_resize();
        else app->on_view_resize();
      }
      return 0;
    case WM_HOTKEY:
      if (app) app->toggle_menu();
      return 0;
    case kTrayMsg:
      if (app) app->on_tray(lparam);
      return 0;
    case WM_COMMAND:
      if (app && lparam == 0) app->on_shell_cmd(LOWORD(wparam));
      return 0;
    case WM_CLOSE:
      // The overlay menu hides; quit is in the menu and the tray.
      if (app && app->shell()) {
        app->on_close();
        return 0;
      }
      break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
      if (app && !binding->control && app->view_interactive())
        app->on_view_click(static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam)),
                           msg == WM_RBUTTONDOWN);
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
  view_ox_ = ox;
  view_oy_ = oy;
  view_scale_ = scale;
  if (args_.demo_flow) demo_flow();

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
  if (show_diag_ && have_frame_ && last_draw_.box_w > 1.f) {
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
    // Boxes also show while the menu is open: a click on one fixes its team.
    const std::uint32_t shown =
        pipe_ && !pipe_->config().load().show_boxes && !view_interactive_ ? 0 : last_det_count_;
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
    draw_heat(cw, ch);  // on the background list, under every panel
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
      const int map_v0 = dl->VtxBuffer.Size;
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
      fade_vtx(dl, map_v0, fade_map_.a);
    }
  }
  if (pipe_)
    if (const MatchInfo* m = pipe_->match_ui().try_read())
      if (!args_.demo_flow) match_ = *m;
  pump_flow();
  if (have_frame_) draw_news(cw);
  if (shell() && pipe_) {
    // First-run news, once each: kits, then direction.
    const std::uint8_t src = pipe_->team_source.load(std::memory_order_relaxed);
    if (src != seen_team_source_ && src == 1) toast("Teams found from the shirts.");
    seen_team_source_ = src;
    const bool dir = pipe_->dir_found.load(std::memory_order_relaxed);
    if (dir && !seen_dir_)
      toast(std::string("Attack direction found: team A attacks ") + (pipe_->attack_dir.load() > 0 ? "right." : "left."));
    seen_dir_ = dir;
  }
  if (!toast_.empty() && now < toast_until_) {
    // Top centre pill, fading over its last half second.
    const float fade = std::min(1.f, static_cast<float>(toast_until_ - now) / 5.0e8f);
    const ImVec2 sz = ImGui::CalcTextSize(toast_.c_str());
    const float px = 14.f * dpi_, py = 8.f * dpi_;
    const ImVec2 a(0.5f * (cw - sz.x) - px, 18.f * dpi_);
    const ImVec2 b(a.x + sz.x + 2.f * px, a.y + sz.y + 2.f * py);
    auto* fg = ImGui::GetForegroundDrawList();
    fg->AddRectFilled(a, b, IM_COL32(16, 18, 22, static_cast<int>(225 * fade)), 0.5f * (b.y - a.y));
    fg->AddCircleFilled(ImVec2(a.x + 0.55f * px, 0.5f * (a.y + b.y)), 3.f * dpi_,
                        IM_COL32(46, 200, 110, static_cast<int>(255 * fade)));
    fg->AddText(ImVec2(a.x + px, a.y + py), IM_COL32(240, 242, 245, static_cast<int>(255 * fade)), toast_.c_str());
  }
  if (view_interactive_) {
    char hint[160];
    if (calib_mode_)
      std::snprintf(hint, sizeof(hint), "Click: %s   (right-click skips, Esc finishes)", kPitchLandmarkName[calib_pick_]);
    else
      std::snprintf(hint, sizeof(hint), "Left-click a player: team A.  Right-click: team B.");
    const ImVec2 sz = ImGui::CalcTextSize(hint);
    const ImVec2 a(0.5f * (cw - sz.x) - 12.f * dpi_, ch - sz.y - 34.f * dpi_);
    auto* fg = ImGui::GetForegroundDrawList();
    if (calib_mode_)
      for (int i = 0; i < kPitchLandmarks; ++i) {
        if (!sent_anchor_[i]) continue;
        const ImVec2 p(ox + sent_ax_[i] * scale, oy + sent_ay_[i] * scale);
        char id[4];
        std::snprintf(id, sizeof(id), "%d", i);
        fg->AddCircle(p, 5.f * dpi_, IM_COL32(0, 255, 255, 255), 0, 2.f);
        fg->AddText(ImVec2(p.x + 6.f * dpi_, p.y - 16.f * dpi_), IM_COL32(0, 255, 255, 255), id);
      }
    fg->AddRectFilled(a, ImVec2(a.x + sz.x + 24.f * dpi_, a.y + sz.y + 14.f * dpi_), IM_COL32(16, 18, 22, 210),
                      8.f * dpi_);
    fg->AddText(ImVec2(a.x + 12.f * dpi_, a.y + 7.f * dpi_), IM_COL32(240, 242, 245, 255), hint);
  }
  if (show_diag_) {
  ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.55f);
  ImGui::Begin("HUD", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);
  const double p50 = pipe_ ? static_cast<double>(pipe_->present_p50_ns.load()) / 1.0e6 : 0;
  const double p99 = pipe_ ? static_cast<double>(pipe_->present_p99_ns.load()) / 1.0e6 : 0;
  ImGui::Text("capture-to-present-call  p50 %.2f ms   p99 %.2f ms", p50, p99);
  ImGui::Text("present %.1f fps   frame %llu", fps_, static_cast<unsigned long long>(last_frame_));
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
  } else {
    hud_h_ = 0.f;
  }
  if (have_frame_) {
    draw_stats_panel();
    draw_card();
    draw_replay();
    draw_dead_panel(cw, ch);
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
// on-ball player or the scorebug sits on it, fading out and back in. Top: stats (home right) with the
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
    const PanelRect r = rect(f.side() == 0 ? home_left : 1 - home_left);
    return ImVec2(r.x0, r.y0);
  };
  const float fdt = std::min(dt, 0.1f);
  stats_pos_ = fade_stats_.step(place(flip_stats_, 0, top_rect), fdt);
  replay_pos_ = ImVec2(stats_pos_.x, stats_pos_.y + stats_sz_.y + 6.f);
  map_pos_ = fade_map_.step(place(flip_map_, 1, map_rect), fdt);
  card_pos_ = fade_card_.step(place(flip_card_, 0, card_rect), fdt);
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
  fade_vtx(ImGui::GetWindowDrawList(), 0, fade_stats_.a);
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
  fade_vtx(ImGui::GetWindowDrawList(), 0, fade_card_.a);
  ImGui::End();
}

// M15: play the oldest ready clip at half speed, twice, then fade. A newer
// clip takes over once the current one has played through once.
void App::draw_replay() {
  if (!pipe_) return;
  constexpr float kFadeS = 0.5f;
  constexpr int kLog = 2;  // M18: the clip the user picked from the replay log
  const bool on = pipe_->config().load().mini_replay != 0;
  const bool autoplay = on && layer(kLayerReplayAuto);
  const TimeNs now = now_ns();
  auto clip_at = [&](int i) -> ClipBuffer& { return i == kLog ? pipe_->log_clip() : pipe_->clip(i); };
  int next = -1;
  for (int i = 0; i < 2; ++i) {
    if (i == replay_clip_ || pipe_->clip(i).state.load(std::memory_order_acquire) != ClipState::Ready) continue;
    if (!autoplay) {  // logged by T7 already; not shown
      ClipState s = ClipState::Ready;
      pipe_->clip(i).state.compare_exchange_strong(s, ClipState::Free);
      continue;
    }
    if (next < 0 || pipe_->clip(i).seq < pipe_->clip(next).seq) next = i;
  }
  auto length = [&](const ClipBuffer& c) {
    return std::max(0.5f, 2.f * (c.meta[c.n - 1].t_s - c.meta[0].t_s));
  };
  // A pick from the log plays at once, over whatever is on.
  const bool log_ready = replay_clip_ != kLog && pipe_->log_clip().state.load(std::memory_order_acquire) == ClipState::Ready;
  if (replay_clip_ >= 0) {
    ClipBuffer& c = clip_at(replay_clip_);
    const float loops = static_cast<float>(now - replay_t0_) * 1e-9f / length(c);
    const bool is_log = replay_clip_ == kLog;
    if ((is_log ? stop_log_ : !autoplay || log_ready || (next >= 0 && loops >= 1.f)) ||
        loops * length(c) >= 2.f * length(c) + kFadeS) {
      c.state.store(ClipState::Free, std::memory_order_release);
      replay_clip_ = -1;
    }
  }
  stop_log_ = false;
  if (replay_clip_ < 0 && (log_ready || next >= 0)) {
    const int pick = log_ready ? kLog : next;
    ClipState s = ClipState::Ready;
    if (clip_at(pick).state.compare_exchange_strong(s, ClipState::Playing)) {
      replay_clip_ = pick;
      replay_t0_ = now;
      replay_frame_ = -1;
    }
  }
  if (replay_clip_ < 0) return;
  const ClipBuffer& c = clip_at(replay_clip_);
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
  ImGui::Text("%s   x0.5%s", what, replay_clip_ == kLog ? "   from the replay log" : "");
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
  fade_vtx(ImGui::GetWindowDrawList(), 0, fade_stats_.a);  // it moves with the stats panel
  ImGui::End();
  ImGui::PopStyleVar();
}

// ---- M18: match flow overlay ----

namespace {
ImU32 with_alpha(ImU32 c, float a) {
  const ImU32 al = static_cast<ImU32>(static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xff) * std::clamp(a, 0.f, 1.f));
  return (c & ~IM_COL32_A_MASK) | (al << IM_COL32_A_SHIFT);
}

// 0..1 from an integer, the same every frame.
float hash01(int i) {
  std::uint32_t h = static_cast<std::uint32_t>(i) * 2654435761u;
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  return static_cast<float>(h & 0xffffu) / 65535.f;
}

// Black or white, whichever reads on the colour.
ImU32 ink_on(ImU32 c) {
  const float l = 0.299f * static_cast<float>((c >> IM_COL32_R_SHIFT) & 0xff) +
                  0.587f * static_cast<float>((c >> IM_COL32_G_SHIFT) & 0xff) +
                  0.114f * static_cast<float>((c >> IM_COL32_B_SHIFT) & 0xff);
  return l > 150.f ? IM_COL32(10, 10, 12, 255) : IM_COL32(250, 250, 250, 255);
}

// A quad with one colour along the top edge and another along the bottom.
void grad_quad(ImDrawList* dl, ImVec2 tl, ImVec2 tr, ImVec2 br, ImVec2 bl, ImU32 top, ImU32 bot) {
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  dl->PrimReserve(6, 4);
  const auto i = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
  dl->PrimWriteIdx(i);
  dl->PrimWriteIdx(static_cast<ImDrawIdx>(i + 1));
  dl->PrimWriteIdx(static_cast<ImDrawIdx>(i + 2));
  dl->PrimWriteIdx(i);
  dl->PrimWriteIdx(static_cast<ImDrawIdx>(i + 2));
  dl->PrimWriteIdx(static_cast<ImDrawIdx>(i + 3));
  dl->PrimWriteVtx(tl, uv, top);
  dl->PrimWriteVtx(tr, uv, top);
  dl->PrimWriteVtx(br, uv, bot);
  dl->PrimWriteVtx(bl, uv, bot);
}

void pitch_bg(ImDrawList* dl, ImVec2 p, float w, float h, float alpha) {
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), with_alpha(IM_COL32(22, 78, 36, 235), alpha), 4.f);
  draw_pitch_lines(
      dl,
      [&](float X, float Y, ImVec2& out) {
        out = ImVec2(p.x + (X + 52.5f) / 105.f * w, p.y + (34.f - Y) / 68.f * h);
        return true;
      },
      with_alpha(IM_COL32(235, 235, 235, 150), alpha), 1.f);
}
}  // namespace

// The away kit when the two club colours are too alike to tell apart.
ImU32 App::kit_color(int t) const {
  const ImU32 c = team_color(match_, t);
  if (t == 0 || !match_.teams_ok || !match_.team[1].alt_color) return c;
  const ImU32 h = team_color(match_, 0);
  int d = 0;
  for (const int sh : {IM_COL32_R_SHIFT, IM_COL32_G_SHIFT, IM_COL32_B_SHIFT})
    d += std::abs(static_cast<int>((c >> sh) & 0xff) - static_cast<int>((h >> sh) & 0xff));
  if (d >= 90) return c;
  const std::uint32_t a = match_.team[1].alt_color;
  return IM_COL32((a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff, 255);
}

// T3's news and the flow snapshot; the goal banner follows MatchInfo.
void App::pump_flow() {
  if (!pipe_) return;
  const Config cfg = pipe_->config().load();
  layers_ = cfg.layers;
  popup_s_ = cfg.popup_s;
  goal_banner_s_ = cfg.goal_banner_s;
  dead_panel_s_ = args_.demo_flow ? 0.3f : cfg.dead_panel_s;
  line_high_m_ = cfg.line_high_m;
  if (const FlowSnapshot* f = pipe_->flow().try_read())
    if (!args_.demo_flow) flow_ = *f;
  const TimeNs now = now_ns();
  FeedItem it;
  while (pipe_->feed().try_pop(it)) {
    if (args_.demo_flow) continue;
    if (it.kind == FeedKind::SecondHalf) {
      toast("Second half: the teams have swapped ends. Wrong? Use Flip direction in the menu.", 8.0);
      continue;
    }
    if (it.kind == FeedKind::Goal || (it.team != 1 && it.team != 2)) continue;  // goals: the banner
    const bool turn = it.kind == FeedKind::Turnover;
    if (!layer(turn ? kLayerTurnover : kLayerEventPopups)) continue;
    // A turnover replaces the last one; otherwise the slot that ends first.
    Popup* dst = nullptr;
    for (auto& p : popups_)
      if (turn && p.kind == FeedKind::Turnover && p.until > now) dst = &p;
    if (!dst) {
      dst = &popups_[0];
      for (auto& p : popups_)
        if (p.until < dst->until) dst = &p;
    }
    const char* c = code(it.team - 1);
    if (it.kind == FeedKind::Shot)
      std::snprintf(dst->text, sizeof(dst->text), "Shot   %s   xG %.2f", c, static_cast<double>(it.xg));
    else if (it.kind == FeedKind::KeyPass)
      std::snprintf(dst->text, sizeof(dst->text), "Key pass   %s   xG %.2f", c, static_cast<double>(it.xg));
    else
      std::snprintf(dst->text, sizeof(dst->text), "%s win the ball", c);
    dst->team = it.team;
    dst->kind = it.kind;
    dst->until = now + static_cast<TimeNs>(static_cast<double>(turn ? std::min(popup_s_, 2.f) : popup_s_) * 1.0e9);
  }
  if (match_.goal_n > seen_goal_n_) {
    banner_goal_ = static_cast<int>(match_.goal_n) - 1;
    banner_t0_ = now;
  } else if (banner_goal_ >= static_cast<int>(match_.goal_n)) {
    banner_goal_ = -1;  // the read that added it was corrected
  }
  seen_goal_n_ = match_.goal_n;
}

// Pixel flames along the bottom edge, in the attacking team's colour, that
// grow as the team in possession nears the other goal: nothing before the
// attacking half, full height in the box.
void App::draw_heat(float cw, float ch) {
  const TimeNs now = now_ns();
  const float dt = heat_t_ ? std::min(0.1f, static_cast<float>(now - heat_t_) * 1e-9f) : 0.f;
  heat_t_ = now;
  float target = 0.f;
  const std::uint8_t team = last_poss_team_;
  if (pipe_ && layer(kLayerHeat) && !last_play_dead_ && last_pitch_valid_ && last_ball_ok_ && (team == 1 || team == 2)) {
    const int ad = pipe_->attack_dir.load(std::memory_order_relaxed) >= 0 ? 1 : -1;
    const float prog = (last_ball_x_ * static_cast<float>(team == 1 ? ad : -ad) + 52.5f) / 105.f;
    const float u = std::clamp((prog - 0.55f) / 0.37f, 0.f, 1.f);  // 58 m to 97 m from the team's own goal
    target = u * u * (3.f - 2.f * u);
    // The other team took over: burn down before the new fire starts.
    if (heat_team_ != team) {
      if (heat_ > 0.03f) target = 0.f;
      else heat_team_ = team;
    }
  }
  heat_ += std::clamp(target - heat_, -1.2f * dt, 2.5f * dt);
  if (heat_ < 0.01f || (heat_team_ != 1 && heat_team_ != 2)) return;

  auto* bg = ImGui::GetBackgroundDrawList();
  const float t = static_cast<float>(static_cast<double>(now % 600000000000LL) * 1e-9);
  const float tq = std::floor(t * 12.f) / 12.f;                // stepped, like sprite frames
  const float cell = std::max(3.f, std::round(ch / 216.f));    // 5 px at 1080p
  const float hmax = ch * 0.30f * heat_;
  const float a = 0.45f + 0.5f * heat_;
  // The team's colour, lifted when the kit is too dark to read as fire.
  const ImU32 kit = kit_color(heat_team_ - 1);
  float rgb[3] = {static_cast<float>((kit >> IM_COL32_R_SHIFT) & 0xff), static_cast<float>((kit >> IM_COL32_G_SHIFT) & 0xff),
                  static_cast<float>((kit >> IM_COL32_B_SHIFT) & 0xff)};
  const float lum = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
  if (lum < 80.f)
    for (float& c : rgb) c += (255.f - c) * (80.f - lum) / 160.f;
  auto shade = [&](float white, float dark, float alpha) {
    auto ch8 = [&](float c) { return static_cast<int>((c + (255.f - c) * white) * dark); };
    return IM_COL32(ch8(rgb[0]), ch8(rgb[1]), ch8(rgb[2]), static_cast<int>(255.f * std::clamp(alpha, 0.f, 1.f)));
  };
  const ImU32 core = shade(0.6f, 1.f, a), body = shade(0.12f, 1.f, 0.9f * a), tip = shade(0.f, 0.72f, 0.55f * a);
  constexpr int kTongue = 3;  // columns per flame: thin and tall
  const int cols = static_cast<int>(cw / cell) + 1;
  for (int i = 0; i < cols; ++i) {
    const int j = i / kTongue;
    const float r0 = hash01(j), r1 = hash01(j + 7919);
    // Each flame flickers on its own clock; a slow wave runs along the edge.
    const float flick = 0.5f + 0.5f * std::sin(tq * (5.f + 9.f * r0) + 6.283f * r1);
    const float wave = 0.5f + 0.5f * std::sin(static_cast<float>(j) * 0.35f - tq * 2.1f);
    float h = hmax * (0.25f + 0.75f * r1) * (0.35f + 0.4f * flick + 0.25f * wave);
    if (i % kTongue != 1) h *= 0.5f + 0.25f * hash01(i + 31);  // the shoulders of the flame
    const int n = static_cast<int>(h / cell);
    if (n <= 0) continue;
    const float x = static_cast<float>(i) * cell;
    auto band = [&](int from, int to, ImU32 col) {
      if (to > from)
        bg->AddRectFilled(ImVec2(x, ch - static_cast<float>(to) * cell), ImVec2(x + cell, ch - static_cast<float>(from) * cell),
                          col);
    };
    const int n_core = n * 3 / 10, n_body = n * 7 / 10;
    band(0, n_core, core);
    band(n_core, n_body, body);
    band(n_body, n, tip);
    // An ember rising off some of the flames.
    if (i % kTongue == 1 && r0 > 0.6f) {
      const float rise = std::fmod(tq * (0.5f + r1) + r0, 1.f);
      const float ey = ch - (static_cast<float>(n + 1) + std::floor(rise * 8.f)) * cell;
      bg->AddRectFilled(ImVec2(x, ey - cell), ImVec2(x + cell, ey), with_alpha(body, 1.f - rise));
    }
  }
}

// Top centre, under the toast: the goal banner, then the event pop-ups.
void App::draw_news(float cw) {
  auto* fg = ImGui::GetForegroundDrawList();
  ImFont* font = ImGui::GetFont();
  const float fs = ImGui::GetFontSize();
  const TimeNs now = now_ns();
  float y = 18.f * dpi_ + fs + 28.f * dpi_;
  if (banner_goal_ >= 0 && banner_goal_ < static_cast<int>(match_.goal_n) && layer(kLayerGoalBanner)) {
    const float el = static_cast<float>(now - banner_t0_) * 1e-9f;
    if (el >= goal_banner_s_) {
      banner_goal_ = -1;
    } else {
      const float a = std::min(1.f, el / 0.3f) * std::min(1.f, goal_banner_s_ - el);
      const GoalInfo& g = match_.goals[banner_goal_];
      const int t = g.side & 1;
      char l1[64], l2[96], min[12] = "";
      if (match_.teams_ok)
        std::snprintf(l1, sizeof(l1), "GOAL   %s %d - %d %s", match_.team[0].code, std::max<int>(0, match_.team[0].score),
                      std::max<int>(0, match_.team[1].score), match_.team[1].code);
      else
        std::snprintf(l1, sizeof(l1), "GOAL");
      if (g.minute) std::snprintf(min, sizeof(min), "   %u'", g.minute);
      const char* team = match_.teams_ok ? match_.team[t].name : (t == 0 ? "Team A" : "Team B");
      if (g.name[0] && g.jersey)
        std::snprintf(l2, sizeof(l2), "%u  %s%s   %s%s", g.jersey, g.name, g.own_goal ? " (OG)" : "", team, min);
      else if (g.name[0])
        std::snprintf(l2, sizeof(l2), "%s%s   %s%s", g.name, g.own_goal ? " (OG)" : "", team, min);
      else
        std::snprintf(l2, sizeof(l2), "%s%s", team, min);
      const float big = fs * 1.6f, mid = fs * 1.2f;
      const ImVec2 s1 = font->CalcTextSizeA(big, FLT_MAX, 0.f, l1), s2 = font->CalcTextSizeA(mid, FLT_MAX, 0.f, l2);
      const float px = 18.f * dpi_, py = 10.f * dpi_, bar = 8.f * dpi_;
      const float w = std::max(s1.x, s2.x) + 2.f * px + bar, h = s1.y + s2.y + 2.5f * py;
      const ImVec2 p(0.5f * (cw - w), y);
      fg->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(14, 16, 20, static_cast<int>(235 * a)), 8.f * dpi_);
      fg->AddRectFilled(p, ImVec2(p.x + bar, p.y + h), with_alpha(kit_color(t), a), 8.f * dpi_,
                        ImDrawFlags_RoundCornersLeft);
      fg->AddText(font, big, ImVec2(p.x + bar + px, p.y + py), IM_COL32(255, 255, 255, static_cast<int>(255 * a)), l1);
      fg->AddText(font, mid, ImVec2(p.x + bar + px, p.y + py * 1.5f + s1.y),
                  IM_COL32(225, 228, 232, static_cast<int>(255 * a)), l2);
      y += h + 8.f * dpi_;
    }
  }
  for (const Popup& p : popups_) {
    if (p.until <= now) continue;
    const float a = std::min(1.f, static_cast<float>(p.until - now) / 4.0e8f);
    const ImVec2 sz = ImGui::CalcTextSize(p.text);
    const float px = 12.f * dpi_, py = 6.f * dpi_, dot = 5.f * dpi_;
    const float w = sz.x + 2.f * px + 2.f * dot + 6.f * dpi_, h = sz.y + 2.f * py;
    const ImVec2 o(0.5f * (cw - w), y);
    fg->AddRectFilled(o, ImVec2(o.x + w, o.y + h), IM_COL32(16, 18, 22, static_cast<int>(215 * a)), 0.5f * h);
    fg->AddCircleFilled(ImVec2(o.x + px + dot, o.y + 0.5f * h), dot, with_alpha(kit_color(p.team - 1), a));
    fg->AddCircle(ImVec2(o.x + px + dot, o.y + 0.5f * h), dot, IM_COL32(255, 255, 255, static_cast<int>(140 * a)));
    fg->AddText(ImVec2(o.x + px + 2.f * dot + 6.f * dpi_, o.y + py), IM_COL32(240, 242, 245, static_cast<int>(255 * a)),
                p.text);
    y += h + 6.f * dpi_;
  }
}

// One panel per stoppage, in turn: lineups, back lines, momentum, passes. A long
// break (half time) moves on every 20 s.
void App::draw_dead_panel(float cw, float ch) {
  constexpr int kDeadPanels = 4;
  const TimeNs now = now_ns();
  auto next_panel = [&](int from) {
    const bool ok[kDeadPanels] = {layer(kLayerLineup) && match_.lineup_ok != 0,
                                  layer(kLayerDefLine) && flow_.line.seen_s[0] + flow_.line.seen_s[1] >= 20.f,
                                  layer(kLayerMomentum) && flow_.max_bin >= 0,
                                  layer(kLayerPassMap) && flow_.pass_n[0] + flow_.pass_n[1] >= 10};
    for (int k = 0; k < kDeadPanels; ++k)
      if (ok[(from + k) % kDeadPanels]) return (from + k) % kDeadPanels;
    return -1;
  };
  if (!last_play_dead_) {
    if (dead_panel_ >= 0) dead_rot_ = (dead_panel_ + 1) % kDeadPanels;
    dead_panel_ = -1;
    return;
  }
  const float dead_s = static_cast<float>(now - dead_since_) * 1e-9f;
  if (dead_s < dead_panel_s_) return;
  if (dead_panel_ < 0 || now - dead_panel_t0_ > 20000000000LL) {
    const int pick = next_panel(dead_panel_ < 0 ? dead_rot_ : dead_panel_ + 1);
    // The only one available stays, without fading in again.
    dead_panel_t0_ = pick == dead_panel_ ? now - 1000000000LL : now;
    dead_panel_ = pick;
  }
  static constexpr std::uint32_t kBit[kDeadPanels] = {kLayerLineup, kLayerDefLine, kLayerMomentum, kLayerPassMap};
  if (dead_panel_ < 0 || !layer(kBit[dead_panel_])) {
    dead_panel_ = -1;
    return;
  }
  const float alpha = std::min(1.f, static_cast<float>(now - dead_panel_t0_) * 1e-9f / 0.3f);
  // Between the minimap and the on-ball card when there is room.
  const float mw = std::clamp(cw * 0.22f, 160.f, 420.f);
  float w = std::clamp(cw * (dead_panel_ == 0 ? 0.3f : dead_panel_ == 3 ? 0.44f : 0.34f), 360.f,
                       dead_panel_ == 0 ? 560.f : dead_panel_ == 3 ? 880.f : 700.f);
  if (cw - 2.f * (mw + 40.f) >= 360.f) w = std::min(w, cw - 2.f * (mw + 40.f));
  w = std::min(w, cw - 32.f);
  ImGui::SetNextWindowPos(ImVec2(0.5f * cw, ch - 16.f), ImGuiCond_Always, ImVec2(0.5f, 1.f));
  ImGui::SetNextWindowBgAlpha(0.78f * alpha);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
  ImGui::Begin("Dead ball", nullptr, kPanelFlags);
  if (dead_panel_ == 0) draw_lineup(w);
  else if (dead_panel_ == 1) draw_lines(w);
  else if (dead_panel_ == 2) draw_momentum(w);
  else draw_passes(w);
  ImGui::End();
  ImGui::PopStyleVar();
}

// Both XIs on one pitch: home on the left half, away on the right.
void App::draw_lineup(float w) {
  const float alpha = ImGui::GetStyle().Alpha;
  ImGui::TextUnformatted("Starting XI");
  ImGui::SameLine();
  ImGui::TextDisabled("%s %s   v   %s %s", match_.team[0].name, match_.team[0].formation, match_.team[1].name,
                      match_.team[1].formation);
  const float h = w * 68.f / 105.f;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(w, h));
  auto* dl = ImGui::GetWindowDrawList();
  pitch_bg(dl, p, w, h, alpha);
  ImFont* font = ImGui::GetFont();
  const float fs = ImGui::GetFontSize();
  const float r = std::max(8.f, h * 0.042f);
  const float num_fs = std::min(fs, r * 1.25f), name_fs = std::clamp(h * 0.045f, 10.f, fs);
  dl->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
  for (int t = 0; t < 2; ++t) {
    const TeamInfo& team = match_.team[t];
    XiSpot spots[11];
    const int n = layout_xi(team, spots);
    const ImU32 col = kit_color(t);
    for (int k = 0; k < n; ++k) {
      const PlayerInfo& pl = team.squad[spots[k].idx];
      // Home attacks right: its left wing is the top of the picture.
      const float X = (t == 0 ? -1.f : 1.f) * (45.f - spots[k].depth * 38.f);
      const float Y = (t == 0 ? -1.f : 1.f) * spots[k].lat * 27.f;
      const ImVec2 c(p.x + (X + 52.5f) / 105.f * w, p.y + (34.f - Y) / 68.f * h - 0.25f * r);
      const float a = alpha * (pl.subbed == 1 ? 0.4f : 1.f);
      dl->AddCircleFilled(c, r, with_alpha(col, a));
      dl->AddCircle(c, r, with_alpha(IM_COL32(255, 255, 255, 230), a), 0, 1.5f);
      char num[8];
      std::snprintf(num, sizeof(num), "%u", pl.jersey);
      const ImVec2 ns = font->CalcTextSizeA(num_fs, FLT_MAX, 0.f, num);
      dl->AddText(font, num_fs, ImVec2(c.x - 0.5f * ns.x, c.y - 0.5f * ns.y), with_alpha(ink_on(col), a), num);
      char name[16];
      std::snprintf(name, sizeof(name), "%.14s", surname(pl.name));
      const ImVec2 ts = font->CalcTextSizeA(name_fs, FLT_MAX, 0.f, name);
      const ImVec2 tp(c.x - 0.5f * ts.x, c.y + r + 1.f);
      dl->AddText(font, name_fs, ImVec2(tp.x + 1.f, tp.y + 1.f), with_alpha(IM_COL32(0, 0, 0, 200), a), name);
      dl->AddText(font, name_fs, tp, with_alpha(IM_COL32(255, 255, 255, 255), a), name);
    }
  }
  dl->PopClipRect();
}

// Where each back line has stood: the average (solid) and now (thin).
void App::draw_lines(float w) {
  const float alpha = ImGui::GetStyle().Alpha;
  const LineStats& l = flow_.line;
  ImGui::TextUnformatted("Back line height");
  ImGui::SameLine();
  ImGui::TextDisabled("vision estimate, metres from own goal");
  const float pw = w * 0.52f, ph = pw * 68.f / 105.f;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(pw, ph));
  auto* dl = ImGui::GetWindowDrawList();
  pitch_bg(dl, p, pw, ph, alpha);
  const int ad = pipe_ && pipe_->attack_dir.load(std::memory_order_relaxed) < 0 ? -1 : 1;
  for (int t = 0; t < 2; ++t) {
    const float dir = static_cast<float>(t == 0 ? ad : -ad);
    const ImU32 col = kit_color(t);
    auto vline = [&](float m, float thick, float a) {
      const float x = p.x + (dir * (m - 52.5f) + 52.5f) / 105.f * pw;
      dl->AddLine(ImVec2(x, p.y + 2.f), ImVec2(x, p.y + ph - 2.f), with_alpha(IM_COL32(255, 255, 255, 220), a * alpha),
                  thick + 2.f);
      dl->AddLine(ImVec2(x, p.y + 2.f), ImVec2(x, p.y + ph - 2.f), with_alpha(col, a * alpha), thick);
      return x;
    };
    if (l.cur_m[t] >= 0.f) {  // now: a thin dashed line
      const float x = p.x + (dir * (l.cur_m[t] - 52.5f) + 52.5f) / 105.f * pw;
      for (float y = p.y + 3.f; y < p.y + ph - 3.f; y += 9.f)
        dl->AddLine(ImVec2(x, y), ImVec2(x, std::min(y + 5.f, p.y + ph - 3.f)), with_alpha(IM_COL32(255, 255, 255, 230), alpha),
                    1.5f);
    }
    if (l.avg_m(t) >= 0.f) {
      const float x = vline(l.avg_m(t), 3.f, 1.f);
      const ImVec2 ts = ImGui::CalcTextSize(code(t));
      const ImVec2 tp(x - 0.5f * ts.x, t == 0 ? p.y + 4.f : p.y + ph - ts.y - 4.f);
      dl->AddRectFilled(ImVec2(tp.x - 3.f, tp.y - 1.f), ImVec2(tp.x + ts.x + 3.f, tp.y + ts.y + 1.f),
                        with_alpha(IM_COL32(14, 16, 20, 220), alpha), 3.f);
      dl->AddText(tp, with_alpha(IM_COL32(255, 255, 255, 255), alpha), code(t));
    }
  }
  ImGui::SameLine();
  ImGui::BeginGroup();
  if (ImGui::BeginTable("lines", 3, ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    for (int t = 0; t < 2; ++t) {
      ImGui::TableNextColumn();
      const ImVec2 q = ImGui::GetCursorScreenPos();
      const float s = ImGui::GetTextLineHeight();
      dl->AddRectFilled(ImVec2(q.x, q.y + 0.2f * s), ImVec2(q.x + 0.6f * s, q.y + 0.8f * s), with_alpha(kit_color(t), alpha),
                        2.f);
      ImGui::Dummy(ImVec2(0.6f * s, s));
      ImGui::SameLine();
      ImGui::TextUnformatted(code(t));
    }
    auto row = [&](const char* name, auto cell) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(name);
      for (int t = 0; t < 2; ++t) {
        ImGui::TableNextColumn();
        cell(t);
      }
    };
    auto metres = [](float m) {
      if (m < 0.f) ImGui::TextDisabled("-");
      else ImGui::Text("%.0f m", static_cast<double>(m));
    };
    row("average", [&](int t) { metres(l.avg_m(t)); });
    row("now", [&](int t) { metres(l.cur_m[t]); });
    row("high line", [&](int t) { ImGui::Text("%.0f%%", static_cast<double>(l.high_pct(t))); });
    row("2+ in the box", [&](int t) { ImGui::Text("%.0f%%", static_cast<double>(l.box_pct(t))); });
    ImGui::EndTable();
  }
  ImGui::TextDisabled("solid: average   dashed: now");
  ImGui::TextDisabled("high: %.0f m or more", static_cast<double>(line_high_m_));
  ImGui::EndGroup();
}

// Attacking threat per minute, team A up and team B down, with the goals.
void App::draw_momentum(float w) {
  const float alpha = ImGui::GetStyle().Alpha;
  ImGui::TextUnformatted("Match momentum");
  for (int t = 0; t < 2; ++t) {  // legend: which way each team's threat points
    ImGui::SameLine(0.f, 14.f);
    const ImVec2 q = ImGui::GetCursorScreenPos();
    const float s = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(q.x, q.y + 0.2f * s), ImVec2(q.x + 0.6f * s, q.y + 0.8f * s),
                                              with_alpha(kit_color(t), alpha), 2.f);
    ImGui::Dummy(ImVec2(0.6f * s, s));
    ImGui::SameLine(0.f, 4.f);
    ImGui::Text("%s %s", code(t), t == 0 ? "up" : "down");
  }
  ImGui::SameLine(0.f, 14.f);
  ImGui::TextDisabled("vision estimate");
  ImFont* font = ImGui::GetFont();
  const float fs = ImGui::GetFontSize(), sm_fs = std::max(10.f, fs * 0.85f);
  const float h = std::clamp(w * 0.32f, 120.f, 210.f);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(w, h));
  auto* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), with_alpha(IM_COL32(10, 12, 16, 210), alpha), 4.f);
  const float x0 = p.x + 8.f, x1 = p.x + w - 8.f, y0 = p.y + 6.f, y1 = p.y + h - sm_fs - 6.f;
  const float mid = 0.5f * (y0 + y1), half = 0.5f * (y1 - y0);
  const int nb = std::clamp(static_cast<int>(flow_.max_bin) + 1, 1, kFlowBins);
  const int xmax = nb <= 45 ? 45 : nb <= 90 ? 90 : (nb + 4) / 5 * 5;
  float scale = 1.f;
  for (int b = 0; b < nb; ++b) scale = std::max(scale, std::fabs(flow_value(flow_, b)));
  auto X = [&](float minute) { return x0 + minute / static_cast<float>(xmax) * (x1 - x0); };
  auto Y = [&](float v) { return mid - v / scale * half * 0.92f; };
  const ImU32 col[2] = {with_alpha(kit_color(0), 0.9f * alpha), with_alpha(kit_color(1), 0.9f * alpha)};
  const ImU32 grid = with_alpha(IM_COL32(255, 255, 255, 40), alpha), ink = with_alpha(IM_COL32(200, 204, 210, 255), alpha);
  for (int m = 0; m <= xmax; m += 15) {
    dl->AddLine(ImVec2(X(static_cast<float>(m)), y0), ImVec2(X(static_cast<float>(m)), y1), grid);
    char lab[8];
    std::snprintf(lab, sizeof(lab), "%d'", m);
    const ImVec2 ls = font->CalcTextSizeA(sm_fs, FLT_MAX, 0.f, lab);
    dl->AddText(font, sm_fs, ImVec2(std::clamp(X(static_cast<float>(m)) - 0.5f * ls.x, p.x + 2.f, p.x + w - ls.x - 2.f), y1 + 3.f),
                ink, lab);
  }
  // Area between the curve and the axis, split where it crosses.
  const ImDrawListFlags flags = dl->Flags;
  dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;  // mixed winding below
  auto side = [&](float v) { return col[v >= 0.f ? 0 : 1]; };
  if (nb == 1) {
    const float v = flow_value(flow_, 0);
    dl->AddRectFilled(ImVec2(X(0.f), std::min(mid, Y(v))), ImVec2(X(1.f), std::max(mid, Y(v))), side(v));
  }
  for (int b = 0; b + 1 < nb; ++b) {
    const float v0 = flow_value(flow_, b), v1 = flow_value(flow_, b + 1);
    const float xa = X(static_cast<float>(b) + 0.5f), xb = X(static_cast<float>(b) + 1.5f);
    if (v0 * v1 >= 0.f) {
      dl->AddQuadFilled(ImVec2(xa, mid), ImVec2(xa, Y(v0)), ImVec2(xb, Y(v1)), ImVec2(xb, mid), side(v0 + v1));
    } else {
      const float xz = xa + (xb - xa) * std::fabs(v0) / (std::fabs(v0) + std::fabs(v1));
      dl->AddTriangleFilled(ImVec2(xa, mid), ImVec2(xa, Y(v0)), ImVec2(xz, mid), side(v0));
      dl->AddTriangleFilled(ImVec2(xz, mid), ImVec2(xb, Y(v1)), ImVec2(xb, mid), side(v1));
    }
    dl->AddLine(ImVec2(xa, Y(v0)), ImVec2(xb, Y(v1)), with_alpha(IM_COL32(255, 255, 255, 150), alpha), 1.f);
  }
  dl->Flags = flags;
  dl->AddLine(ImVec2(x0, mid), ImVec2(x1, mid), with_alpha(IM_COL32(255, 255, 255, 170), alpha), 1.f);
  if (flow_.half2_bin >= 0) {
    const float x = X(static_cast<float>(flow_.half2_bin));
    dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), with_alpha(IM_COL32(255, 255, 255, 150), alpha), 1.5f);
    dl->AddText(font, sm_fs, ImVec2(x + 3.f, y0), ink, "HT");
  }
  for (int i = 0; i < flow_.goal_n; ++i) {
    const auto& g = flow_.goals[i];
    const float x = X(static_cast<float>(g.bin) + 0.5f);
    const float y = g.team == 1 ? y0 + 6.f : y1 - 6.f;
    dl->AddLine(ImVec2(x, y), ImVec2(x, mid), with_alpha(IM_COL32(255, 255, 255, 110), alpha), 1.f);
    dl->AddCircleFilled(ImVec2(x, y), 5.f, with_alpha(IM_COL32(255, 255, 255, 255), alpha));
    dl->AddCircle(ImVec2(x, y), 5.f, with_alpha(kit_color(g.team - 1), alpha), 0, 2.f);
  }
}

// Each team's last passes as arrows on its own pitch, both attacking left to
// right: where they start, how far they go and which way.
void App::draw_passes(float w) {
  const float alpha = ImGui::GetStyle().Alpha;
  ImGui::TextUnformatted("Pass map");
  ImGui::SameLine();
  ImGui::TextDisabled("vision estimate, both teams attacking left to right");
  auto* dl = ImGui::GetWindowDrawList();
  const ImU32 col_ok = IM_COL32(255, 165, 30, 255), col_bad = IM_COL32(150, 110, 240, 255);
  const float s = ImGui::GetTextLineHeight();
  const float gap = 12.f, pw = 0.5f * (w - gap), ph = pw * 68.f / 105.f;
  const float thick = std::max(1.2f, pw / 320.f), head = std::max(6.f, pw / 55.f);
  for (int t = 0; t < 2; ++t) {
    if (t) ImGui::SameLine(0.f, gap);
    ImGui::BeginGroup();
    const ImVec2 q = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(ImVec2(q.x, q.y + 0.2f * s), ImVec2(q.x + 0.6f * s, q.y + 0.8f * s), with_alpha(kit_color(t), alpha), 2.f);
    ImGui::Dummy(ImVec2(0.6f * s, s));
    ImGui::SameLine(0.f, 4.f);
    ImGui::TextUnformatted(code(t));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(pw, ph));
    dl->AddRectFilled(p, ImVec2(p.x + pw, p.y + ph), with_alpha(IM_COL32(28, 32, 38, 235), alpha), 4.f);
    auto M = [&](float X, float Y) { return ImVec2(p.x + (X + 52.5f) / 105.f * pw, p.y + (34.f - Y) / 68.f * ph); };
    draw_pitch_lines(
        dl,
        [&](float X, float Y, ImVec2& out) {
          out = M(X, Y);
          return true;
        },
        with_alpha(IM_COL32(235, 235, 235, 120), alpha), 1.f);
    const int n = flow_.pass_count(t);
    int ok = 0, fwd = 0;
    float len = 0.f;
    dl->PushClipRect(p, ImVec2(p.x + pw, p.y + ph), true);
    // Completed first: the misses are fewer and stay readable on top.
    for (int pass = 0; pass < 2; ++pass)
      for (int i = 0; i < n; ++i) {
        const PassMark& m = flow_.passes[t][i];
        if ((m.ok != 0) != (pass == 0)) continue;
        if (m.ok) ++ok;
        if (m.x1() > m.x0()) ++fwd;
        len += std::hypot(m.x1() - m.x0(), m.y1() - m.y0());
        const ImVec2 a = M(m.x0(), m.y0()), b = M(m.x1(), m.y1());
        const float dx = b.x - a.x, dy = b.y - a.y, d = std::hypot(dx, dy);
        if (d < 1.f) continue;
        const ImU32 c = with_alpha(m.ok ? col_ok : col_bad, 0.8f * alpha);
        const float ux = dx / d, uy = dy / d, hd = std::min(head, 0.5f * d);
        const ImVec2 base(b.x - ux * hd, b.y - uy * hd);
        dl->AddLine(a, base, c, thick);
        dl->AddTriangleFilled(b, ImVec2(base.x - uy * 0.45f * hd, base.y + ux * 0.45f * hd),
                              ImVec2(base.x + uy * 0.45f * hd, base.y - ux * 0.45f * hd), c);
      }
    dl->PopClipRect();
    if (n > 0)
      ImGui::TextDisabled("%d/%d completed   avg %.0f m   forward %.0f%%", ok, n, static_cast<double>(len / static_cast<float>(n)),
                          100.0 * fwd / n);
    else
      ImGui::TextDisabled("no passes yet");
    ImGui::EndGroup();
  }
  auto key = [&](ImU32 c, const char* name) {
    const ImVec2 q = ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(q.x + 0.4f * s, q.y + 0.5f * s), 0.3f * s, with_alpha(c, alpha));
    ImGui::Dummy(ImVec2(0.8f * s, s));
    ImGui::SameLine(0.f, 4.f);
    ImGui::TextUnformatted(name);
  };
  key(col_ok, "Completed");
  ImGui::SameLine(0.f, 16.f);
  key(col_bad, "Not completed");
  ImGui::SameLine(0.f, 16.f);
  ImGui::TextDisabled("last %d passes per team", kFlowPasses);
}

// --demo-flow: a made-up match, so the M18 pieces can be seen (and checked)
// without one. Four seconds each: an attack with pop-ups and the goal banner,
// then the four dead-ball panels with play between them.
void App::demo_flow() {
  const TimeNs now = now_ns();
  if (demo_t0_ == 0) {
    demo_t0_ = now;
    MatchInfo& m = match_;
    m = {};
    m.teams_ok = m.lineup_ok = 1;
    m.clock_ms = 63 * 60000;
    struct P {
      const char* name;
      const char* pos;
      int jersey;
    };
    static constexpr P kHome[11] = {{"Alex Keeper", "G", 1},     {"Rui Rightback", "RB", 2},  {"Leo Leftback", "LB", 3},
                                    {"Carl Stopper", "CD-R", 5}, {"Dan Marker", "CD-L", 6},   {"Max Anchor", "DM", 4},
                                    {"Tom Runner", "CM-R", 8},   {"Sam Passer", "CM-L", 10},  {"Rob Winger", "RF", 7},
                                    {"Lee Flyer", "LF", 11},     {"Joe Striker", "F", 9}};
    static constexpr P kAway[11] = {{"Ivan Gloves", "G", 13},    {"Nico Right", "RB", 22},    {"Otto Left", "LB", 33},
                                    {"Paul Tower", "CD-R", 15},  {"Quin Rock", "CD-L", 16},   {"Raul Wide", "RM", 17},
                                    {"Stan Engine", "CM-R", 18}, {"Theo Link", "CM-L", 20},   {"Ugo Flank", "LM", 21},
                                    {"Vic Second", "F-R", 19},   {"Will Target", "F-L", 29}};
    const char* names[2] = {"Northfield", "Eastbrook"};
    const char* codes[2] = {"NOR", "EAS"};
    const char* forms[2] = {"4-3-3", "4-4-2"};
    const std::uint32_t cols[2] = {0xD01C2Au, 0x1E4FB4u};
    for (int t = 0; t < 2; ++t) {
      TeamInfo& ti = m.team[t];
      std::snprintf(ti.name, sizeof(ti.name), "%s", names[t]);
      std::snprintf(ti.code, sizeof(ti.code), "%s", codes[t]);
      std::snprintf(ti.formation, sizeof(ti.formation), "%s", forms[t]);
      ti.color = cols[t];
      ti.has_color = 1;
      ti.score = static_cast<std::int16_t>(t == 0 ? 2 : 1);
      ti.n = 11;
      for (int i = 0; i < 11; ++i) {
        const P& src = (t == 0 ? kHome : kAway)[i];
        PlayerInfo& pl = ti.squad[i];
        std::snprintf(pl.name, sizeof(pl.name), "%s", src.name);
        std::snprintf(pl.pos, sizeof(pl.pos), "%s", src.pos);
        pl.jersey = static_cast<std::uint8_t>(src.jersey);
        pl.starter = 1;
        pl.place = static_cast<std::uint8_t>(i + 1);
      }
    }
    FlowSnapshot& f = flow_;
    f = {};
    for (int b = 0; b < 63; ++b) {
      const float x = static_cast<float>(b);
      const float v = 2.2f * std::sin(x * 0.21f) + 1.4f * std::sin(x * 0.57f + 1.f) + 0.8f * std::sin(x * 1.3f);
      f.threat[v >= 0.f ? 0 : 1][b] = std::fabs(v) + 0.3f;
      f.threat[v >= 0.f ? 1 : 0][b] = 0.3f;
    }
    f.max_bin = f.cur_bin = 62;
    f.half2_bin = 45;
    f.goal_n = 3;
    f.goals[0] = {8, 1};
    f.goals[1] = {50, 2};
    f.goals[2] = {62, 1};
    f.line.sum_m[0] = 43.f * 900.f;
    f.line.sum_m[1] = 31.f * 900.f;
    f.line.seen_s[0] = f.line.seen_s[1] = 900.f;
    f.line.high_s[0] = 560.f;
    f.line.high_s[1] = 130.f;
    f.line.cur_m[0] = 47.f;
    f.line.cur_m[1] = 27.f;
    f.line.box_s[0] = 260.f;
    f.line.box_s[1] = 90.f;
    f.line.play_s = 2400.f;
    for (int t = 0; t < 2; ++t) {
      for (int i = 0; i < 120; ++i) {
        auto rnd = [&](int k) { return hash01(i * 8 + k + t * 4000); };
        const float x = -42.f + 80.f * rnd(0), y = -30.f + 60.f * rnd(1);
        const float ang = 6.283f * rnd(2) * (rnd(4) < 0.5f ? 0.3f : 1.f), d = 6.f + 26.f * rnd(3) * rnd(3);
        auto q = [](float m) { return static_cast<std::int8_t>(std::clamp(std::round(2.f * m), -127.f, 127.f)); };
        f.passes[t][i] = {q(x), q(y), q(std::clamp(x + d * std::cos(ang), -52.f, 52.f)),
                          q(std::clamp(y + d * std::sin(ang), -33.f, 33.f)), static_cast<std::uint8_t>(rnd(5) > 0.18f ? 1 : 0)};
      }
      f.pass_n[t] = 120;
    }
  }
  const float el = static_cast<float>(now - demo_t0_) * 1e-9f;
  const int step = static_cast<int>(el / 4.f) % 8;
  const bool dead = step % 2 == 1;
  if (step != demo_step_) {
    demo_step_ = step;
    if (dead) dead_since_ = now;
    if (step == 0) {
      // The score goes up; the name arrives a second later.
      match_.goal_n = 1;
      match_.goals[0] = {};
      match_.goals[0].minute = 63;
      seen_goal_n_ = 0;
      static constexpr const char* kNews[3] = {"NOR win the ball", "Key pass   NOR   xG 0.27", "Shot   NOR   xG 0.31"};
      static constexpr FeedKind kKind[3] = {FeedKind::Turnover, FeedKind::KeyPass, FeedKind::Shot};
      for (int k = 0; k < 3; ++k) {
        std::snprintf(popups_[k].text, sizeof(popups_[k].text), "%s", kNews[k]);
        popups_[k].team = 1;
        popups_[k].kind = kKind[k];
        popups_[k].until = now + 3500000000LL;
      }
    }
  }
  if (step == 0 && el - 4.f * std::floor(el / 4.f) > 1.f && !match_.goals[0].name[0]) {
    std::snprintf(match_.goals[0].name, sizeof(match_.goals[0].name), "Joe Striker");
    match_.goals[0].jersey = 9;
  }
  dead_panel_s_ = 0.3f;
  last_play_dead_ = dead ? 1 : 0;
  last_pitch_valid_ = 1;
  last_ball_ok_ = 1;
  last_poss_team_ = 1;
  // Team A works the ball from halfway into the box over each live spell.
  const float u = el / 4.f - std::floor(el / 4.f);
  const int ad = pipe_ && pipe_->attack_dir.load(std::memory_order_relaxed) < 0 ? -1 : 1;
  last_ball_x_ = static_cast<float>(ad) * (5.f + 44.f * std::min(1.f, u * 1.6f));
}

// Menu: what the overlay shows.
void App::draw_layer_toggles(Config& cfg) {
  struct Item {
    const char* label;
    std::uint32_t bit;
    const char* tip;
  };
  static constexpr Item kItems[] = {
      {"Shot / key pass pop-ups", kLayerEventPopups, "A sm_fs note at the top when a shot or key pass is seen."},
      {"Turnover pop-up", kLayerTurnover, "A note when the ball changes team."},
      {"Goal banner", kLayerGoalBanner, "Scorer, number and team after the scorebug's score changes."},
      {"Flames", kLayerHeat, "Pixel fire in the attacking team's colour as it nears the other goal."},
      {"Lineups", kLayerLineup, "Both starting XIs when the ball is out of play."},
      {"Back lines", kLayerDefLine, "How high each defence has stood, when the ball is out of play."},
      {"Momentum", kLayerMomentum, "Match momentum chart when the ball is out of play."},
      {"Pass map", kLayerPassMap, "Each team's passes as arrows, when the ball is out of play."},
      {"Auto-play key moments", kLayerReplayAuto, "Off: shots and key passes still go to the replay log."},
  };
  bool changed = false;
  if (ImGui::BeginTable("layers", 2)) {
    for (const Item& it : kItems) {
      ImGui::TableNextColumn();
      bool on = (cfg.layers & it.bit) != 0;
      if (ImGui::Checkbox(it.label, &on)) {
        cfg.layers = on ? cfg.layers | it.bit : cfg.layers & ~it.bit;
        changed = true;
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.tip);
    }
    ImGui::EndTable();
  }
  if (changed) {
    ++cfg.version;
    pipe_->config().store(cfg);
  }
}

// Menu: the saved key moments, newest first.
void App::draw_replays_tab(Config& cfg) {
  if (!services_) {
    ImGui::TextDisabled("Not available.");
    return;
  }
  int keep = cfg.replay_log_n;
  ImGui::SetNextItemWidth(160.f * dpi_);
  if (ImGui::SliderInt("moments kept", &keep, 0, 20)) {
    cfg.replay_log_n = static_cast<std::uint8_t>(keep);
    ++cfg.version;
    pipe_->config().store(cfg);
  }
  if (!cfg.mini_replay) ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "Mini replay is off, so nothing is being saved.");
  else if (pipe_->replay_over_budget.load(std::memory_order_relaxed))
    ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "Paused: the PC is over its frame budget.");
  const std::vector<LogEntry> log = services_->replay_log();
  if (log.empty()) {
    ImGui::TextDisabled("No key moments yet. Shots and key passes are saved here as they happen.");
    return;
  }
  const bool playing = replay_clip_ == 2;
  if (playing) {
    if (ImGui::Button("Stop")) stop_log_ = true;
    ImGui::SameLine();
    ImGui::TextDisabled("playing on the overlay");
  }
  std::uint32_t kb = 0;
  if (ImGui::BeginTable("log", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
    for (auto it = log.rbegin(); it != log.rend(); ++it) {
      kb += it->kbytes;
      ImGui::PushID(static_cast<int>(it->id));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (it->minute) ImGui::Text("%u'", it->minute);
      else ImGui::TextDisabled("#%u", it->id);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(it->kind == 1 ? (it->goal ? "Shot (goal)" : "Shot") : it->kind == 0 ? "Key pass" : "Manual");
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(it->team == 1 || it->team == 2 ? code(it->team - 1) : "");
      ImGui::TableNextColumn();
      if (ImGui::SmallButton("Play")) {
        stop_log_ = playing;  // the one on screen makes way
        ServiceCmd c;
        c.kind = ServiceCmd::Kind::PlayLog;
        c.log_id = it->id;
        services_->post(c);
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::TextDisabled("%zu saved, %.1f MB", log.size(), static_cast<double>(kb) / 1024.0);
}

void App::refresh_windows() {
  const TimeNs now = now_ns();
  if (now - window_refresh_ < 1000000000 && !windows_.empty()) return;
  window_refresh_ = now;
  list_capture_windows(windows_, last_window_title_);
  // Keep the pick while it is open; otherwise the likeliest window.
  bool still = false;
  for (const auto& w : windows_) still |= w.hwnd == selected_;
  if (!still) selected_ = windows_.empty() ? nullptr : windows_.front().hwnd;
}

namespace {
constexpr const char* kPresetFile = "config/detectors.json";
constexpr const char* kPresetPick = "profiles/detector.txt";
}  // namespace

// --detector wins; overlay mode falls back to the last pick, then the first
// preset whose file is present. Selftest and replay keep the stub unless asked.
void App::load_presets() {
  std::ifstream in(kPresetFile);
  if (in) {
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    presets_ = parse_detector_presets(text);
  }
  preset_ok_.assign(presets_.size(), 0);
  std::error_code ec;
  for (std::size_t i = 0; i < presets_.size(); ++i)
    preset_ok_[i] = std::filesystem::is_regular_file(presets_[i].file, ec) ? 1 : 0;
  auto find = [&](const std::string& name) {
    for (std::size_t i = 0; i < presets_.size(); ++i)
      if (presets_[i].name == name) return static_cast<int>(i);
    return -1;
  };
  if (!args_.model_path.empty()) return;
  if (!args_.detector_name.empty()) {
    preset_ = find(args_.detector_name);
    if (preset_ < 0) log_line("detector preset not found: " + args_.detector_name);
    return;
  }
  if (args_.mode != AppArgs::Mode::Overlay) return;
  std::ifstream last(kPresetPick);
  std::string name;
  if (std::getline(last, name)) preset_ = find(name);
  if (preset_ >= 0 && preset_ok_[preset_]) return;
  preset_ = -1;
  for (std::size_t i = 0; i < presets_.size() && preset_ < 0; ++i)
    if (preset_ok_[i]) preset_ = static_cast<int>(i);
}

std::unique_ptr<Detector> App::make_detector() {
  DetectorPreset p;
  if (!args_.model_path.empty()) {
    // A listed model keeps its resize, normalisation and class list.
    const auto file = std::filesystem::path(args_.model_path).filename();
    for (const auto& q : presets_)
      if (std::filesystem::path(q.file).filename() == file) p = q;
    if (p.name.empty()) p.name = file.string();
    p.file = args_.model_path;
  } else if (preset_ >= 0) {
    p = presets_[preset_];
  }
  if (!p.file.empty()) {
    std::string error;
    auto det = make_ort_detector(p, error);
    if (det) return det;
    log_line("ORT detector unavailable (" + error + "); using stub");
  }
  return make_stub_detector();
}

// Main tab, before capture: the model is fixed for a session (T2 allocates
// its tensors before Pipeline::start).
void App::draw_detector_pick() {
  if (!args_.model_path.empty()) {
    ImGui::Text("detector: %s (--model)", args_.model_path.c_str());
    return;
  }
  if (presets_.empty()) {
    ImGui::TextWrapped("detector: stub (no %s)", kPresetFile);
    return;
  }
  const char* shown = preset_ >= 0 ? presets_[preset_].name.c_str() : "stub (test boxes)";
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##detector", shown)) {
    if (ImGui::Selectable("stub (test boxes)", preset_ < 0)) preset_ = -1;
    for (std::size_t i = 0; i < presets_.size(); ++i) {
      ImGui::BeginDisabled(!preset_ok_[i]);
      const std::string item = presets_[i].name + (preset_ok_[i] ? "" : "  (file missing)");
      if (ImGui::Selectable(item.c_str(), preset_ == static_cast<int>(i))) {
        preset_ = static_cast<int>(i);
        std::error_code ec;
        std::filesystem::create_directories("profiles", ec);
        std::ofstream(kPresetPick) << presets_[i].name;
      }
      ImGui::EndDisabled();
      if (!presets_[i].note.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s\n%s", presets_[i].file.c_str(), presets_[i].note.c_str());
    }
    ImGui::EndCombo();
  }
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
  for (const auto& w : windows_)
    if (w.hwnd == selected_) last_window_title_ = w.title;
  {
    std::error_code ec;
    std::filesystem::create_directories("profiles", ec);
    std::ofstream("profiles/window.txt") << last_window_title_;
  }
  seen_team_source_ = 0;
  seen_dir_ = false;
  flow_ = {};
  for (auto& p : popups_) p = {};
  seen_goal_n_ = 0;
  banner_goal_ = -1;
  heat_ = 0.f;
  dead_panel_ = -1;
  replay_clip_ = -1;
  set_menu(true);  // the menu stays up after the start; Alt+T hides it
  toast(hotkey_ok_ ? "TouchFC is on. Alt+T hides and shows the menu." : "TouchFC is on. The tray icon hides and shows the menu.",
        6.0);
  return true;
}

// Teardown in run()'s order, then back to the start card.
void App::stop_capture(const char* why) {
  if (!pipe_) return;
  if (held_ >= 0) {
    pipe_->release_presented(static_cast<std::uint32_t>(held_));
    held_ = -1;
  }
  if (services_) services_->stop();
  pipe_->join();
  write_validation();
  if (sink_) sink_->write_csv(args_.telemetry_path);
  pipe_->write_spadl("spadl.csv");
  if (source_) source_->close();
  services_.reset();
  pipe_.reset();
  detector_.reset();
  sink_.reset();
  pool_.reset();
  source_.reset();
  have_frame_ = false;
  last_det_count_ = 0;
  mm_valid_ = false;
  calib_mode_ = false;
  set_view_interactive(false);
  ShowWindow(view_hwnd_, SW_HIDE);
  status_ = why ? why : "";
  window_refresh_ = 0;
  set_menu(true);
}

void App::track_target() {
  if (shell() && pipe_ && (!selected_ || !IsWindow(selected_))) {
    stop_capture("That window closed. Pick another one.");
    return;
  }
  if (args_.mode != AppArgs::Mode::Overlay || !view_hwnd_ || !selected_ || !IsWindow(selected_) || !pipe_) return;
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
  raise_menu_ = true;
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
// Provider slug and the name shown. Clubs, then national teams.
constexpr const char* kLeagues[] = {"eng.1", "esp.1", "uefa.champions", "ger.1", "ita.1", "fra.1", "usa.1",
                                    "fifa.world", "fifa.friendly", "uefa.nations", "uefa.euro", "uefa.euroq",
                                    "fifa.worldq.uefa", "conmebol.america", "fifa.worldq.conmebol",
                                    "concacaf.gold", "concacaf.nations.league", "caf.nations", "afc.asian.cup"};
constexpr const char* kLeagueName[] = {"Premier League", "La Liga", "Champions League", "Bundesliga", "Serie A",
                                       "Ligue 1", "MLS",
                                       "World Cup", "International friendly", "UEFA Nations League", "Euros",
                                       "Euro qualifying", "World Cup qualifying (UEFA)", "Copa America",
                                       "World Cup qualifying (CONMEBOL)", "Gold Cup", "CONCACAF Nations League",
                                       "Africa Cup of Nations", "Asian Cup"};
static_assert(IM_ARRAYSIZE(kLeagues) == IM_ARRAYSIZE(kLeagueName));

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
  ImGui::SetNextItemWidth(240.f);
  if (ImGui::Combo("league", &league_idx_, kLeagueName, IM_ARRAYSIZE(kLeagueName))) {
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
  char league[24]{}, date[9]{};
  unsigned home = 0, away = 0;
  if (std::sscanf(args_.match_spec.c_str(), "%23[^:]:%8[0-9]:%u:%u", league, date, &home, &away) != 4) {
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

// ---- Overlay shell: one card over the stream, Alt+T, tray. ----

void App::push_cmd(const UiCommand& cmd) {
  if (!pipe_) return;
  if (!pipe_->commands().try_push(cmd)) {
    pending_ = cmd;
    pending_cmd_ = true;
    pipe_->command_warns.fetch_add(1, std::memory_order_relaxed);
  }
}

void App::toast(std::string text, double seconds) {
  toast_ = std::move(text);
  toast_until_ = now_ns() + static_cast<TimeNs>(seconds * 1.0e9);
}

void App::set_view_interactive(bool on) {
  if (!view_hwnd_ || view_interactive_ == on) return;
  view_interactive_ = on;
  // Layered with no bitmap hit-tests as empty, so both styles go while open.
  constexpr LONG_PTR kPass = WS_EX_TRANSPARENT | WS_EX_LAYERED;
  LONG_PTR ex = GetWindowLongPtrW(view_hwnd_, GWL_EXSTYLE);
  ex = on ? (ex & ~kPass) : (ex | kPass);
  SetWindowLongPtrW(view_hwnd_, GWL_EXSTYLE, ex);
  SetWindowPos(view_hwnd_, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void App::set_menu(bool open) {
  if (!shell() || !control_hwnd_) return;
  menu_open_ = open;
  if (!open) calib_mode_ = false;
  set_view_interactive(open && pipe_ != nullptr);
  if (open) {
    profiles_.clear();
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("profiles", ec))
      if (e.path().extension() == ".json") profiles_.push_back(e.path().stem().string());
    window_refresh_ = 0;
    raise_menu_ = true;
    place_menu();
    ShowWindow(control_hwnd_, SW_SHOW);
    SetForegroundWindow(control_hwnd_);
  } else {
    ShowWindow(control_hwnd_, SW_HIDE);
    if (pipe_ && selected_ && IsWindow(selected_)) SetForegroundWindow(selected_);
  }
}

void App::on_close() {
  if (pipe_) set_menu(false);
  else quit_ = true;
}

void App::on_tray(LPARAM lp) {
  if (lp == WM_LBUTTONUP) toggle_menu();
  else if (lp == WM_RBUTTONUP) tray_popup(control_hwnd_, pipe_ != nullptr);
}

void App::on_shell_cmd(UINT id) {
  if (id == kTrayMenu) set_menu(true);
  else if (id == kTrayStop) stop_capture(nullptr);
  else if (id == kTrayQuit) quit_ = true;
}

// Live: docked top right inside the stream. Before capture: centred on the
// monitor under the cursor.
void App::place_menu() {
  if (!control_hwnd_ || !menu_open_) return;
  RECT area{};
  const int m = static_cast<int>(16.f * dpi_);
  const int w = static_cast<int>(menu_sz_.x + 0.5f), h = static_cast<int>(menu_sz_.y + 0.5f);
  int x = 0, y = 0;
  if (pipe_ && selected_ && IsWindow(selected_)) {
    if (FAILED(DwmGetWindowAttribute(selected_, DWMWA_EXTENDED_FRAME_BOUNDS, &area, sizeof(area))))
      GetWindowRect(selected_, &area);
    x = std::max<int>(area.left + m, area.right - w - m);
    y = area.top + m;
  } else {
    RECT now{};
    GetWindowRect(control_hwnd_, &now);
    const POINT p = IsWindowVisible(control_hwnd_) ? POINT{(now.left + now.right) / 2, (now.top + now.bottom) / 2}
                                                   : [] { POINT c{}; GetCursorPos(&c); return c; }();
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY), &mi);
    area = mi.rcWork;
    x = (area.left + area.right - w) / 2;
    y = std::max<int>(area.top + m, (area.top + area.bottom - h) / 2 - static_cast<int>(60.f * dpi_));
  }
  RECT now{};
  GetWindowRect(control_hwnd_, &now);
  if (!raise_menu_ && now.left == x && now.top == y && now.right - now.left == w && now.bottom - now.top == h) return;
  raise_menu_ = false;
  SetWindowPos(control_hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

void App::on_view_click(int x, int y, bool right) {
  if (!pipe_ || !have_frame_ || view_scale_ <= 0.f) return;
  const float fx = (static_cast<float>(x) - view_ox_) / view_scale_;
  const float fy = (static_cast<float>(y) - view_oy_) / view_scale_;
  if (calib_mode_) {
    if (right) {
      calib_pick_ = (calib_pick_ + 1) % kPitchLandmarks;  // skip one not in view
      return;
    }
    UiCommand cmd;
    cmd.type = CmdType::SetAnchor;
    cmd.u8a = static_cast<std::uint8_t>(calib_pick_);
    cmd.f[0] = fx;
    cmd.f[1] = fy;
    push_cmd(cmd);
    sent_anchor_[calib_pick_] = true;
    sent_ax_[calib_pick_] = fx;
    sent_ay_[calib_pick_] = fy;
    calib_pick_ = (calib_pick_ + 1) % kPitchLandmarks;
    return;
  }
  // Smallest player or keeper box under the cursor.
  int best = -1;
  float best_area = 0.f;
  for (std::uint32_t i = 0; i < last_det_count_; ++i) {
    const Detection& d = last_dets_[i];
    if (d.track_id == 0 || d.class_id > 1) continue;
    if (fx < d.x || fy < d.y || fx > d.x + d.w || fy > d.y + d.h) continue;
    if (best < 0 || d.w * d.h < best_area) {
      best = static_cast<int>(i);
      best_area = d.w * d.h;
    }
  }
  if (best < 0) return;
  UiCommand cmd;
  cmd.type = CmdType::SetTeamSeed;
  cmd.u8a = right ? 2 : 1;
  cmd.u16a = static_cast<std::uint16_t>(last_dets_[best].track_id);
  push_cmd(cmd);
  char msg[64];
  std::snprintf(msg, sizeof(msg), "Player #%u is now team %s.", static_cast<unsigned>(cmd.u16a), right ? "B" : "A");
  toast(msg, 2.5);
}

namespace {
bool accent_button(const char* label, const ImVec2& size) {
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.72f, 0.42f, 1.f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.82f, 0.50f, 1.f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.13f, 0.62f, 0.36f, 1.f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.03f, 0.08f, 0.05f, 1.f));
  const bool hit = ImGui::Button(label, size);
  ImGui::PopStyleColor(4);
  return hit;
}

void status_dot(bool ok) {
  const float r = ImGui::GetFontSize() * 0.22f;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + ImGui::GetFontSize() * 0.55f), r,
                                              ok ? IM_COL32(46, 200, 110, 255) : IM_COL32(240, 170, 40, 255));
  ImGui::Dummy(ImVec2(2.f * r, ImGui::GetFontSize()));
  ImGui::SameLine();
}

std::string clip_text(const std::string& s, std::size_t n) {
  if (s.size() <= n) return s;
  std::size_t k = n;
  while (k > 0 && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80) --k;  // utf-8 boundary
  return s.substr(0, k) + "...";
}
}  // namespace

void App::draw_start_card() {
  if (title_font_) ImGui::PushFont(title_font_);
  ImGui::TextUnformatted("TouchFC");
  if (title_font_) ImGui::PopFont();
  ImGui::TextDisabled("Live stats and tracking over any football stream.");
  ImGui::Spacing();
  refresh_windows();
  ImGui::TextUnformatted("What are you watching?");
  bool go = false;
  if (ImGui::BeginChild("##windows", ImVec2(0, 210.f * dpi_), ImGuiChildFlags_FrameStyle)) {
    if (windows_.empty()) ImGui::TextDisabled("Open the match in a browser or video player first.");
    for (const auto& w : windows_) {
      ImGui::PushID(w.hwnd);
      const std::string t = clip_text(w.title, 52);
      if (ImGui::Selectable(t.c_str(), w.hwnd == selected_, ImGuiSelectableFlags_AllowDoubleClick)) {
        selected_ = w.hwnd;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) go = true;
      }
      if (!w.app.empty()) {
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() -
                        ImGui::CalcTextSize(w.app.c_str()).x - 4.f * dpi_);
        ImGui::TextDisabled("%s", w.app.c_str());
      }
      ImGui::PopID();
    }
  }
  ImGui::EndChild();

  ImGui::Spacing();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Settings");
  ImGui::SameLine(110.f * dpi_);
  std::string pick;
  {
    std::ifstream in("profiles/last.txt");
    in >> pick;
  }
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##profile", pick.empty() ? "Default" : pick.c_str())) {
    auto choose = [&](const std::string& name) {
      std::error_code ec;
      std::filesystem::create_directories("profiles", ec);
      std::ofstream("profiles/last.txt") << name;
    };
    if (ImGui::Selectable("Default", pick.empty())) choose("");
    for (const auto& p : profiles_)
      if (ImGui::Selectable(p.c_str(), p == pick)) choose(p);
    ImGui::EndCombo();
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Model");
  ImGui::SameLine(110.f * dpi_);
  draw_detector_pick();

  ImGui::Spacing();
  ImGui::BeginDisabled(selected_ == nullptr);
  go |= accent_button("Start", ImVec2(-1, 42.f * dpi_));
  go |= selected_ && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
  ImGui::EndDisabled();
  if (go && start_overlay_capture()) return;
  if (!status_.empty()) ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f), "%s", status_.c_str());
  ImGui::Spacing();
  ImGui::TextDisabled(hotkey_ok_ ? "Teams and attack direction are found for you. Alt+T opens the menu."
                                 : "Teams and attack direction are found for you. The tray icon opens the menu.");
  if (ImGui::SmallButton("Quit")) quit_ = true;
}

void App::draw_live_card() {
  Config cfg = pipe_->config().load();
  const char* win = last_window_title_.c_str();
  if (title_font_) ImGui::PushFont(title_font_);
  ImGui::TextUnformatted("TouchFC");
  if (title_font_) ImGui::PopFont();
  ImGui::SameLine();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.f * dpi_);
  status_dot(true);
  ImGui::TextDisabled("Live  %s", clip_text(win, 30).c_str());

  if (ImGui::BeginTabBar("pages")) {
    if (ImGui::BeginTabItem("Home")) {
      const bool named = match_.teams_ok != 0;
      const char* na = named ? match_.team[0].name : "Team A";
      const char* nb = named ? match_.team[1].name : "Team B";
      const std::uint8_t src = pipe_->team_source.load(std::memory_order_relaxed);
      const bool dir = pipe_->dir_found.load(std::memory_order_relaxed);
      ImGui::Spacing();
      {
        // Same colours as the boxes.
        const float s = ImGui::GetFontSize() * 0.7f;
        auto chip = [&](ImU32 col, const char* name) {
          const ImVec2 p = ImGui::GetCursorScreenPos();
          const float y = p.y + 0.5f * (ImGui::GetFontSize() - s) + 1.f;
          ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + s, y + s), col, 3.f * dpi_);
          ImGui::Dummy(ImVec2(s, ImGui::GetFontSize()));
          ImGui::SameLine();
          ImGui::TextUnformatted(name);
        };
        chip(IM_COL32(40, 140, 255, 255), na);
        ImGui::SameLine();
        ImGui::TextDisabled(" vs ");
        ImGui::SameLine();
        chip(IM_COL32(255, 70, 70, 255), nb);
      }
      static constexpr const char* kSource[] = {"Finding the two kits (needs a few seconds of wide play)",
                                                "Kits found from the shirts", "Kits from the match data",
                                                "Kits set by your click"};
      status_dot(src != 0);
      ImGui::TextUnformatted(kSource[src < 4 ? src : 0]);
      const bool dir_user = pipe_->dir_by_user.load(std::memory_order_relaxed) != 0;
      status_dot(dir || dir_user);
      ImGui::Text("%s attacks %s%s", na, pipe_->attack_dir.load() > 0 ? "right  >" : "left  <",
                  dir_user ? "  (set by you)" : dir ? "" : "  (not sure yet)");
      status_dot(last_pitch_valid_ != 0);
      ImGui::TextUnformatted(last_pitch_valid_ ? "Pitch lines locked" : "Looking for the pitch lines");
      ImGui::Spacing();
      const float bw = (ImGui::GetContentRegionAvail().x - 2.f * ImGui::GetStyle().ItemSpacing.x) / 3.f;
      UiCommand c;
      if (ImGui::Button("Swap teams", ImVec2(bw, 0))) {
        c.type = CmdType::SwapTeams;
        push_cmd(c);
      }
      ImGui::SameLine();
      if (ImGui::Button("Flip direction", ImVec2(bw, 0))) {
        c.type = CmdType::ApplyAttackDir;
        c.u8a = static_cast<std::uint8_t>(pipe_->attack_dir.load() > 0 ? 0 : 1);
        push_cmd(c);
      }
      ImGui::SameLine();
      if (ImGui::Button("Find again", ImVec2(bw, 0))) {
        c.type = CmdType::AutoTeams;
        push_cmd(c);
        toast("Looking for the kits again.");
      }
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
      ImGui::TextWrapped("Player on the wrong team? Left-click them on the video for %s, right-click for %s.", na, nb);
      ImGui::PopStyleColor();
      ImGui::Separator();
      bool boxes = cfg.show_boxes != 0, mini = cfg.mini_replay != 0;
      bool hide_map = cfg.minimap_dead_mode == static_cast<std::uint8_t>(MinimapDeadMode::Hide);
      bool changed = false;
      changed |= ImGui::Checkbox("Player boxes", &boxes);
      ImGui::SameLine();
      changed |= ImGui::Checkbox("Mini replay", &mini);
      ImGui::SameLine();
      changed |= ImGui::Checkbox("Hide map when paused", &hide_map);
      if (changed) {
        cfg.show_boxes = boxes ? 1 : 0;
        cfg.mini_replay = mini ? 1 : 0;
        cfg.minimap_dead_mode = static_cast<std::uint8_t>(hide_map ? MinimapDeadMode::Hide : MinimapDeadMode::Freeze);
        ++cfg.version;
        pipe_->config().store(cfg);
      }
      ImGui::Separator();
      draw_layer_toggles(cfg);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Match")) {
      if (ImGui::BeginChild("##match", ImVec2(0, 380.f * dpi_))) draw_match();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Replays")) {
      if (ImGui::BeginChild("##replays", ImVec2(0, 300.f * dpi_))) draw_replays_tab(cfg);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Advanced")) {
      if (ImGui::BeginChild("##adv", ImVec2(0, 420.f * dpi_))) {
        if (ImGui::CollapsingHeader("Pitch calibration")) {
          ImGui::Text("%s, fit error %.2f m", last_pitch_valid_ ? (last_pitch_manual_ ? "manual" : "automatic")
                                                                : "not calibrated",
                      last_reproj_m_);
          if (ImGui::Button(calib_mode_ ? "Done clicking" : "Calibrate by clicking the video")) calib_mode_ = !calib_mode_;
          ImGui::SameLine();
          if (ImGui::Button("Clear")) {
            UiCommand c;
            c.type = CmdType::ResetHomography;
            push_cmd(c);
            for (bool& a : sent_anchor_) a = false;
          }
          ImGui::SetNextItemWidth(-1);
          ImGui::Combo("##landmark", &calib_pick_, kPitchLandmarkName, kPitchLandmarks);
          ImGui::TextDisabled("Pause the stream first. Click 4 or more landmarks; right-click skips one.");
          ImGui::Checkbox("Show the fitted lines", &debug_reproj_);
        }
        if (ImGui::CollapsingHeader("Diagnostics")) {
          ImGui::Checkbox("Performance overlay", &show_diag_);
          ImGui::Text("detector: %s", detector_ ? detector_->label() : "-");
          ImGui::Text("capture to screen p50 %.1f ms, p99 %.1f ms",
                      static_cast<double>(pipe_->present_p50_ns.load()) / 1.0e6,
                      static_cast<double>(pipe_->present_p99_ns.load()) / 1.0e6);
          if (ImGui::Button("Test mini replay")) {
            UiCommand c;
            c.type = CmdType::TestReplay;
            push_cmd(c);
          }
          if (pending_cmd_) ImGui::TextDisabled("command queue full; will retry");
        }
        if (ImGui::CollapsingHeader("Thresholds and profiles")) draw_tuning(cfg);
      }
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::Separator();
  const float bw = (ImGui::GetContentRegionAvail().x - 2.f * ImGui::GetStyle().ItemSpacing.x) / 3.f;
  if (ImGui::Button(hotkey_ok_ ? "Hide  (Alt+T)" : "Hide", ImVec2(bw, 0))) set_menu(false);
  ImGui::SameLine();
  if (ImGui::Button("Change window", ImVec2(bw, 0))) {
    stop_capture(nullptr);
    return;
  }
  ImGui::SameLine();
  if (ImGui::Button("Quit", ImVec2(bw, 0))) quit_ = true;
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    if (calib_mode_) calib_mode_ = false;
    else set_menu(false);
  }
}

void App::draw_menu() {
  if (!menu_open_ || !control_swap_) return;
  place_menu();
  if (control_dirty_) {
    control_rtv_.Reset();
    control_swap_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    recreate_rtv(control_swap_.Get(), control_rtv_);
    control_dirty_ = false;
  }
  const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
  const float clear[4] = {bg.x, bg.y, bg.z, 1.f};
  gpu_.context->OMSetRenderTargets(1, control_rtv_.GetAddressOf(), nullptr);
  gpu_.context->ClearRenderTargetView(control_rtv_.Get(), clear);
  imgui_begin(control_binding_);
  // The window fills the card; the card then takes the content's height.
  // (Auto-resize would clamp to the card itself and shrink it to nothing.)
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(1.f);
  ImGui::Begin("TouchFC", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  if (pipe_) draw_live_card();
  else draw_start_card();
  const ImGuiStyle& st = ImGui::GetStyle();
  const float content_h = ImGui::GetCursorPosY() - st.ItemSpacing.y + st.WindowPadding.y;
  menu_sz_ = ImVec2(470.f * dpi_, std::max(80.f * dpi_, std::ceil(content_h)));
  ImGui::End();
  imgui_end(control_rtv_.Get());
  control_swap_->Present(view_hwnd_ && IsWindowVisible(view_hwnd_) ? 0u : 1u, 0);
}

void App::draw_control() {
  if (shell()) {
    draw_menu();
    return;
  }
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
      if (ImGui::BeginTabItem("Extras")) {
        draw_layer_toggles(cfg);
        ImGui::Separator();
        draw_replays_tab(cfg);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    if (main_tab) {
    auto push_cmd = [&](CmdType type) {
      UiCommand cmd;
      cmd.type = type;
      if (!pipe_->commands().try_push(cmd)) {
        pending_ = cmd;
        pending_cmd_ = true;
        pipe_->command_warns.fetch_add(1, std::memory_order_relaxed);
      }
    };
    static constexpr const char* kTeamSource[] = {"looking for the two kits (wide shots, in play)",
                                                  "kits found from the players", "kits from the Match tab",
                                                  "set by a click"};
    const std::uint8_t src = pipe_->team_source.load(std::memory_order_relaxed);
    ImGui::Text("teams: %s", kTeamSource[src < 4 ? src : 0]);
    if (ImGui::Button("swap A/B")) push_cmd(CmdType::SwapTeams);
    ImGui::SameLine();
    if (ImGui::Button("find teams again")) push_cmd(CmdType::AutoTeams);
    ImGui::Text("team A attacks %s%s", pipe_->attack_dir.load() > 0 ? "right" : "left",
                pipe_->dir_found.load(std::memory_order_relaxed) ? " (from positions)" : "");
    ImGui::SameLine();
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
    ImGui::TextUnformatted("Wrong team? Click a track's A or B; the rest follow its colour.");
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
    draw_detector_pick();
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
  load_presets();
  if (!create_gpu(gpu_)) return 1;
  if (!compile_pass(gpu_.device.Get(), false, copy_pass_)) return 1;
  if (!compile_pass(gpu_.device.Get(), true, premul_pass_)) return 1;

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"TouchFCWindow";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = shell_icon();
  wc.hIconSm = shell_icon();
  if (!RegisterClassExW(&wc)) return 1;

  // Overlay mode: the control window is a borderless card over the stream,
  // off the taskbar, out of captures, opened with Alt+T or the tray.
  control_binding_.control = true;
  const bool sh = shell();
  if (!create_window(control_hwnd_, sh ? L"TouchFC" : L"TouchFC control", sh ? WS_POPUP : WS_OVERLAPPEDWINDOW,
                     sh ? (WS_EX_TOPMOST | WS_EX_TOOLWINDOW) : 0, sh ? 470 : 440, sh ? 300 : 720, control_binding_))
    return 1;
  if (!create_hwnd_swap(control_hwnd_, control_swap_, false)) return 1;
  recreate_rtv(control_swap_.Get(), control_rtv_);
  control_binding_.imgui = ImGui::CreateContext();
  ImGui::SetCurrentContext(control_binding_.imgui);
  ImGui::GetIO().IniFilename = nullptr;
  ImGui_ImplWin32_Init(control_hwnd_);
  ImGui_ImplDX11_Init(gpu_.device.Get(), gpu_.context.Get());
  if (sh) {
    dpi_ = static_cast<float>(GetDpiForWindow(control_hwnd_)) / 96.f;
    if (dpi_ <= 0.f) dpi_ = 1.f;
    title_font_ = apply_shell_style(dpi_);
    ImGui::GetStyle().WindowRounding = 0.f;  // DWM rounds the window itself
    const DWORD round = 2;                   // DWMWCP_ROUND
    DwmSetWindowAttribute(control_hwnd_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    if (!args_.recordable) SetWindowDisplayAffinity(control_hwnd_, WDA_EXCLUDEFROMCAPTURE);
    hotkey_ok_ = RegisterHotKey(control_hwnd_, 1, MOD_ALT | MOD_NOREPEAT, 'T') == TRUE;
    if (!hotkey_ok_) log_line("Alt+T is taken by another app; use the tray icon");
    tray_ok_ = tray_add(control_hwnd_);
    std::ifstream in("profiles/window.txt");
    std::getline(in, last_window_title_);
    if (args_.keypoints_path.empty()) {
      std::error_code ec;
      constexpr const char* kKeypoints = "third_party/models/football-pitch-detection.onnx";
      if (std::filesystem::is_regular_file(kKeypoints, ec)) args_.keypoints_path = kKeypoints;
    }
    show_diag_ = false;
  } else {
    ShowWindow(control_hwnd_, SW_SHOW);
  }

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
    apply_shell_style(dpi_);
    affinity_ok_ = args_.recordable || SetWindowDisplayAffinity(view_hwnd_, WDA_EXCLUDEFROMCAPTURE) == TRUE;
    if (!affinity_ok_) log_line("SetWindowDisplayAffinity failed");
    status_.clear();
    set_menu(true);
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
    if (quit_) quit = true;
    track_target();
    pump_pipeline();
    const bool view_on = view_hwnd_ && view_swap_ && IsWindowVisible(view_hwnd_);
    if (view_on) draw_view();
    if (!shell() || menu_open_) draw_control();
    // Nothing on screen to pace the loop: wait for input instead of spinning.
    else if (!view_on) MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
  }
  if (tray_ok_) tray_remove(control_hwnd_);
  if (hotkey_ok_) UnregisterHotKey(control_hwnd_, 1);

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
