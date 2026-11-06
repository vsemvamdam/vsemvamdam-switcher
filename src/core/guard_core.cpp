#include "guard_core.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "crypto.h"
#include "protobuf.h"
#include "str.h"

using nlohmann::json;

namespace vs::guard {

// ============================================================ JSON helpers
static std::string js(const json& j, const char* k) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_unsigned()) return std::to_string(it->get<uint64_t>());
    if (it->is_number_integer()) return std::to_string(it->get<int64_t>());
    if (it->is_boolean()) return it->get<bool>() ? "true" : "false";
    return {};
}
static uint64_t ju64(const json& j, const char* k) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return 0;
    if (it->is_number_unsigned()) return it->get<uint64_t>();
    if (it->is_number_integer()) return (uint64_t)std::max<int64_t>(0, it->get<int64_t>());
    if (it->is_number_float()) return (uint64_t)std::max(0.0, it->get<double>());
    uint64_t v = 0;
    if (it->is_string()) parse_u64(trim(it->get<std::string>()), v);
    return v;
}
static int64_t ji64(const json& j, const char* k) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return 0;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number_float()) return (int64_t)it->get<double>();
    int64_t v = 0;
    if (it->is_string()) parse_i64(trim(it->get<std::string>()), v);
    if (it->is_boolean()) v = it->get<bool>() ? 1 : 0;
    return v;
}
static bool jbool(const json& j, const char* k, bool def = false) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return def;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0;
    if (it->is_string()) {
        std::string s = to_lower(it->get<std::string>());
        return s == "true" || s == "1";
    }
    return def;
}
// case-insensitive key lookup (maFiles from different tools vary in capitalisation)
static const json* jfind_ci(const json& j, const char* k) {
    if (!j.is_object()) return nullptr;
    for (auto it = j.begin(); it != j.end(); ++it)
        if (iequals(it.key(), k)) return &it.value();
    return nullptr;
}
static uint64_t steamid_prefix(const std::string& cookie) {
    // legacy "76561198xxxxxxxxx%7C%7C<token>" / "...||..."
    std::string digits;
    for (char c : cookie) {
        if (c >= '0' && c <= '9') digits += c;
        else break;
    }
    uint64_t v = 0;
    if (digits.size() == 17 && parse_u64(digits, v)) return v;
    return 0;
}
static bool valid_id64(uint64_t v) { return v > 76561197960265728ULL && v < 76561202255233023ULL; }

// ============================================================ maFile
bool parse_mafile(const std::string& text_in, Account& a, std::string* err) {
    a = Account{};
    std::string text = text_in;
    if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) text = text.substr(3);
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) {
        if (err) *err = "not a JSON object";
        return false;
    }
    a.extra = j;
    a.shared_secret = js(j, "shared_secret");
    a.identity_secret = js(j, "identity_secret");
    a.secret_1 = js(j, "secret_1");
    a.revocation_code = js(j, "revocation_code");
    a.serial_number = js(j, "serial_number");
    a.uri = js(j, "uri");
    a.token_gid = js(j, "token_gid");
    a.device_id = js(j, "device_id");
    a.account_name = js(j, "account_name");
    a.phone_number_hint = js(j, "phone_number_hint");
    a.server_time = ji64(j, "server_time");
    a.status = (int)ji64(j, "status");
    a.confirm_type = (int)ji64(j, "confirm_type");
    a.fully_enrolled = jbool(j, "fully_enrolled", true);
    crypto::Bytes sec;
    if (a.shared_secret.empty() || !crypto::base64_decode(a.shared_secret, sec) || sec.size() < 10) {
        if (err) *err = "shared_secret is missing or invalid";
        return false;
    }
    if (const json* s = jfind_ci(j, "Session"); s && s->is_object()) {
        a.session.steamid = ju64(*s, "SteamID");
        if (!a.session.steamid) a.session.steamid = ju64(*s, "steamid");
        a.session.access_token = js(*s, "AccessToken");
        a.session.refresh_token = js(*s, "RefreshToken");
        a.session.session_id = js(*s, "SessionID");
        if (!a.session.steamid) a.session.steamid = steamid_prefix(js(*s, "SteamLoginSecure"));
        if (!a.session.steamid) a.session.steamid = steamid_prefix(js(*s, "SteamLogin"));
        if (!a.session.steamid && !a.session.refresh_token.empty()) a.session.steamid = jwt_steamid(a.session.refresh_token);
    }
    uint64_t id = a.session.steamid;
    for (const char* k : {"steamid", "SteamID", "steam_id", "steamid64"})
        if (!id) id = ju64(j, k);
    if (valid_id64(id)) a.steamid = id;
    if (a.session.steamid && !valid_id64(a.session.steamid)) a.session.steamid = 0;
    if (!a.session.steamid) a.session.steamid = a.steamid;
    return true;
}

