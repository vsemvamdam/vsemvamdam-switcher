// vsemvamdam-switcher — Steam Guard mobile authenticator (Steam Desktop Authenticator replacement):
// encrypted storage of maFiles, login codes, trade/market confirmations, re-login, linking and removal.
// Network work runs on background threads; results are published through ui_post().
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../core/guard_core.h"

namespace vs {

class Store;

struct GuardConfs {
    bool loading = false;
    bool loaded = false;
    bool needauth = false;      // the session is gone, a fresh login is needed
    int64_t fetched_at = 0;
    std::string error;          // last error, user-facing
    std::vector<guard::Confirmation> items;
    std::set<std::string> busy;  // ids being accepted / denied right now
};

struct GuardView {
    std::string key;             // storage key (SteamID64, or "acc-<login>" until the SteamID is known)
    std::string account_name;
    uint64_t steamid = 0;
    bool has_session = false;    // refresh token present and not expired
    int64_t session_expires = 0; // refresh token expiry (unix)
    bool has_revocation = false;
    bool fully_enrolled = true;
    int64_t added_at = 0;
    GuardConfs confs;
};

struct ImportReport {
    int added = 0, updated = 0;
    std::vector<std::string> errors;     // per file, user-facing
    bool need_passkey = false;           // an encrypted SDA folder needs its passkey
    bool wrong_passkey = false;
    std::vector<std::string> retry;      // paths to import again once the passkey is known
    std::vector<std::string> keys;       // imported items
};

enum class FlowKind { Login, Link, Session };  // Session: sign in an account that has no authenticator here
enum class FlowStep { Idle, Working, NeedEmailCode, NeedDeviceCode, WaitConfirmation, NeedPhone, NeedPhoneEmail, NeedPhoneSms,
                      NeedActivation, Done, Failed };
struct FlowState {
    FlowKind kind = FlowKind::Login;
    FlowStep step = FlowStep::Idle;
    std::string key;          // item the flow works on (Login) / produced (Link)
    std::string account_name;
    std::string message;      // progress, info or error
    std::string hint;         // e-mail domain, phone hint, confirmation e-mail
    std::string revocation;   // Link: revocation code (shown before activation and at the end)
    bool error = false;       // message is an error (step stays interactive)
};

enum class GuardEventKind { Changed, NewConfirmations, AutoConfirmed, SessionExpired };
struct GuardEvent {
    GuardEventKind kind = GuardEventKind::Changed;
    std::string key;
    int count = 0;
    std::string text;   // e.g. headline of the first new confirmation
};

class Guard {
public:
    explicit Guard(Store& st);
    ~Guard();
    void load(bool start_threads = true);
    void stop();
    std::string dir() const;

    // ---- time and codes
    int64_t now() const;              // Steam server time (local clock + measured offset)
    int seconds_left() const;         // seconds until the next code
    float period_progress() const;    // 0..1 inside the current 30 s period
    void sync_time_async();
    std::string code(const std::string& key);
    std::string key_for_steamid(uint64_t id);
    std::string key_for_login(const std::string& login);

    // ---- items
    std::vector<GuardView> list();
    bool view(const std::string& key, GuardView& out);
    int pending_total();
    bool empty();

    // ---- files
    // Imports .maFile / .json files and folders (an SDA "maFiles" folder with manifest.json, or any folder with maFiles).
    // resolve_login maps an account name to a SteamID64 when the maFile does not say (0 if unknown).
    ImportReport import_paths(const std::vector<std::string>& paths, const std::string& passkey,
                              const std::function<uint64_t(const std::string&)>& resolve_login);
    // Writes <folder>\maFiles\manifest.json + <steamid>.maFile in SDA layout (encrypted when passkey is not empty).
    bool export_sda(const std::vector<std::string>& keys, const std::string& folder, const std::string& passkey, std::string& out_dir,
                    std::string& err);
    bool remove_local(const std::string& key);
    bool mark_enrolled(const std::string& key);  // linking finished on Steam's side but the answer was lost
    std::string revocation_code(const std::string& key);
    std::string icon_path(const std::string& url) const;  // local cache file for a confirmation icon ("" if not cached)
    // Plausible SDA installs on this PC (maFiles folders with a manifest).
    static std::vector<std::string> find_sda_folders();

    // ---- confirmations (async)
    void fetch(const std::string& key, bool background = false);
    void fetch_all(bool background);
    void respond(const std::string& key, const std::vector<std::string>& ids, bool accept);

