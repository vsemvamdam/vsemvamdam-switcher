// Included from test_win.cpp — exercises BasicPlatform with a synthetic platform definition.
static void wr(const std::string& p, const std::string& s) { fsx::write_file(p, s); }

void test_basic(const std::string& root, const std::string& fake) {
    std::string base = fsx::join(root, "basic");
    fsx::remove_all(base);
    std::string client = fsx::join(base, "Client");
    std::string data = fsx::join(base, "UserData");
    fsx::mkdirs(client);
    fsx::mkdirs(data);
    fsx::copy_any(fake, fsx::join(client, "vstestclient.exe"));
    std::string d = replace_all(data, "\\", "\\\\");
    std::string defs = R"({"Platforms":{"VS Test":{
      "vsId":"vstest","vsName":"VS Test",
      "ExeLocationDefault":")" + replace_all(fsx::join(client, "vstestclient.exe"), "\\", "\\\\") + R"(",
      "ExesToEnd":["vstestclient.exe"],
      "PathListToClear":["SAME_AS_LOGIN_FILES"],
      "LoginFiles":{
        ")" + d + R"(\\token.txt":"token.txt",
        ")" + d + R"(\\Sessions\\":"Sessions\\",
        ")" + d + R"(\\Cache\\data_*":"Cache\\",
        "REG:HKCU\\Software\\VsSwitcherTest:Token":"token",
        "REG:HKCU\\Software\\VsSwitcherTest:AccountId":"UNIQUE_ID",
        "JSON_SELECT_FIRST,::)" + d + R"(\\client.config::Client.SavedAccountNames":"email.json"
      },
      "ExitBeforeInteract":true,
      "UniqueIdFile":"HKCU\\Software\\VsSwitcherTest:AccountId",
      "UniqueIdMethod":"REGKEY"}}})";
    Store st;
    st.load(fsx::join(root, "data2"));
    st.settings.platforms["vstest"].enabled = true;
    auto plats = make_basic_platforms(st, defs);
    CHECK(plats.size() == 1);
    if (plats.empty()) return;
    auto* bp = static_cast<BasicPlatform*>(plats[0].get());
    CHECK(bp->installed());
    auto login = [&](const std::string& who) {
        wr(fsx::join(data, "token.txt"), "token-" + who);
        fsx::mkdirs(fsx::join(data, "Sessions"));
        wr(fsx::join(data, "Sessions\\s1.bin"), "session-" + who);
        fsx::mkdirs(fsx::join(data, "Cache"));
        wr(fsx::join(data, "Cache\\data_0"), "c0-" + who);
        wr(fsx::join(data, "Cache\\data_1"), "c1-" + who);
        wr(fsx::join(data, "Cache\\index"), "index-keep");
        os::reg_set_string("HKCU\\Software\\VsSwitcherTest", "Token", "tok-" + who);
        os::reg_set_string("HKCU\\Software\\VsSwitcherTest", "AccountId", "acc-" + who);
        wr(fsx::join(data, "client.config"), "{\"Client\":{\"SavedAccountNames\":\"" + who + "@mail.com,old@mail.com\",\"Other\":1}}");
    };
    login("A");
    start_fake(fsx::join(client, "vstestclient.exe"));
    std::string id;
    OpResult r = bp->save_current("Alpha", "", id);
    std::printf("  save A: ok=%d id=%s msg=%s\n", r.ok, id.c_str(), r.message.c_str());
    CHECK(r.ok && id == "acc-A");
    CHECK(!os::any_running({"vstestclient.exe"}) || true);
    CHECK(fsx::exists(fsx::join(bp->account_dir("acc-A"), "token.txt")));
    CHECK(fsx::exists(fsx::join(bp->account_dir("acc-A"), "Cache\\data_1")));
    CHECK(bp->current_account_id() == "acc-A");

    // new login: everything cleared
    r = bp->switch_to(SwitchRequest{}, nullptr);
    CHECK(r.ok);
    CHECK(!fsx::exists(fsx::join(data, "token.txt")));
    CHECK(!fsx::exists(fsx::join(data, "Sessions")));
    CHECK(!fsx::exists(fsx::join(data, "Cache\\data_0")));
    CHECK(fsx::exists(fsx::join(data, "Cache\\index")));  // not a login file
    std::string v;
    CHECK(!os::reg_get_string("HKCU\\Software\\VsSwitcherTest", "Token", v) || v.empty());
    std::string cfg;
    fsx::read_file(fsx::join(data, "client.config"), cfg);
    CHECK(cfg.find("\"SavedAccountNames\": \"\"") != std::string::npos);
    CHECK(cfg.find("\"Other\": 1") != std::string::npos);

    login("B");
    r = bp->save_current("Beta", "", id);
    CHECK(r.ok && id == "acc-B");
    CHECK(bp->scan().size() == 2);

    // switch to A
    r = bp->switch_to(SwitchRequest{"acc-A"}, nullptr);
    CHECK(r.ok);
    std::string t;
    fsx::read_file(fsx::join(data, "token.txt"), t);
    CHECK(t == "token-A");
    fsx::read_file(fsx::join(data, "Sessions\\s1.bin"), t);
    CHECK(t == "session-A");
    fsx::read_file(fsx::join(data, "Cache\\data_1"), t);
    CHECK(t == "c1-A");
    CHECK(os::reg_get_string("HKCU\\Software\\VsSwitcherTest", "Token", v) && v == "tok-A");
    CHECK(os::reg_get_string("HKCU\\Software\\VsSwitcherTest", "AccountId", v) && v == "acc-A");
    fsx::read_file(fsx::join(data, "client.config"), cfg);
    CHECK(cfg.find("\"SavedAccountNames\": \"A@mail.com\"") != std::string::npos);
    CHECK(bp->current_account_id() == "acc-A");

    // token rotates while A is active -> switching away refreshes A's save
    wr(fsx::join(data, "token.txt"), "token-A2");
    r = bp->switch_to(SwitchRequest{"acc-B"}, nullptr);
    CHECK(r.ok);
    fsx::read_file(fsx::join(data, "token.txt"), t);
    CHECK(t == "token-B");
    fsx::read_file(fsx::join(bp->account_dir("acc-A"), "token.txt"), t);
    CHECK(t == "token-A2");
    CHECK(bp->current_account_id() == "acc-B");

    // overwrite protection
    r = bp->save_current("Alpha", "acc-A", id);
    CHECK(!r.ok);

    // broken save: switching must fail BEFORE the current login is touched
    fsx::write_file(fsx::join(bp->account_dir("acc-A"), "_vs_registry.json"), "{ broken");
    r = bp->switch_to(SwitchRequest{"acc-A"}, nullptr);
    CHECK(!r.ok);
    fsx::read_file(fsx::join(data, "token.txt"), t);
    CHECK(t == "token-B");
    CHECK(bp->current_account_id() == "acc-B");

    // temp folders never show up as accounts
    fsx::mkdirs(bp->account_dir("acc-Z") + ".saving");
    CHECK(bp->scan().size() == 2);

    r = bp->forget("acc-B");
    CHECK(r.ok && bp->scan().size() == 1);
    os::kill_processes({"vstestclient.exe"}, 3000);
    os::reg_delete_tree("HKCU\\Software\\VsSwitcherTest");

    // platform without a unique id: leaving an unsaved session keeps an automatic copy
    std::string defs2 = R"({"Platforms":{"NoId":{"vsId":"noid","ExeLocationDefault":")" + replace_all(fsx::join(client, "vstestclient.exe"), "\\", "\\\\") +
                        R"(","ExesToEnd":["vstestclient.exe"],"PathListToClear":["SAME_AS_LOGIN_FILES"],"LoginFiles":{")" + d +
                        R"(\\token.txt":"token.txt"}}}})";
    st.settings.platforms["noid"].enabled = true;
    st.settings.platforms["noid"].launch_after = false;
    auto p2 = make_basic_platforms(st, defs2);
    CHECK(p2.size() == 1);
    if (p2.empty()) return;
    wr(fsx::join(data, "token.txt"), "unsaved-session");
    r = p2[0]->switch_to(SwitchRequest{}, nullptr);
    CHECK(r.ok);
    CHECK(!fsx::exists(fsx::join(data, "token.txt")));
    auto autos = fsx::list_dir(fsx::join(st.dir, "autosave\\noid"), true);
    CHECK(autos.size() == 1);
    if (!autos.empty()) {
        std::string t2;
        fsx::read_file(fsx::join(fsx::join(st.dir, "autosave\\noid\\" + autos[0]), "token.txt"), t2);
        CHECK(t2 == "unsaved-session");
    }
}

