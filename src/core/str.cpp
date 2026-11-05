#include "str.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <ctime>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace vs {

std::string to_lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

bool istarts_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && iequals(s.substr(0, p.size()), p);
}
bool starts_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }
bool ends_with(std::string_view s, std::string_view x) { return s.size() >= x.size() && s.substr(s.size() - x.size()) == x; }

static void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

std::string fold_case_utf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp; int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 6 && i + 1 < s.size()) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); len = 2; }
        else if ((c >> 4) == 14 && i + 2 < s.size()) { cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); len = 3; }
        else if ((c >> 3) == 30 && i + 3 < s.size()) { cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); len = 4; }
        else { cp = c; len = 1; }
        i += len;
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        else if (cp >= 0x410 && cp <= 0x42F) cp += 0x20;      // А-Я
        else if (cp >= 0x400 && cp <= 0x40F) cp += 0x50;      // Ѐ-Џ (incl. Ё)
        else if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) cp += 0x20;
        if (cp == 0x451) cp = 0x435;                           // ё -> е for search
        append_utf8(out, cp);
    }
    return out;
}

bool icontains(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    std::string h = fold_case_utf8(hay), n = fold_case_utf8(needle);
    return h.find(n) != std::string::npos;
}

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return std::string(s.substr(a, b - a));
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> r;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            r.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return r;
}

std::string replace_all(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string ireplace_all(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) return s;
    std::string ls = to_lower(s), lf = to_lower(from);
    std::string out;
    size_t pos = 0, last = 0;
    while ((pos = ls.find(lf, last)) != std::string::npos) {
        out.append(s, last, pos - last);
        out.append(to);
        last = pos + from.size();
    }
    out.append(s, last, std::string::npos);
    return out;
}

std::string join(const std::vector<std::string>& v, std::string_view sep) {
    std::string r;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) r += sep;
        r += v[i];
    }
    return r;
}

std::string sanitize_filename(std::string_view s) {
    std::string r;
    for (unsigned char c : s) {
        if (c < 32 || std::string_view("\\/:*?\"<>|").find((char)c) != std::string_view::npos) r += '_';
        else r += (char)c;
    }
    while (!r.empty() && (r.back() == '.' || r.back() == ' ')) r.pop_back();
    if (r.empty()) r = "_";
    return r;
}

bool parse_u64(std::string_view s, uint64_t& out) {
    std::string t = trim(s);
    if (t.empty()) return false;
    auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), out);
    return ec == std::errc() && p == t.data() + t.size();
}

bool parse_i64(std::string_view s, int64_t& out) {
    std::string t = trim(s);
    if (t.empty()) return false;
    auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), out);
    return ec == std::errc() && p == t.data() + t.size();
}

const char* plural_ru(int64_t n, const char* one, const char* few, const char* many) {
    int64_t a = n < 0 ? -n : n;
    int64_t m10 = a % 10, m100 = a % 100;
    if (m10 == 1 && m100 != 11) return one;
    if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return few;
    return many;
}

std::string time_ago_ru(int64_t t, int64_t now) {
    if (t <= 0) return "давно";
    int64_t d = now - t;
    if (d < 60) return "только что";
    if (d < 3600) { int64_t m = d / 60; return std::to_string(m) + " " + plural_ru(m, "минуту", "минуты", "минут") + " назад"; }
    if (d < 86400) { int64_t h = d / 3600; return std::to_string(h) + " " + plural_ru(h, "час", "часа", "часов") + " назад"; }
    if (d < 86400 * 30) { int64_t dd = d / 86400; if (dd == 1) return "вчера"; return std::to_string(dd) + " " + plural_ru(dd, "день", "дня", "дней") + " назад"; }
    if (d < 86400 * 365) { int64_t mo = d / (86400 * 30); return std::to_string(mo) + " " + plural_ru(mo, "месяц", "месяца", "месяцев") + " назад"; }
    int64_t y = d / (86400 * 365);
    return std::to_string(y) + " " + plural_ru(y, "год", "года", "лет") + " назад";
}

std::string time_ago_en(int64_t t, int64_t now) {
    if (t <= 0) return "long ago";
    int64_t d = now - t;
    auto f = [](int64_t n, const char* u) { return std::to_string(n) + " " + u + (n == 1 ? "" : "s") + " ago"; };
    if (d < 60) return "just now";
    if (d < 3600) return f(d / 60, "minute");
    if (d < 86400) return f(d / 3600, "hour");
    if (d < 86400 * 30) return d / 86400 == 1 ? "yesterday" : f(d / 86400, "day");
    if (d < 86400 * 365) return f(d / (86400 * 30), "month");
    return f(d / (86400 * 365), "year");
}

int64_t unix_now() { return (int64_t)std::time(nullptr); }

#ifdef _WIN32
std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
#endif

}  // namespace vs
