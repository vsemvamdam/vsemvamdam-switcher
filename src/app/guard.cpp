// vsemvamdam-switcher — Steam Guard service (storage, sessions, confirmations, login & linking)
#include "guard.h"

#include <algorithm>
#include <chrono>

#include "../core/crypto.h"
#include "../core/fsx.h"
#include "../core/i18n.h"
#include "../core/log.h"
#include "../core/settings.h"
#include "../core/str.h"
#include "../os/win.h"
#include "host.h"
#include "nlohmann/json.hpp"

using nlohmann::json;

namespace vs {

namespace {
const char* kApi = "https://api.steampowered.com/";
const char* kCommunity = "https://steamcommunity.com";
// what the Steam mobile app sends (as used by node-steam-session / SteamAuth)
const char* kOkHttpUA = "okhttp/4.9.2";
const char* kDalvikUA = "Dalvik/2.1.0 (Linux; U; Android 9; Valve Steam App Version/3)";
const char* kMobileCookie = "mobileClient=android; mobileClientVersion=777777 3.10.3";
const char* kDeviceName = "vsemvamdam switcher";

struct Api {
    bool net_ok = false;
    int http = 0;
    int eresult = 0;
    std::string body;
    std::string error;
};

int eresult_of(const os::HttpResponse& r) {
    std::string h = r.header("x-eresult");
    int64_t v = 0;
    if (!h.empty() && parse_i64(h, v)) return (int)v;
    return r.status == 200 ? 1 : 0;
}

Api to_api(const os::HttpResponse& r) {
    Api a;
    a.net_ok = r.ok;
    a.http = r.status;
    a.eresult = r.ok ? eresult_of(r) : 0;
    a.body = r.body;
    a.error = r.ok ? r.header("x-error_message") : r.error;
    return a;
}

// IAuthenticationService call with a protobuf payload (GET only for GetPasswordRSAPublicKey, like the mobile app)
Api call_pb(const std::string& path, const std::string& msg, bool get) {
    os::HttpRequest rq;
    std::string b64 = crypto::base64_encode((const uint8_t*)msg.data(), msg.size());
    rq.url = std::string(kApi) + path + "/";
    rq.headers = {{"User-Agent", kOkHttpUA}, {"Cookie", kMobileCookie}, {"Accept", "application/json, text/plain, */*"}};
    if (get) {
        rq.method = "GET";
        rq.url += "?input_protobuf_encoded=" + guard::url_encode(b64) + "&origin=SteamMobile";
    } else {
        rq.method = "POST";
        std::string boundary = "----vsemvamdam" + crypto::hex_encode(crypto::random_bytes(8));
        rq.body = guard::multipart_body({{"input_protobuf_encoded", b64}}, boundary);
        rq.headers.push_back({"Content-Type", "multipart/form-data; boundary=" + boundary});
    }
    return to_api(os::http_request(rq));
}

// WebAPI call with form fields and a JSON answer (as SteamAuth does it)
Api call_form(const std::string& url, const guard::KV& form) {
    os::HttpRequest rq;
    rq.method = "POST";
    rq.url = url;
    rq.body = guard::form_encode(form);
    rq.headers = {{"User-Agent", kDalvikUA},
                  {"Cookie", kMobileCookie},
                  {"Accept", "application/json, text/plain, */*"},
                  {"Content-Type", "application/x-www-form-urlencoded; charset=UTF-8"}};
    return to_api(os::http_request(rq));
}

std::string api_url(const std::string& path, const std::string& access_token) {
    return std::string(kApi) + path + "?access_token=" + guard::url_encode(access_token);
}

std::string steam_lang() { return g_lang_en ? "english" : "russian"; }

os::HttpResponse community(const guard::Account& a, const std::string& method, const std::string& path_query, const std::string& form) {
    uint64_t sid = a.session.steamid ? a.session.steamid : a.steamid;
    os::HttpRequest rq;
    rq.method = method;
    rq.url = std::string(kCommunity) + path_query;
    rq.follow_redirects = false;  // a redirect means the login is gone
    rq.body = form;
    std::string cookie = "steamLoginSecure=" + std::to_string(sid) + "%7C%7C" + a.session.access_token + "; sessionid=" + a.session.session_id +
                         "; " + kMobileCookie + "; Steam_Language=" + steam_lang();
    rq.headers = {{"User-Agent", kDalvikUA}, {"Cookie", cookie}, {"Accept", "application/json, text/javascript, */*; q=0.01"},
                  {"X-Requested-With", "com.valvesoftware.android.steam.community"}};
    if (method == "POST") {
        rq.headers.push_back({"Content-Type", "application/x-www-form-urlencoded; charset=UTF-8"});
        rq.headers.push_back({"Origin", kCommunity});
        rq.headers.push_back({"Referer", std::string(kCommunity) + "/mobileconf/conf"});
    }
    return os::http_request(rq);
}

bool contains(const std::vector<int>& v, int x) { return std::find(v.begin(), v.end(), x) != v.end(); }

std::string random_session_id() {
    crypto::Bytes b = crypto::random_bytes(12);
    if (b.size() != 12) b = crypto::sha1(crypto::to_bytes(std::to_string(unix_now()) + std::to_string((uintptr_t)&b))), b.resize(12);
    return crypto::hex_encode(b);
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string net_error_text(const Api& a) {
    if (!a.net_ok) {
        std::string why = TR("проверьте интернет", "check the internet connection");
        if (a.error.find("12002") != std::string::npos) why = TR("Steam не ответил вовремя", "Steam timed out");
        if (a.error.find("12175") != std::string::npos || a.error.find("12157") != std::string::npos)
            why = TR("ошибка защищённого соединения — проверьте дату и время на компьютере", "secure connection error — check the PC date and time");
        return std::string(TR("Нет связи со Steam — ", "Can't reach Steam — ")) + why + (a.error.empty() ? "" : " (" + a.error + ")");
    }
    if (a.http == 429) return TR("Steam ограничил частоту запросов — подождите пару минут", "Steam rate-limited the requests — wait a couple of minutes");
    if (a.http >= 500) return std::string(TR("Steam временно недоступен (HTTP ", "Steam is temporarily unavailable (HTTP ")) + std::to_string(a.http) + ")";
    if (a.eresult != 1 && a.eresult != 0) return eresult_text(a.eresult);
    return std::string(TR("Неожиданный ответ Steam (HTTP ", "Unexpected Steam response (HTTP ")) + std::to_string(a.http) + ")";
}
}  // namespace

std::string eresult_text(int r) {
    switch (r) {
        case 1: return TR("Готово", "OK");
        case 2: return TR("Steam вернул общую ошибку — попробуйте ещё раз", "Steam returned a generic failure — try again");
        case 3: return TR("Нет соединения со Steam", "No connection to Steam");
        case 5: return TR("Неверный логин или пароль", "Wrong login or password");
        case 8: return TR("Steam не принял параметры запроса", "Steam rejected the request parameters");
        case 9: return TR("Запрос входа устарел или отклонён — начните заново", "The sign-in request expired or was denied — start again");
        case 10: return TR("Steam занят — попробуйте через минуту", "Steam is busy — try again in a minute");
        case 15: return TR("Доступ запрещён — войдите заново", "Access denied — sign in again");
        case 16: return TR("Steam не ответил вовремя", "Steam timed out");
        case 20: return TR("Сервис Steam недоступен", "Steam service unavailable");
        case 27: return TR("Срок действия истёк — войдите заново", "Expired — sign in again");
        case 29: return TR("Повторный запрос", "Duplicate request");
        case 63: return TR("Steam требует подтверждение входа", "Steam requires a sign-in confirmation");
        case 65: return TR("Неверный код", "Wrong code");
        case 84: return TR("Слишком много попыток — подождите и попробуйте позже", "Too many attempts — wait and try later");
        case 85: return TR("Нужен код Steam Guard", "A Steam Guard code is required");
        case 88: return TR("Код Steam Guard не подошёл — проверьте время на компьютере", "The Steam Guard code didn't match — check the PC clock");
        default: return std::string(TR("Ошибка Steam (код ", "Steam error (code ")) + std::to_string(r) + ")";
    }
}

bool Guard::self_test(std::vector<std::string>& out) {
    bool ok = true;
    auto line = [&](const std::string& name, const Api& a, const std::string& extra) {
        out.push_back(name + ": net=" + (a.net_ok ? "ok" : "FAIL " + a.error) + " http=" + std::to_string(a.http) + " eresult=" +
                      std::to_string(a.eresult) + " body=" + std::to_string(a.body.size()) + "B" + (extra.empty() ? "" : " " + extra));
    };
    Api t = call_form(std::string(kApi) + "ITwoFactorService/QueryTime/v0001", {{"steamid", "0"}});
    int64_t st = guard::parse_query_time(t.body);
    line("QueryTime", t, "server_time=" + std::to_string(st) + " offset=" + std::to_string(st ? st - unix_now() : 0));
    if (st <= 0) ok = false;

    std::string name = "vsprobe" + crypto::hex_encode(crypto::random_bytes(5));
    Api k = call_pb("IAuthenticationService/GetPasswordRSAPublicKey/v1", guard::pb_rsa_key_request(name), true);
    std::string mod, ex;
    uint64_t ts = 0;
    bool rsa = k.http == 200 && guard::pb_rsa_key_response(k.body, mod, ex, ts);
    line("GetPasswordRSAPublicKey", k, rsa ? "mod_bits=" + std::to_string(mod.size() * 4) + " exp=" + ex + " ts=" + std::to_string(ts) : "PARSE FAIL");
    if (!rsa) return false;

    crypto::Bytes enc = crypto::rsa_pkcs1_encrypt(mod, ex, crypto::random_bytes(12));
    Api b = call_pb("IAuthenticationService/BeginAuthSessionViaCredentials/v1",
                    guard::pb_begin_auth_request(name, crypto::base64_encode(enc), ts, kDeviceName), false);
    guard::BeginAuth ba;
    bool parsed = guard::pb_begin_auth_response(b.body, ba);
    line("BeginAuthSessionViaCredentials", b, std::string("(expected: http 200, eresult != 1)") + (parsed ? " client_id set" : "") +
                                                  (b.error.empty() ? "" : " msg=" + b.error));
    if (!b.net_ok || b.http != 200 || b.eresult == 0) ok = false;

    guard::Account a;
    a.identity_secret = crypto::base64_encode(crypto::random_bytes(20));
    a.steamid = 76561197960265729ULL;
    a.session.steamid = a.steamid;
    a.session.access_token = "invalid";
    a.session.session_id = random_session_id();
    os::HttpResponse r = community(a, "GET", "/mobileconf/getlist?" + guard::confirmation_params(a, unix_now(), "conf"), "");
    guard::ConfList cl;
    guard::parse_confirmations(r.body, cl);
    out.push_back("mobileconf/getlist (no session): net=" + std::string(r.ok ? "ok" : "FAIL " + r.error) + " http=" + std::to_string(r.status) +
                  " needauth=" + (cl.needauth || (r.status >= 300 && r.status < 400) ? "yes" : "no") + " location=" + r.header("location"));
    if (!r.ok) ok = false;
    return ok;
}

// ============================================================ lifecycle
Guard::Guard(Store& st) : store_(st) {}
Guard::~Guard() { stop(); }

std::string Guard::dir() const { return fsx::join(store_.dir, "guard"); }
std::string Guard::file_of(const std::string& key) const { return fsx::join(dir(), key + ".vsguard"); }

void Guard::load(bool start_threads) {
    fsx::mkdirs(dir());
    {
        std::lock_guard<std::mutex> lk(mtx_);
        items_.clear();
        for (auto& name : fsx::list_dir(dir())) {
            if (!ends_with(to_lower(name), ".vsguard")) continue;
            std::string raw;
            if (!fsx::read_file(fsx::join(dir(), name), raw)) continue;
            std::vector<uint8_t> in(raw.begin(), raw.end()), out;
            if (!os::dpapi_unprotect(in, out)) {
                log::warn("guard: cannot decrypt " + name + " (other Windows user?)");
                continue;
            }
            std::string text(out.begin(), out.end());
            crypto::wipe(out);
            json j = json::parse(text, nullptr, false);
            crypto::wipe(text);
            if (!j.is_object() || !j.contains("mafile")) continue;
            Item it;
            it.key = name.substr(0, name.size() - 8);
            std::string err;
            if (!guard::parse_mafile(j["mafile"].dump(), it.acc, &err)) {
                log::warn("guard: bad entry " + name + ": " + err);
                continue;
            }
            if (j.contains("added") && j["added"].is_number()) it.added_at = j["added"].get<int64_t>();
            if (j.contains("source") && j["source"].is_string()) it.source = j["source"].get<std::string>();
            items_[it.key] = std::move(it);
        }
        log::info("guard: " + std::to_string(items_.size()) + " authenticator(s)");
        sessions_.clear();
        std::string sdir = fsx::join(dir(), "sessions");
        for (auto& name : fsx::list_dir(sdir)) {
            if (!ends_with(to_lower(name), ".vssession")) continue;
            std::string raw;
            if (!fsx::read_file(fsx::join(sdir, name), raw)) continue;
            std::vector<uint8_t> in(raw.begin(), raw.end()), out;
            if (!os::dpapi_unprotect(in, out)) continue;
            json j = json::parse(std::string(out.begin(), out.end()), nullptr, false);
            crypto::wipe(out);
            if (!j.is_object()) continue;
            WebSession w;
            uint64_t sid = 0;
            try {
                parse_u64(j.value("steamid", std::string()), sid);
                w.account_name = j.value("account_name", std::string());
                w.session.access_token = j.value("access_token", std::string());
                w.session.refresh_token = j.value("refresh_token", std::string());
                w.session.session_id = j.value("session_id", std::string());
            } catch (...) {
                continue;
            }
            w.session.steamid = sid;
            if (sid && !w.session.refresh_token.empty()) sessions_[sid] = w;
        }
    }
    if (!start_threads) return;
    stop_ = false;
    worker_ = std::thread([this] { worker_main(); });
    checker_ = std::thread([this] { checker_main(); });
    sync_time_async();
}

void Guard::stop() {
    if (stop_.exchange(true)) return;
    flow_cancel_ = true;
    qcv_.notify_all();
    ccv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (checker_.joinable()) checker_.join();
    if (flow_thread_.joinable()) flow_thread_.join();
    for (auto& t : old_flows_)
        if (t.joinable()) t.join();
    old_flows_.clear();
}

void Guard::post(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lk(qmtx_);
        queue_.push_back(std::move(job));
    }
    qcv_.notify_one();
}

void Guard::worker_main() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(qmtx_);
            qcv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            job();
        } catch (const std::exception& e) {
            log::error(std::string("guard job: ") + e.what());
        }
    }
}

