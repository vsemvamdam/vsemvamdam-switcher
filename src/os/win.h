// vsemvamdam-switcher — thin Win32 helpers (UTF-8 in/out)
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace vs::os {

// ---------- Registry ----------
// Path form: "HKCU\\Software\\Valve\\Steam" (HKCU, HKLM, HKCR, HKU, HKEY_* accepted)
enum class RegView { Default, Wow64_32, Wow64_64 };

struct RegValue {
    std::string name;
    uint32_t type = 0;            // REG_SZ, REG_DWORD, REG_BINARY...
    std::vector<uint8_t> data;    // raw bytes
};

bool reg_get_string(const std::string& key, const std::string& name, std::string& out, RegView v = RegView::Default);
bool reg_get_dword(const std::string& key, const std::string& name, uint32_t& out, RegView v = RegView::Default);
bool reg_get_raw(const std::string& key, const std::string& name, RegValue& out, RegView v = RegView::Default);
bool reg_set_string(const std::string& key, const std::string& name, const std::string& val);
bool reg_set_dword(const std::string& key, const std::string& name, uint32_t val);
bool reg_set_raw(const std::string& key, const RegValue& val);
bool reg_delete_value(const std::string& key, const std::string& name);
bool reg_delete_tree(const std::string& key);
bool reg_key_exists(const std::string& key, RegView v = RegView::Default);
std::vector<std::string> reg_subkeys(const std::string& key);
std::vector<RegValue> reg_values(const std::string& key);
// Export/import a whole key tree (values + subkeys) as JSON-able structure
struct RegTree {
    std::string path;  // relative to the exported root ("" = root)
    std::vector<RegValue> values;
};
bool reg_export_tree(const std::string& key, std::vector<RegTree>& out);
bool reg_import_tree(const std::string& key, const std::vector<RegTree>& in, bool clear_first);

// ---------- Processes ----------
struct Proc {
    uint32_t pid = 0;
    uint32_t parent = 0;
    std::string exe;  // file name only, e.g. "steam.exe"
};
std::vector<Proc> list_processes();
bool any_running(const std::vector<std::string>& exe_names);
struct KillResult {
    int found = 0;
    int killed = 0;
    int access_denied = 0;
    bool timed_out = false;
};
// Kill every process whose exe matches one of names (case-insensitive, "*" wildcards allowed).
KillResult kill_processes(const std::vector<std::string>& exe_names, uint32_t timeout_ms);
// Ask top-level windows of matching processes to close (WM_CLOSE); returns count of windows signaled.
int close_windows_of(const std::vector<std::string>& exe_names);
bool wait_all_exit(const std::vector<std::string>& exe_names, uint32_t timeout_ms);
std::string exe_path_of_running(const std::string& exe_name);

// ---------- Launching ----------
bool is_elevated();
// Launch a program. If we are elevated and unelevated==true, start it through the shell (Explorer) so it runs as normal user.
bool launch(const std::string& exe, const std::string& args, const std::string& workdir, bool unelevated = true);
bool shell_open(const std::string& target, const std::string& args = "");
bool relaunch_self_elevated(const std::string& args);
std::string self_exe_path();

// ---------- Paths ----------
std::string expand_env(const std::string& s);  // %APPDATA% etc.
std::string appdata_roaming();
std::string appdata_local();
std::string desktop_dir();
std::string program_files_x86();
std::string program_files();
std::string programdata();
std::string user_profile();
std::string documents_dir();
std::string start_menu_programs();

// ---------- Shell ----------
bool clipboard_set(const std::string& text);
bool create_shortcut(const std::string& lnk_path, const std::string& target, const std::string& args,
                     const std::string& workdir, const std::string& icon_path, const std::string& description);
// Resolves a .lnk to its target path ("" if not a shortcut).
std::string resolve_shortcut(const std::string& lnk_path);
// Finds a Start Menu / Desktop shortcut by display name (without .lnk), returns its target.
std::string find_shortcut_target(const std::string& name);
bool set_run_at_startup(const std::string& name, const std::string& command, bool enable);
bool is_run_at_startup(const std::string& name);

// ---------- Icons ----------
// Extract an icon from an exe/dll/ico at the requested size as RGBA8.
bool extract_icon_rgba(const std::string& path, int size, int& w, int& h, std::vector<uint8_t>& rgba);
// Write a multi-size .ico from an RGBA square image.
bool write_ico(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba);

// ---------- Credentials / DPAPI ----------
struct Credential {
    std::string target, username, alias, comment;
    uint32_t persist = 2;  // CRED_PERSIST_LOCAL_MACHINE
    std::vector<uint8_t> blob;
};
bool cred_read(const std::string& target, Credential& out);   // generic credential
bool cred_write(const Credential& c);
bool cred_delete(const std::string& target);
bool dpapi_protect(const std::vector<uint8_t>& in, std::vector<uint8_t>& out);
bool dpapi_unprotect(const std::vector<uint8_t>& in, std::vector<uint8_t>& out);

// ---------- Network ----------
// HTTP(S) GET; returns false on failure. status receives HTTP status code.
bool http_get(const std::string& url, std::string& body, int* status = nullptr, uint32_t timeout_ms = 8000);

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;  // name, value
    std::string body;
    uint32_t timeout_ms = 15000;
    bool follow_redirects = true;
};
struct HttpResponse {
    bool ok = false;      // transport succeeded (any HTTP status)
    int status = 0;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;  // lower-cased names
    std::string error;    // transport error text
    std::string header(const std::string& lower_name) const {
        for (auto& [k, v] : headers)
            if (k == lower_name) return v;
        return {};
    }
};
HttpResponse http_request(const HttpRequest& r);

// ---------- UI helpers ----------
// Standard "Open file" dialog for images; returns "" if cancelled.
std::string pick_image_file(void* owner);
// Multi-select "Open file" dialog. filter is a Win32 filter list with '|' separators: "maFile|*.maFile;*.json|All|*.*".
std::vector<std::string> pick_files(void* owner, const std::string& title, const std::string& filter, bool multi);
// Folder picker; returns "" if cancelled.
std::string pick_folder(void* owner, const std::string& title);
void message_box(const std::string& title, const std::string& text, bool error = false);

}  // namespace vs::os
