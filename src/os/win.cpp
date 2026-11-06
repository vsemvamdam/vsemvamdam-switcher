#include "win.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <exdisp.h>
#include <objbase.h>
#include <shellapi.h>
#include <shldisp.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <commdlg.h>
#include <wincred.h>
#include <dpapi.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <thread>

#include "../core/image.h"
#include "../core/str.h"

namespace vs::os {

// ============================================================ Registry
static bool split_key(const std::string& key_in, HKEY& root, std::wstring& sub) {
    std::string key = key_in;
    if (istarts_with(key, "Computer\\")) key = key.substr(9);
    auto p = key.find('\\');
    std::string r = to_lower(p == std::string::npos ? key : key.substr(0, p));
    sub = p == std::string::npos ? L"" : widen(key.substr(p + 1));
    while (!sub.empty() && sub.back() == L'\\') sub.pop_back();
    if (r == "hkcu" || r == "hkey_current_user") root = HKEY_CURRENT_USER;
    else if (r == "hklm" || r == "hkey_local_machine") root = HKEY_LOCAL_MACHINE;
    else if (r == "hkcr" || r == "hkey_classes_root") root = HKEY_CLASSES_ROOT;
    else if (r == "hku" || r == "hkey_users") root = HKEY_USERS;
    else return false;
    return true;
}

static REGSAM view_flag(RegView v) {
    switch (v) {
        case RegView::Wow64_32: return KEY_WOW64_32KEY;
        case RegView::Wow64_64: return KEY_WOW64_64KEY;
        default: return 0;
    }
}

struct HKeyGuard {
    HKEY h = nullptr;
    ~HKeyGuard() { if (h) RegCloseKey(h); }
};

bool reg_get_raw(const std::string& key, const std::string& name, RegValue& out, RegView v) {
    HKEY root; std::wstring sub;
    if (!split_key(key, root, sub)) return false;
    HKeyGuard k;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_QUERY_VALUE | view_flag(v), &k.h) != ERROR_SUCCESS) return false;
    std::wstring wn = widen(name);
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(k.h, wn.c_str(), nullptr, &type, nullptr, &size) != ERROR_SUCCESS) return false;
    out.data.resize(size);
    if (size && RegQueryValueExW(k.h, wn.c_str(), nullptr, &type, out.data.data(), &size) != ERROR_SUCCESS) return false;
    out.data.resize(size);
    out.type = type;
    out.name = name;
    return true;
}

static std::string reg_bytes_to_string(const std::vector<uint8_t>& d) {
    std::wstring w(reinterpret_cast<const wchar_t*>(d.data()), d.size() / sizeof(wchar_t));
    while (!w.empty() && w.back() == L'\0') w.pop_back();
    return narrow(w);
}

bool reg_get_string(const std::string& key, const std::string& name, std::string& out, RegView v) {
    RegValue rv;
    if (!reg_get_raw(key, name, rv, v)) return false;
    if (rv.type == REG_SZ || rv.type == REG_EXPAND_SZ || rv.type == REG_MULTI_SZ) {
        out = reg_bytes_to_string(rv.data);
        return true;
    }
    if (rv.type == REG_DWORD && rv.data.size() >= 4) {
        uint32_t d; memcpy(&d, rv.data.data(), 4);
        out = std::to_string(d);
        return true;
    }
    return false;
}

bool reg_get_dword(const std::string& key, const std::string& name, uint32_t& out, RegView v) {
    RegValue rv;
    if (!reg_get_raw(key, name, rv, v)) return false;
    if (rv.type == REG_DWORD && rv.data.size() >= 4) { memcpy(&out, rv.data.data(), 4); return true; }
    if (rv.type == REG_SZ) {
        uint64_t u;
        if (parse_u64(reg_bytes_to_string(rv.data), u)) { out = (uint32_t)u; return true; }
    }
    return false;
}

static bool open_create(const std::string& key, HKeyGuard& k) {
    HKEY root; std::wstring sub;
    if (!split_key(key, root, sub)) return false;
    return RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &k.h, nullptr) == ERROR_SUCCESS;
}

