#include "fsx.h"

#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <random>

#include "str.h"

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

bool remove_all(const std::string& p) {
    std::error_code ec;
    if (!exists(p)) return true;
    // clear read-only attributes first
    if (is_dir(p)) {
        for (auto it = fs::recursive_directory_iterator(P(p), fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, ec);
        }
    } else {
        fs::permissions(P(p), fs::perms::owner_write, fs::perm_options::add, ec);
    }
    ec.clear();
    fs::remove_all(P(p), ec);
    return !exists(p);
}

bool remove_file(const std::string& p) {
    std::error_code ec;
    fs::permissions(P(p), fs::perms::owner_write, fs::perm_options::add, ec);
    ec.clear();
    fs::remove(P(p), ec);
    return !exists(p);
}

bool copy_any(const std::string& from, const std::string& to, std::string* err) {
    std::error_code ec;
    fs::path f = P(from), t = P(to);
    if (fs::is_directory(f, ec)) {
        fs::create_directories(t, ec);
        bool ok = true;
        for (auto it = fs::recursive_directory_iterator(f, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ok = false; break; }
            fs::path rel = fs::relative(it->path(), f, ec);
            fs::path dst = t / rel;
            if (it->is_directory(ec)) { fs::create_directories(dst, ec); continue; }
            fs::create_directories(dst.parent_path(), ec);
            fs::permissions(dst, fs::perms::owner_write, fs::perm_options::add, ec);
            ec.clear();
            fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, ec);
            if (ec) { ok = false; if (err) *err = U(it->path()) + ": " + ec.message(); ec.clear(); }
        }
        return ok;
    }
    if (!fs::exists(f, ec)) { if (err) *err = from + ": not found"; return false; }
    if (t.has_parent_path()) fs::create_directories(t.parent_path(), ec);
    fs::permissions(t, fs::perms::owner_write, fs::perm_options::add, ec);
    ec.clear();
    fs::copy_file(f, t, fs::copy_options::overwrite_existing, ec);
    if (ec) { if (err) *err = from + ": " + ec.message(); return false; }
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