void Guard::checker_main() {
    int64_t last_sync = unix_now();
    bool first = true;
    while (!stop_) {
        bool poll;
        int sec;
        {
            std::lock_guard<std::recursive_mutex> lk(store_.mtx);
            poll = store_.settings.guard_poll;
            sec = std::clamp(store_.settings.guard_poll_sec, 10, 3600);
        }
        int wait = first && poll ? 4 : (poll ? sec : 5);
        {
            std::unique_lock<std::mutex> lk(cmtx_);
            ccv_.wait_for(lk, std::chrono::seconds(wait), [this] { return stop_.load(); });
        }
        if (stop_) break;
        if (unix_now() - last_sync > 6 * 3600) { last_sync = unix_now(); sync_time(); }
        {
            std::lock_guard<std::recursive_mutex> lk(store_.mtx);
            poll = store_.settings.guard_poll;
        }
        if (!poll) { first = true; continue; }
        first = false;
        fetch_all(true);
    }
}

void Guard::emit(GuardEvent e) {
    if (!on_event_) return;
    auto cb = on_event_;
    ui_post([cb, e] { cb(e); });
}

// ============================================================ time and codes
void Guard::sync_time() {
    Api r = call_form(std::string(kApi) + "ITwoFactorService/QueryTime/v0001", {{"steamid", "0"}});
    int64_t st = r.net_ok ? guard::parse_query_time(r.body) : 0;
    if (st > 0) {
        int64_t off = st - unix_now();
        if (off > -86400 && off < 86400) {
            offset_ = off;
            time_synced_ = true;
            if (off > 10 || off < -10) log::info("guard: clock offset " + std::to_string(off) + " s");
        }
    }
}
void Guard::sync_time_async() { post([this] { sync_time(); emit({}); }); }
int64_t Guard::now() const { return unix_now() + offset_.load(); }
int Guard::seconds_left() const { return guard::kCodePeriod - (int)(now() % guard::kCodePeriod); }
float Guard::period_progress() const {
    int64_t ms = now_ms() + offset_.load() * 1000;
    return (float)(ms % (guard::kCodePeriod * 1000)) / (guard::kCodePeriod * 1000.0f);
}

std::string Guard::code(const std::string& key) {
    std::string secret;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = items_.find(key);
        if (it == items_.end()) return {};
        secret = it->second.acc.shared_secret;
    }
    return guard::code_for_time(secret, now());
}

std::string Guard::key_for_steamid(uint64_t id) {
    if (!id) return {};
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& [k, it] : items_)
        if (it.acc.steamid == id) return k;
    return {};
}
std::string Guard::key_for_login(const std::string& login) {
    if (login.empty()) return {};
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& [k, it] : items_)
        if (iequals(it.acc.account_name, login)) return k;
    return {};
}

// ============================================================ items
static GuardView view_of(const std::string& key, const guard::Account& a, const GuardConfs& c, int64_t added) {
    GuardView v;
    v.key = key;
    v.account_name = a.account_name;
    v.steamid = a.steamid;
    int64_t exp = guard::jwt_exp(a.session.refresh_token);
    v.session_expires = exp;
    v.has_session = !a.session.refresh_token.empty() && (exp == 0 || exp > unix_now() + 60) && !c.needauth;
    v.has_revocation = !a.revocation_code.empty();
    v.fully_enrolled = a.fully_enrolled;
    v.added_at = added;
    v.confs = c;
    return v;
}

std::vector<GuardView> Guard::list() {
    std::vector<GuardView> out;
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& [k, it] : items_) out.push_back(view_of(k, it.acc, it.confs, it.added_at));
    std::sort(out.begin(), out.end(), [](const GuardView& a, const GuardView& b) { return to_lower(a.account_name) < to_lower(b.account_name); });
    return out;
}

bool Guard::view(const std::string& key, GuardView& out) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = items_.find(key);
    if (it == items_.end()) return false;
    out = view_of(key, it->second.acc, it->second.confs, it->second.added_at);
    return true;
}

int Guard::pending_total() {
    std::lock_guard<std::mutex> lk(mtx_);
    int n = 0;
    for (auto& [k, it] : items_) n += (int)it.confs.items.size();
    return n;
}
bool Guard::empty() {
    std::lock_guard<std::mutex> lk(mtx_);
    return items_.empty();
}

std::string Guard::revocation_code(const std::string& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = items_.find(key);
    return it == items_.end() ? std::string() : it->second.acc.revocation_code;
}

// ============================================================ storage
bool Guard::save_locked(const Item& it) {
    json w = json::object();
    w["v"] = 1;
    w["added"] = it.added_at;
    w["source"] = it.source;
    w["mafile"] = json::parse(guard::to_mafile(it.acc), nullptr, false);
    std::string text = w.dump();
    std::vector<uint8_t> in(text.begin(), text.end()), out;
    crypto::wipe(text);
    bool ok = os::dpapi_protect(in, out);
    crypto::wipe(in);
    if (!ok) {
        log::error("guard: DPAPI protect failed");
        return false;
    }
    fsx::mkdirs(dir());
    ok = fsx::write_file_atomic(file_of(it.key), std::string(out.begin(), out.end()));
    if (!ok) log::error("guard: cannot write " + file_of(it.key));
    return ok;
}