bool reg_set_string(const std::string& key, const std::string& name, const std::string& val) {
    HKeyGuard k;
    if (!open_create(key, k)) return false;
    std::wstring w = widen(val);
    return RegSetValueExW(k.h, widen(name).c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(w.c_str()),
                          (DWORD)((w.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool reg_set_dword(const std::string& key, const std::string& name, uint32_t val) {
    HKeyGuard k;
    if (!open_create(key, k)) return false;
    DWORD d = val;
    return RegSetValueExW(k.h, widen(name).c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&d), sizeof(d)) == ERROR_SUCCESS;
}

bool reg_set_raw(const std::string& key, const RegValue& val) {
    HKeyGuard k;
    if (!open_create(key, k)) return false;
    return RegSetValueExW(k.h, widen(val.name).c_str(), 0, val.type, val.data.empty() ? nullptr : val.data.data(),
                          (DWORD)val.data.size()) == ERROR_SUCCESS;
}

bool reg_delete_value(const std::string& key, const std::string& name) {
    HKEY root; std::wstring sub;
    if (!split_key(key, root, sub)) return false;
    HKeyGuard k;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_SET_VALUE, &k.h) != ERROR_SUCCESS) return true;  // nothing to delete
    LSTATUS s = RegDeleteValueW(k.h, widen(name).c_str());
    return s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND;
}

bool reg_delete_tree(const std::string& key) {
    HKEY root; std::wstring sub;
    if (!split_key(key, root, sub) || sub.empty()) return false;  // never delete a hive root
    LSTATUS s = RegDeleteTreeW(root, sub.c_str());
    if (s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND) {
        RegDeleteKeyW(root, sub.c_str());
        return true;
    }
    return false;
}

bool reg_key_exists(const std::string& key, RegView v) {
    HKEY root; std::wstring sub;
    if (!split_key(key, root, sub)) return false;
    HKeyGuard k;
    return RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ | view_flag(v), &k.h) == ERROR_SUCCESS;
}

std::vector<std::string> reg_subkeys(const std::string& key) {
    std::vector<std::string> out;
    HKEY root; std::wstring sub;
    if (!split_key(key, root, sub)) return out;
    HKeyGuard k;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &k.h) != ERROR_SUCCESS) return out;
    wchar_t name[512];
    for (DWORD i = 0;; ++i) {
        DWORD len = 512;
        if (RegEnumKeyExW(k.h, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        out.push_back(narrow(std::wstring_view(name, len)));
    }
    return out;
}

std::vector<RegValue> reg_values(const std::string& key) {
    std::vector<RegValue> out;
    HKEY root; std::wstring sub;
    if (!split_key(key, root, sub)) return out;
    HKeyGuard k;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &k.h) != ERROR_SUCCESS) return out;
    DWORD maxName = 0, maxData = 0;
    RegQueryInfoKeyW(k.h, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &maxName, &maxData, nullptr, nullptr);
    std::vector<wchar_t> nbuf(maxName + 2);
    std::vector<uint8_t> dbuf(maxData + 2);
    for (DWORD i = 0;; ++i) {
        DWORD nl = (DWORD)nbuf.size(), dl = (DWORD)dbuf.size(), type = 0;
        LSTATUS s = RegEnumValueW(k.h, i, nbuf.data(), &nl, nullptr, &type, dbuf.data(), &dl);
        if (s == ERROR_MORE_DATA) {
            nbuf.resize(nbuf.size() * 2 + 64); dbuf.resize(dbuf.size() * 2 + 1024);
            --i; continue;
        }
        if (s != ERROR_SUCCESS) break;
        RegValue rv;
        rv.name = narrow(std::wstring_view(nbuf.data(), nl));
        rv.type = type;
        rv.data.assign(dbuf.begin(), dbuf.begin() + dl);
        out.push_back(std::move(rv));
    }
    return out;
}

