#include "cards.h"

#include <algorithm>
#include <cstdlib>
#include <regex>

#include "nlohmann/json.hpp"
#include "str.h"

namespace vs::cards {

std::string html_unescape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) { o += s[i]; continue; }
        std::string ent = s.substr(i + 1, semi - i - 1);
        uint32_t cp = 0;
        if (ent == "amp") cp = '&';
        else if (ent == "lt") cp = '<';
        else if (ent == "gt") cp = '>';
        else if (ent == "quot") cp = '"';
        else if (ent == "apos" || ent == "#39") cp = '\'';
        else if (ent == "nbsp") cp = ' ';
        else if (!ent.empty() && ent[0] == '#') {
            cp = (uint32_t)(ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? std::strtoul(ent.c_str() + 2, nullptr, 16)
                                                                                : std::strtoul(ent.c_str() + 1, nullptr, 10));
        }
        if (!cp) { o += s[i]; continue; }
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        i = semi;
    }
    return o;
}

static std::string strip_tags(const std::string& s) {
    std::string o;
    bool tag = false;
    for (char c : s) {
        if (c == '<') tag = true;
        else if (c == '>') tag = false;
        else if (!tag) o += c;
    }
    return o;
}

// text of the first element whose class attribute is exactly `cls` (up to its first closing tag)
static std::string first_class_text(const std::string& chunk, const std::string& cls, size_t* at = nullptr) {
    std::string needle = "class=\"" + cls + "\"";
    size_t p = chunk.find(needle);
    if (p == std::string::npos) return {};
    size_t gt = chunk.find('>', p);
    if (gt == std::string::npos) return {};
    // up to the next opening of a block-level child or closing tag
    size_t end = chunk.find("</", gt);
    if (end == std::string::npos) end = chunk.size();
    if (at) *at = gt + 1;
    return chunk.substr(gt + 1, end - gt - 1);
}

// first number in an English-formatted text ("1,234.5 hrs on record", "3 card drops remaining"); -1 if none
static double first_number(const std::string& s) {
    std::string n;
    for (char c : s) {
        if ((c >= '0' && c <= '9') || (c == '.' && !n.empty())) n += c;
        else if (c == ',' && !n.empty()) continue;  // thousands separator
        else if (!n.empty()) break;
    }
    return n.empty() ? -1 : std::atof(n.c_str());
}

bool parse_badges(const std::string& html, BadgePage& out) {
    out = BadgePage{};
    static const std::string row = "class=\"badge_row is_link";
    // pages: links like "?p=3" / "&p=3"
    {
        std::regex re("[?&]p=(\\d+)");
        for (auto it = std::sregex_iterator(html.begin(), html.end(), re); it != std::sregex_iterator(); ++it)
            out.last_page = std::max(out.last_page, std::atoi((*it)[1].str().c_str()));
        out.last_page = std::min(out.last_page, 200);
    }
    size_t pos = html.find(row);
    if (pos == std::string::npos) {
        // an own badges page always has the "badges" header; without a session Steam shows a sign-in link
        out.signed_in = html.find("badge_row") != std::string::npos || html.find("g_steamID = \"7656") != std::string::npos;
        if (html.find("g_steamID = false") != std::string::npos || html.find("login/home") != std::string::npos) out.signed_in = false;
        return html.find("<html") != std::string::npos || html.find("<!DOCTYPE") != std::string::npos;
    }
    if (html.find("g_steamID = false") != std::string::npos) out.signed_in = false;
    while (pos != std::string::npos) {
        size_t next = html.find(row, pos + row.size());
        std::string chunk = html.substr(pos, (next == std::string::npos ? html.size() : next) - pos);
        pos = next;
        BadgeGame g;
        std::smatch m;
        static const std::regex appre("gamecards/(\\d+)/");
        static const std::regex dlgre("card_drop_info_gamebadge_(\\d+)_");
        if (std::regex_search(chunk, m, appre)) g.appid = (uint32_t)std::strtoul(m[1].str().c_str(), nullptr, 10);
        else if (std::regex_search(chunk, m, dlgre)) g.appid = (uint32_t)std::strtoul(m[1].str().c_str(), nullptr, 10);
        if (!g.appid) continue;  // non-game badges (years of service, events, ...)
        std::string drops = first_class_text(chunk, "progress_info_bold");
        if (!drops.empty()) {
            double d = first_number(drops);
            g.drops = d > 0 ? (int)d : 0;
        }
        std::string hours = first_class_text(chunk, "badge_title_stats_playtime");
        if (!hours.empty()) {
            double h = first_number(html_unescape(hours));
            g.hours = h > 0 ? h : 0;
        }
        std::string title = first_class_text(chunk, "badge_title");
        title = title.substr(0, title.find('<'));  // "Name&nbsp;<span class="badge_view_details">..."
        title = trim(html_unescape(title));
        // "Game Name&nbsp;<span class="badge_view_details">View details</span>"
        g.name = title;
        if (g.name.empty()) {
            std::string body = html_unescape(strip_tags(first_class_text(chunk, "card_drop_info_body")));
            size_t by = body.find(" by playing ");
            if (by != std::string::npos) {
                g.name = trim(body.substr(by + 12));
                if (!g.name.empty() && g.name.back() == '.') g.name.pop_back();
            }
        }
        // the same app can appear twice (foil badge); keep the row with drops
        auto dup = std::find_if(out.games.begin(), out.games.end(), [&](const BadgeGame& x) { return x.appid == g.appid; });
        if (dup != out.games.end()) {
            if (g.drops > dup->drops) dup->drops = g.drops;
            dup->hours = std::max(dup->hours, g.hours);
            continue;
        }
        out.games.push_back(g);
    }
    return true;
}