bool Guard::save(const std::string& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = items_.find(key);
    return it != items_.end() && save_locked(it->second);
}

void Guard::backup_file_locked(const std::string& key) {
    std::string f = file_of(key);
    if (!fsx::exists(f)) return;
    std::string bdir = fsx::join(dir(), "backup");
    fsx::mkdirs(bdir);
    std::string data;
    if (fsx::read_file(f, data))
        fsx::write_file_atomic(fsx::join(bdir, key + "-" + std::to_string(unix_now()) + "-" + crypto::hex_encode(crypto::random_bytes(3)) + ".vsguard"), data);
}

std::string Guard::make_key(const guard::Account& a) const {
    if (a.steamid) return std::to_string(a.steamid);
    std::string n = sanitize_filename(to_lower(a.account_name));
    for (auto& c : n)
        if (c == ' ') c = '_';
    return "acc-" + (n.empty() ? crypto::hex_encode(crypto::random_bytes(6)) : n);
}

std::string Guard::rekey_locked(const std::string& old_key, uint64_t steamid) {
    auto it = items_.find(old_key);
    if (it == items_.end() || !steamid) return old_key;
    std::string nk = std::to_string(steamid);
    if (nk == old_key) return old_key;
    // never clobber another entry (or an unreadable file) under the new name without a copy
    backup_file_locked(nk);
    Item item = it->second;
    item.key = nk;
    item.acc.steamid = steamid;
    if (!item.acc.session.steamid) item.acc.session.steamid = steamid;
    if (!save_locked(item)) return old_key;  // keep the old file and key if the new one can't be written
    items_.erase(it);
    items_[nk] = std::move(item);
    fsx::remove_file(file_of(old_key));
    return nk;
}

std::string Guard::add_or_update(guard::Account a, const std::string& source, bool& was_new) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string key;
    for (auto& [k, it] : items_) {
        if ((a.steamid && it.acc.steamid == a.steamid) || (!a.account_name.empty() && iequals(it.acc.account_name, a.account_name) &&
                                                           (!a.steamid || !it.acc.steamid))) {
            key = k;
            break;
        }
    }
    was_new = key.empty();
    if (was_new) {
        Item it;
        it.key = make_key(a);
        backup_file_locked(it.key);  // a file that could not be read at start-up (e.g. other Windows user) is kept
        it.acc = std::move(a);
        it.added_at = unix_now();
        it.source = source;
        key = it.key;
        items_[key] = std::move(it);
    } else {
        Item& it = items_[key];
        backup_file_locked(key);  // every replacement leaves a copy in guard\backup
        if (it.acc.shared_secret == a.shared_secret) {
            // same authenticator: an older or trimmed maFile must not erase what we already know
            auto keep = [](std::string& dst, const std::string& old) { if (dst.empty()) dst = old; };
            keep(a.identity_secret, it.acc.identity_secret);
            keep(a.secret_1, it.acc.secret_1);
            keep(a.revocation_code, it.acc.revocation_code);
            keep(a.serial_number, it.acc.serial_number);
            keep(a.uri, it.acc.uri);
            keep(a.token_gid, it.acc.token_gid);
            keep(a.device_id, it.acc.device_id);
            if (!a.server_time) a.server_time = it.acc.server_time;
            // keep a still-valid session when the imported file has none
            if (a.session.refresh_token.empty() && !it.acc.session.refresh_token.empty()) a.session = it.acc.session;
            if (it.acc.fully_enrolled) a.fully_enrolled = true;
        }
        if (!a.steamid) a.steamid = it.acc.steamid;
        if (a.account_name.empty()) a.account_name = it.acc.account_name;
        it.acc = std::move(a);
        it.source = source;
        it.confs = GuardConfs{};
        it.notified.clear();
        if (it.acc.steamid && starts_with(key, "acc-")) key = rekey_locked(key, it.acc.steamid);
    }
    if (!save_locked(items_[key])) log::error("guard: could not save " + key);
    return key;
}

bool Guard::mark_enrolled(const std::string& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = items_.find(key);
    if (it == items_.end()) return false;
    it->second.acc.fully_enrolled = true;
    return save_locked(it->second);
}

bool Guard::remove_local(const std::string& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = items_.find(key);
    if (it == items_.end()) return false;
    backup_file_locked(key);  // a safety copy stays in guard\backup (still DPAPI-encrypted)
    fsx::remove_file(file_of(key));
    items_.erase(it);
    return true;
}

// ============================================================ import / export
static bool looks_like_json(const std::string& s) {
    size_t p = s.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    return p != std::string::npos && s[p] == '{';
}

// an SDA-encrypted maFile is one base64 blob; anything else that isn't JSON is just not a maFile
static bool looks_like_encrypted(const std::string& s) {
    std::string t = trim(s);
    if (t.size() < 24 || t.size() % 4 != 0) return false;
    return t.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") == std::string::npos;
}

// parse_mafile's technical reasons in the interface language
static std::string mafile_error_text(const std::string& e) {
    if (e == "not a JSON object") return TR("файл повреждён или это не maFile", "the file is damaged or not a maFile");
    if (e == "shared_secret is missing or invalid")
        return TR("в файле нет ключа shared_secret — это не maFile аутентификатора", "no shared_secret in the file — not an authenticator maFile");
    return e;
}

ImportReport Guard::import_paths(const std::vector<std::string>& paths, const std::string& passkey,
                                 const std::function<uint64_t(const std::string&)>& resolve) {
    ImportReport rep;
    std::vector<std::string> sda_dirs, files;
    for (const auto& p0 : paths) {
        std::string p = p0;
        while (p.size() > 3 && (p.back() == '\\' || p.back() == '/')) p.pop_back();
        if (fsx::is_dir(p)) {
            if (fsx::is_file(fsx::join(p, "manifest.json"))) sda_dirs.push_back(p);
            else if (fsx::is_file(fsx::join(p, "maFiles\\manifest.json"))) sda_dirs.push_back(fsx::join(p, "maFiles"));
            else {
                bool any = false;
                for (auto& n : fsx::list_dir(p)) {
                    std::string l = to_lower(n);
                    if (ends_with(l, ".mafile") || (ends_with(l, ".json") && l != "manifest.json")) {
                        files.push_back(fsx::join(p, n));
                        any = true;
                    }
                }
                if (!any && fsx::is_dir(fsx::join(p, "maFiles")))
                    for (auto& n : fsx::list_dir(fsx::join(p, "maFiles")))
                        if (ends_with(to_lower(n), ".mafile")) files.push_back(fsx::join(fsx::join(p, "maFiles"), n));
                if (files.empty() && sda_dirs.empty())
                    rep.errors.push_back(fsx::filename(p) + TR(": в папке нет maFile", ": no maFiles in the folder"));
            }
        } else if (iequals(fsx::filename(p), "manifest.json")) {
            sda_dirs.push_back(fsx::parent(p));
        } else if (fsx::is_file(p)) {
            files.push_back(p);
        }
    }
    // an encrypted maFile picked on its own is imported through its folder's manifest (once per folder)
    for (auto& f : files) {
        std::string head;
        if (!fsx::read_file(f, head) || looks_like_json(head)) continue;
        std::string d = fsx::parent(f);
        if (fsx::is_file(fsx::join(d, "manifest.json"))) sda_dirs.push_back(d);
    }
    std::sort(sda_dirs.begin(), sda_dirs.end());
    sda_dirs.erase(std::unique(sda_dirs.begin(), sda_dirs.end()), sda_dirs.end());

    auto finish = [&](guard::Account a, const std::string& path, uint64_t hint_id) {
        if (!a.steamid) a.steamid = hint_id;
        if (!a.steamid) a.steamid = guard::steamid_from_filename(fsx::filename(path));
        if (!a.steamid && resolve) a.steamid = resolve(a.account_name);
        if (!a.session.steamid) a.session.steamid = a.steamid;
        bool was_new = false;
        std::string key = add_or_update(std::move(a), path, was_new);
        (was_new ? rep.added : rep.updated)++;
        rep.keys.push_back(key);
    };

    std::vector<std::string> handled;  // files covered by an SDA manifest
    for (auto& d : sda_dirs) {
        std::string mtext;
        guard::Manifest m;
        if (!fsx::read_file(fsx::join(d, "manifest.json"), mtext) || !guard::parse_manifest(mtext, m)) {
            rep.errors.push_back(fsx::filename(d) + TR(": не удалось прочитать manifest.json", ": cannot read manifest.json"));
            continue;
        }
        if (m.encrypted && passkey.empty()) {
            rep.need_passkey = true;
            rep.retry.push_back(d);
            for (auto& e : m.entries) handled.push_back(to_lower(fsx::join(d, e.filename)));
            continue;
        }
        bool bad_key = false;
        for (auto& e : m.entries) {
            std::string path = fsx::join(d, e.filename);
            handled.push_back(to_lower(path));
            std::string text;
            if (!fsx::read_file(path, text)) {
                rep.errors.push_back(e.filename + TR(": файл не найден", ": file not found"));
                continue;
            }
            if (m.encrypted) {
                std::string plain;
                if (!guard::sda_decrypt(passkey, e.salt, e.iv, text, plain)) {
                    bad_key = true;
                    break;
                }
                text.swap(plain);
                crypto::wipe(plain);
            }
            guard::Account a;
            std::string err;
            if (!guard::parse_mafile(text, a, &err)) rep.errors.push_back(e.filename + ": " + mafile_error_text(err));
            else finish(std::move(a), path, e.steamid);
            crypto::wipe(text);
        }
        if (bad_key) {
            rep.wrong_passkey = rep.need_passkey = true;
            rep.retry.push_back(d);
        }
    }
    for (auto& f : files) {
        if (std::find(handled.begin(), handled.end(), to_lower(f)) != handled.end()) continue;
        std::string text;
        if (!fsx::read_file(f, text) || text.size() > 1024 * 1024) {
            rep.errors.push_back(fsx::filename(f) + TR(": не удалось прочитать", ": cannot read"));
            continue;
        }
        if (!looks_like_json(text)) {
            if (!looks_like_encrypted(text)) {
                rep.errors.push_back(fsx::filename(f) + ": " + mafile_error_text("not a JSON object"));
                continue;
            }
            if (fsx::is_file(fsx::join(fsx::parent(f), "manifest.json"))) continue;  // handled with its folder above
            rep.errors.push_back(fsx::filename(f) + TR(": файл зашифрован — выберите всю папку maFiles вместе с manifest.json",
                                                         ": the file is encrypted — pick the whole maFiles folder with manifest.json"));
            continue;
        }
        guard::Account a;
        std::string err;
        if (!guard::parse_mafile(text, a, &err)) rep.errors.push_back(fsx::filename(f) + ": " + mafile_error_text(err));
        else finish(std::move(a), f, 0);
        crypto::wipe(text);
    }
    std::sort(rep.retry.begin(), rep.retry.end());
    rep.retry.erase(std::unique(rep.retry.begin(), rep.retry.end()), rep.retry.end());
    if (rep.added || rep.updated) {
        for (auto& k : rep.keys) fetch(k, false);
        emit({});
    }
    return rep;
}

