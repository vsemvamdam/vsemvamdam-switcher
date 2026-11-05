#include "fsx.h"

#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <random>

#include "str.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace vs::fsx {

fs::path P(const std::string& s) {
#ifdef _WIN32
    return fs::path(widen(s));
#else
    return fs::path(s);
#endif
}

std::string U(const fs::path& p) {
#ifdef _WIN32
    return narrow(p.wstring());
#else
    return p.string();
#endif
}

bool exists(const std::string& p) { std::error_code ec; return !p.empty() && fs::exists(P(p), ec); }
bool is_dir(const std::string& p) { std::error_code ec; return !p.empty() && fs::is_directory(P(p), ec); }
bool is_file(const std::string& p) { std::error_code ec; return !p.empty() && fs::is_regular_file(P(p), ec); }

bool read_file(const std::string& p, std::string& out) {
    std::ifstream f(P(p), std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

bool write_file(const std::string& p, const std::string& data) {
    std::error_code ec;
    fs::path target = P(p);
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
    static std::mt19937 rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
    fs::path tmp = target;
    tmp += ".tmp" + std::to_string(rng() % 100000);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), (std::streamsize)data.size());
        if (!f) { f.close(); fs::remove(tmp, ec); return false; }
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        // rename over an existing file can fail on some filesystems; fall back to direct write
        fs::remove(tmp, ec);
        std::ofstream f(target, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), (std::streamsize)data.size());
        return (bool)f;
    }
    return true;
}

bool mkdirs(const std::string& p) { std::error_code ec; fs::create_directories(P(p), ec); return is_dir(p); }

#ifdef _WIN32
// Win32 tree operations that never follow junctions / symbolic links (they are removed or skipped as links).
static bool w_remove(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return RemoveDirectoryW(path.c_str()) != 0;  // the link itself, not its target
    bool ok = true;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            ok &= w_remove(path + L"\\" + n);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    ok &= RemoveDirectoryW(path.c_str()) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND;
    return ok;
}

