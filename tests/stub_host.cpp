// Stubs of the UI host for headless integration tests
#include <functional>
#include "../src/app/host.h"
namespace vs {
void ui_wake() {}
void ui_post(std::function<void()> fn) { fn(); }
ID3D11Device* d3d_device() { return nullptr; }
void* main_hwnd() { return nullptr; }
void host_show_window() {}
void host_hide_to_tray() {}
void host_minimize() {}
bool host_is_hidden() { return true; }
void host_quit() {}
void host_set_theme_dark(bool) {}
float host_dpi_scale() { return 1.0f; }
}