bool Guard::export_sda(const std::vector<std::string>& keys, const std::string& folder, const std::string& passkey, std::string& out_dir,
                       std::string& err) {
    std::vector<guard::Account> accs;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& k : keys) {
            auto it = items_.find(k);
            if (it != items_.end()) accs.push_back(it->second.acc);
        }
    }
    if (accs.empty()) { err = TR("Нечего экспортировать", "Nothing to export"); return false; }
    auto empty_dir = [](const std::string& d) { return !fsx::exists(d) || (fsx::is_dir(d) && fsx::list_dir(d).empty()); };
    std::string target = fsx::join(folder, "maFiles");
    if (iequals(fsx::filename(folder), "maFiles") && empty_dir(folder)) target = folder;
    // never write into a folder that already has files (an existing SDA install, an earlier backup)
    if (!empty_dir(target)) target = fsx::join(folder, "maFiles-vsemvamdam-" + std::to_string(unix_now()));
    if (!empty_dir(target)) { err = TR("Папка для копии уже существует", "The backup folder already exists"); return false; }
    if (!fsx::mkdirs(target)) { err = TR("Не удалось создать папку ", "Cannot create folder ") + target; return false; }
    json man = {{"encrypted", !passkey.empty()}, {"first_run", false}, {"entries", json::array()}, {"periodic_checking", false},
                {"periodic_checking_interval", 5}, {"periodic_checking_checkall", false}, {"auto_confirm_market_transactions", false},
                {"auto_confirm_trades", false}};
    for (auto& a0 : accs) {
        guard::Account a = a0;
        // a refresh token signs in without the password: only encrypted backups carry the session
        if (passkey.empty()) a.session.access_token.clear(), a.session.refresh_token.clear();
        std::string name = a.steamid ? std::to_string(a.steamid) : sanitize_filename(a.account_name);
        name += ".maFile";
        if (fsx::exists(fsx::join(target, name))) name = crypto::hex_encode(crypto::random_bytes(4)) + "-" + name;
        std::string body = guard::to_mafile(a);
        json e = {{"filename", name}, {"steamid", a.steamid}, {"encryption_iv", nullptr}, {"encryption_salt", nullptr}};
        if (!passkey.empty()) {
            std::string salt = crypto::base64_encode(crypto::random_bytes(8));
            std::string iv = crypto::base64_encode(crypto::random_bytes(16));
            std::string enc = guard::sda_encrypt(passkey, salt, iv, body);
            crypto::wipe(body);
            if (enc.empty()) { err = TR("Ошибка шифрования", "Encryption failed"); return false; }
            body = enc;
            e["encryption_iv"] = iv;
            e["encryption_salt"] = salt;
        }
        bool ok = fsx::write_file(fsx::join(target, name), body);
        crypto::wipe(body);
        if (!ok) { err = TR("Не удалось записать ", "Cannot write ") + name; return false; }
        man["entries"].push_back(e);
    }
    if (!fsx::write_file(fsx::join(target, "manifest.json"), man.dump())) { err = TR("Не удалось записать manifest.json", "Cannot write manifest.json"); return false; }
    out_dir = target;
    return true;
}

std::string Guard::icon_path(const std::string& url) const {
    if (url.empty()) return {};
    std::string p = fsx::join(fsx::join(dir(), "icons"), crypto::hex_encode(crypto::sha1((const uint8_t*)url.data(), url.size())) + ".img");
    return fsx::exists(p) ? p : std::string();
}

void Guard::cache_icons(const std::vector<guard::Confirmation>& list) {
    std::string idir = fsx::join(dir(), "icons");
    fsx::mkdirs(idir);
    int fetched = 0;
    for (auto& c : list) {
        if (c.icon.empty() || !istarts_with(c.icon, "https://") || stop_) continue;
        std::string p = fsx::join(idir, crypto::hex_encode(crypto::sha1((const uint8_t*)c.icon.data(), c.icon.size())) + ".img");
        if (fsx::exists(p) || ++fetched > 30) continue;
        std::string body;
        if (os::http_get(c.icon, body, nullptr, 5000) && body.size() > 64 && body.size() < 2 * 1024 * 1024) fsx::write_file(p, body);
    }
}

std::vector<std::string> Guard::find_sda_folders() {
    std::vector<std::string> out;
    auto check = [&](const std::string& d) {
        std::string m = fsx::join(d, "maFiles");
        if (fsx::is_file(fsx::join(m, "manifest.json")) &&
            std::none_of(out.begin(), out.end(), [&](const std::string& x) { return iequals(x, m); }))
            out.push_back(m);
    };
    std::string running = os::exe_path_of_running("Steam Desktop Authenticator.exe");
    if (!running.empty()) check(fsx::parent(running));
    std::string home = os::user_profile();
    std::vector<std::string> bases = {os::desktop_dir(), fsx::join(home, "Downloads"), os::documents_dir(), home, os::appdata_local(),
                                      os::program_files(), os::program_files_x86(), "C:\\", "D:\\"};
    int budget = 900;  // directories visited, keeps the scan fast
    for (auto& b : bases) {
        if (b.empty() || !fsx::is_dir(b)) continue;
        check(b);
        for (auto& d1 : fsx::list_dir(b, true)) {
            if (--budget <= 0) break;
            if (d1.empty() || d1[0] == '.' || d1[0] == '$') continue;
            std::string p1 = fsx::join(b, d1);
            check(p1);
            std::string l = to_lower(d1);
            bool promising = l.find("sda") != std::string::npos || l.find("auth") != std::string::npos || l.find("steam") != std::string::npos ||
                             l.find("guard") != std::string::npos || l.find("desktop") != std::string::npos;
            if (!promising) continue;
            for (auto& d2 : fsx::list_dir(p1, true)) {
                if (--budget <= 0) break;
                check(fsx::join(p1, d2));
            }
        }
    }
    return out;
}

// ============================================================ sessions
Guard::Sess Guard::ensure_session(const std::string& key, bool force, guard::Account& out) {
    guard::Account a;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = items_.find(key);
        if (it == items_.end()) return Sess::NeedLogin;
        if (it->second.acc.session.session_id.empty()) {
            it->second.acc.session.session_id = random_session_id();
            save_locked(it->second);
        }
        a = it->second.acc;
    }
    if (a.session.refresh_token.empty()) return Sess::NeedLogin;
    uint64_t sid = a.session.steamid ? a.session.steamid : a.steamid;
    if (!sid) sid = guard::jwt_steamid(a.session.refresh_token);
    if (!sid) return Sess::NeedLogin;
    int64_t now = unix_now();
    int64_t exp_r = guard::jwt_exp(a.session.refresh_token);
    if (exp_r && exp_r <= now + 60) return Sess::NeedLogin;
    int64_t exp_a = guard::jwt_exp(a.session.access_token);
    if (!force && !a.session.access_token.empty() && exp_a > now + 120) {
        out = a;
        return Sess::Ok;
    }
    bool renew = exp_r && exp_r < now + 30 * 86400;
    Api r = call_form(std::string(kApi) + "IAuthenticationService/GenerateAccessTokenForApp/v1/",
                      {{"refresh_token", a.session.refresh_token}, {"steamid", std::to_string(sid)}, {"renewal_type", renew ? "1" : "0"}});
    if (!r.net_ok || r.http >= 500 || r.http == 429) return Sess::NetError;
    std::string access, refresh;
    if (r.http != 200 || r.eresult != 1 || !guard::parse_access_token(r.body, access, refresh)) {
        log::warn("guard: token refresh failed http=" + std::to_string(r.http) + " eresult=" + std::to_string(r.eresult));
        return Sess::NeedLogin;
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = items_.find(key);
        if (it == items_.end()) return Sess::NeedLogin;
        if (it->second.acc.session.refresh_token != a.session.refresh_token) {  // a new login landed meanwhile
            out = it->second.acc;
            return Sess::Ok;
        }
        it->second.acc.session.access_token = access;
        if (!refresh.empty()) it->second.acc.session.refresh_token = refresh;
        if (!it->second.acc.session.steamid) it->second.acc.session.steamid = sid;
        save_locked(it->second);
        out = it->second.acc;
    }
    return Sess::Ok;
}

// ============================================================ confirmations
void Guard::fetch(const std::string& key, bool background) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = items_.find(key);
        if (it == items_.end() || it->second.confs.loading) return;
        it->second.confs.loading = true;
    }
    emit({});
    post([this, key, background] { do_fetch(key, background); });
}

