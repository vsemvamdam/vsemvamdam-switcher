// vsemvamdam-switcher — interface between the Win32/D3D host and the app
#pragma once
#include <functional>
#include <string>
#include <vector>

struct ID3D11Device;

namespace vs {

// Wakes the UI thread (safe from any thread) so it renders a new frame.
void ui_wake();
// Runs fn on the UI thread before the next frame (safe from any thread).
void ui_post(std::function<void()> fn);

ID3D11Device* d3d_device();
void* main_hwnd();

// Window control from the app
void host_show_window();
void host_hide_to_tray();
void host_minimize();
bool host_is_hidden();
void host_quit();
void host_set_theme_dark(bool dark);  // title bar colours
float host_dpi_scale();

struct TrayEntry {
    int id = 0;               // command id (>0), 0 = separator
    std::string text;
    bool checked = false;
    bool disabled = false;
    std::vector<TrayEntry> children;  // submenu if not empty
};

}  // namespace vs