static void export_rec(const std::string& base, const std::string& rel, std::vector<RegTree>& out, int depth) {
    if (depth > 32) return;
    std::string full = rel.empty() ? base : base + "\\" + rel;
    RegTree t;
    t.path = rel;
    t.values = reg_values(full);
    out.push_back(std::move(t));
    for (auto& s : reg_subkeys(full)) export_rec(base, rel.empty() ? s : rel + "\\" + s, out, depth + 1);
}

bool reg_export_tree(const std::string& key, std::vector<RegTree>& out) {
    out.clear();
    if (!reg_key_exists(key)) return false;
    export_rec(key, "", out, 0);
    return true;
}

bool reg_import_tree(const std::string& key, const std::vector<RegTree>& in, bool clear_first) {
    if (clear_first) reg_delete_tree(key);
    bool ok = true;
    for (auto& t : in) {
        std::string full = t.path.empty() ? key : key + "\\" + t.path;
        HKeyGuard k;
        if (!open_create(full, k)) { ok = false; continue; }
        for (auto& v : t.values) ok &= reg_set_raw(full, v);
    }
    return ok;
}

// ============================================================ Processes
static bool glob_match(const char* p, const char* s) {
    // case-insensitive glob with * and ?
    while (*p) {
        if (*p == '*') {
            while (*p == '*') ++p;
            if (!*p) return true;
            for (; *s; ++s)
                if (glob_match(p, s)) return true;
            return false;
        }
        if (!*s) return false;
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return false;
        ++p; ++s;
    }
    return !*s;
}

static bool name_matches(const std::string& exe, const std::vector<std::string>& names) {
    for (auto& n : names) {
        std::string nn = n;
        if (!icontains(nn, ".exe") && nn.find('*') == std::string::npos) nn += ".exe";
        if (glob_match(nn.c_str(), exe.c_str())) return true;
    }
    return false;
}

std::vector<Proc> list_processes() {
    std::vector<Proc> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            out.push_back({pe.th32ProcessID, pe.th32ParentProcessID, narrow(pe.szExeFile)});
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

bool any_running(const std::vector<std::string>& names) {
    for (auto& p : list_processes())
        if (name_matches(p.exe, names)) return true;
    return false;
}

KillResult kill_processes(const std::vector<std::string>& names, uint32_t timeout_ms) {
    KillResult r;
    DWORD self = GetCurrentProcessId();
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    std::set<uint32_t> denied;
    for (int round = 0; round < 6; ++round) {
        std::vector<HANDLE> handles;
        int found_now = 0;
        for (auto& p : list_processes()) {
            if (p.pid == self || p.pid == 0 || !name_matches(p.exe, names)) continue;
            if (denied.count(p.pid)) continue;
            ++found_now;
            if (round == 0) ++r.found;
            HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, p.pid);
            if (!h) {
                if (GetLastError() == ERROR_ACCESS_DENIED) { denied.insert(p.pid); ++r.access_denied; }
                continue;
            }
            if (TerminateProcess(h, 0)) {
                ++r.killed;
                handles.push_back(h);
            } else {
                if (GetLastError() == ERROR_ACCESS_DENIED) { denied.insert(p.pid); ++r.access_denied; }
                CloseHandle(h);
            }
        }
        // Wait for terminations to complete (in chunks of 64)
        for (size_t i = 0; i < handles.size(); i += MAXIMUM_WAIT_OBJECTS) {
            DWORD cnt = (DWORD)std::min<size_t>(MAXIMUM_WAIT_OBJECTS, handles.size() - i);
            ULONGLONG now = GetTickCount64();
            DWORD left = now >= deadline ? 0 : (DWORD)(deadline - now);
            WaitForMultipleObjects(cnt, handles.data() + i, TRUE, left);
        }
        for (HANDLE h : handles) CloseHandle(h);
        if (found_now == 0) return r;
        if (GetTickCount64() >= deadline) break;
        Sleep(round == 0 ? 30 : 80);  // children (e.g. webhelpers) may still be dying
    }
    // final check
    for (auto& p : list_processes())
        if (p.pid != self && name_matches(p.exe, names) && !denied.count(p.pid)) { r.timed_out = true; break; }
    return r;
}