void Guard::fetch_all(bool background) {
    std::vector<std::string> keys;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& [k, it] : items_)
            if (!it.acc.session.refresh_token.empty() && !it.confs.needauth && it.acc.fully_enrolled) keys.push_back(k);
    }
    for (auto& k : keys) fetch(k, background);
}

void Guard::do_fetch(const std::string& key, bool background) {
    auto fail = [&](const std::string& msg, bool needauth) {
        bool was_ok = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = items_.find(key);
            if (it == items_.end()) return;
            was_ok = it->second.confs.loaded && !it->second.confs.needauth;
            it->second.confs.loading = false;
            it->second.confs.error = msg;
            if (needauth) {
                it->second.confs.needauth = true;
                it->second.confs.items.clear();
            }
        }
        if (needauth && was_ok) emit({GuardEventKind::SessionExpired, key, 0, ""});
        emit({});
    };
    guard::Account a;
    Sess s = ensure_session(key, false, a);
    if (stop_) return;
    if (s == Sess::NeedLogin) return fail(TR("Сессия Steam истекла — войдите заново", "The Steam session expired — sign in again"), true);
    if (s == Sess::NetError) return fail(TR("Нет связи со Steam", "Can't reach Steam"), false);
    guard::ConfList cl;
    for (int attempt = 0; attempt < 2; ++attempt) {
        os::HttpResponse r = community(a, "GET", "/mobileconf/getlist?" + guard::confirmation_params(a, now(), "conf"), "");
        if (!r.ok) return fail(TR("Нет связи со Steam", "Can't reach Steam"), false);
        if (r.status == 429) return fail(TR("Steam ограничил частоту запросов — подождите", "Steam rate-limited the requests — wait a bit"), false);
        if (r.status >= 300 && r.status < 400) { cl = guard::ConfList{}; cl.needauth = true; }
        else guard::parse_confirmations(r.body, cl);
        if (cl.ok || !cl.needauth || attempt == 1) break;
        // the access token may have been revoked early: get a fresh one and retry once
        if (ensure_session(key, true, a) != Sess::Ok) break;
    }
    if (stop_) return;
    if (!cl.ok) {
        if (cl.needauth) return fail(TR("Сессия Steam истекла — войдите заново", "The Steam session expired — sign in again"), true);
        return fail(cl.message.empty() ? std::string(TR("Steam не отдал список подтверждений", "Steam didn't return the confirmations")) : cl.message, false);
    }
    cache_icons(cl.items);
    std::vector<guard::Confirmation> autos, fresh;
    bool auto_market = false, auto_trades = false;
    {
        std::lock_guard<std::recursive_mutex> lk(store_.mtx);
        auto_market = store_.settings.guard_auto_market;
        auto_trades = store_.settings.guard_auto_trades;
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = items_.find(key);
        if (it == items_.end()) return;
        auto& c = it->second.confs;
        c.items = cl.items;
        c.loading = false;
        c.loaded = true;
        c.needauth = false;
        c.error.clear();
        c.fetched_at = unix_now();
        std::set<std::string> ids;
        for (auto& x : c.items) ids.insert(x.id);
        for (auto b = c.busy.begin(); b != c.busy.end();) b = ids.count(*b) ? std::next(b) : c.busy.erase(b);
        if (background) {
            for (auto& x : c.items) {
                if ((x.type == guard::ConfMarket && auto_market) || (x.type == guard::ConfTrade && auto_trades)) autos.push_back(x);
                else if (!it->second.notified.count(x.id)) fresh.push_back(x);
            }
        }
        for (auto& x : c.items) it->second.notified.insert(x.id);
    }
    if (!autos.empty()) {
        std::string err;
        if (do_respond(a, autos, true, err)) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                auto it = items_.find(key);
                if (it != items_.end()) {
                    auto& v = it->second.confs.items;
                    v.erase(std::remove_if(v.begin(), v.end(), [&](const guard::Confirmation& x) {
                                return std::any_of(autos.begin(), autos.end(), [&](const guard::Confirmation& y) { return y.id == x.id; });
                            }),
                            v.end());
                }
            }
            log::info("guard: auto-confirmed " + std::to_string(autos.size()) + " for " + a.account_name);
            emit({GuardEventKind::AutoConfirmed, key, (int)autos.size(), autos.front().headline});
        } else {
            log::warn("guard: auto-confirm failed: " + err);
        }
    }
    if (!fresh.empty()) emit({GuardEventKind::NewConfirmations, key, (int)fresh.size(), fresh.front().headline});
    emit({});
}

bool Guard::do_respond(const guard::Account& a, const std::vector<guard::Confirmation>& list, bool accept, std::string& err) {
    if (list.empty()) return true;
    std::string op = accept ? "allow" : "cancel";
    std::string tag = accept ? "accept" : "reject";
    os::HttpResponse r;
    if (list.size() == 1) {
        r = community(a, "GET", "/mobileconf/ajaxop?op=" + op + "&" + guard::confirmation_params(a, now(), tag) + "&cid=" + guard::url_encode(list[0].id) +
                                    "&ck=" + guard::url_encode(list[0].nonce), "");
    } else {
        std::string body = "op=" + op + "&" + guard::confirmation_params(a, now(), tag);
        for (auto& c : list) body += "&cid%5B%5D=" + guard::url_encode(c.id) + "&ck%5B%5D=" + guard::url_encode(c.nonce);
        r = community(a, "POST", "/mobileconf/multiajaxop", body);
    }
    if (!r.ok) { err = TR("Нет связи со Steam", "Can't reach Steam"); return false; }
    if (r.status >= 300 && r.status < 400) { err = TR("Сессия Steam истекла — войдите заново", "The Steam session expired — sign in again"); return false; }
    if (!guard::parse_success(r.body)) {
        err = TR("Steam не принял ответ — обновите список и попробуйте ещё раз", "Steam didn't accept it — refresh the list and try again");
        return false;
    }
    return true;
}

void Guard::respond(const std::string& key, const std::vector<std::string>& ids, bool accept) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = items_.find(key);
        if (it == items_.end()) return;
        for (auto& id : ids) it->second.confs.busy.insert(id);
    }
    emit({});
    post([this, key, ids, accept] {
        guard::Account a;
        Sess s = ensure_session(key, false, a);
        std::vector<guard::Confirmation> list;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = items_.find(key);
            if (it == items_.end()) return;
            for (auto& c : it->second.confs.items)
                if (std::find(ids.begin(), ids.end(), c.id) != ids.end()) list.push_back(c);
        }
        std::string err;
        bool ok = s == Sess::Ok && do_respond(a, list, accept, err);
        if (s != Sess::Ok) err = s == Sess::NeedLogin ? TR("Сессия Steam истекла — войдите заново", "The Steam session expired — sign in again")
                                                     : TR("Нет связи со Steam", "Can't reach Steam");
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = items_.find(key);
            if (it == items_.end()) return;
            auto& c = it->second.confs;
            for (auto& id : ids) c.busy.erase(id);
            if (ok) {
                c.items.erase(std::remove_if(c.items.begin(), c.items.end(),
                                             [&](const guard::Confirmation& x) { return std::find(ids.begin(), ids.end(), x.id) != ids.end(); }),
                              c.items.end());
                c.error.clear();
            } else {
                c.error = err;
                if (s == Sess::NeedLogin) c.needauth = true;
            }
        }
        emit({});
        if (ok) do_fetch(key, false);  // Steam may have more queued behind these
    });
}

// ============================================================ web sessions
bool Guard::save_session_locked(uint64_t steamid) {
    auto it = sessions_.find(steamid);
    if (it == sessions_.end()) return false;
    json j = {{"steamid", std::to_string(steamid)}, {"account_name", it->second.account_name}, {"access_token", it->second.session.access_token},
              {"refresh_token", it->second.session.refresh_token}, {"session_id", it->second.session.session_id}};
    std::string text = j.dump();
    std::vector<uint8_t> in(text.begin(), text.end()), out;
    crypto::wipe(text);
    bool ok = os::dpapi_protect(in, out);
    crypto::wipe(in);
    if (!ok) return false;
    std::string sdir = fsx::join(dir(), "sessions");
    fsx::mkdirs(sdir);
    return fsx::write_file_atomic(fsx::join(sdir, std::to_string(steamid) + ".vssession"), std::string(out.begin(), out.end()));
}

bool Guard::has_web_session(uint64_t steamid) {
    if (!steamid) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    auto now = unix_now();
    for (auto& [k, it] : items_) {
        if (it.acc.steamid != steamid || it.acc.session.refresh_token.empty() || it.confs.needauth) continue;
        int64_t e = guard::jwt_exp(it.acc.session.refresh_token);
        if (!e || e > now + 60) return true;
    }
    auto s = sessions_.find(steamid);
    if (s == sessions_.end()) return false;
    int64_t e = guard::jwt_exp(s->second.session.refresh_token);
    return !e || e > now + 60;
}

std::string Guard::web_session_name(uint64_t steamid) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& [k, it] : items_)
        if (it.acc.steamid == steamid) return it.acc.account_name;
    auto s = sessions_.find(steamid);
    return s == sessions_.end() ? std::string() : s->second.account_name;
}

void Guard::forget_web_session(uint64_t steamid) {
    std::lock_guard<std::mutex> lk(mtx_);
    sessions_.erase(steamid);
    fsx::remove_file(fsx::join(fsx::join(dir(), "sessions"), std::to_string(steamid) + ".vssession"));
}

