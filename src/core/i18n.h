// vsemvamdam-switcher — two-language UI strings (Russian first, English second)
#pragma once
#include <string>

namespace vs {
extern bool g_lang_en;
inline const char* TR(const char* ru, const char* en) { return g_lang_en ? en : ru; }
std::string ago(long long unix_ts);  // localized "time ago"
std::string accounts_count(long long n);
}  // namespace vs
