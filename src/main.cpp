// vsemvamdam-switcher — Win32 + Direct3D 11 host, message loop, tray, single instance.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "app/app.h"
#include "app/host.h"
#include "core/log.h"
#include "core/str.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "os/win.h"
#include "ui/theme.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

const wchar_t* kWndClass = L"vsemvamdam-switcher-main";
const wchar_t* kMutexName = L"Local\\vsemvamdam-switcher.single-instance";
constexpr UINT WM_APP_WAKE = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;
constexpr ULONG_PTR kCopyDataTag = 0x56535343;  // 'VSSC'
constexpr UINT kTrayUid = 1;

HWND g_hwnd = nullptr;
HINSTANCE g_hinst = nullptr;
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool g_flip_model = false;
UINT g_resize_w = 0, g_resize_h = 0;
bool g_hidden = false;
bool g_minimized = false;
bool g_quitting = false;
bool g_dark_titlebar = true;
float g_dpi_scale = 1.0f;
bool g_dpi_changed = false;
UINT g_taskbar_created = 0;
std::atomic<int> g_force_frames{4};

std::mutex g_post_mtx;
std::vector<std::function<void()>> g_posted;

void create_rtv() {
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb) {
        g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
        bb->Release();
    }
}
void cleanup_rtv() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }

bool create_device(HWND hwnd) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    for (int attempt = 0; attempt < 4 && !g_swap; ++attempt) {
        bool flip = attempt % 2 == 0;
        D3D_DRIVER_TYPE dt = attempt < 2 ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = flip ? (DXGI_SWAP_EFFECT)4 /*FLIP_DISCARD*/ : DXGI_SWAP_EFFECT_DISCARD;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, dt, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
        if (SUCCEEDED(hr)) { g_flip_model = flip; break; }
        g_swap = nullptr; g_dev = nullptr; g_ctx = nullptr;
    }
    if (!g_swap) return false;
    // Disable Alt+Enter fullscreen
    IDXGIFactory* f = nullptr;
    if (SUCCEEDED(g_swap->GetParent(IID_PPV_ARGS(&f))) && f) { f->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER); f->Release(); }
    create_rtv();
    return true;
}

void cleanup_device() {
    cleanup_rtv();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

void apply_titlebar() {
    if (!g_hwnd) return;
    BOOL dark = g_dark_titlebar ? TRUE : FALSE;
    DwmSetWindowAttribute(g_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    COLORREF cap = g_dark_titlebar ? RGB(20, 20, 22) : RGB(239, 236, 230);
    COLORREF txt = g_dark_titlebar ? RGB(237, 235, 231) : RGB(23, 23, 26);
    DwmSetWindowAttribute(g_hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof(cap));
    DwmSetWindowAttribute(g_hwnd, 36 /*DWMWA_TEXT_COLOR*/, &txt, sizeof(txt));
}

float dpi_for(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    UINT dpi = fn && hwnd ? fn(hwnd) : 96;
    if (!dpi) dpi = 96;
    return dpi / 96.0f;
}

// ---------------------------------------------------------------- Tray
void tray_add() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = kTrayUid;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy_s(nid.szTip, L"vsemvamdam switcher");
    Shell_NotifyIconW(NIM_ADD, &nid);
}
void tray_remove() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = kTrayUid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

HMENU build_menu(const std::vector<vs::TrayEntry>& items) {
    HMENU m = CreatePopupMenu();
    for (auto& e : items) {
        std::wstring t = vs::widen(e.text);
        if (e.id == 0 && e.children.empty() && e.text.empty()) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); continue; }
        UINT flags = MF_STRING | (e.checked ? MF_CHECKED : 0) | (e.disabled ? MF_GRAYED : 0);
        if (!e.children.empty()) AppendMenuW(m, flags | MF_POPUP, (UINT_PTR)build_menu(e.children), t.c_str());
        else AppendMenuW(m, flags, e.id, t.c_str());
    }
    return m;
}

void show_tray_menu() {
    auto items = vs::App::get().tray_menu();
    HMENU m = build_menu(items);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd > 0) vs::App::get().on_tray_command(cmd);
    g_force_frames = 4;
}

void show_window() {
    g_hidden = false;
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    else ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    g_force_frames = 4;
}