std::string to_mafile(const Account& a, bool pretty) {
    json j = a.extra.is_object() ? a.extra : json::object();
    j["shared_secret"] = a.shared_secret;
    j["serial_number"] = a.serial_number;
    j["revocation_code"] = a.revocation_code;
    j["uri"] = a.uri;
    j["server_time"] = a.server_time;
    j["account_name"] = a.account_name;
    j["token_gid"] = a.token_gid;
    j["identity_secret"] = a.identity_secret;
    j["secret_1"] = a.secret_1;
    j["status"] = a.status;
    j["device_id"] = a.device_id;
    j["fully_enrolled"] = a.fully_enrolled;
    if (!a.phone_number_hint.empty()) j["phone_number_hint"] = a.phone_number_hint;
    if (a.confirm_type) j["confirm_type"] = a.confirm_type;
    // drop any differently-cased legacy session key, then write the current SDA layout
    for (auto it = j.begin(); it != j.end();) {
        if (iequals(it.key(), "session")) it = j.erase(it);
        else ++it;
    }
    json s = json::object();
    uint64_t sid = a.session.steamid ? a.session.steamid : a.steamid;
    s["SteamID"] = sid;
    s["AccessToken"] = a.session.access_token.empty() ? json(nullptr) : json(a.session.access_token);
    s["RefreshToken"] = a.session.refresh_token.empty() ? json(nullptr) : json(a.session.refresh_token);
    s["SessionID"] = a.session.session_id.empty() ? json(nullptr) : json(a.session.session_id);
    j["Session"] = s;
    return pretty ? j.dump(2) : j.dump();
}

uint64_t steamid_from_filename(const std::string& filename) {
    std::string digits;
    for (char c : filename) {
        if (c >= '0' && c <= '9') digits += c;
        else if (!digits.empty()) break;
    }
    uint64_t v = 0;
    if (digits.size() == 17 && parse_u64(digits, v) && valid_id64(v)) return v;
    return 0;
}

// ============================================================ codes
std::string code_for_time(const std::string& secret_b64, int64_t t) {
    crypto::Bytes key;
    if (!crypto::base64_decode(secret_b64, key) || key.empty()) return {};
    uint64_t step = (uint64_t)(t / kCodePeriod);
    uint8_t msg[8];
    for (int i = 7; i >= 0; --i) { msg[i] = (uint8_t)step; step >>= 8; }
    crypto::Bytes h = crypto::hmac_sha1(key, msg, 8);
    int off = h[19] & 0x0F;
    uint32_t v = ((uint32_t)(h[off] & 0x7F) << 24) | ((uint32_t)h[off + 1] << 16) | ((uint32_t)h[off + 2] << 8) | h[off + 3];
    static const char* chars = "23456789BCDFGHJKMNPQRTVWXY";
    std::string code;
    for (int i = 0; i < 5; ++i) {
        code += chars[v % 26];
        v /= 26;
    }
    crypto::wipe(key);
    return code;
}

std::string confirmation_key(const std::string& secret_b64, int64_t t, const std::string& tag) {
    crypto::Bytes key;
    if (!crypto::base64_decode(secret_b64, key) || key.empty()) return {};
    crypto::Bytes msg(8);
    uint64_t v = (uint64_t)t;
    for (int i = 7; i >= 0; --i) { msg[i] = (uint8_t)v; v >>= 8; }
    msg.insert(msg.end(), tag.begin(), tag.begin() + std::min<size_t>(tag.size(), 32));
    std::string r = crypto::base64_encode(crypto::hmac_sha1(key, msg));
    crypto::wipe(key);
    return r;
}

