#include "basic_core.h"

#include <cctype>
#include <chrono>
#include <random>
#include <regex>

#include "str.h"

namespace vs::basic {

Spec parse_spec(const std::string& raw) {
    Spec s;
    std::string r = trim(raw);
    if (istarts_with(r, "REG:")) {
        s.kind = Spec::Reg;
        std::string rest = r.substr(4);
        size_t c = rest.rfind(':');
        // ignore a drive-letter style colon at position 1 (not expected for registry paths)
        if (c != std::string::npos && c > 1) {
            s.path = rest.substr(0, c);
            s.value_name = rest.substr(c + 1);
        } else {
            s.path = rest;
        }
        return s;
    }
    if (istarts_with(r, "JSON_")) {
        size_t a = r.find("::");
        size_t b = a == std::string::npos ? std::string::npos : r.find("::", a + 2);
        if (a != std::string::npos && b != std::string::npos) {
            s.kind = Spec::Json;
            std::string mode = r.substr(0, a);
            s.path = r.substr(a + 2, b - a - 2);
            s.json_path = r.substr(b + 2);
            // "JSON_SELECT_FIRST," -> mode + delimiter
            if (istarts_with(mode, "JSON_SELECT_FIRST")) {
                s.json_mode = "JSON_SELECT_FIRST";
                s.delimiter = mode.substr(17);
                if (s.delimiter.empty()) s.delimiter = ",";
            } else if (istarts_with(mode, "JSON_EMPTY_VALUE")) {
                s.json_mode = "JSON_EMPTY_VALUE";
            } else {
                s.json_mode = "JSON_SELECT";
            }
            return s;
        }
    }
    s.kind = Spec::File;
    s.path = r;
    return s;
}

std::string expand_vars(const std::string& s, const std::map<std::string, std::string>& vars) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '%') {
            size_t j = s.find('%', i + 1);
            if (j != std::string::npos) {
                std::string name = to_lower(s.substr(i + 1, j - i - 1));
                auto it = vars.find(name);
                if (it != vars.end()) {
                    out += it->second;
                    i = j + 1;
                    continue;
                }
            }
        }
        out += s[i++];
    }
    return out;
}

const nlohmann::json* json_get(const nlohmann::json& root, const std::string& path) {
    const nlohmann::json* cur = &root;
    for (auto& part : split(path, '.')) {
        if (part.empty()) continue;
        if (cur->is_object()) {
            auto it = cur->find(part);
            if (it == cur->end()) return nullptr;
            cur = &*it;
        } else if (cur->is_array()) {
            uint64_t idx;
            if (!parse_u64(part, idx) || idx >= cur->size()) return nullptr;
            cur = &(*cur)[idx];
        } else {
            return nullptr;
        }
    }
    return cur;
}

bool json_set(nlohmann::json& root, const std::string& path, const nlohmann::json& value) {
    nlohmann::json* cur = &root;
    auto parts = split(path, '.');
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string& part = parts[i];
        if (part.empty()) continue;
        bool last = i + 1 == parts.size();
        if (!cur->is_object()) {
            if (cur->is_null()) *cur = nlohmann::json::object();
            else return false;
        }
        if (last) {
            (*cur)[part] = value;
            return true;
        }
        cur = &(*cur)[part];
    }
    *cur = value;
    return true;
}

std::string first_of(const std::string& s, const std::string& delim) {
    if (delim.empty()) return s;
    size_t p = s.find(delim);
    return trim(p == std::string::npos ? s : s.substr(0, p));
}