struct EnumCtx { std::set<DWORD> pids; int count = 0; };
static BOOL CALLBACK enum_close(HWND hwnd, LPARAM lp) {
    auto* c = reinterpret_cast<EnumCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (c->pids.count(pid) && GetWindow(hwnd, GW_OWNER) == nullptr) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        ++c->count;
    }
    return TRUE;
}

int close_windows_of(const std::vector<std::string>& names) {
    EnumCtx c;
    for (auto& p : list_processes())
        if (name_matches(p.exe, names)) c.pids.insert(p.pid);
    if (c.pids.empty()) return 0;
    EnumWindows(enum_close, reinterpret_cast<LPARAM>(&c));
    return c.count;
}

bool wait_all_exit(const std::vector<std::string>& names, uint32_t timeout_ms) {
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        if (!any_running(names)) return true;
        if (GetTickCount64() >= deadline) return false;
        Sleep(50);
    }
}

std::string exe_path_of_running(const std::string& exe_name) {
    for (auto& p : list_processes()) {
        if (!iequals(p.exe, exe_name)) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid);
        if (!h) continue;
        wchar_t buf[MAX_PATH * 2];
        DWORD len = MAX_PATH * 2;
        std::string res;
        if (QueryFullProcessImageNameW(h, 0, buf, &len)) res = narrow(std::wstring_view(buf, len));
        CloseHandle(h);
        if (!res.empty()) return res;
    }
    return {};
}

// ============================================================ Launching
bool is_elevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e{};
    DWORD sz = 0;
    bool r = GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &sz) && e.TokenIsElevated;
    CloseHandle(tok);
    return r;
}

// Launch via the desktop's Explorer instance so the child is not elevated
// (technique described by Raymond Chen: IShellWindows -> IShellDispatch2::ShellExecute).
static bool launch_via_explorer(const std::wstring& exe, const std::wstring& args, const std::wstring& dir) {
    bool ok = false;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellWindows* sw = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sw)))) {
        VARIANT vEmpty; VariantInit(&vEmpty);
        long hwnd = 0;
        IDispatch* disp = nullptr;
        if (sw->FindWindowSW(&vEmpty, &vEmpty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp) == S_OK && disp) {
            IServiceProvider* sp = nullptr;
            if (SUCCEEDED(disp->QueryInterface(IID_PPV_ARGS(&sp)))) {
                IShellBrowser* sb = nullptr;
                if (SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb)))) {
                    IShellView* sv = nullptr;
                    if (SUCCEEDED(sb->QueryActiveShellView(&sv))) {
                        IDispatch* bgDisp = nullptr;
                        if (SUCCEEDED(sv->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&bgDisp)))) {
                            IShellFolderViewDual* fvd = nullptr;
                            if (SUCCEEDED(bgDisp->QueryInterface(IID_PPV_ARGS(&fvd)))) {
                                IDispatch* appDisp = nullptr;
                                if (SUCCEEDED(fvd->get_Application(&appDisp))) {
                                    IShellDispatch2* sd = nullptr;
                                    if (SUCCEEDED(appDisp->QueryInterface(IID_PPV_ARGS(&sd)))) {
                                        BSTR bFile = SysAllocString(exe.c_str());
                                        VARIANT vArgs, vDir, vOp, vShow;
                                        VariantInit(&vArgs); vArgs.vt = VT_BSTR; vArgs.bstrVal = SysAllocString(args.c_str());
                                        VariantInit(&vDir); vDir.vt = VT_BSTR; vDir.bstrVal = SysAllocString(dir.c_str());
                                        VariantInit(&vOp); vOp.vt = VT_BSTR; vOp.bstrVal = SysAllocString(L"open");
                                        VariantInit(&vShow); vShow.vt = VT_I4; vShow.lVal = SW_SHOWNORMAL;
                                        ok = SUCCEEDED(sd->ShellExecute(bFile, vArgs, vDir, vOp, vShow));
                                        SysFreeString(bFile);
                                        VariantClear(&vArgs); VariantClear(&vDir); VariantClear(&vOp);
                                        sd->Release();
                                    }
                                    appDisp->Release();
                                }
                                fvd->Release();
                            }
                            bgDisp->Release();
                        }
                        sv->Release();
                    }
                    sb->Release();
                }
                sp->Release();
            }
            disp->Release();
        }
        sw->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return ok;
}