static bool w_copy_file(const std::wstring& from, const std::wstring& to, std::string* err) {
    DWORD a = GetFileAttributesW(to.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(to.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
    if (CopyFileW(from.c_str(), to.c_str(), FALSE)) return true;
    if (err) *err = narrow(from) + ": copy error " + std::to_string(GetLastError());
    return false;
}

static bool w_copy_tree(const std::wstring& from, const std::wstring& to, std::string* err, int depth) {
    if (depth > 64) return false;
    CreateDirectoryW(to.c_str(), nullptr);
    bool ok = true;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring s = from + L"\\" + n, d = to + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // never descend into junctions
            ok &= w_copy_tree(s, d, err, depth + 1);
        } else {
            ok &= w_copy_file(s, d, err);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}
#endif

bool remove_all(const std::string& p) {
    if (p.empty() || !exists(p)) {
#ifdef _WIN32
        // a dangling link still has attributes
        if (!p.empty() && GetFileAttributesW(widen(p).c_str()) != INVALID_FILE_ATTRIBUTES) return w_remove(widen(p));
#endif
        return true;
    }
#ifdef _WIN32
    w_remove(widen(p));
    return !exists(p);
#else
    std::error_code ec;
    fs::remove_all(P(p), ec);
    return !exists(p);
#endif
}

bool remove_file(const std::string& p) {
#ifdef _WIN32
    std::wstring w = widen(p);
    DWORD a = GetFileAttributesW(w.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (a & FILE_ATTRIBUTE_DIRECTORY) return false;
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(w.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
    return DeleteFileW(w.c_str()) != 0;
#else
    std::error_code ec;
    fs::remove(P(p), ec);
    return !exists(p);
#endif
}

bool copy_any(const std::string& from, const std::string& to, std::string* err) {
    std::error_code ec;
    fs::path f = P(from), t = P(to);
    if (!fs::exists(f, ec)) { if (err) *err = from + ": not found"; return false; }
    if (t.has_parent_path()) fs::create_directories(t.parent_path(), ec);
#ifdef _WIN32
    if (fs::is_directory(f, ec)) {
        fs::create_directories(t, ec);
        return w_copy_tree(widen(from), widen(to), err, 0);
    }
    return w_copy_file(widen(from), widen(to), err);
#else
    if (fs::is_directory(f, ec)) {
        fs::create_directories(t, ec);
        bool ok = true;
        for (auto it = fs::recursive_directory_iterator(f, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ok = false; break; }
            if (it->is_symlink(ec)) { it.disable_recursion_pending(); continue; }
            fs::path rel = fs::relative(it->path(), f, ec);
            fs::path dst = t / rel;
            if (it->is_directory(ec)) { fs::create_directories(dst, ec); continue; }
            fs::create_directories(dst.parent_path(), ec);
            fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, ec);
            if (ec) { ok = false; if (err) *err = U(it->path()) + ": " + ec.message(); ec.clear(); }
        }
        return ok;
    }
    fs::copy_file(f, t, fs::copy_options::overwrite_existing, ec);
    if (ec) { if (err) *err = from + ": " + ec.message(); return false; }
    return true;
#endif
}

bool replace_dir(const std::string& tmp, const std::string& dir) {
    // dir -> dir.old, tmp -> dir, then drop dir.old; on failure put the old one back
    std::error_code ec;
    std::string old = dir + ".old";
    remove_all(old);
    bool had = exists(dir);
    if (had) {
        fs::rename(P(dir), P(old), ec);
        if (ec) return false;
    }
    fs::rename(P(tmp), P(dir), ec);
    if (ec) {
        if (had) { std::error_code e2; fs::rename(P(old), P(dir), e2); }
        return false;
    }
    if (had) remove_all(old);
    return true;
}

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return U(P(a) / P(b));
}
std::string parent(const std::string& p) { return U(P(p).parent_path()); }
std::string filename(const std::string& p) { return U(P(p).filename()); }

int64_t mtime_unix(const std::string& p) {
    std::error_code ec;
    auto t = fs::last_write_time(P(p), ec);
    if (ec) return 0;
    auto sys = std::chrono::time_point_cast<std::chrono::seconds>(t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return sys.time_since_epoch().count();
}

uint64_t file_size(const std::string& p) { std::error_code ec; auto s = fs::file_size(P(p), ec); return ec ? 0 : s; }

std::vector<std::string> list_dir(const std::string& p, bool dirs_only) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(P(p), fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (dirs_only && !it->is_directory(ec)) continue;
        out.push_back(U(it->path().filename()));
    }
    return out;
}

bool wildcard_match(const std::string& pat, const std::string& s) {
    size_t p = 0, i = 0, star = std::string::npos, mark = 0;
    while (i < s.size()) {
        if (p < pat.size() && (pat[p] == '?' || std::tolower((unsigned char)pat[p]) == std::tolower((unsigned char)s[i]))) { ++p; ++i; }
        else if (p < pat.size() && pat[p] == '*') { star = p++; mark = i; }
        else if (star != std::string::npos) { p = star + 1; i = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

std::vector<std::string> glob(const std::string& pattern) {
    // Split into components; expand components containing wildcards.
    std::string norm = replace_all(pattern, "/", "\\");
#ifndef _WIN32
    norm = replace_all(pattern, "\\", "/");
    const char sep = '/';
#else
    const char sep = '\\';
#endif
    if (norm.find_first_of("*?") == std::string::npos) return exists(pattern) ? std::vector<std::string>{pattern} : std::vector<std::string>{};
    auto parts = split(norm, sep);
    std::vector<std::string> cur = {""};
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string& part = parts[i];
        std::vector<std::string> next;
        for (auto& base : cur) {
            if (part.find_first_of("*?") == std::string::npos) {
                std::string n = base.empty() ? part : (i == 0 ? part : base + sep + part);
                if (i == 0 && part.empty()) n = std::string(1, sep);  // absolute unix path
                next.push_back(n);
            } else {
                std::string dir = base.empty() ? "." : base;
                for (auto& name : list_dir(dir))
                    if (wildcard_match(part, name)) next.push_back(base.empty() ? name : (base.back() == sep ? base + name : base + sep + name));
            }
        }
        cur.swap(next);
        if (cur.empty()) break;
    }
    std::vector<std::string> out;
    for (auto& c : cur) if (exists(c)) out.push_back(c);
    return out;
}

}  // namespace vs::fsx