bool parse_owned_games(const std::string& text, std::vector<OwnedGame>& out) {
    out.clear();
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return false;
    auto r = j.find("response");
    if (r == j.end() || !r->is_object()) return false;
    auto g = r->find("games");
    if (g == r->end()) return true;  // private / empty library
    if (!g->is_array()) return false;
    for (auto& x : *g) {
        if (!x.is_object()) continue;
        OwnedGame o;
        try {
            o.appid = x.value("appid", 0u);
            o.name = x.value("name", std::string());
            o.minutes = x.value("playtime_forever", 0);
            o.has_stats = x.value("has_community_visible_stats", false);
            o.icon = x.value("img_icon_url", std::string());
        } catch (...) {
            continue;
        }
        if (o.appid) out.push_back(o);
    }
    return true;
}

std::vector<uint32_t> parse_sam_games_xml(const std::string& xml) {
    std::vector<uint32_t> out;
    static const std::regex re("<game(\\s+type=\"([a-z]*)\")?\\s*>(\\d+)</game>");
    for (auto it = std::sregex_iterator(xml.begin(), xml.end(), re); it != std::sregex_iterator(); ++it) {
        std::string type = (*it)[2].str();
        if (type == "junk") continue;
        uint32_t id = (uint32_t)std::strtoul((*it)[3].str().c_str(), nullptr, 10);
        if (id) out.push_back(id);
    }
    return out;
}

std::vector<uint32_t> plan(const std::vector<BadgeGame>& games, Mode mode, int max_parallel, double threshold) {
    std::vector<BadgeGame> left;
    for (auto& g : games)
        if (g.drops > 0) left.push_back(g);
    std::vector<uint32_t> out;
    if (left.empty()) return out;
    max_parallel = std::clamp(max_parallel, 1, kMaxParallel);
    auto by_drops = [](const BadgeGame& a, const BadgeGame& b) { return a.drops != b.drops ? a.drops > b.drops : a.hours > b.hours; };
    if (mode == Mode::Simultaneous) {
        std::stable_sort(left.begin(), left.end(), by_drops);
        for (auto& g : left)
            if ((int)out.size() < max_parallel) out.push_back(g.appid);
        return out;
    }
    if (mode == Mode::Auto) {
        std::vector<BadgeGame> young;
        for (auto& g : left)
            if (g.hours < threshold) young.push_back(g);
        if (!young.empty()) {
            // closest to the threshold first, they start dropping soonest
            std::stable_sort(young.begin(), young.end(), [](const BadgeGame& a, const BadgeGame& b) { return a.hours > b.hours; });
            for (auto& g : young)
                if ((int)out.size() < max_parallel) out.push_back(g.appid);
            return out;
        }
    }
    std::stable_sort(left.begin(), left.end(), by_drops);
    out.push_back(left.front().appid);
    return out;
}

}  // namespace vs::cards