std::string confirmation_params(const Account& a, int64_t t, const std::string& tag) {
    uint64_t sid = a.session.steamid ? a.session.steamid : a.steamid;
    std::string dev = a.device_id.empty() ? device_id_for(sid) : a.device_id;
    return "p=" + url_encode(dev) + "&a=" + std::to_string(sid) + "&k=" + url_encode(confirmation_key(a.identity_secret, t, tag)) +
           "&t=" + std::to_string(t) + "&m=react&tag=" + url_encode(tag);
}

static std::string uuid_format(const std::string& hex) {
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20, 12);
}
std::string device_id_for(uint64_t steamid) {
    std::string s = std::to_string(steamid);
    return "android:" + uuid_format(crypto::hex_encode(crypto::sha1((const uint8_t*)s.data(), s.size())));
}
std::string random_device_id() {
    crypto::Bytes b = crypto::random_bytes(16);
    if (b.size() != 16) b = crypto::sha1(crypto::to_bytes(std::to_string(unix_now()))), b.resize(16);
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);
    return "android:" + uuid_format(crypto::hex_encode(b));
}

// ============================================================ SDA
bool parse_manifest(const std::string& text_in, Manifest& m) {
    m = Manifest{};
    std::string text = text_in;
    if (text.size() >= 3 && (uint8_t)text[0] == 0xEF) text = text.substr(3);
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return false;
    m.encrypted = jbool(j, "encrypted");
    m.periodic_checking = jbool(j, "periodic_checking");
    m.check_all = jbool(j, "periodic_checking_checkall");
    m.auto_market = jbool(j, "auto_confirm_market_transactions");
    m.auto_trades = jbool(j, "auto_confirm_trades");
    m.interval = (int)ji64(j, "periodic_checking_interval");
    if (m.interval <= 0) m.interval = 5;
    auto it = j.find("entries");
    if (it != j.end() && it->is_array()) {
        for (auto& e : *it) {
            if (!e.is_object()) continue;
            ManifestEntry me;
            me.filename = js(e, "filename");
            me.iv = js(e, "encryption_iv");
            me.salt = js(e, "encryption_salt");
            me.steamid = ju64(e, "steamid");
            if (!me.filename.empty()) m.entries.push_back(me);
        }
    }
    return true;
}

bool sda_decrypt(const std::string& passkey, const std::string& salt_b64, const std::string& iv_b64, const std::string& data_b64,
                 std::string& plain) {
    plain.clear();
    crypto::Bytes salt, iv, data, out;
    if (!crypto::base64_decode(salt_b64, salt) || !crypto::base64_decode(iv_b64, iv) || !crypto::base64_decode(trim(data_b64), data))
        return false;
    if (iv.size() != 16 || salt.empty()) return false;
    crypto::Bytes key = crypto::pbkdf2_sha1(passkey, salt, kSdaIterations, 32);
    bool ok = crypto::aes256_cbc_decrypt(key, iv, data, out);
    crypto::wipe(key);
    if (!ok) return false;
    plain = crypto::to_string(out);
    crypto::wipe(out);
    if (plain.size() >= 3 && (uint8_t)plain[0] == 0xEF && (uint8_t)plain[1] == 0xBB && (uint8_t)plain[2] == 0xBF) plain = plain.substr(3);
    // a wrong passkey with lucky padding still yields garbage; the result must be JSON
    size_t p = plain.find_first_not_of(" \t\r\n");
    if (p == std::string::npos || plain[p] != '{') {
        crypto::wipe(plain);
        return false;
    }
    return true;
}

std::string sda_encrypt(const std::string& passkey, const std::string& salt_b64, const std::string& iv_b64, const std::string& plain) {
    crypto::Bytes salt, iv;
    if (!crypto::base64_decode(salt_b64, salt) || !crypto::base64_decode(iv_b64, iv) || iv.size() != 16) return {};
    crypto::Bytes key = crypto::pbkdf2_sha1(passkey, salt, kSdaIterations, 32);
    crypto::Bytes c = crypto::aes256_cbc_encrypt(key, iv, crypto::to_bytes(plain));
    crypto::wipe(key);
    return crypto::base64_encode(c);
}

// ============================================================ JWT
static json jwt_payload(const std::string& token) {
    auto parts = split(token, '.');
    if (parts.size() < 2) return {};
    crypto::Bytes b;
    if (!crypto::base64_decode(parts[1], b)) return {};
    return json::parse(crypto::to_string(b), nullptr, false);
}
int64_t jwt_exp(const std::string& token) {
    json p = jwt_payload(token);
    return p.is_object() ? ji64(p, "exp") : 0;
}
uint64_t jwt_steamid(const std::string& token) {
    json p = jwt_payload(token);
    return p.is_object() ? ju64(p, "sub") : 0;
}

