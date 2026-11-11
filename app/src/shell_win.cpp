#include "shell_win.hpp"

#include <dwmapi.h>
#include <shellapi.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>

namespace touchfc {

namespace {

std::string utf8(const wchar_t* w) {
  char buf[512]{};
  WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
  return buf;
}

std::string exe_stem(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return {};
  wchar_t path[MAX_PATH]{};
  DWORD n = MAX_PATH;
  const bool ok = QueryFullProcessImageNameW(p, 0, path, &n) != 0;
  CloseHandle(p);
  if (!ok) return {};
  const wchar_t* name = std::wcsrchr(path, L'\\');
  name = name ? name + 1 : path;
  std::wstring stem(name);
  if (const auto dot = stem.rfind(L'.'); dot != std::wstring::npos) stem.resize(dot);
  for (auto& c : stem) c = static_cast<wchar_t>(std::towlower(c));
  return utf8(stem.c_str());
}

bool contains_ci(const std::string& hay, const char* needle) {
  const std::size_t n = std::strlen(needle);
  if (n > hay.size()) return false;
  for (std::size_t i = 0; i + n <= hay.size(); ++i) {
    std::size_t k = 0;
    while (k < n && std::tolower(static_cast<unsigned char>(hay[i + k])) == needle[k]) ++k;
    if (k == n) return true;
  }
  return false;
}

int score_of(const ShellWindow& w, const std::string& last_title) {
  static constexpr const char* kPlayers[] = {"vlc", "mpv", "potplayermini64", "potplayer", "mpc-hc64", "mpc-be64",
                                             "wmplayer", "video.ui", "obs64", "ffplay", "kodi"};
  static constexpr const char* kBrowsers[] = {"chrome", "msedge", "firefox", "brave", "opera", "vivaldi", "arc"};
  static constexpr const char* kWords[] = {"live", " vs", " v ", "football", "soccer", "premier", "league", "cup",
                                           "dazn", "sky", "espn", "tnt", "peacock", "paramount", "bein", "youtube",
                                           "twitch", "iplayer", "itvx", "fubo", "stan", "optus", "match"};
  int s = 0;
  if (!last_title.empty() && w.title == last_title) s += 100;
  for (const char* p : kPlayers)
    if (w.app == p) s += 30;
  for (const char* b : kBrowsers)
    if (w.app == b) s += 20;
  for (const char* k : kWords)
    if (contains_ci(w.title, k)) {
      s += 15;
      break;
    }
  return s;
}

}  // namespace

void list_capture_windows(std::vector<ShellWindow>& out, const std::string& last_title) {
  out.clear();
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
        if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) return TRUE;
        wchar_t klass[64]{};
        GetClassNameW(hwnd, klass, 64);
        if (!wcscmp(klass, L"TouchFCWindow") || !wcscmp(klass, L"Progman") || !wcscmp(klass, L"Shell_TrayWnd"))
          return TRUE;
        RECT r{};
        GetWindowRect(hwnd, &r);
        if (r.right - r.left < 320 || r.bottom - r.top < 180) return TRUE;
        wchar_t title[256]{};
        if (GetWindowTextW(hwnd, title, 256) <= 0) return TRUE;
        auto& v = *reinterpret_cast<std::vector<ShellWindow>*>(lp);
        v.push_back(ShellWindow{hwnd, utf8(title), exe_stem(hwnd), 0});
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&out));
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i].score = score_of(out[i], last_title) + std::max(0, 10 - static_cast<int>(i));  // z-order
  std::stable_sort(out.begin(), out.end(), [](const ShellWindow& a, const ShellWindow& b) { return a.score > b.score; });
}

HICON shell_icon() {
  static HICON icon = [] {
    constexpr int n = 32;
    BITMAPV5HEADER bh{};
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = n;
    bh.bV5Height = -n;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00ff0000;
    bh.bV5GreenMask = 0x0000ff00;
    bh.bV5BlueMask = 0x000000ff;
    bh.bV5AlphaMask = 0xff000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bh), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return LoadIcon(nullptr, IDI_APPLICATION);
    auto* px = static_cast<std::uint32_t*>(bits);
    // 4x4 supersampled: green rounded tile, white ball, dark centre patch.
    for (int y = 0; y < n; ++y) {
      for (int x = 0; x < n; ++x) {
        float tile = 0, ball = 0, patch = 0;
        for (int sy = 0; sy < 4; ++sy)
          for (int sx = 0; sx < 4; ++sx) {
            const float fx = x + (sx + 0.5f) / 4.f, fy = y + (sy + 0.5f) / 4.f;
            const float qx = std::max(std::fabs(fx - 16.f) - 9.f, 0.f), qy = std::max(std::fabs(fy - 16.f) - 9.f, 0.f);
            if (std::sqrt(qx * qx + qy * qy) <= 6.f) tile += 1.f / 16.f;
            const float d = std::hypot(fx - 16.f, fy - 16.f);
            if (d <= 9.5f) ball += 1.f / 16.f;
            if (d <= 3.6f) patch += 1.f / 16.f;
          }
        float r = 31, g = 157, b = 85;  // tile
        auto mix = [&](float t, float R, float G, float B) {
          r += (R - r) * t;
          g += (G - g) * t;
          b += (B - b) * t;
        };
        mix(ball, 250, 250, 250);
        mix(patch, 24, 28, 34);
        const float a = tile;  // premultiplied
        px[y * n + x] = (static_cast<std::uint32_t>(a * 255.f) << 24) | (static_cast<std::uint32_t>(r * a) << 16) |
                        (static_cast<std::uint32_t>(g * a) << 8) | static_cast<std::uint32_t>(b * a);
      }
    }
    HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON h = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return h ? h : LoadIcon(nullptr, IDI_APPLICATION);
  }();
  return icon;
}