    // ---- login / link flow (one at a time)
    FlowState flow();
    void flow_reset();
    void flow_login(const std::string& key, const std::string& password);
    void flow_link(const std::string& login, const std::string& password);
    void flow_session(const std::string& login, const std::string& password);
    void flow_code(const std::string& code);
    void flow_phone(const std::string& phone, const std::string& country);
    void flow_phone_email_done();
    void flow_sms(const std::string& code);
    void flow_finalize(const std::string& activation_code);
    // Removes the authenticator from the Steam account (scheme 1: back to e-mail codes, 2: no Steam Guard at all).
    void deactivate(const std::string& key, int scheme, std::function<void(bool ok, const std::string& msg)> done);

    // Talks to Steam without any account (server time, RSA key, a sign-in with a random name, confirmations without a
    // session) to check the transport. Lines describe each step; false when the protocol looks broken.
    static bool self_test(std::vector<std::string>& lines);

    // ---- web sessions (for the card farm etc.): from an authenticator's session or a sign-in without one
    bool has_web_session(uint64_t steamid);
    std::string web_session_name(uint64_t steamid);
    void forget_web_session(uint64_t steamid);
    // GET on steamcommunity.com as that account (cookie session; refreshes the token once on a redirect to login)
    bool community_get(uint64_t steamid, const std::string& path_query, std::string& body, int& status);
    // GET on api.steampowered.com with the account's access token appended (url already has a '?')
    bool webapi_get(uint64_t steamid, const std::string& url, std::string& body, int& status);

    // ---- events (called on the UI thread)
    void on_event(std::function<void(const GuardEvent&)> cb) { on_event_ = std::move(cb); }

private:
    struct Item {
        std::string key;
        guard::Account acc;
        GuardConfs confs;
        int64_t added_at = 0;
        std::string source;
        std::set<std::string> notified;  // confirmation ids already reported
    };
    enum class Sess { Ok, NeedLogin, NetError };

    Store& store_;
    std::mutex mtx_;
    std::map<std::string, Item> items_;
    struct WebSession {
        std::string account_name;
        guard::Session session;
    };
    std::map<uint64_t, WebSession> sessions_;  // sign-ins without an authenticator (guard\\sessions)
    bool web_session(uint64_t steamid, guard::Session& out, bool force_refresh);
    bool save_session_locked(uint64_t steamid);
    std::atomic<int64_t> offset_{0};
    std::atomic<bool> time_synced_{false};
    std::atomic<bool> stop_{false};
    std::function<void(const GuardEvent&)> on_event_;

    // job queue (confirmations, refreshes)
    std::thread worker_;
    std::mutex qmtx_;
    std::condition_variable qcv_;
    std::deque<std::function<void()>> queue_;
    void post(std::function<void()> job);
    void worker_main();
    // periodic checks
    std::thread checker_;
    std::condition_variable ccv_;
    std::mutex cmtx_;
    void checker_main();

    // flow
    std::thread flow_thread_;
    std::mutex fmtx_;
    FlowState fs_;
    std::atomic<bool> flow_cancel_{false};
    std::atomic<int> flow_gen_{0};
    struct Pending {
        uint64_t client_id = 0, steamid = 0;
        std::string request_id;
        float interval = 5;
        std::vector<int> allowed;
        guard::Session session;
        guard::Account linking;   // Link: the new authenticator before finalisation
        std::string login;
        bool own_code = false;    // sign-in confirmed with a code from our own shared_secret
    } pend_;
    std::vector<std::thread> old_flows_;
    void run_flow(std::function<void(int gen)> fn);
    void set_flow(FlowStep step, const std::string& msg, bool error = false);
    void set_flow(int gen, FlowStep step, const std::string& msg, bool error = false);  // ignored for a stale flow
    bool flow_cancelled(int gen) const { return flow_cancel_ || flow_gen_ != gen || stop_; }
    void login_begin(const std::string& login, const std::string& password, int gen);
    void login_poll(int gen, int timeout_s);
    void login_finish(const guard::PollResult& pr, int gen);
    void link_add(int gen);

    // helpers
    void emit(GuardEvent e);
    bool save(const std::string& key);  // caller must not hold mtx_
    bool save_locked(const Item& it);
    std::string file_of(const std::string& key) const;
    std::string make_key(const guard::Account& a) const;
    std::string add_or_update(guard::Account a, const std::string& source, bool& was_new);
    void backup_file_locked(const std::string& key);
    std::string rekey_locked(const std::string& old_key, uint64_t steamid);
    Sess ensure_session(const std::string& key, bool force_refresh, guard::Account& out);
    void do_fetch(const std::string& key, bool background);
    bool do_respond(const guard::Account& a, const std::vector<guard::Confirmation>& list, bool accept, std::string& err);
    void cache_icons(const std::vector<guard::Confirmation>& list);
    void sync_time();
};

// User-facing text for a Steam EResult
std::string eresult_text(int r);

}  // namespace vs
