#include "i18n.h"

#include "str.h"

namespace vs {
bool g_lang_en = false;

std::string ago(long long t) { return g_lang_en ? time_ago_en(t, unix_now()) : time_ago_ru(t, unix_now()); }

std::string accounts_count(long long n) {
    if (g_lang_en) return std::to_string(n) + (n == 1 ? " account" : " accounts");
    return std::to_string(n) + " " + plural_ru(n, "аккаунт", "аккаунта", "аккаунтов");
}
}  // namespace vs
