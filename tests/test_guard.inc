// Steam Guard / crypto unit tests (included from test_core.cpp). Expected values come from
// published test vectors (RFC 2202 / 6070, FIPS-197, SP 800-38A) and an independent Python implementation.
#include "../src/core/crypto.h"
#include "../src/core/guard_core.h"
#include "../src/core/protobuf.h"

static std::string hexof(const crypto::Bytes& b) { return crypto::hex_encode(b); }
static crypto::Bytes unhex(const char* h) { crypto::Bytes b; crypto::hex_decode(h, b); return b; }

static void test_crypto() {
    using namespace crypto;
    CHECK_EQ(hexof(sha1(to_bytes("abc"))), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    CHECK_EQ(hexof(sha1(to_bytes(""))), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    CHECK_EQ(hexof(sha1(to_bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
             std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    CHECK_EQ(hexof(hmac_sha1(to_bytes("Jefe"), to_bytes("what do ya want for nothing?"))), std::string("effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"));
    CHECK_EQ(hexof(hmac_sha1(Bytes(80, 0xaa), to_bytes("Test Using Larger Than Block-Size Key - Hash Key First"))),
             std::string("aa4ae5e15272d00e95705637ce8a3b55ed402112"));
    CHECK_EQ(hexof(pbkdf2_sha1("password", to_bytes("salt"), 1, 20)), std::string("0c60c80f961f0e71f3a9b524af6012062fe037a6"));
    CHECK_EQ(hexof(pbkdf2_sha1("password", to_bytes("salt"), 4096, 20)), std::string("4b007901b765489abead49d926f721d065a429c1"));
    CHECK_EQ(hexof(pbkdf2_sha1("passwordPASSWORDpassword", to_bytes("saltSALTsaltSALTsaltSALTsaltSALTsalt"), 4096, 25)),
             std::string("3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038"));

    // FIPS-197 C.3
    Bytes key = unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    Bytes pt = unhex("00112233445566778899aabbccddeeff");
    uint8_t ct[16], back[16];
    aes256_encrypt_block(key.data(), pt.data(), ct);
    CHECK_EQ(hexof(Bytes(ct, ct + 16)), std::string("8ea2b7ca516745bfeafc49904b496089"));
    aes256_decrypt_block(key.data(), ct, back);
    CHECK_EQ(hexof(Bytes(back, back + 16)), hexof(pt));
    // SP 800-38A F.2.5 (first two blocks) + PKCS#7 round trip
    Bytes k2 = unhex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4");
    Bytes iv = unhex("000102030405060708090a0b0c0d0e0f");
    Bytes p2 = unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
    Bytes c2 = aes256_cbc_encrypt(k2, iv, p2);
    CHECK_EQ(c2.size(), (size_t)48);
    CHECK_EQ(hexof(Bytes(c2.begin(), c2.begin() + 32)), std::string("f58c4c04d6e5f1ba779eabfb5f7bfbd69cfc4e967edb808d679f777bc6702c7d"));
    Bytes d2;
    CHECK(aes256_cbc_decrypt(k2, iv, c2, d2) && d2 == p2);
    c2.back() ^= 1;
    CHECK(!aes256_cbc_decrypt(k2, iv, c2, d2));

    Bytes b;
    CHECK(base64_decode("aGVsbG8gd29ybGQ=", b) && to_string(b) == "hello world");
    CHECK(base64_decode("aGVsbG8gd29ybGQ", b) && to_string(b) == "hello world");  // no padding
    CHECK(base64_decode("_-8", b) && b.size() == 2 && b[0] == 0xff && b[1] == 0xef);  // url-safe alphabet
    CHECK(!base64_decode("a$b", b));
    CHECK_EQ(base64_encode(to_bytes("hello world")), std::string("aGVsbG8gd29ybGQ="));
    CHECK_EQ(base64_encode(to_bytes("ab")), std::string("YWI="));
    CHECK_EQ(random_bytes(24).size(), (size_t)24);

    // RSA: c = m^e mod n against Python's pow(), then decrypt with d
    const char* n = "bd579bc8c4976dbae709242f342776ae1332dc59952138263845914d19ab6ce52bb6a7a011fafc5e9e95a0cbb99d25f17d31247362870900ceb5ff7adcc553365274cd3a49c244b14e82033949bfe95875f5a28107923355bd6de575308f8518fb4dff8371c236c3c6dbd25e491443ea803dfbc5f7f1ab9afa8e02628ba494b1";
    const char* d = "6d25135f15f43495f6984e680c162153a4e21a6c64d40a89fd9cf800c2efe9fa7a3862b17562cb3c522e37cbca7ca7874435c6a8bcd9f5e8eef68d05d83d9e26d8fb7daa9f8b98c7f56c56511d12eb75e22c9ffe3ec516d6ab71f53d92d095fb311610176a2bfe6cabab8149aacfd99debf09bdda46d21e93315229d6d754d";
    Bytes m = unhex("201010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010068656c6c6f");
    Bytes c = modexp(m, unhex("010001"), unhex(n));
    CHECK_EQ(hexof(c), std::string("56ae707737a883333e29381c34e6fe1af3d3b2ecf4c8554c70efcbf4572cc998ef3fb18835802ce6e81be9fa098c2d880446c6c73ae8b3a2cc18ca20c3a414f0a9ca9e2f990833d66bb0ed639405c10e1124a5e773ab31b0eefa8495fb4692cd53815f0a38aae79295d49db1d93a75eba718c2d4fa46e044b5a1bd329e8d6f56"));
    Bytes mm = modexp(c, unhex(d), unhex(n));
    CHECK(mm.size() == 128 && Bytes(mm.end() - m.size(), mm.end()) == m && std::all_of(mm.begin(), mm.end() - m.size(), [](uint8_t v) { return v == 0; }));
    Bytes enc = rsa_pkcs1_encrypt(n, "010001", to_bytes("hunter2"));
    CHECK_EQ(enc.size(), (size_t)128);
    Bytes em = modexp(enc, unhex(d), unhex(n));
    CHECK(em.size() == 128 && em[0] == 0 && em[1] == 2);
    size_t z = 2;
    while (z < em.size() && em[z] != 0) ++z;
    CHECK(z >= 10 && z < em.size() && to_string(Bytes(em.begin() + z + 1, em.end())) == "hunter2");
    CHECK(rsa_pkcs1_encrypt(n, "010001", Bytes(200, 1)).empty());  // too long for the key
}

static void test_guard() {
    using namespace guard;
    const std::string sec = "AQIDBAUGBwgJCgsMDQ4PEBESExQ=";
    CHECK_EQ(code_for_time(sec, 0), std::string("6KR59"));
    CHECK_EQ(code_for_time(sec, 29), std::string("6KR59"));
    CHECK_EQ(code_for_time(sec, 30), std::string("DW55B"));
    CHECK_EQ(code_for_time(sec, 1700000000), std::string("3M9KK"));
    CHECK_EQ(code_for_time(sec, 1730000015), std::string("TK35F"));
    CHECK_EQ(code_for_time(sec, 2000000000), std::string("PMG8G"));
    CHECK_EQ(code_for_time("not base64!", 30), std::string(""));
    const std::string ids = "aWRlbnRpdHlfc2VjcmV0X2Zvcl90ZXN0cw==";
    CHECK_EQ(confirmation_key(ids, 1700000000, "conf"), std::string("nSq9NJXMaTusnfh6I9mkJAzTJ3c="));
    CHECK_EQ(confirmation_key(ids, 1700000000, "accept"), std::string("HGEfGICpcwgtyM/uwnj7jznRFqo="));
    CHECK_EQ(confirmation_key(ids, 1700000000, std::string(40, 'x')), std::string("f9Qu7r5SaiTWE81yamUOPv/lhGE="));  // tag cut at 32
    CHECK_EQ(device_id_for(76561198000000001ULL), std::string("android:ca748b58-133d-73d0-adcd-109baa02c0fa"));
    std::string rid = random_device_id();
    CHECK(rid.size() == 44 && starts_with(rid, "android:") && rid[22] == '4');

    // maFile: new SDA layout
    const char* mafile = R"({"shared_secret":"AQIDBAUGBwgJCgsMDQ4PEBESExQ=","serial_number":"1234567890","revocation_code":"R12345",
      "uri":"otpauth://totp/Steam:main_acc?secret=AAAA&issuer=Steam","server_time":1700000000,"account_name":"main_acc",
      "token_gid":"abc","identity_secret":"aWRlbnRpdHlfc2VjcmV0X2Zvcl90ZXN0cw==","secret_1":"c2Vj","status":1,
      "device_id":"android:11111111-2222-3333-4444-555555555555","fully_enrolled":true,"custom_field":42,
      "Session":{"SteamID":76561198000000001,"AccessToken":"at","RefreshToken":"rt","SessionID":null}})";
    Account a;
    std::string err;
    CHECK(parse_mafile(mafile, a, &err));
    CHECK_EQ(a.account_name, std::string("main_acc"));
    CHECK_EQ(a.steamid, 76561198000000001ULL);
    CHECK_EQ(a.session.access_token, std::string("at"));
    CHECK_EQ(a.session.refresh_token, std::string("rt"));
    CHECK(a.session.session_id.empty());
    CHECK_EQ(a.revocation_code, std::string("R12345"));
    CHECK_EQ(a.server_time, (int64_t)1700000000);
    CHECK_EQ(confirmation_params(a, 1700000000, "conf"),
             std::string("p=android%3A11111111-2222-3333-4444-555555555555&a=76561198000000001&k=nSq9NJXMaTusnfh6I9mkJAzTJ3c%3D&t=1700000000&m=react&tag=conf"));
    // round trip keeps unknown fields and the session
    Account b;
    CHECK(parse_mafile(to_mafile(a), b));
    CHECK_EQ(b.shared_secret, a.shared_secret);
    CHECK_EQ(b.steamid, a.steamid);
    CHECK_EQ(b.session.refresh_token, std::string("rt"));
    CHECK(b.extra.contains("custom_field") && b.extra["custom_field"] == 42);
    // legacy layout (OAuth era), string numbers, BOM
    const char* legacy = "\xEF\xBB\xBF{\"shared_secret\":\"AQIDBAUGBwgJCgsMDQ4PEBESExQ=\",\"account_name\":\"old\",\"server_time\":\"1500000000\","
                         "\"status\":\"1\",\"Session\":{\"SessionID\":\"x\",\"SteamLoginSecure\":\"76561198000000002%7C%7CABCDEF\",\"OAuthToken\":\"o\"}}";
    CHECK(parse_mafile(legacy, a));
    CHECK_EQ(a.steamid, 76561198000000002ULL);
    CHECK_EQ(a.server_time, (int64_t)1500000000);
    CHECK_EQ(a.status, 1);
    CHECK(a.session.access_token.empty());
    std::string re = to_mafile(a);
    CHECK(re.find("OAuthToken") == std::string::npos && re.find("\"SteamID\":76561198000000002") != std::string::npos);
    CHECK(!parse_mafile("{\"account_name\":\"x\"}", a, &err));
    CHECK(!parse_mafile("not json", a));
    CHECK_EQ(steamid_from_filename("76561198000000003.maFile"), 76561198000000003ULL);
    CHECK_EQ(steamid_from_filename("main.maFile"), 0ULL);

    // SDA manifest + encryption (ciphertext made with pycryptodome)
    Manifest m;
    CHECK(parse_manifest(R"({"encrypted":true,"first_run":false,"entries":[{"encryption_iv":"AAECAwQFBgcICQoLDA0ODw==","encryption_salt":"c2FsdHNhbHQ=",
        "filename":"76561198000000001.maFile","steamid":76561198000000001}],"periodic_checking":true,"periodic_checking_interval":10,
        "periodic_checking_checkall":true,"auto_confirm_market_transactions":true,"auto_confirm_trades":false})", m));
    CHECK(m.encrypted && m.entries.size() == 1 && m.interval == 10 && m.auto_market && !m.auto_trades && m.check_all);
    CHECK_EQ(m.entries[0].steamid, 76561198000000001ULL);
    const char* ct = "pf4LvgrqlKbYWygMCNBk4m8wRVHAsp8DHQUqjTarb1gxrsgmmOsdAzO/o2foDoTDZDjSOzMohHWDSHujKdy/lLB1YCbbiRyqsRTVv16b9ZrNtymCuYdO7BVq+Bdkr9vgsl3xopacDD/mur/MJzb0dH2eYaY2EDeiBy0/7qxB8KZ5cjHDSANt4Kf9EpzBv8Bj4uwuRFdhWyHkGqYyz5UepTp2hj/H/pRZdTWb9+ajGCJXbKdMvo14yqjI70ufM4sr";
    std::string plain;
    CHECK(sda_decrypt("пароль-key", m.entries[0].salt, m.entries[0].iv, ct, plain));
    CHECK(parse_mafile(plain, a) && a.account_name == "тест_acc" && a.steamid == 76561198000000001ULL);
    CHECK(!sda_decrypt("wrong", m.entries[0].salt, m.entries[0].iv, ct, plain));
    std::string again = sda_encrypt("pk", "c2FsdHNhbHQ=", "AAECAwQFBgcICQoLDA0ODw==", "{\"a\":1}");
    CHECK(sda_decrypt("pk", "c2FsdHNhbHQ=", "AAECAwQFBgcICQoLDA0ODw==", again, plain) && plain == "{\"a\":1}");

    // JWT
    std::string payload = crypto::base64_encode(crypto::to_bytes("{\"iss\":\"steam\",\"sub\":\"76561198000000001\",\"exp\":1800000000}"));
    while (!payload.empty() && payload.back() == '=') payload.pop_back();
    std::string tok = "eyJhbGciOiJFZERTQSJ9." + replace_all(replace_all(payload, "+", "-"), "/", "_") + ".sig";
    CHECK_EQ(jwt_exp(tok), (int64_t)1800000000);
    CHECK_EQ(jwt_steamid(tok), 76561198000000001ULL);
    CHECK_EQ(jwt_exp("garbage"), (int64_t)0);

    // confirmations
    ConfList cl;
    CHECK(parse_confirmations(R"({"success":true,"conf":[{"type":2,"type_name":"Trade Offer","id":"13785263547","creator_id":"6104012345",
        "nonce":"9837452321234","creation_time":1730000000,"cancel":"Cancel","accept":"Send Offer","icon":"https://a/b.jpg","multi":false,
        "headline":"Friend","summary":["You will give up your 1 item","You will receive nothing"],"warn":null},
        {"type":3,"id":11,"nonce":22,"headline":"AK-47","summary":["Sell for 10,00 руб."]},{"type":2,"id":"x"}]})", cl));
    CHECK_EQ(cl.items.size(), (size_t)2);
    CHECK_EQ(cl.items[0].type, 2);
    CHECK_EQ(cl.items[0].summary.size(), (size_t)2);
    CHECK_EQ(cl.items[0].time, (int64_t)1730000000);
    CHECK_EQ(cl.items[1].id, std::string("11"));
    CHECK_EQ(cl.items[1].nonce, std::string("22"));
    CHECK(!parse_confirmations(R"({"success":false,"needauth":true})", cl) && cl.needauth);
    CHECK(!parse_confirmations("<html><body>Sign In</body></html>", cl) && cl.needauth);
    CHECK(parse_success(R"({"success":true})"));
    CHECK(!parse_success(R"({"success":false})"));

    // web helpers
    CHECK_EQ(url_encode("a b+c/=ё"), std::string("a%20b%2Bc%2F%3D%D1%91"));
    CHECK_EQ(form_encode({{"op", "allow"}, {"cid[]", "1"}}), std::string("op=allow&cid%5B%5D=1"));
    CHECK_EQ(multipart_body({{"k", "v"}}, "BND"), std::string("--BND\r\nContent-Disposition: form-data; name=\"k\"\r\n\r\nv\r\n--BND--\r\n"));

    // protobuf: begin auth request encodes the fields the mobile app sends
    std::string req = pb_begin_auth_request("acc", "ENC", 123456789, "vsemvamdam switcher");
    std::vector<pb::Field> f;
    CHECK(pb::parse(req, f));
    CHECK_EQ(pb::get_str(f, 2), std::string("acc"));
    CHECK_EQ(pb::get_str(f, 3), std::string("ENC"));
    CHECK_EQ(pb::get_num(f, 4), 123456789ULL);
    CHECK_EQ(pb::get_num(f, 5), 1ULL);
    CHECK_EQ(pb::get_num(f, 7), 1ULL);
    CHECK_EQ(pb::get_str(f, 8), std::string("Mobile"));
    std::vector<pb::Field> dd;
    CHECK(pb::parse(pb::get_str(f, 9), dd));
    CHECK_EQ(pb::get_num(dd, 2), 3ULL);
    CHECK_EQ((int32_t)pb::get_num(dd, 3), -500);
    CHECK_EQ(pb::get_num(dd, 4), 528ULL);
    // begin auth response
    pb::Writer conf1, conf2, resp;
    conf1.u64(1, GuardEmailCode);
    conf1.str(2, "mail.ru");
    conf2.u64(1, GuardDeviceCode);
    resp.u64(1, 9876543210ULL);
    resp.bytes(2, std::string("\x01\x02\x00\x03", 4));
    resp.tag(3, 5);
    float iv5 = 5.0f; uint32_t fu; std::memcpy(&fu, &iv5, 4);
    for (int i = 0; i < 4; ++i) resp.out += (char)(fu >> (8 * i));
    resp.message(4, conf1);
    resp.message(4, conf2);
    resp.u64(5, 76561198000000001ULL);
    BeginAuth ba;
    CHECK(pb_begin_auth_response(resp.out, ba));
    CHECK_EQ(ba.client_id, 9876543210ULL);
    CHECK_EQ(ba.request_id.size(), (size_t)4);
    CHECK(ba.interval > 4.9f && ba.interval < 5.1f);
    CHECK(ba.allowed.size() == 2 && ba.allowed[0] == GuardEmailCode && ba.allowed[1] == GuardDeviceCode && ba.messages[0] == "mail.ru");
    CHECK_EQ(ba.steamid, 76561198000000001ULL);
    // update code: steamid is fixed64
    CHECK(pb::parse(pb_update_code_request(1, 76561198000000001ULL, "ABCDE", GuardDeviceCode), f));
    CHECK(pb::find(f, 2) && pb::find(f, 2)->wire == 1 && pb::get_num(f, 2) == 76561198000000001ULL && pb::get_str(f, 3) == "ABCDE");
    pb::Writer poll;
    poll.str(3, "refresh.jwt");
    poll.str(4, "access.jwt");
    poll.str(6, "main_acc");
    PollResult pr;
    CHECK(pb_poll_response(poll.out, pr) && pr.refresh_token == "refresh.jwt" && pr.access_token == "access.jwt" && pr.account_name == "main_acc");
    CHECK(!pb::parse(std::string("\x0a\x05" "ab", 4), f));  // truncated length
    pb::Writer rsa;
    rsa.str(1, "abcdef");
    rsa.str(2, "010001");
    rsa.u64(3, 424242);
    std::string mh, eh;
    uint64_t ts = 0;
    CHECK(pb_rsa_key_response(rsa.out, mh, eh, ts) && mh == "abcdef" && eh == "010001" && ts == 424242);

    // JSON responses
    CHECK_EQ(parse_query_time(R"({"response":{"server_time":"1730000123","skew_tolerance_seconds":"60"}})"), (int64_t)1730000123);
    std::string at, rt;
    CHECK(parse_access_token(R"({"response":{"access_token":"A"}})", at, rt) && at == "A" && rt.empty());
    AddAuthResult ar;
    CHECK(parse_add_authenticator(R"({"response":{"shared_secret":"AQIDBAUGBwgJCgsMDQ4PEBESExQ=","serial_number":"77","revocation_code":"R99999",
        "uri":"otpauth://x","server_time":"1730000000","account_name":"newacc","token_gid":"g","identity_secret":"aWQ=","secret_1":"czE=",
        "status":1,"phone_number_hint":"23","confirm_type":1}})", ar));
    CHECK(ar.status == 1 && ar.account.revocation_code == "R99999" && !ar.account.fully_enrolled && ar.account.phone_number_hint == "23");
    CHECK(parse_add_authenticator(R"({"response":{"status":29}})", ar) && ar.status == 29);
    FinalizeResult fr;
    CHECK(parse_finalize(R"({"response":{"status":89,"server_time":"1","want_more":false,"success":false}})", fr) && fr.status == 89 && !fr.success);
    RemoveResult rr;
    CHECK(parse_remove(R"({"response":{"success":true,"revocation_attempts_remaining":4}})", rr) && rr.success && rr.attempts_left == 4);
    bool flag = false;
    CHECK(parse_waiting_email(R"({"response":{"awaiting_email_confirmation":true,"seconds_to_wait":10}})", flag) && flag);
    std::string email;
    CHECK(parse_set_phone(R"({"response":{"confirmation_email_address":"g***@mail.ru","phone_number_formatted":"+7 999"}})", email) && email == "g***@mail.ru");
    CHECK_EQ(parse_user_country(R"({"response":{"country":"RU"}})"), std::string("RU"));
}
