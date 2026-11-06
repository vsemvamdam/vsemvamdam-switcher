// vsemvamdam-switcher — Steam Guard mobile authenticator core (portable, unit-tested):
// maFile (SDA-compatible) model, login codes, confirmation keys, SDA manifest + encryption,
// Steam auth-service protobuf messages and JSON responses.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

namespace vs::guard {

struct Session {
    uint64_t steamid = 0;
    std::string access_token, refresh_token, session_id;
};

struct Account {
    std::string shared_secret, identity_secret, secret_1;
    std::string revocation_code, serial_number, uri, token_gid, device_id, account_name;
    std::string phone_number_hint;
    int64_t server_time = 0;
    int status = 0;
    int confirm_type = 0;
    bool fully_enrolled = true;
    uint64_t steamid = 0;   // resolved from Session / file name / manifest / login
    Session session;
    nlohmann::json extra = nlohmann::json::object();  // original JSON, unknown fields survive an export
};

// ---- maFile
bool parse_mafile(const std::string& text, Account& out, std::string* err = nullptr);
std::string to_mafile(const Account& a, bool pretty = false);
// Steam ID from a file name like "76561198000000001.maFile" (0 if none).
uint64_t steamid_from_filename(const std::string& filename);

// ---- codes
constexpr int kCodePeriod = 30;
// Five-character Steam Guard code ("" if the secret is not valid base64).
std::string code_for_time(const std::string& shared_secret_b64, int64_t unix_time);
// Base64 HMAC used by /mobileconf (not URL-encoded).
std::string confirmation_key(const std::string& identity_secret_b64, int64_t unix_time, const std::string& tag);
// p=<device>&a=<steamid>&k=<key>&t=<time>&m=react&tag=<tag>
std::string confirmation_params(const Account& a, int64_t unix_time, const std::string& tag);
std::string device_id_for(uint64_t steamid);   // deterministic "android:xxxxxxxx-xxxx-..." (steam-totp style)
std::string random_device_id();               // "android:<uuid4>"

// ---- SDA maFiles folder
struct ManifestEntry {
    std::string filename, iv, salt;
    uint64_t steamid = 0;
};
struct Manifest {
    bool encrypted = false;
    std::vector<ManifestEntry> entries;
    bool periodic_checking = false, check_all = false, auto_market = false, auto_trades = false;
    int interval = 5;
};
bool parse_manifest(const std::string& text, Manifest& out);
constexpr uint32_t kSdaIterations = 50000;
bool sda_decrypt(const std::string& passkey, const std::string& salt_b64, const std::string& iv_b64, const std::string& data_b64,
                 std::string& plain);
std::string sda_encrypt(const std::string& passkey, const std::string& salt_b64, const std::string& iv_b64, const std::string& plain);

// ---- JWT (access / refresh tokens)
int64_t jwt_exp(const std::string& token);     // 0 if unparseable
uint64_t jwt_steamid(const std::string& token);  // "sub" claim

// ---- confirmations
enum ConfType { ConfGeneric = 1, ConfTrade = 2, ConfMarket = 3, ConfFeatureOptOut = 4, ConfPhoneChange = 5, ConfRecovery = 6,
                ConfApiKey = 9, ConfFamily = 11 };
struct Confirmation {
    std::string id, nonce, creator_id, type_name, headline, icon, accept, cancel, warn;
    std::vector<std::string> summary;
    int type = 0;
    int64_t time = 0;
    bool multi = false;
};
struct ConfList {
    bool ok = false;        // response parsed and success == true
    bool needauth = false;  // session is not valid any more
    std::string message;
    std::vector<Confirmation> items;
};
bool parse_confirmations(const std::string& body, ConfList& out);
bool parse_success(const std::string& body);  // {"success": true}

// ---- web helpers
using KV = std::vector<std::pair<std::string, std::string>>;
std::string url_encode(std::string_view s);
std::string form_encode(const KV& kv);
std::string multipart_body(const KV& kv, const std::string& boundary);

// ---- IAuthenticationService (protobuf over WebAPI, MobileApp platform)
enum GuardType { GuardUnknown = 0, GuardNone = 1, GuardEmailCode = 2, GuardDeviceCode = 3, GuardDeviceConfirmation = 4,
                 GuardEmailConfirmation = 5, GuardMachineToken = 6 };
std::string pb_rsa_key_request(const std::string& account);
bool pb_rsa_key_response(const std::string& body, std::string& mod_hex, std::string& exp_hex, uint64_t& timestamp);
struct BeginAuth {
    uint64_t client_id = 0, steamid = 0;
    std::string request_id, weak_token, extended_error;
    float interval = 5;
    std::vector<int> allowed;           // GuardType values
    std::vector<std::string> messages;  // associated_message per allowed entry (e.g. e-mail domain)
};
std::string pb_begin_auth_request(const std::string& account, const std::string& enc_password_b64, uint64_t timestamp,
                                  const std::string& device_name);
bool pb_begin_auth_response(const std::string& body, BeginAuth& out);
std::string pb_update_code_request(uint64_t client_id, uint64_t steamid, const std::string& code, int code_type);
std::string pb_poll_request(uint64_t client_id, const std::string& request_id);
struct PollResult {
    uint64_t new_client_id = 0;
    std::string refresh_token, access_token, account_name, new_guard_data;
    bool had_remote_interaction = false;
};
bool pb_poll_response(const std::string& body, PollResult& out);

// ---- JSON responses of ITwoFactorService / IAuthenticationService (form transport)
int64_t parse_query_time(const std::string& body);  // 0 on failure
bool parse_access_token(const std::string& body, std::string& access, std::string& refresh);
struct AddAuthResult {
    bool parsed = false;
    int status = 0;   // 1 ok, 2 no phone, 29 already has authenticator, 84 rate limit
    Account account;  // when status == 1
};
bool parse_add_authenticator(const std::string& body, AddAuthResult& out);
struct FinalizeResult {
    bool parsed = false, success = false, want_more = false;
    int status = 0;  // 89 bad activation code, 88 code mismatch
    int64_t server_time = 0;
};
bool parse_finalize(const std::string& body, FinalizeResult& out);
struct RemoveResult {
    bool parsed = false, success = false;
    int attempts_left = -1;
};
bool parse_remove(const std::string& body, RemoveResult& out);
// IPhoneService helpers
bool parse_phone_status(const std::string& body, bool& verified_phone);
bool parse_set_phone(const std::string& body, std::string& confirmation_email);
bool parse_waiting_email(const std::string& body, bool& awaiting);
std::string parse_user_country(const std::string& body);

}  // namespace vs::guard