bool Guard::web_session(uint64_t steamid, guard::Session& out, bool force, bool* need_login) {
    if (need_login) *need_login = false;
    std::string key = key_for_steamid(steamid);
    bool guard_net_error = false;
    if (!key.empty()) {
        guard::Account a;
        Sess r = ensure_session(key, force, a);
        if (r == Sess::Ok) {
            out = a.session;
            if (!out.steamid) out.steamid = steamid;
            return true;
        }
        guard_net_error = r == Sess::NetError;
    }
    WebSession w;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = sessions_.find(steamid);
        if (it == sessions_.end()) {
            if (need_login) *need_login = !guard_net_error;
            return false;
        }
        if (it->second.session.session_id.empty()) {
            it->second.session.session_id = random_session_id();
            save_session_locked(steamid);
        }
        w = it->second;
    }
    int64_t now = unix_now();
    int64_t exp_r = guard::jwt_exp(w.session.refresh_token);
    if (w.session.refresh_token.empty() || (exp_r && exp_r <= now + 60)) {
        if (need_login) *need_login = true;
        return false;
    }
    if (!force && !w.session.access_token.empty() && guard::jwt_exp(w.session.access_token) > now + 120) {
        out = w.session;
        return true;
    }
    bool renew = exp_r && exp_r < now + 30 * 86400;
    Api r = call_form(std::string(kApi) + "IAuthenticationService/GenerateAccessTokenForApp/v1/",
                      {{"refresh_token", w.session.refresh_token}, {"steamid", std::to_string(steamid)}, {"renewal_type", renew ? "1" : "0"}});
    std::string access, refresh;
    if (!r.net_ok || r.http != 200 || r.eresult != 1 || !guard::parse_access_token(r.body, access, refresh)) {
        // Steam answered and said no (revoked / password changed): only a new sign-in helps
        if (need_login) *need_login = r.net_ok && (r.http == 401 || r.http == 403 || (r.http == 200 && r.eresult != 1 && r.eresult != 0));
        return false;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sessions_.find(steamid);
    if (it == sessions_.end()) return false;
    if (it->second.session.refresh_token == w.session.refresh_token) {
        it->second.session.access_token = access;
        if (!refresh.empty()) it->second.session.refresh_token = refresh;
        save_session_locked(steamid);
    }
    out = it->second.session;
    return true;
}

bool Guard::community_get(uint64_t steamid, const std::string& path_query, std::string& body, int& status, bool* need_login) {
    body.clear();
    status = 0;
    if (need_login) *need_login = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        guard::Session s;
        if (!web_session(steamid, s, attempt == 1, need_login)) return false;
        guard::Account a;
        a.steamid = steamid;
        a.session = s;
        os::HttpResponse r = community(a, "GET", path_query, "");
        if (!r.ok) return false;
        status = r.status;
        if (r.status >= 300 && r.status < 400) {
            if (attempt == 0) continue;  // signed out: refresh the token and retry
            if (need_login) *need_login = true;  // still sent to the login page with a fresh token
            return false;
        }
        body = std::move(r.body);
        return r.status == 200;
    }
    return false;
}

bool Guard::webapi_get(uint64_t steamid, const std::string& url, std::string& body, int& status) {
    guard::Session s;
    body.clear();
    status = 0;
    if (!web_session(steamid, s, false)) return false;
    os::HttpRequest rq;
    rq.url = url + "&access_token=" + guard::url_encode(s.access_token);
    rq.headers = {{"User-Agent", kDalvikUA}, {"Accept", "application/json"}};
    os::HttpResponse r = os::http_request(rq);
    status = r.status;
    body = std::move(r.body);
    return r.ok && r.status == 200;
}

// ============================================================ flows
FlowState Guard::flow() {
    std::lock_guard<std::mutex> lk(fmtx_);
    return fs_;
}

void Guard::set_flow(FlowStep step, const std::string& msg, bool error) {
    {
        std::lock_guard<std::mutex> lk(fmtx_);
        fs_.step = step;
        fs_.message = msg;
        fs_.error = error;
    }
    emit({});
}

void Guard::set_flow(int gen, FlowStep step, const std::string& msg, bool error) {
    {
        std::lock_guard<std::mutex> lk(fmtx_);
        if (flow_gen_ != gen) return;  // a cancelled flow must not overwrite the current one
        fs_.step = step;
        fs_.message = msg;
        fs_.error = error;
    }
    emit({});
}

void Guard::run_flow(std::function<void(int)> fn) {
    int gen = ++flow_gen_;
    flow_cancel_ = false;
    if (flow_thread_.joinable()) old_flows_.push_back(std::move(flow_thread_));
    flow_thread_ = std::thread([this, fn, gen] {
        try {
            fn(gen);
        } catch (const std::exception& e) {
            log::error(std::string("guard flow: ") + e.what());
            set_flow(gen, FlowStep::Failed, std::string(TR("Внутренняя ошибка: ", "Internal error: ")) + e.what(), true);
        }
    });
}

void Guard::flow_reset() {
    FlowState st = flow();
    flow_cancel_ = true;
    ++flow_gen_;
    // An added but not activated authenticator stays (marked "linking not finished"): Steam may have accepted
    // the activation even if its answer never arrived, and then these secrets are the only way into the account.
    (void)st;
    {
        std::lock_guard<std::mutex> lk(fmtx_);
        fs_ = FlowState{};
        pend_ = Pending{};
    }
    emit({});
}

void Guard::flow_login(const std::string& key, const std::string& password) {
    std::string login;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = items_.find(key);
        if (it == items_.end()) return;
        login = it->second.acc.account_name;
    }
    {
        std::lock_guard<std::mutex> lk(fmtx_);
        fs_ = FlowState{};
        fs_.kind = FlowKind::Login;
        fs_.key = key;
        fs_.account_name = login;
        fs_.step = FlowStep::Working;
        fs_.message = TR("Вход в Steam…", "Signing in to Steam…");
        pend_ = Pending{};
        pend_.login = login;
    }
    emit({});
    std::string pw = password;
    run_flow([this, login, pw](int gen) mutable {
        login_begin(login, pw, gen);
        crypto::wipe(pw);
    });
}

void Guard::flow_session(const std::string& login, const std::string& password) {
    {
        std::lock_guard<std::mutex> lk(fmtx_);
        fs_ = FlowState{};
        fs_.kind = FlowKind::Session;
        fs_.account_name = login;
        fs_.step = FlowStep::Working;
        fs_.message = TR("Вход в Steam…", "Signing in to Steam…");
        pend_ = Pending{};
        pend_.login = login;
    }
    emit({});
    std::string pw = password;
    run_flow([this, login, pw](int gen) mutable {
        login_begin(login, pw, gen);
        crypto::wipe(pw);
    });
}

void Guard::flow_link(const std::string& login, const std::string& password) {
    {
        std::lock_guard<std::mutex> lk(fmtx_);
        fs_ = FlowState{};
        fs_.kind = FlowKind::Link;
        fs_.account_name = login;
        fs_.step = FlowStep::Working;
        fs_.message = TR("Вход в Steam…", "Signing in to Steam…");
        pend_ = Pending{};
        pend_.login = login;
    }
    emit({});
    std::string pw = password;
    run_flow([this, login, pw](int gen) mutable {
        login_begin(login, pw, gen);
        crypto::wipe(pw);
    });
}

void Guard::login_begin(const std::string& login, const std::string& password, int gen) {
    if (!time_synced_) sync_time();
    if (flow_cancelled(gen)) return;
    Api k = call_pb("IAuthenticationService/GetPasswordRSAPublicKey/v1", guard::pb_rsa_key_request(login), true);
    if (flow_cancelled(gen)) return;
    std::string mod, ex;
    uint64_t ts = 0;
    if (!k.net_ok || k.http != 200 || k.eresult != 1 || !guard::pb_rsa_key_response(k.body, mod, ex, ts))
        return set_flow(gen, FlowStep::Failed, k.eresult == 9 ? std::string(TR("Аккаунт не найден", "Account not found")) : net_error_text(k), true);
    crypto::Bytes pwb = crypto::to_bytes(password);
    crypto::Bytes enc = crypto::rsa_pkcs1_encrypt(mod, ex, pwb);
    crypto::wipe(pwb);
    if (enc.empty()) return set_flow(gen, FlowStep::Failed, TR("Не удалось зашифровать пароль", "Could not encrypt the password"), true);
    Api b = call_pb("IAuthenticationService/BeginAuthSessionViaCredentials/v1",
                    guard::pb_begin_auth_request(login, crypto::base64_encode(enc), ts, kDeviceName), false);
    if (flow_cancelled(gen)) return;
    guard::BeginAuth ba;
    if (!b.net_ok || b.http != 200 || b.eresult != 1 || !guard::pb_begin_auth_response(b.body, ba)) {
        log::warn("guard: begin auth http=" + std::to_string(b.http) + " eresult=" + std::to_string(b.eresult));
        return set_flow(gen, FlowStep::Failed, net_error_text(b), true);
    }
    FlowKind kind;
    {
        std::lock_guard<std::mutex> lk(fmtx_);
        if (flow_gen_ != gen) return;
        pend_.client_id = ba.client_id;
        pend_.request_id = ba.request_id;
        pend_.steamid = ba.steamid;
        pend_.interval = ba.interval;
        pend_.allowed = ba.allowed;
        kind = fs_.kind;
    }
    std::string secret;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& [kk, it] : items_)
            if ((ba.steamid && it.acc.steamid == ba.steamid) || iequals(it.acc.account_name, login))
                if (it.acc.fully_enrolled) secret = it.acc.shared_secret;
    }
    std::string email_hint;
    for (size_t i = 0; i < ba.allowed.size(); ++i)
        if (ba.allowed[i] == guard::GuardEmailCode && i < ba.messages.size()) email_hint = ba.messages[i];
    const auto& al = ba.allowed;
    if (contains(al, guard::GuardNone)) return login_poll(gen, 30);
    if (contains(al, guard::GuardDeviceCode) && !secret.empty()) {
        set_flow(gen, FlowStep::Working, TR("Подтверждаю вход кодом Steam Guard…", "Confirming with the Steam Guard code…"));
        for (int attempt = 0; attempt < 2; ++attempt) {
            Api u = call_pb("IAuthenticationService/UpdateAuthSessionWithSteamGuardCode/v1",
                            guard::pb_update_code_request(ba.client_id, ba.steamid, guard::code_for_time(secret, now()), guard::GuardDeviceCode), false);
            if (flow_cancelled(gen)) return;
            if (u.net_ok && u.http == 200 && (u.eresult == 1 || u.eresult == 29)) {
                {
                    std::lock_guard<std::mutex> lk(fmtx_);
                    if (flow_gen_ == gen) pend_.own_code = true;
                }
                return login_poll(gen, 30);
            }
            if (u.eresult == 88 && attempt == 0) {
                sync_time();
                continue;
            }
            return set_flow(gen, FlowStep::Failed,
                            u.eresult == 88 ? std::string(TR("Код Steam Guard не подошёл. Проверьте время на компьютере и что maFile от этого аккаунта.",
                                                             "The Steam Guard code didn't match. Check the PC clock and that the maFile belongs to this account."))
                                            : net_error_text(u),
                            true);
        }
        return;
    }
    if (kind == FlowKind::Link && contains(al, guard::GuardDeviceCode))
        return set_flow(gen, FlowStep::Failed,
                        TR("На этом аккаунте уже включён мобильный аутентификатор Steam Guard. Чтобы пользоваться им здесь, импортируйте его maFile. "
                           "Либо сначала удалите аутентификатор в приложении Steam (или кодом восстановления R…), а затем привяжите заново.",
                           "This account already has a Steam Guard mobile authenticator. To use it here, import its maFile. "
                           "Or remove it in the Steam app (or with the R… revocation code) first and link again."),
                        true);
    if (contains(al, guard::GuardEmailCode)) {
        {
            std::lock_guard<std::mutex> lk(fmtx_);
            if (flow_gen_ != gen) return;
            fs_.hint = email_hint;
        }
        return set_flow(gen, FlowStep::NeedEmailCode, "");
    }
    if (contains(al, guard::GuardDeviceCode)) {
        if (!contains(al, guard::GuardDeviceConfirmation)) return set_flow(gen, FlowStep::NeedDeviceCode, "");
        // the code can be typed, or the sign-in approved in the Steam app: keep polling meanwhile
        {
            std::lock_guard<std::mutex> lk(fmtx_);
            if (flow_gen_ != gen) return;
            fs_.hint = "app";
        }
        set_flow(gen, FlowStep::NeedDeviceCode, "");
        return login_poll(gen, 180);
    }
    if (contains(al, guard::GuardDeviceConfirmation) || contains(al, guard::GuardEmailConfirmation)) {
        set_flow(gen, FlowStep::WaitConfirmation, contains(al, guard::GuardDeviceConfirmation)
                                                 ? TR("Подтвердите вход в мобильном приложении Steam", "Approve the sign-in in the Steam mobile app")
                                                 : TR("Откройте письмо от Steam и подтвердите вход по ссылке", "Open the e-mail from Steam and approve the sign-in"));
        return login_poll(gen, 180);
    }
    set_flow(gen, FlowStep::Failed, TR("Steam запросил неизвестный способ подтверждения входа", "Steam asked for an unknown confirmation method"), true);
}