static std::string literal_hint(const std::string& pat) {
    // longest run of plain literal characters (used to pre-filter lines)
    std::string best, cur;
    for (size_t i = 0; i < pat.size(); ++i) {
        char c = pat[i];
        if (c == '\\') {
            if (i + 1 < pat.size() && !std::isalnum((unsigned char)pat[i + 1])) { cur += pat[i + 1]; ++i; continue; }
            if (cur.size() > best.size()) best = cur;
            cur.clear();
            ++i;
            continue;
        }
        if (std::string_view("^$.|?*+()[]{}").find(c) != std::string_view::npos) {
            // a quantifier makes the previous char optional
            if ((c == '?' || c == '*' || c == '{') && !cur.empty()) cur.pop_back();
            if (cur.size() > best.size()) best = cur;
            cur.clear();
            if (c == '[') { size_t e = pat.find(']', i + 1); if (e != std::string::npos) i = e; }
            if (c == '{') { size_t e = pat.find('}', i + 1); if (e != std::string::npos) i = e; }
            continue;
        }
        cur += c;
    }
    if (cur.size() > best.size()) best = cur;
    return best;
}

std::string regex_id(const std::string& text, const std::string& pattern_in) {
    std::string pat = pattern_in;
    if (pat == "EMAIL_REGEX") pat = R"(([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}))";
    auto flags = std::regex::ECMAScript | std::regex::optimize;
    bool dotall = false;
    for (;;) {
        if (starts_with(pat, "(?i)")) { flags |= std::regex::icase; pat = pat.substr(4); continue; }
        if (starts_with(pat, "(?s)")) { dotall = true; pat = pat.substr(4); continue; }
        if (starts_with(pat, "(?is)") || starts_with(pat, "(?si)")) { flags |= std::regex::icase; dotall = true; pat = pat.substr(5); continue; }
        break;
    }
    // A leading greedy ".*" only exists to reach the last occurrence; we take the last match anyway.
    bool want_last = false;  // Go's regexp returns the first match; a leading greedy ".*" makes it the last one
    while (starts_with(pat, ".*")) { pat = pat.substr(2); want_last = true; }
    std::regex re;
    try {
        re = std::regex(pat, flags);
    } catch (...) {
        return {};
    }
    std::string hint = pat.find('|') == std::string::npos ? literal_hint(pat) : std::string();
    std::string last;
    bool done = false;
    auto take = [&](const std::smatch& m) {
        if (done) return;
        last = m.size() > 1 && m[1].matched ? m[1].str() : m[0].str();
        if (!want_last) done = true;
    };
    try {
        if (dotall && pat.find('.') != std::string::npos) {
            // dot must match newlines: run on the whole text with [\s\S]
            std::string p2;
            bool in_class = false;
            for (size_t i = 0; i < pat.size(); ++i) {
                char c = pat[i];
                if (c == '\\' && i + 1 < pat.size()) { p2 += c; p2 += pat[++i]; continue; }
                if (c == '[') in_class = true;
                if (c == ']') in_class = false;
                if (c == '.' && !in_class) p2 += "[\\s\\S]";
                else p2 += c;
            }
            std::regex re2(p2, flags);
            for (std::sregex_iterator it(text.begin(), text.end(), re2), end; it != end && !done; ++it) take(*it);
            return trim(last);
        }
        // line by line (fast and safe for big logs)
        size_t start = 0;
        while (start <= text.size()) {
            size_t nl = text.find('\n', start);
            size_t end = nl == std::string::npos ? text.size() : nl;
            std::string line = text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (hint.empty() || ((flags & std::regex::icase) ? icontains(line, hint) : line.find(hint) != std::string::npos)) {
                for (std::sregex_iterator it(line.begin(), line.end(), re), e; it != e && !done; ++it) take(*it);
            }
            if (done) break;
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    } catch (...) {
    }
    return trim(last);
}

std::string slug(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (std::isalnum(c)) out += (char)std::tolower(c);
        else if (!out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "platform" : out;
}

std::string random_id() {
    static std::mt19937_64 rng((uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count() ^ (uint64_t)(uintptr_t)&rng);
    static const char* hx = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 32; ++i) s += hx[rng() & 15];
    return s;
}

}  // namespace vs::basic
