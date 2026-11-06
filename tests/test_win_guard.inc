// Steam Guard storage on real Windows (DPAPI, files, SDA folders) — no network.
#include "../src/app/guard.h"
#include "../src/core/crypto.h"
#include "../src/core/guard_core.h"

static std::string fake_jwt(uint64_t sid, int64_t exp) {
    std::string p = crypto::base64_encode(crypto::to_bytes("{\"sub\":\"" + std::to_string(sid) + "\",\"exp\":" + std::to_string(exp) + "}"));
    while (!p.empty() && p.back() == '=') p.pop_back();
    for (auto& c : p) c = c == '+' ? '-' : c == '/' ? '_' : c;
    return "eyJhbGciOiJFZERTQSJ9." + p + ".c2ln";
}

static void test_guard_store(const std::string& root) {
    const std::string sec1 = "AQIDBAUGBwgJCgsMDQ4PEBESExQ=";
    const std::string sec2 = "FBMSERAPDg0MCwoJCAcGBQQDAgE=";
    const std::string sec3 = "ICAgICAgICAgICAgICAgICAgICA=";
    std::string base = fsx::join(root, "guard");
    fsx::remove_all(base);
    fsx::mkdirs(base);
    Store st;
    st.load(fsx::join(base, "data"));

    // a plain maFile (new SDA layout, session included)
    std::string f1 = fsx::join(base, "main.maFile");
    fsx::write_file(f1, "{\"shared_secret\":\"" + sec1 + "\",\"identity_secret\":\"aWQ=\",\"account_name\":\"main_acc\",\"revocation_code\":\"R11111\","
                        "\"device_id\":\"android:x\",\"Session\":{\"SteamID\":76561198000000001,\"AccessToken\":\"" + fake_jwt(76561198000000001ULL, 4102444800) +
                        "\",\"RefreshToken\":\"" + fake_jwt(76561198000000001ULL, 4102444800) + "\"}}");
    // a maFile without SteamID: resolved from the Steam login list
    std::string f2 = fsx::join(base, "smurf.maFile");
    fsx::write_file(f2, "{\"shared_secret\":\"" + sec2 + "\",\"account_name\":\"Smurf\"}");
    // an encrypted SDA folder
    std::string sda = fsx::join(base, "SDA\\maFiles");
    fsx::mkdirs(sda);
    std::string salt = crypto::base64_encode(crypto::to_bytes("8bytes!!"));
    std::string iv = crypto::base64_encode(crypto::Bytes(16, 7));
    std::string body = guard::sda_encrypt("ключ-pass", salt, iv, "{\"shared_secret\":\"" + sec3 + "\",\"account_name\":\"third\",\"revocation_code\":\"R33333\"}");
    fsx::write_file(fsx::join(sda, "76561198000000003.maFile"), body);
    fsx::write_file(fsx::join(sda, "manifest.json"),
                    "{\"encrypted\":true,\"entries\":[{\"encryption_iv\":\"" + iv + "\",\"encryption_salt\":\"" + salt +
                        "\",\"filename\":\"76561198000000003.maFile\",\"steamid\":76561198000000003}]}");
    auto resolve = [](const std::string& login) -> uint64_t { return iequals(login, "smurf") ? 76561198000000002ULL : 0; };
    {
        Guard g(st);
        g.load(false);
        CHECK(g.empty());
        auto r = g.import_paths({f1, f2}, "", resolve);
        CHECK(r.added == 2 && r.errors.empty());
        CHECK(g.key_for_steamid(76561198000000001ULL) == "76561198000000001");
        CHECK(g.key_for_steamid(76561198000000002ULL) == "76561198000000002");
        CHECK(g.key_for_login("MAIN_ACC") == "76561198000000001");
        CHECK(g.code("76561198000000001") == guard::code_for_time(sec1, g.now()));
        GuardView v;
        CHECK(g.view("76561198000000001", v) && v.has_session && v.has_revocation);
        CHECK(g.view("76561198000000002", v) && !v.has_session);
        // the SDA folder asks for its passkey, rejects a wrong one, then imports
        r = g.import_paths({fsx::join(base, "SDA")}, "", resolve);
        CHECK(r.need_passkey && r.retry.size() == 1 && r.added == 0);
        auto retry = r.retry;
        r = g.import_paths(retry, "wrong", resolve);
        CHECK(r.need_passkey && r.wrong_passkey && r.added == 0);
        r = g.import_paths(retry, "ключ-pass", resolve);
        CHECK(r.added == 1 && !r.need_passkey);
        CHECK(g.revocation_code("76561198000000003") == "R33333");
        // picking the encrypted file alone still finds its manifest
        r = g.import_paths({fsx::join(sda, "76561198000000003.maFile")}, "", resolve);
        CHECK(r.need_passkey);
        // re-import updates instead of duplicating
        r = g.import_paths({f1}, "", resolve);
        CHECK(r.added == 0 && r.updated == 1 && g.list().size() == 3);
        // nothing stored in plain text
        std::string raw;
        CHECK(fsx::read_file(fsx::join(g.dir(), "76561198000000001.vsguard"), raw) && raw.find(sec1) == std::string::npos && raw.find("main_acc") == std::string::npos);

        // export: plain and encrypted SDA layouts that SDA (and we) can read back
        std::string out, err;
        CHECK(g.export_sda({"76561198000000001", "76561198000000003"}, fsx::join(base, "exp_plain"), "", out, err));
        guard::Manifest m;
        std::string text;
        CHECK(fsx::read_file(fsx::join(out, "manifest.json"), text) && guard::parse_manifest(text, m) && !m.encrypted && m.entries.size() == 2);
        guard::Account a;
        CHECK(fsx::read_file(fsx::join(out, "76561198000000001.maFile"), text) && guard::parse_mafile(text, a) && a.shared_secret == sec1 &&
              a.session.steamid == 76561198000000001ULL);
        std::string out2;
        CHECK(g.export_sda({"76561198000000002"}, fsx::join(base, "exp_enc"), "pk2", out2, err));
        CHECK(fsx::read_file(fsx::join(out2, "manifest.json"), text) && guard::parse_manifest(text, m) && m.encrypted && m.entries.size() == 1);
        std::string enc, plain;
        CHECK(fsx::read_file(fsx::join(out2, m.entries[0].filename), enc) && guard::sda_decrypt("pk2", m.entries[0].salt, m.entries[0].iv, enc, plain));
        CHECK(guard::parse_mafile(plain, a) && a.shared_secret == sec2 && a.account_name == "Smurf");
        // exporting into the same folder again never overwrites an existing manifest
        std::string out3;
        CHECK(g.export_sda({"76561198000000002"}, fsx::join(base, "exp_enc"), "", out3, err) && !iequals(out3, out2));

        // removing keeps a safety copy
        CHECK(g.remove_local("76561198000000002"));
        CHECK(!fsx::exists(fsx::join(g.dir(), "76561198000000002.vsguard")));
        CHECK(!fsx::list_dir(fsx::join(g.dir(), "backup")).empty());
    }
    // everything survives a restart (DPAPI round trip)
    {
        Guard g(st);
        g.load(false);
        auto l = g.list();
        CHECK(l.size() == 2);
        CHECK(g.code("76561198000000003") == guard::code_for_time(sec3, g.now()));
        GuardView v;
        CHECK(g.view("76561198000000001", v) && v.account_name == "main_acc" && v.has_session);
    }
}