void Guard::flow_code(const std::string& code_in) {
    FlowState st = flow();
    if (st.step != FlowStep::NeedEmailCode && st.step != FlowStep::NeedDeviceCode) return;
    int type = st.step == FlowStep::NeedEmailCode ? guard::GuardEmailCode : guard::GuardDeviceCode;
    FlowStep back = st.step;
    std::string code = to_lower(trim(code_in));
    for (auto& c : code) c = (char)toupper((unsigned char)c);
    set_flow(FlowStep::Working, TR("Проверяю код…", "Checking the code…"));
    run_flow([this, type, back, code](int gen) {
        Pending p;
        {
            std::lock_guard<std::mutex> lk(fmtx_);
            p = pend_;
        }
        Api u = call_pb("IAuthenticationService/UpdateAuthSessionWithSteamGuardCode/v1", guard::pb_update_code_request(p.client_id, p.steamid, code, type), false);
        if (flow_cancelled(gen)) return;
        if (u.net_ok && u.http == 200 && (u.eresult == 1 || u.eresult == 29)) return login_poll(gen, 30);
        if (u.eresult == 65 || u.eresult == 88) return set_flow(back, TR("Неверный код — проверьте и введите ещё раз", "Wrong code — check it and try again"), true);
        set_flow(gen, FlowStep::Failed, net_error_text(u), true);
    });
}

