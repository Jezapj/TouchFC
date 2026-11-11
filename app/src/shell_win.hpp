#pragma once

// Overlay-mode shell: what to capture, tray icon, look. Present thread only.

#include "win_prelude.hpp"

#include <string>
#include <vector>

struct ImFont;

namespace touchfc {

struct ShellWindow {
  HWND hwnd = nullptr;
  std::string title;
  std::string app;  // exe stem: "chrome", "vlc"
  int score = 0;    // higher: more likely the match
};

// Visible, uncloaked, top-level windows big enough to hold a video, likeliest
// first: the last pick, then browsers and players, then z-order.
void list_capture_windows(std::vector<ShellWindow>& out, const std::string& last_title);

HICON shell_icon();  // drawn once: a ball on a pitch-green tile

inline constexpr UINT kTrayMsg = WM_APP + 1;
enum TrayCmd : UINT { kTrayMenu = 1, kTrayStop, kTrayQuit };
bool tray_add(HWND owner);
void tray_remove(HWND owner);
void tray_popup(HWND owner, bool capturing);

// Segoe UI, rounded dark theme. Call before the context's first frame.
// Returns the title font, or nullptr when Segoe UI is missing.
ImFont* apply_shell_style(float dpi_scale);

}  // namespace touchfc