bool tray_add(HWND owner) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = owner;
  nid.uID = 1;
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  nid.uCallbackMessage = kTrayMsg;
  nid.hIcon = shell_icon();
  wcscpy_s(nid.szTip, L"TouchFC  (Alt+T: menu)");
  return Shell_NotifyIconW(NIM_ADD, &nid) == TRUE;
}

void tray_remove(HWND owner) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = owner;
  nid.uID = 1;
  Shell_NotifyIconW(NIM_DELETE, &nid);
}

void tray_popup(HWND owner, bool capturing) {
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, kTrayMenu, L"Menu\tAlt+T");
  if (capturing) AppendMenuW(m, MF_STRING, kTrayStop, L"Stop and pick another window");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, kTrayQuit, L"Quit TouchFC");
  POINT p{};
  GetCursorPos(&p);
  SetForegroundWindow(owner);  // else the menu never closes
  TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, owner, nullptr);
  PostMessageW(owner, WM_NULL, 0, 0);
  DestroyMenu(m);
}

ImFont* apply_shell_style(float s) {
  ImGuiIO& io = ImGui::GetIO();
  char dir[MAX_PATH]{};
  GetWindowsDirectoryA(dir, MAX_PATH);
  const std::string fonts = std::string(dir) + "\\Fonts\\";
  ImFont* title = nullptr;
  if (GetFileAttributesA((fonts + "segoeui.ttf").c_str()) != INVALID_FILE_ATTRIBUTES) {
    io.Fonts->AddFontFromFileTTF((fonts + "segoeui.ttf").c_str(), 16.f * s);
    const std::string bold = fonts + "segoeuib.ttf";
    if (GetFileAttributesA(bold.c_str()) != INVALID_FILE_ATTRIBUTES)
      title = io.Fonts->AddFontFromFileTTF(bold.c_str(), 24.f * s);
  }
  ImGuiStyle& st = ImGui::GetStyle();
  st = ImGuiStyle();
  ImGui::StyleColorsDark(&st);
  st.WindowRounding = 10.f;
  st.ChildRounding = 8.f;
  st.FrameRounding = 6.f;
  st.PopupRounding = 8.f;
  st.GrabRounding = 6.f;
  st.TabRounding = 6.f;
  st.ScrollbarRounding = 8.f;
  st.WindowBorderSize = 0.f;
  st.WindowPadding = ImVec2(16.f, 14.f);
  st.FramePadding = ImVec2(10.f, 6.f);
  st.ItemSpacing = ImVec2(10.f, 8.f);
  st.ScaleAllSizes(s);
  const ImVec4 bg(0.075f, 0.085f, 0.105f, 0.96f), panel(0.13f, 0.145f, 0.17f, 1.f);
  const ImVec4 accent(0.16f, 0.72f, 0.42f, 1.f), accent_hi(0.22f, 0.82f, 0.50f, 1.f);
  ImVec4* c = st.Colors;
  c[ImGuiCol_WindowBg] = bg;
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.11f, 0.13f, 0.98f);
  c[ImGuiCol_FrameBg] = panel;
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.20f, 0.23f, 1.f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.21f, 0.23f, 0.27f, 1.f);
  c[ImGuiCol_Button] = panel;
  c[ImGuiCol_ButtonHovered] = ImVec4(0.19f, 0.21f, 0.25f, 1.f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.23f, 0.25f, 0.30f, 1.f);
  c[ImGuiCol_Header] = ImVec4(accent.x, accent.y, accent.z, 0.30f);
  c[ImGuiCol_HeaderHovered] = ImVec4(accent.x, accent.y, accent.z, 0.42f);
  c[ImGuiCol_HeaderActive] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
  c[ImGuiCol_Tab] = panel;
  c[ImGuiCol_TabHovered] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
  c[ImGuiCol_TabSelected] = ImVec4(accent.x, accent.y, accent.z, 0.40f);
  c[ImGuiCol_CheckMark] = accent_hi;
  c[ImGuiCol_SliderGrab] = accent;
  c[ImGuiCol_SliderGrabActive] = accent_hi;
  c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
  c[ImGuiCol_Text] = ImVec4(0.93f, 0.94f, 0.96f, 1.f);
  c[ImGuiCol_TextDisabled] = ImVec4(0.58f, 0.61f, 0.66f, 1.f);
  return title;
}

}  // namespace touchfc