void save_placement() {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(g_hwnd, &wp)) return;
    RECT r = wp.rcNormalPosition;
    vs::App::get().on_window_placement(r.left, r.top, r.right - r.left, r.bottom - r.top, wp.showCmd == SW_SHOWMAXIMIZED);
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    if (g_taskbar_created && msg == g_taskbar_created) { tray_add(); return 0; }
    switch (msg) {
        case WM_SIZE:
            g_minimized = wp == SIZE_MINIMIZED;
            if (wp != SIZE_MINIMIZED) { g_resize_w = LOWORD(lp); g_resize_h = HIWORD(lp); g_force_frames = 4; }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize.x = (LONG)(760 * g_dpi_scale);
            mm->ptMinTrackSize.y = (LONG)(500 * g_dpi_scale);
            return 0;
        }
        case WM_DPICHANGED: {
            g_dpi_scale = HIWORD(wp) / 96.0f;
            g_dpi_changed = true;
            const RECT* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            g_force_frames = 4;
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_APP_WAKE:
            g_force_frames = std::max(g_force_frames.load(), 3);
            return 0;
        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case WM_LBUTTONUP:
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    if (g_hidden || IsIconic(hwnd) || GetForegroundWindow() != hwnd) show_window();
                    else vs::host_hide_to_tray();
                    break;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    show_tray_menu();
                    break;
                case NIN_BALLOONUSERCLICK:
                    show_window();
                    vs::ui_post([] { vs::App::get().on_notify_click(); });
                    break;
            }
            return 0;
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wp;
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::string> files;
            for (UINT i = 0; i < n; ++i) {
                wchar_t buf[MAX_PATH * 2];
                if (DragQueryFileW(drop, i, buf, MAX_PATH * 2)) files.push_back(vs::narrow(buf));
            }
            DragFinish(drop);
            if (!files.empty()) vs::ui_post([files] { vs::App::get().on_drop_files(files); });
            return 0;
        }
        case WM_COPYDATA: {
            auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
            if (cds && cds->dwData == kCopyDataTag && cds->lpData) {
                std::string cmd(static_cast<const char*>(cds->lpData), cds->cbData);
                vs::ui_post([cmd] { vs::App::get().on_external_command(cmd); });
                PostMessageW(hwnd, WM_APP_WAKE, 0, 0);
                return TRUE;
            }
            return FALSE;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE) vs::ui_post([] { vs::App::get().on_activated(); });
            break;
        case WM_EXITSIZEMOVE:
            save_placement();
            return 0;
        case WM_CLOSE:
            if (!g_quitting && vs::App::get().close_to_tray()) { save_placement(); vs::host_hide_to_tray(); return 0; }
            save_placement();
            DestroyWindow(hwnd);
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp) { save_placement(); vs::App::get().shutdown(); }
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;  // disable ALT menu beep
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool forward_to_running_instance(const std::string& cmdline) {
    HWND other = FindWindowW(kWndClass, nullptr);
    if (!other) return false;
    COPYDATASTRUCT cds{};
    cds.dwData = kCopyDataTag;
    cds.cbData = (DWORD)cmdline.size();
    cds.lpData = (void*)cmdline.data();
    DWORD_PTR res = 0;
    SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 3000, &res);
    return true;
}

std::vector<std::string> command_line_args() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> a;
    for (int i = 1; i < argc; ++i) a.push_back(vs::narrow(argv[i]));
    LocalFree(argv);
    return a;
}

std::string raw_args_tail() {
    // Everything after argv[0] in the original command line.
    std::wstring cl = GetCommandLineW();
    size_t i = 0;
    if (!cl.empty() && cl[0] == L'"') { i = cl.find(L'"', 1); i = i == std::wstring::npos ? cl.size() : i + 1; }
    else { while (i < cl.size() && cl[i] != L' ' && cl[i] != L'\t') ++i; }
    while (i < cl.size() && (cl[i] == L' ' || cl[i] == L'\t')) ++i;
    return vs::narrow(cl.substr(i));
}

}  // namespace