bool launch(const std::string& exe, const std::string& args, const std::string& workdir, bool unelevated) {
    std::wstring wexe = widen(exe), wargs = widen(args), wdir = widen(workdir);
    if (unelevated && is_elevated()) {
        if (launch_via_explorer(wexe, wargs, wdir)) return true;
    }
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = wexe.c_str();
    sei.lpParameters = wargs.empty() ? nullptr : wargs.c_str();
    sei.lpDirectory = wdir.empty() ? nullptr : wdir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

bool shell_open(const std::string& target, const std::string& args) {
    std::wstring t = widen(target), a = widen(args);
    auto r = (INT_PTR)ShellExecuteW(nullptr, L"open", t.c_str(), a.empty() ? nullptr : a.c_str(), nullptr, SW_SHOWNORMAL);
    return r > 32;
}

std::string self_exe_path() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    return narrow(std::wstring_view(buf, n));
}

bool relaunch_self_elevated(const std::string& args) {
    std::wstring exe = widen(self_exe_path()), a = widen(args);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = a.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

// ============================================================ Paths
std::string expand_env(const std::string& s) {
    std::wstring w = widen(s);
    DWORD n = ExpandEnvironmentStringsW(w.c_str(), nullptr, 0);
    if (!n) return s;
    std::wstring out(n, L'\0');
    ExpandEnvironmentStringsW(w.c_str(), out.data(), n);
    while (!out.empty() && out.back() == L'\0') out.pop_back();
    return narrow(out);
}

static std::string known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::string r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p))) r = narrow(p);
    if (p) CoTaskMemFree(p);
    return r;
}
std::string appdata_roaming() { return known(FOLDERID_RoamingAppData); }
std::string appdata_local() { return known(FOLDERID_LocalAppData); }
std::string desktop_dir() { return known(FOLDERID_Desktop); }
std::string program_files_x86() { return known(FOLDERID_ProgramFilesX86); }
std::string program_files() { return known(FOLDERID_ProgramFiles); }
std::string programdata() { return known(FOLDERID_ProgramData); }
std::string user_profile() { return known(FOLDERID_Profile); }
std::string documents_dir() { return known(FOLDERID_Documents); }
std::string start_menu_programs() { return known(FOLDERID_Programs); }

// ============================================================ Shell
bool clipboard_set(const std::string& text) {
    std::wstring w = widen(text);
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    bool ok = false;
    if (h) {
        void* p = GlobalLock(h);
        memcpy(p, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    return ok;
}

bool create_shortcut(const std::string& lnk, const std::string& target, const std::string& args, const std::string& workdir,
                     const std::string& icon, const std::string& desc) {
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    IShellLinkW* sl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
        sl->SetPath(widen(target).c_str());
        sl->SetArguments(widen(args).c_str());
        if (!workdir.empty()) sl->SetWorkingDirectory(widen(workdir).c_str());
        if (!icon.empty()) sl->SetIconLocation(widen(icon).c_str(), 0);
        if (!desc.empty()) sl->SetDescription(widen(desc).c_str());
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
            ok = SUCCEEDED(pf->Save(widen(lnk).c_str(), TRUE));
            pf->Release();
        }
        sl->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return ok;
}

std::string resolve_shortcut(const std::string& lnk) {
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::string out;
    IShellLinkW* sl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
            if (SUCCEEDED(pf->Load(widen(lnk).c_str(), STGM_READ))) {
                wchar_t buf[MAX_PATH * 2];
                if (SUCCEEDED(sl->GetPath(buf, MAX_PATH * 2, nullptr, SLGP_RAWPATH))) out = expand_env(narrow(buf));
            }
            pf->Release();
        }
        sl->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return out;
}

