// vsemvamdam-switcher — string helpers (portable)
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vs {

std::string to_lower(std::string_view s);           // ASCII-only lowercase
bool iequals(std::string_view a, std::string_view b); // ASCII case-insensitive
bool istarts_with(std::string_view s, std::string_view prefix);
bool starts_with(std::string_view s, std::string_view prefix);
bool ends_with(std::string_view s, std::string_view suffix);
bool icontains(std::string_view hay, std::string_view needle); // UTF-8 aware for Cyrillic/Latin
std::string trim(std::string_view s);
std::vector<std::string> split(std::string_view s, char sep);
std::string replace_all(std::string s, std::string_view from, std::string_view to);
std::string ireplace_all(std::string s, std::string_view from, std::string_view to);
std::string join(const std::vector<std::string>& v, std::string_view sep);

// Lowercase for search: handles ASCII, Cyrillic (А-Я, Ё) and Latin-1 basics.
std::string fold_case_utf8(std::string_view s);

// Safe file-name fragment (removes \/:*?"<>| and control chars).
std::string sanitize_filename(std::string_view s);

bool parse_u64(std::string_view s, uint64_t& out);
bool parse_i64(std::string_view s, int64_t& out);

// "только что", "5 мин назад", "2 ч назад", "3 дня назад"
std::string time_ago_ru(int64_t unix_seconds, int64_t now_unix);
std::string time_ago_en(int64_t unix_seconds, int64_t now_unix);
// Russian plural: plural_ru(5, "аккаунт", "аккаунта", "аккаунтов") -> "аккаунтов"
const char* plural_ru(int64_t n, const char* one, const char* few, const char* many);

int64_t unix_now();

#ifdef _WIN32
std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);
#endif

}  // namespace vs