// ============================================================ confirmations
bool parse_confirmations(const std::string& body, ConfList& out) {
    out = ConfList{};
    json j = json::parse(body, nullptr, false);
    if (!j.is_object()) {
        // the community redirects to a login page when the session is gone
        out.needauth = icontains(body, "<html") || icontains(body, "login");
        out.message = "unexpected response";
        return false;
    }
    out.needauth = jbool(j, "needauth");
    out.message = js(j, "message");
    if (out.message.empty()) out.message = js(j, "detail");
    out.ok = jbool(j, "success");
    auto it = j.find("conf");
    if (it != j.end() && it->is_array()) {
        for (auto& c : *it) {
            if (!c.is_object()) continue;
            Confirmation x;
            x.id = js(c, "id");
            x.nonce = js(c, "nonce");
            x.creator_id = js(c, "creator_id");
            x.type = (int)ji64(c, "type");
            x.type_name = js(c, "type_name");
            x.headline = js(c, "headline");
            x.icon = js(c, "icon");
            x.accept = js(c, "accept");
            x.cancel = js(c, "cancel");
            x.warn = js(c, "warn");
            x.time = ji64(c, "creation_time");
            x.multi = jbool(c, "multi");
            auto s = c.find("summary");
            if (s != c.end() && s->is_array())
                for (auto& line : *s)
                    if (line.is_string()) x.summary.push_back(line.get<std::string>());
            if (!x.id.empty() && !x.nonce.empty()) out.items.push_back(std::move(x));
        }
    }
    return out.ok;
}

bool parse_success(const std::string& body) {
    json j = json::parse(body, nullptr, false);
    return j.is_object() && jbool(j, "success");
}

// ============================================================ web helpers
std::string url_encode(std::string_view s) {
    static const char* H = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += H[c >> 4]; o += H[c & 15]; }
    }
    return o;
}
std::string form_encode(const KV& kv) {
    std::string o;
    for (auto& [k, v] : kv) {
        if (!o.empty()) o += '&';
        o += url_encode(k) + "=" + url_encode(v);
    }
    return o;
}
std::string multipart_body(const KV& kv, const std::string& boundary) {
    std::string o;
    for (auto& [k, v] : kv) {
        o += "--" + boundary + "\r\n";
        o += "Content-Disposition: form-data; name=\"" + k + "\"\r\n\r\n";
        o += v + "\r\n";
    }
    o += "--" + boundary + "--\r\n";
    return o;
}