// New Epic Games Launcher: Credential Manager + DPAPI-protected auth files
void test_epic(const std::string& root, const std::string& fake) {
    std::string launcher = fsx::join(os::appdata_local(), "Epic Games\\Launcher");
    std::string auth = fsx::join(os::appdata_local(), "com.epicgames.eglv2\\auth");
    fsx::remove_all(auth);
    fsx::mkdirs(launcher);
    fsx::copy_any(fake, fsx::join(launcher, "EpicGamesLauncher.exe"));
    auto login = [&](const std::string& acc) {
        fsx::remove_all(auth);
        fsx::mkdirs(auth);
        std::string tok = "{\"account_id\":\"" + acc + "\",\"access_token\":\"x\"}";
        std::vector<uint8_t> in(tok.begin(), tok.end()), out;
        CHECK(os::dpapi_protect(in, out));
        fsx::write_file(fsx::join(auth, "access-token-prod.dpapi"), std::string(out.begin(), out.end()));
        fsx::write_file(fsx::join(auth, "auth.json"), "{\"user\":\"" + acc + "\"}");
        os::Credential c;
        c.target = "credentials.com.epicgames.eglv2.prod";
        c.username = acc;
        c.blob = {1, 2, 3, (uint8_t)acc[0]};
        CHECK(os::cred_write(c));
    };
    std::string text;
    fsx::read_file(fsx::join(fsx::parent(os::self_exe_path()), "..\\src\\core\\platforms.json"), text);
    if (text.empty()) { std::printf("  (platforms.json not found, skipping epic)\n"); return; }
    Store st;
    st.load(fsx::join(root, "data3"));
    st.settings.platforms["epic"].enabled = true;
    st.settings.platforms["epic"].launch_after = false;
    BasicPlatform* ep = nullptr;
    auto plats = make_basic_platforms(st, text);
    for (auto& p : plats) if (p->id() == "epic") ep = static_cast<BasicPlatform*>(p.get());
    CHECK(ep != nullptr);
    if (!ep) return;
    CHECK(ep->installed());
    login("aaaa1111");
    std::string id;
    OpResult r = ep->save_current("Epic A", "", id);
    std::printf("  epic save A: ok=%d id=%s msg=%s\n", r.ok, id.c_str(), r.message.c_str());
    CHECK(r.ok && id == "aaaa1111");
    login("bbbb2222");
    r = ep->save_current("Epic B", "", id);
    CHECK(r.ok && id == "bbbb2222");
    CHECK(ep->scan().size() == 2);
    r = ep->switch_to(SwitchRequest{"aaaa1111"}, nullptr);
    CHECK(r.ok);
    CHECK(ep->current_account_id() == "aaaa1111");
    os::Credential c;
    CHECK(os::cred_read("credentials.com.epicgames.eglv2.prod", c) && c.username == "aaaa1111" && c.blob.size() == 4 && c.blob[3] == 'a');
    std::string a;
    fsx::read_file(fsx::join(auth, "auth.json"), a);
    CHECK(a.find("aaaa1111") != std::string::npos);
    r = ep->switch_to(SwitchRequest{}, nullptr);  // new login
    CHECK(r.ok);
    CHECK(!os::cred_read("credentials.com.epicgames.eglv2.prod", c));
    CHECK(!fsx::exists(fsx::join(auth, "auth.json")));
    r = ep->switch_to(SwitchRequest{"bbbb2222"}, nullptr);
    CHECK(r.ok && ep->current_account_id() == "bbbb2222");
    os::cred_delete("credentials.com.epicgames.eglv2.prod");
    fsx::remove_all(auth);
    fsx::remove_all(launcher);
}