static std::string find_lnk_in(const std::wstring& dir, const std::wstring& name, int depth) {
    if (depth > 3) return {};
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string found;
    do {
        std::wstring fn = fd.cFileName;
        if (fn == L"." || fn == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            found = find_lnk_in(dir + L"\\" + fn, name, depth + 1);
        } else if (_wcsicmp(fn.c_str(), (name + L".lnk").c_str()) == 0) {
            found = resolve_shortcut(narrow(dir + L"\\" + fn));
        }
    } while (found.empty() && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

std::string find_shortcut_target(const std::string& name) {
    std::vector<std::string> dirs = {start_menu_programs(), known(FOLDERID_CommonPrograms), desktop_dir(), known(FOLDERID_PublicDesktop)};
    for (auto& n : split(name, '|')) {
        for (auto& d : dirs) {
            if (d.empty()) continue;
            std::string t = find_lnk_in(widen(d), widen(trim(n)), 0);
            if (!t.empty()) return t;
        }
    }
    return {};
}

static const char* kRunKey = "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
bool set_run_at_startup(const std::string& name, const std::string& command, bool enable) {
    if (enable) return reg_set_string(kRunKey, name, command);
    return reg_delete_value(kRunKey, name);
}
bool is_run_at_startup(const std::string& name) {
    std::string s;
    return reg_get_string(kRunKey, name, s) && !s.empty();
}

// ============================================================ Icons
bool extract_icon_rgba(const std::string& path, int size, int& w, int& h, std::vector<uint8_t>& rgba) {
    std::wstring wp = widen(path);
    HICON hi = nullptr;
    UINT id = 0;
    UINT got = PrivateExtractIconsW(wp.c_str(), 0, size, size, &hi, &id, 1, 0);
    if (got == 0 || got == 0xFFFFFFFF || !hi) {
        SHFILEINFOW fi{};
        if (!SHGetFileInfoW(wp.c_str(), 0, &fi, sizeof(fi), SHGFI_ICON | SHGFI_LARGEICON) || !fi.hIcon) return false;
        hi = fi.hIcon;
    }
    ICONINFO ii{};
    bool ok = false;
    if (GetIconInfo(hi, &ii) && ii.hbmColor) {
        BITMAP bm{};
        GetObject(ii.hbmColor, sizeof(bm), &bm);
        w = bm.bmWidth; h = bm.bmHeight;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<uint8_t> px((size_t)w * h * 4);
        HDC dc = GetDC(nullptr);
        if (GetDIBits(dc, ii.hbmColor, 0, h, px.data(), &bi, DIB_RGB_COLORS)) {
            bool hasAlpha = false;
            for (size_t i = 3; i < px.size(); i += 4) if (px[i]) { hasAlpha = true; break; }
            if (!hasAlpha && ii.hbmMask) {
                std::vector<uint8_t> mk((size_t)w * h * 4);
                if (GetDIBits(dc, ii.hbmMask, 0, h, mk.data(), &bi, DIB_RGB_COLORS))
                    for (size_t i = 0; i < px.size(); i += 4) px[i + 3] = mk[i] ? 0 : 255;
                else
                    for (size_t i = 0; i < px.size(); i += 4) px[i + 3] = 255;
            }
            rgba.resize(px.size());
            for (size_t i = 0; i < px.size(); i += 4) {
                rgba[i] = px[i + 2]; rgba[i + 1] = px[i + 1]; rgba[i + 2] = px[i]; rgba[i + 3] = px[i + 3];
            }
            ok = true;
        }
        ReleaseDC(nullptr, dc);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    DestroyIcon(hi);
    return ok;
}

bool write_ico(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> file = img::make_ico(w, h, rgba);
    if (file.empty()) return false;
    HANDLE f = CreateFileW(widen(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    bool ok = WriteFile(f, file.data(), (DWORD)file.size(), &wr, nullptr) && wr == file.size();
    CloseHandle(f);
    return ok;
}

// ============================================================ Credentials / DPAPI
bool cred_read(const std::string& target, Credential& out) {
    PCREDENTIALW c = nullptr;
    std::wstring t = widen(target);
    if (!CredReadW(t.c_str(), CRED_TYPE_GENERIC, 0, &c) || !c) return false;
    out.target = narrow(c->TargetName ? c->TargetName : L"");
    out.username = narrow(c->UserName ? c->UserName : L"");
    out.alias = narrow(c->TargetAlias ? c->TargetAlias : L"");
    out.comment = narrow(c->Comment ? c->Comment : L"");
    out.persist = c->Persist;
    out.blob.assign(c->CredentialBlob, c->CredentialBlob + c->CredentialBlobSize);
    CredFree(c);
    return true;
}

bool cred_write(const Credential& cr) {
    std::wstring t = widen(cr.target), u = widen(cr.username), a = widen(cr.alias), cm = widen(cr.comment);
    CREDENTIALW c{};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = t.data();
    c.UserName = u.empty() ? nullptr : u.data();
    c.TargetAlias = a.empty() ? nullptr : a.data();
    c.Comment = cm.empty() ? nullptr : cm.data();
    c.Persist = cr.persist ? cr.persist : CRED_PERSIST_LOCAL_MACHINE;
    std::vector<uint8_t> blob = cr.blob;
    c.CredentialBlobSize = (DWORD)blob.size();
    c.CredentialBlob = blob.empty() ? nullptr : blob.data();
    return CredWriteW(&c, 0) != FALSE;
}

bool cred_delete(const std::string& target) {
    std::wstring t = widen(target);
    if (CredDeleteW(t.c_str(), CRED_TYPE_GENERIC, 0)) return true;
    return GetLastError() == ERROR_NOT_FOUND;
}

bool dpapi_protect(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    DATA_BLOB a{(DWORD)in.size(), const_cast<BYTE*>(in.data())}, b{};
    if (!CryptProtectData(&a, L"vsemvamdam-switcher", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &b)) return false;
    out.assign(b.pbData, b.pbData + b.cbData);
    LocalFree(b.pbData);
    return true;
}

bool dpapi_unprotect(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    DATA_BLOB a{(DWORD)in.size(), const_cast<BYTE*>(in.data())}, b{};
    if (!CryptUnprotectData(&a, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &b)) return false;
    out.assign(b.pbData, b.pbData + b.cbData);
    SecureZeroMemory(b.pbData, b.cbData);
    LocalFree(b.pbData);
    return true;
}

// ============================================================ Network
HttpResponse http_request(const HttpRequest& req) {
    HttpResponse res;
    std::wstring wurl = widen(req.url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[4096], extra[8192];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 8192;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { res.error = "bad url"; return res; }
    std::wstring object(uc.lpszUrlPath, uc.dwUrlPathLength);
    object += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    std::wstring agent = L"vsemvamdam-switcher/1.0";
    std::wstring hdrs;
    for (auto& [k, v] : req.headers) {
        if (iequals(k, "user-agent")) { agent = widen(v); continue; }
        hdrs += widen(k) + L": " + widen(v) + L"\r\n";
    }
    HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { res.error = "WinHttpOpen failed"; return res; }
    int to = (int)req.timeout_ms;
    WinHttpSetTimeouts(s, to, to, to, to);
    HINTERNET c = WinHttpConnect(s, std::wstring(uc.lpszHostName, uc.dwHostNameLength).c_str(), uc.nPort, 0);
    if (c) {
        DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        std::wstring method = widen(req.method);
        HINTERNET r = WinHttpOpenRequest(c, method.c_str(), object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (r) {
            // cookies are passed explicitly by the caller; never let WinHTTP keep or replay any
            DWORD feat = WINHTTP_DISABLE_COOKIES | (req.follow_redirects ? 0 : WINHTTP_DISABLE_REDIRECTS);
            WinHttpSetOption(r, WINHTTP_OPTION_DISABLE_FEATURE, &feat, sizeof(feat));
            BOOL sent = WinHttpSendRequest(r, hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(), hdrs.empty() ? 0 : (DWORD)-1L,
                                           req.body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)req.body.data(), (DWORD)req.body.size(),
                                           (DWORD)req.body.size(), 0);
            if (sent && WinHttpReceiveResponse(r, nullptr)) {
                DWORD code = 0, sz = sizeof(code);
                WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz,
                                    WINHTTP_NO_HEADER_INDEX);
                res.status = (int)code;
                DWORD hsz = 0;
                WinHttpQueryHeaders(r, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &hsz, WINHTTP_NO_HEADER_INDEX);
                if (hsz > 0) {
                    std::wstring raw(hsz / sizeof(wchar_t) + 1, L'\0');
                    if (WinHttpQueryHeaders(r, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &hsz, WINHTTP_NO_HEADER_INDEX)) {
                        raw.resize(hsz / sizeof(wchar_t));
                        for (auto& line : split(narrow(raw), '\n')) {
                            size_t colon = line.find(':');
                            if (colon == std::string::npos) continue;
                            res.headers.push_back({to_lower(trim(line.substr(0, colon))), trim(line.substr(colon + 1))});
                        }
                    }
                }
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
                    size_t old = res.body.size();
                    res.body.resize(old + avail);
                    DWORD read = 0;
                    if (!WinHttpReadData(r, res.body.data() + old, avail, &read)) { res.body.resize(old); break; }
                    res.body.resize(old + read);
                    if (res.body.size() > 32 * 1024 * 1024) break;
                }
                res.ok = true;
            } else {
                res.error = "WinHTTP " + std::to_string(GetLastError());
            }
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(c);
    } else {
        res.error = "connect failed";
    }
    WinHttpCloseHandle(s);
    return res;
}

bool http_get(const std::string& url, std::string& body, int* status, uint32_t timeout_ms) {
    HttpRequest rq;
    rq.url = url;
    rq.timeout_ms = timeout_ms;
    HttpResponse rs = http_request(rq);
    body = std::move(rs.body);
    if (status) *status = rs.status;
    return rs.ok && rs.status >= 200 && rs.status < 300;
}

// ============================================================ UI
std::string pick_image_file(void* owner) {
    wchar_t buf[MAX_PATH * 2] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = (HWND)owner;
    ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return {};
    return narrow(buf);
}

std::vector<std::string> pick_files(void* owner, const std::string& title, const std::string& filter, bool multi) {
    std::vector<wchar_t> buf(32768, 0);
    std::wstring f = widen(filter);
    for (auto& ch : f)
        if (ch == L'|') ch = L'\0';
    f.push_back(L'\0');
    f.push_back(L'\0');
    std::wstring t = widen(title);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = (HWND)owner;
    ofn.lpstrFilter = f.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.lpstrTitle = t.empty() ? nullptr : t.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | (multi ? OFN_ALLOWMULTISELECT : 0);
    std::vector<std::string> out;
    if (!GetOpenFileNameW(&ofn)) return out;
    std::wstring first(buf.data());
    const wchar_t* p = buf.data() + first.size() + 1;
    if (!*p) {
        out.push_back(narrow(first));
    } else {
        while (*p) {
            std::wstring name(p);
            out.push_back(narrow(first + L"\\" + name));
            p += name.size() + 1;
        }
    }
    return out;
}

std::string pick_folder(void* owner, const std::string& title) {
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::string out;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) && dlg) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        if (!title.empty()) dlg->SetTitle(widen(title).c_str());
        if (SUCCEEDED(dlg->Show((HWND)owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                    out = narrow(p);
                    CoTaskMemFree(p);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return out;
}

void message_box(const std::string& title, const std::string& text, bool error) {
    MessageBoxW(nullptr, widen(text).c_str(), widen(title).c_str(), MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}

}  // namespace vs::os