// ---------------------------------------------------------------- host API
namespace vs {
void ui_wake() {
    g_force_frames = std::max(g_force_frames.load(), 3);
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_WAKE, 0, 0);
}
void ui_post(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(g_post_mtx);
        g_posted.push_back(std::move(fn));
    }
    ui_wake();
}
ID3D11Device* d3d_device() { return g_dev; }
void* main_hwnd() { return g_hwnd; }
void host_show_window() { if (g_hwnd) show_window(); }
void host_hide_to_tray() {
    if (!g_hwnd) return;
    g_hidden = true;
    ShowWindow(g_hwnd, SW_HIDE);
}
void host_minimize() { if (g_hwnd && !g_hidden) ShowWindow(g_hwnd, SW_MINIMIZE); }
bool host_is_hidden() { return g_hidden || !g_hwnd || !IsWindowVisible(g_hwnd); }
void host_quit() {
    g_quitting = true;
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}
void host_set_theme_dark(bool dark) {
    if (g_dark_titlebar == dark && g_hwnd) return;
    g_dark_titlebar = dark;
    apply_titlebar();
}
float host_dpi_scale() { return g_dpi_scale; }
void host_notify(const std::string& title, const std::string& text) {
    if (!g_hwnd) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = kTrayUid;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    nid.hBalloonIcon = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, 0);
    std::wstring t = widen(title), b = widen(text);
    wcsncpy_s(nid.szInfoTitle, t.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, b.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}
}  // namespace vs

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int nShow) {
    g_hinst = hinst;
    auto args = command_line_args();
    std::string tail = raw_args_tail();
    vs::App& app = vs::App::get();

    // Pure command-line modes (no window)
    int cli_rc = 0;
    if (app.handle_cli_early(args, cli_rc)) return cli_rc;

    // Single instance
    HANDLE mtx = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (forward_to_running_instance(tail.empty() ? std::string("--show") : tail)) return 0;
    }

    // A switch requested from a shortcut while no instance is running: do it headless and exit.
    if (app.handle_cli_headless(args, cli_rc)) return cli_rc;

    SetProcessDPIAware();
    ImGui_ImplWin32_EnableDpiAwareness();

    if (!app.init(args)) return 1;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.hIcon = LoadIconW(hinst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(20, 20, 22));
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    auto pl = app.initial_placement();
    // Initial DPI from the primary monitor (adjusted again if the window lands elsewhere)
    {
        HDC dc = GetDC(nullptr);
        g_dpi_scale = GetDeviceCaps(dc, LOGPIXELSX) / 96.0f;
        ReleaseDC(nullptr, dc);
    }
    int w = pl.w > 0 ? pl.w : (int)(1040 * g_dpi_scale);
    int h = pl.h > 0 ? pl.h : (int)(680 * g_dpi_scale);
    int x = pl.w > 0 ? pl.x : CW_USEDEFAULT;
    int y = pl.w > 0 ? pl.y : CW_USEDEFAULT;
    // Make sure the saved position is still on a monitor
    if (pl.w > 0) {
        RECT r{x, y, x + w, y + h};
        if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) { x = CW_USEDEFAULT; y = CW_USEDEFAULT; }
    }
    g_hwnd = CreateWindowExW(0, kWndClass, L"vsemvamdam switcher", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, hinst, nullptr);
    if (!g_hwnd) return 1;
    g_dpi_scale = dpi_for(g_hwnd);
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    if (!create_device(g_hwnd)) {
        vs::os::message_box("vsemvamdam switcher", "Не удалось инициализировать Direct3D 11.", true);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    vs::ui::load_fonts();
    app.on_ready(g_dpi_scale);
    apply_titlebar();
    tray_add();
    DragAcceptFiles(g_hwnd, TRUE);
    // let files be dropped from a non-elevated Explorer when we run as administrator
    ChangeWindowMessageFilterEx(g_hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_hwnd, 0x0049 /*WM_COPYGLOBALDATA*/, MSGFLT_ALLOW, nullptr);

    bool start_hidden = app.start_hidden();
    if (!start_hidden) {
        ShowWindow(g_hwnd, pl.maximized ? SW_SHOWMAXIMIZED : (nShow == SW_SHOWMINNOACTIVE ? SW_SHOWNORMAL : nShow));
        UpdateWindow(g_hwnd);
    } else {
        g_hidden = true;
    }

    bool running = true;
    while (running) {
        // nothing is drawn while minimized / in the tray, so nothing can animate there either:
        // without this the loop would spin (e.g. a toast that only expires when drawn) and eat a CPU core
        bool animate = !g_hidden && !g_minimized && app.animating();
        if ((g_force_frames.load() <= 0 || g_hidden || g_minimized) && !animate) {
            DWORD timeout = INFINITE;
            if (!g_hidden && !g_minimized && ImGui::GetCurrentContext()) {
                if (ImGui::IsAnyItemHovered()) timeout = 60;
                else if (int t = app.tick_ms(); t > 0) timeout = (DWORD)t;  // e.g. Steam Guard code countdown
            }
            MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        MSG msg;
        bool had_msg = false;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
            if (msg.message != WM_TIMER) had_msg = true;
        }
        if (!running) break;
        if (had_msg) g_force_frames = std::max(g_force_frames.load(), 3);

        std::vector<std::function<void()>> posted;
        {
            std::lock_guard<std::mutex> lk(g_post_mtx);
            posted.swap(g_posted);
        }
        for (auto& f : posted) f();
        if (!posted.empty()) g_force_frames = std::max(g_force_frames.load(), 3);

        if (g_hidden || g_minimized) { g_force_frames = 0; continue; }

        if (g_resize_w != 0 && g_resize_h != 0) {
            cleanup_rtv();
            g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            create_rtv();
        }
        if (g_dpi_changed) {
            g_dpi_changed = false;
            app.on_dpi_changed(g_dpi_scale);
        }

        app.before_frame();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();
        float cc[4];
        vs::ui::clear_color(cc);
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, cc);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        HRESULT hr = g_swap->Present(1, 0);
        if (hr == DXGI_STATUS_OCCLUDED) Sleep(10);
        if (g_force_frames.load() > 0) --g_force_frames;
    }

    ShowWindow(g_hwnd, SW_HIDE);  // waiting for background work must not look like a hang
    tray_remove();
    app.shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device();
    DestroyWindow(g_hwnd);
    UnregisterClassW(kWndClass, hinst);
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    return 0;
}