// ============================================================ protobuf messages
std::string pb_rsa_key_request(const std::string& account) {
    pb::Writer w;
    w.str(1, account);
    return w.out;
}
bool pb_rsa_key_response(const std::string& body, std::string& mod, std::string& exp, uint64_t& ts) {
    std::vector<pb::Field> f;
    if (!pb::parse(body, f)) return false;
    mod = pb::get_str(f, 1);
    exp = pb::get_str(f, 2);
    ts = pb::get_num(f, 3);
    return !mod.empty() && !exp.empty();
}
std::string pb_begin_auth_request(const std::string& account, const std::string& enc_pw, uint64_t ts, const std::string& device_name) {
    pb::Writer dd;
    dd.str(1, device_name);
    dd.u64(2, 3);      // EAuthTokenPlatformType_MobileApp
    dd.i32(3, -500);   // EOSType_AndroidUnknown
    dd.u64(4, 528);    // gaming_device_type, as the Steam mobile app sends it
    pb::Writer w;
    w.str(2, account);
    w.str(3, enc_pw);
    w.u64(4, ts);
    w.boolean(5, true);   // remember_login
    w.u64(7, 1);          // ESessionPersistence_Persistent
    w.str(8, "Mobile");   // website_id
    w.message(9, dd);
    return w.out;
}
bool pb_begin_auth_response(const std::string& body, BeginAuth& out) {
    out = BeginAuth{};
    std::vector<pb::Field> f;
    if (!pb::parse(body, f)) return false;
    for (auto& x : f) {
        switch (x.number) {
            case 1: out.client_id = x.num; break;
            case 2: out.request_id = x.data; break;
            case 3: out.interval = x.as_float(); break;
            case 4: {
                std::vector<pb::Field> c;
                if (pb::parse(x.data, c)) {
                    out.allowed.push_back((int)pb::get_num(c, 1));
                    out.messages.push_back(pb::get_str(c, 2));
                }
                break;
            }
            case 5: out.steamid = x.num; break;
            case 6: out.weak_token = x.data; break;
            case 8: out.extended_error = x.data; break;
            default: break;
        }
    }
    if (!(out.interval > 0.5f && out.interval < 60.0f)) out.interval = 5;
    return out.client_id != 0;
}
std::string pb_update_code_request(uint64_t client_id, uint64_t steamid, const std::string& code, int code_type) {
    pb::Writer w;
    w.u64(1, client_id);
    w.fixed64(2, steamid);
    w.str(3, code);
    w.u64(4, (uint64_t)code_type);
    return w.out;
}
std::string pb_poll_request(uint64_t client_id, const std::string& request_id) {
    pb::Writer w;
    w.u64(1, client_id);
    w.bytes(2, request_id);
    return w.out;
}
bool pb_poll_response(const std::string& body, PollResult& out) {
    out = PollResult{};
    std::vector<pb::Field> f;
    if (!pb::parse(body, f)) return false;
    out.new_client_id = pb::get_num(f, 1);
    out.refresh_token = pb::get_str(f, 3);
    out.access_token = pb::get_str(f, 4);
    out.had_remote_interaction = pb::get_num(f, 5) != 0;
    out.account_name = pb::get_str(f, 6);
    out.new_guard_data = pb::get_str(f, 7);
    return true;
}

// ============================================================ JSON responses
static json response_of(const std::string& body) {
    json j = json::parse(body, nullptr, false);
    if (!j.is_object()) return {};
    auto it = j.find("response");
    if (it == j.end() || !it->is_object()) return {};
    return *it;
}
int64_t parse_query_time(const std::string& body) {
    json r = response_of(body);
    return r.is_object() ? ji64(r, "server_time") : 0;
}
bool parse_access_token(const std::string& body, std::string& access, std::string& refresh) {
    json r = response_of(body);
    if (!r.is_object()) return false;
    access = js(r, "access_token");
    refresh = js(r, "refresh_token");
    return !access.empty();
}
bool parse_add_authenticator(const std::string& body, AddAuthResult& out) {
    out = AddAuthResult{};
    json r = response_of(body);
    if (!r.is_object()) return false;
    out.parsed = true;
    out.status = (int)ji64(r, "status");
    if (out.status == 1) {
        std::string err;
        if (!parse_mafile(r.dump(), out.account, &err)) { out.parsed = false; return false; }
        out.account.fully_enrolled = false;
    }
    return true;
}
bool parse_finalize(const std::string& body, FinalizeResult& out) {
    out = FinalizeResult{};
    json r = response_of(body);
    if (!r.is_object()) return false;
    out.parsed = true;
    out.success = jbool(r, "success");
    out.want_more = jbool(r, "want_more");
    out.status = (int)ji64(r, "status");
    out.server_time = ji64(r, "server_time");
    return true;
}
bool parse_remove(const std::string& body, RemoveResult& out) {
    out = RemoveResult{};
    json r = response_of(body);
    if (!r.is_object()) return false;
    out.parsed = true;
    out.success = jbool(r, "success");
    if (r.contains("revocation_attempts_remaining")) out.attempts_left = (int)ji64(r, "revocation_attempts_remaining");
    return true;
}
bool parse_phone_status(const std::string& body, bool& verified) {
    json r = response_of(body);
    if (!r.is_object()) return false;
    verified = jbool(r, "verified_phone");
    return true;
}
bool parse_set_phone(const std::string& body, std::string& email) {
    json r = response_of(body);
    if (!r.is_object()) return false;
    email = js(r, "confirmation_email_address");
    return true;
}
bool parse_waiting_email(const std::string& body, bool& awaiting) {
    json r = response_of(body);
    if (!r.is_object()) return false;
    awaiting = jbool(r, "awaiting_email_confirmation");
    return true;
}
std::string parse_user_country(const std::string& body) {
    json r = response_of(body);
    return r.is_object() ? js(r, "country") : std::string();
}

}  // namespace vs::guard