void Guard::login_poll(int gen, int timeout_s) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    for (;;) {
        Pending p;
        {
            std::lock_guard<std::mutex> lk(fmtx_);
            if (flow_gen_ != gen) return;
            p = pend_;
        }
        Api r = call_pb("IAuthenticationService/PollAuthSessionStatus/v1", guard::pb_poll_request(p.client_id, p.request_id), false);
        if (flow_cancelled(gen)) return;
        guard::PollResult pr;
        if (!r.net_ok || r.http != 200 || r.eresult != 1 || !guard::pb_poll_response(r.body, pr)) return set_flow(gen, FlowStep::Failed, net_error_text(r), true);
        if (pr.new_client_id) {
            std::lock_guard<std::mutex> lk(fmtx_);
            if (flow_gen_ == gen) pend_.client_id = pr.new_client_id;
        }
        if (!pr.refresh_token.empty()) return login_finish(pr, gen);
        if (std::chrono::steady_clock::now() > deadline)
            return set_flow(gen, FlowStep::Failed, TR("Время ожидания вышло — попробуйте ещё раз", "Timed out — please try again"), true);
        int ms = (int)(std::clamp(p.interval, 1.0f, 10.0f) * 1000);
        for (int waited = 0; waited < ms; waited += 100) {
            if (flow_cancelled(gen)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void Guard::login_finish(const guard::PollResult& pr, int gen) {
    Pending p;
    FlowState st;
    {
        std::lock_guard<std::mutex> lk(fmtx_);
        if (flow_gen_ != gen) return;
        p = pend_;
        st = fs_;
    }
    guard::Session s;
    s.steamid = p.steamid ? p.steamid : guard::jwt_steamid(pr.refresh_token);
    s.refresh_token = pr.refresh_token;
    s.access_token = pr.access_token;
    s.session_id = random_session_id();
    if (s.access_token.empty()) {
        Api r = call_form(std::string(kApi) + "IAuthenticationService/GenerateAccessTokenForApp/v1/",
                          {{"refresh_token", s.refresh_token}, {"steamid", std::to_string(s.steamid)}, {"renewal_type", "0"}});
        std::string access, refresh;
        if (r.net_ok && r.http == 200 && guard::parse_access_token(r.body, access, refresh)) s.access_token = access;
    }
    if (flow_cancelled(gen)) return;
    if (st.kind == FlowKind::Session) {
        std::string key = key_for_steamid(s.steamid);
        if (!key.empty()) {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = items_.find(key);
            if (it != items_.end()) {
                it->second.acc.session = s;
                it->second.confs.needauth = false;
                save_locked(it->second);
            }
        } else {
            std::lock_guard<std::mutex> lk(mtx_);
            WebSession& w = sessions_[s.steamid];
            w.account_name = pr.account_name.empty() ? p.login : pr.account_name;
            w.session = s;
            save_session_locked(s.steamid);
        }
        {
            std::lock_guard<std::mutex> lk(fmtx_);
            if (flow_gen_ != gen) return;
            fs_.key = std::to_string(s.steamid);
        }
        set_flow(gen, FlowStep::Done, TR("Вход выполнен — сессия сохранена, пароль нет", "Signed in — the session is saved, the password is not"));
        return;
    }
    if (st.kind == FlowKind::Login) {
        std::string key = st.key;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = items_.find(key);
            if (it == items_.end()) return set_flow(gen, FlowStep::Failed, TR("Аккаунт удалён из списка", "The account was removed"), true);
            bool other = (it->second.acc.steamid && s.steamid && it->second.acc.steamid != s.steamid) ||
                         (!it->second.acc.steamid && !pr.account_name.empty() && !it->second.acc.account_name.empty() &&
                          !iequals(pr.account_name, it->second.acc.account_name));
            if (other)
                return set_flow(gen, FlowStep::Failed, TR("Вы вошли в другой аккаунт, чем у этого аутентификатора", "You signed in to a different account than this authenticator's"), true);
            it->second.acc.session = s;
            if (p.own_code) it->second.acc.fully_enrolled = true;  // Steam accepted a code from these secrets
            it->second.confs.needauth = false;
            it->second.confs.error.clear();
            if (!pr.account_name.empty()) it->second.acc.account_name = pr.account_name;
            save_locked(it->second);
            if (!it->second.acc.steamid && s.steamid) key = rekey_locked(key, s.steamid);
        }
        {
            std::lock_guard<std::mutex> lk(fmtx_);
            if (flow_gen_ != gen) return;
            fs_.key = key;
        }
        set_flow(gen, FlowStep::Done, TR("Сессия обновлена — подтверждения снова работают", "Session refreshed — confirmations work again"));
        fetch(key, false);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(fmtx_);
        if (flow_gen_ != gen) return;
        pend_.session = s;
        if (!pr.account_name.empty()) {
            pend_.login = pr.account_name;
            fs_.account_name = pr.account_name;
        }
    }
    link_add(gen);
}

void Guard::link_add(int gen) {
    set_flow(gen, FlowStep::Working, TR("Подключаю аутентификатор…", "Adding the authenticator…"));
    Pending p;
    {
        std::lock_guard<std::mutex> lk(fmtx_);
        if (flow_gen_ != gen) return;
        p = pend_;
    }
    std::string device = guard::random_device_id();
    Api r = call_form(api_url("ITwoFactorService/AddAuthenticator/v1/", p.session.access_token),
                      {{"steamid", std::to_string(p.session.steamid)},
                       {"authenticator_time", std::to_string(now())},
                       {"authenticator_type", "1"},
                       {"device_identifier", device},
                       {"sms_phone_id", "1"},
                       {"version", "2"}});
    if (flow_cancelled(gen)) return;
    guard::AddAuthResult ar;
    if (!r.net_ok || r.http != 200 || !guard::parse_add_authenticator(r.body, ar)) return set_flow(gen, FlowStep::Failed, net_error_text(r), true);
    switch (ar.status) {
        case 1: {
            guard::Account a = ar.account;
            a.device_id = device;
            a.session = p.session;
            a.steamid = p.session.steamid;
            if (a.account_name.empty()) a.account_name = p.login;
            a.fully_enrolled = false;
            bool was_new = false;
            // saved right away: the secrets must never exist only in memory
            std::string key = add_or_update(a, "link", was_new);
            {
                std::lock_guard<std::mutex> lk(fmtx_);
                if (flow_gen_ != gen) return;
                pend_.linking = a;
                fs_.key = key;
                fs_.revocation = a.revocation_code;
                fs_.hint = a.phone_number_hint;
            }
            emit({});
            return set_flow(gen, FlowStep::NeedActivation, "");
        }
        case 2:
            return set_flow(gen, FlowStep::NeedPhone, "");
        case 29:
            return set_flow(gen, FlowStep::Failed,
                            TR("На аккаунте уже есть мобильный аутентификатор. Импортируйте его maFile или сначала удалите аутентификатор в приложении Steam.",
                               "The account already has a mobile authenticator. Import its maFile or remove the authenticator in the Steam app first."),
                            true);
        case 84:
            return set_flow(gen, FlowStep::Failed, eresult_text(84), true);
        default:
            return set_flow(gen, FlowStep::Failed, std::string(TR("Steam отказал в привязке (статус ", "Steam refused to link (status ")) + std::to_string(ar.status) + ")", true);
    }
}

void Guard::flow_phone(const std::string& phone_in, const std::string& country_in) {
    std::string phone;
    for (char c : phone_in)
        if ((c >= '0' && c <= '9') || (c == '+' && phone.empty())) phone += c;
    if (!phone.empty() && phone[0] != '+') phone = "+" + phone;
    std::string country = trim(country_in);
    for (auto& c : country) c = (char)toupper((unsigned char)c);
    set_flow(FlowStep::Working, TR("Добавляю номер телефона…", "Adding the phone number…"));
    run_flow([this, phone, country](int gen) {
        Pending p;
        {
            std::lock_guard<std::mutex> lk(fmtx_);
            p = pend_;
        }
        std::string cc = country;
        if (cc.empty()) {
            Api c = call_form(api_url("IUserAccountService/GetUserCountry/v1", p.session.access_token), {{"steamid", std::to_string(p.session.steamid)}});
            cc = c.net_ok ? guard::parse_user_country(c.body) : std::string();
        }
        Api r = call_form(api_url("IPhoneService/SetAccountPhoneNumber/v1", p.session.access_token), {{"phone_number", phone}, {"phone_country_code", cc}});
        if (flow_cancelled(gen)) return;
        std::string email;
        if (r.net_ok && r.http == 200 && guard::parse_set_phone(r.body, email) && !email.empty()) {
            {
                std::lock_guard<std::mutex> lk(fmtx_);
                if (flow_gen_ != gen) return;
                fs_.hint = email;
            }
            return set_flow(gen, FlowStep::NeedPhoneEmail, "");
        }
        set_flow(gen, FlowStep::NeedPhone,
                 r.net_ok ? std::string(TR("Steam не принял номер. Укажите его целиком, с кодом страны: +7 999 123-45-67", "Steam didn't accept the number. Enter it in full with the country code: +1 555 123 4567"))
                          : net_error_text(r),
                 true);
    });
}

void Guard::flow_phone_email_done() {
    set_flow(FlowStep::Working, TR("Проверяю подтверждение почты…", "Checking the e-mail confirmation…"));
    run_flow([this](int gen) {
        Pending p;
        {
            std::lock_guard<std::mutex> lk(fmtx_);
            p = pend_;
        }
        Api w = call_form(api_url("IPhoneService/IsAccountWaitingForEmailConfirmation/v1", p.session.access_token), {});
        bool awaiting = true;
        if (!w.net_ok || !guard::parse_waiting_email(w.body, awaiting)) return set_flow(gen, FlowStep::NeedPhoneEmail, net_error_text(w), true);
        if (awaiting) return set_flow(gen, FlowStep::NeedPhoneEmail, TR("Steam ещё ждёт подтверждения — нажмите ссылку в письме и повторите", "Steam is still waiting — click the link in the e-mail and retry"), true);
        call_form(api_url("IPhoneService/SendPhoneVerificationCode/v1", p.session.access_token), {{"language", "0"}});
        for (int i = 0; i < 20 && !flow_cancelled(gen); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (flow_cancelled(gen)) return;
        set_flow(gen, FlowStep::NeedPhoneSms, "");
    });
}

void Guard::flow_sms(const std::string& code) {
    std::string c = trim(code);
    set_flow(FlowStep::Working, TR("Проверяю код из SMS…", "Checking the SMS code…"));
    run_flow([this, c](int gen) {
        Pending p;
        {
            std::lock_guard<std::mutex> lk(fmtx_);
            p = pend_;
        }
        Api r = call_form(api_url("IPhoneService/VerifyAccountPhoneWithCode/v1/", p.session.access_token), {{"code", c}});
        if (flow_cancelled(gen)) return;
        if (!r.net_ok || r.http != 200 || (r.eresult != 1 && r.eresult != 0))
            return set_flow(gen, FlowStep::NeedPhoneSms, r.eresult == 65 || r.eresult == 8 ? std::string(TR("Неверный код из SMS", "Wrong SMS code")) : net_error_text(r), true);
        link_add(gen);
    });
}

void Guard::flow_finalize(const std::string& activation_in) {
    std::string activation = trim(activation_in);
    for (auto& c : activation) c = (char)toupper((unsigned char)c);
    set_flow(FlowStep::Working, TR("Активирую аутентификатор…", "Activating the authenticator…"));
    run_flow([this, activation](int gen) {
        Pending p;
        std::string key;
        {
            std::lock_guard<std::mutex> lk(fmtx_);
            p = pend_;
            key = fs_.key;
        }
        int64_t t = now();
        int resyncs = 0;
        for (int tries = 0; tries <= 10; ++tries) {
            Api r = call_form(api_url("ITwoFactorService/FinalizeAddAuthenticator/v1/", p.session.access_token),
                              {{"steamid", std::to_string(p.session.steamid)},
                               {"authenticator_code", guard::code_for_time(p.linking.shared_secret, t)},
                               {"authenticator_time", std::to_string(t)},
                               {"activation_code", activation},
                               {"validate_sms_code", "1"}});
            if (flow_cancelled(gen)) return;
            guard::FinalizeResult fr;
            if (!r.net_ok || r.http != 200 || !guard::parse_finalize(r.body, fr)) return set_flow(gen, FlowStep::NeedActivation, net_error_text(r), true);
            if (fr.status == 89) return set_flow(gen, FlowStep::NeedActivation, TR("Неверный код активации — проверьте и введите ещё раз", "Wrong activation code — check it and try again"), true);
            if (fr.status == 88) {
                if (resyncs++ < 2) {
                    sync_time();
                    t = now();
                    continue;
                }
                return set_flow(gen, FlowStep::Failed, eresult_text(88), true);
            }
            if (!fr.success) return set_flow(gen, FlowStep::NeedActivation, std::string(TR("Steam не активировал аутентификатор (статус ", "Steam didn't activate it (status ")) + std::to_string(fr.status) + ")", true);
            if (fr.want_more) {
                t += guard::kCodePeriod;
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(mtx_);
                auto it = items_.find(key);
                if (it != items_.end()) {
                    it->second.acc.fully_enrolled = true;
                    if (fr.server_time) it->second.acc.server_time = fr.server_time;
                    save_locked(it->second);
                }
            }
            log::info("guard: linked authenticator for " + p.linking.account_name);
            set_flow(gen, FlowStep::Done, "");
            fetch(key, false);
            return;
        }
        set_flow(gen, FlowStep::Failed, TR("Steam так и не принял коды — попробуйте позже", "Steam kept rejecting the codes — try again later"), true);
    });
}

void Guard::deactivate(const std::string& key, int scheme, std::function<void(bool, const std::string&)> done) {
    post([this, key, scheme, done] {
        guard::Account a;
        Sess s = ensure_session(key, false, a);
        bool ok = false;
        std::string msg;
        if (s != Sess::Ok) {
            msg = s == Sess::NeedLogin ? TR("Сначала войдите заново — сессия Steam истекла", "Sign in again first — the Steam session expired")
                                       : TR("Нет связи со Steam", "Can't reach Steam");
        } else if (a.revocation_code.empty()) {
            msg = TR("В maFile нет кода восстановления — удалите аутентификатор через приложение Steam или сайт", "The maFile has no revocation code — remove it via the Steam app or website");
        } else {
            Api r = call_form(api_url("ITwoFactorService/RemoveAuthenticator/v1", a.session.access_token),
                              {{"revocation_code", a.revocation_code}, {"revocation_reason", "1"}, {"steamguard_scheme", std::to_string(scheme)}});
            guard::RemoveResult rr;
            if (r.net_ok && r.http == 200 && guard::parse_remove(r.body, rr) && rr.success) {
                ok = true;
                remove_local(key);
                msg = scheme == 2 ? TR("Steam Guard отключён на аккаунте", "Steam Guard is off for the account")
                                  : TR("Аутентификатор удалён, коды снова будут приходить на почту", "Authenticator removed, codes will come by e-mail again");
                log::info("guard: removed authenticator for " + a.account_name);
            } else {
                msg = r.net_ok && rr.parsed ? std::string(TR("Steam не удалил аутентификатор", "Steam didn't remove the authenticator")) +
                                                  (rr.attempts_left >= 0 ? std::string(TR(" · осталось попыток: ", " · attempts left: ")) + std::to_string(rr.attempts_left) : "")
                                            : net_error_text(r);
            }
        }
        emit({});
        if (done) ui_post([done, ok, msg] { done(ok, msg); });
    });
}

}  // namespace vs
