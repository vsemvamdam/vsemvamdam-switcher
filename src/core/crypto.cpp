#include "crypto.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
extern "C" BOOLEAN NTAPI SystemFunction036(PVOID buffer, ULONG length);  // RtlGenRandom (advapi32)
#endif

namespace vs::crypto {

Bytes to_bytes(std::string_view s) { return Bytes(s.begin(), s.end()); }
std::string to_string(const Bytes& b) { return std::string(b.begin(), b.end()); }

// ============================================================ SHA-1
static inline uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

namespace {
struct Sha1 {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint8_t buf[64];
    size_t used = 0;
    uint64_t total = 0;
    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    void update(const uint8_t* p, size_t n) {
        total += n;
        while (n) {
            if (used == 0 && n >= 64) { block(p); p += 64; n -= 64; continue; }
            size_t take = std::min(n, 64 - used);
            std::memcpy(buf + used, p, take);
            used += take; p += take; n -= take;
            if (used == 64) { block(buf); used = 0; }
        }
    }
    void final(uint8_t out[20]) {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t z = 0;
        while (used != 56) update(&z, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(len, 8);
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 4; ++j) out[i * 4 + j] = (uint8_t)(h[i] >> (24 - 8 * j));
    }
};
// HMAC-SHA1 with the padded key absorbed once
struct HmacSha1 {
    Sha1 inner, outer;
    explicit HmacSha1(const Bytes& key_in) {
        Bytes key = key_in.size() > 64 ? sha1(key_in) : key_in;
        key.resize(64, 0);
        uint8_t ip[64], op[64];
        for (int i = 0; i < 64; ++i) { ip[i] = key[i] ^ 0x36; op[i] = key[i] ^ 0x5c; }
        inner.update(ip, 64);
        outer.update(op, 64);
        wipe(key);
    }
    void mac(const uint8_t* m, size_t n, uint8_t out[20]) const {
        Sha1 i = inner, o = outer;
        uint8_t ih[20];
        i.update(m, n);
        i.final(ih);
        o.update(ih, 20);
        o.final(out);
    }
};
}  // namespace

Bytes sha1(const uint8_t* data, size_t n) {
    Sha1 c;
    c.update(data, n);
    Bytes out(20);
    c.final(out.data());
    return out;
}

Bytes hmac_sha1(const Bytes& key, const uint8_t* data, size_t n) {
    HmacSha1 h(key);
    Bytes out(20);
    h.mac(data, n, out.data());
    return out;
}

Bytes pbkdf2_sha1(std::string_view password, const Bytes& salt, uint32_t iterations, size_t dk_len) {
    Bytes pw = to_bytes(password);
    HmacSha1 h(pw);
    wipe(pw);
    Bytes out;
    for (uint32_t block = 1; out.size() < dk_len; ++block) {
        Bytes s = salt;
        s.push_back((uint8_t)(block >> 24)); s.push_back((uint8_t)(block >> 16));
        s.push_back((uint8_t)(block >> 8)); s.push_back((uint8_t)block);
        uint8_t u[20], t[20];
        h.mac(s.data(), s.size(), u);
        std::memcpy(t, u, 20);
        for (uint32_t i = 1; i < iterations; ++i) {
            h.mac(u, 20, u);
            for (int j = 0; j < 20; ++j) t[j] ^= u[j];
        }
        out.insert(out.end(), t, t + 20);
    }
    out.resize(dk_len);
    return out;
}

// ============================================================ AES-256
namespace {
struct AesTables {
    uint8_t sbox[256], inv[256];
    AesTables() {
        auto rotl8 = [](uint8_t x, int s) { return (uint8_t)((x << s) | (x >> (8 - s))); };
        uint8_t p = 1, q = 1;
        do {
            p = (uint8_t)(p ^ (uint8_t)(p << 1) ^ ((p & 0x80) ? 0x1B : 0));  // p * 3
            q ^= (uint8_t)(q << 1);                                            // q / 3
            q ^= (uint8_t)(q << 2);
            q ^= (uint8_t)(q << 4);
            if (q & 0x80) q ^= 0x09;
            uint8_t x = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4));
            sbox[p] = (uint8_t)(x ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; ++i) inv[sbox[i]] = (uint8_t)i;
    }
};
const AesTables& T() {
    static const AesTables t;
    return t;
}
inline uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}
void expand_key(const uint8_t key[32], uint8_t rk[240]) {
    static const uint8_t rcon[7] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40};
    const auto& t = T();
    std::memcpy(rk, key, 32);
    for (int i = 8; i < 60; ++i) {
        uint8_t w[4];
        std::memcpy(w, rk + 4 * (i - 1), 4);
        if (i % 8 == 0) {
            uint8_t f = w[0];
            w[0] = (uint8_t)(t.sbox[w[1]] ^ rcon[i / 8 - 1]);
            w[1] = t.sbox[w[2]];
            w[2] = t.sbox[w[3]];
            w[3] = t.sbox[f];
        } else if (i % 8 == 4) {
            for (auto& b : w) b = t.sbox[b];
        }
        for (int j = 0; j < 4; ++j) rk[4 * i + j] = (uint8_t)(rk[4 * (i - 8) + j] ^ w[j]);
    }
}
void enc_block(const uint8_t rk[240], const uint8_t in[16], uint8_t out[16]) {
    const auto& t = T();
    uint8_t s[16], tmp[16];
    for (int i = 0; i < 16; ++i) s[i] = in[i] ^ rk[i];
    for (int round = 1; round <= 14; ++round) {
        for (auto& b : s) b = t.sbox[b];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) tmp[r + 4 * c] = s[r + 4 * ((c + r) % 4)];
        if (round != 14) {
            for (int c = 0; c < 4; ++c) {
                uint8_t a0 = tmp[4 * c], a1 = tmp[4 * c + 1], a2 = tmp[4 * c + 2], a3 = tmp[4 * c + 3];
                s[4 * c] = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
                s[4 * c + 1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
                s[4 * c + 2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
                s[4 * c + 3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
            }
        } else {
            std::memcpy(s, tmp, 16);
        }
        for (int i = 0; i < 16; ++i) s[i] ^= rk[16 * round + i];
    }
    std::memcpy(out, s, 16);
}
void dec_block(const uint8_t rk[240], const uint8_t in[16], uint8_t out[16]) {
    const auto& t = T();
    uint8_t s[16], tmp[16];
    for (int i = 0; i < 16; ++i) s[i] = in[i] ^ rk[16 * 14 + i];
    for (int round = 13; round >= 0; --round) {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) tmp[r + 4 * c] = s[r + 4 * ((c - r + 4) % 4)];
        for (int i = 0; i < 16; ++i) s[i] = (uint8_t)(t.inv[tmp[i]] ^ rk[16 * round + i]);
        if (round != 0) {
            for (int c = 0; c < 4; ++c) {
                uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
                s[4 * c] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
                s[4 * c + 1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
                s[4 * c + 2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
                s[4 * c + 3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
            }
        }
    }
    std::memcpy(out, s, 16);
}
}  // namespace

void aes256_encrypt_block(const uint8_t key[32], const uint8_t in[16], uint8_t out[16]) {
    uint8_t rk[240];
    expand_key(key, rk);
    enc_block(rk, in, out);
}
void aes256_decrypt_block(const uint8_t key[32], const uint8_t in[16], uint8_t out[16]) {
    uint8_t rk[240];
    expand_key(key, rk);
    dec_block(rk, in, out);
}

Bytes aes256_cbc_encrypt(const Bytes& key, const Bytes& iv, const Bytes& plain) {
    if (key.size() != 32 || iv.size() != 16) return {};
    uint8_t rk[240];
    expand_key(key.data(), rk);
    Bytes p = plain;
    size_t pad = 16 - p.size() % 16;
    p.insert(p.end(), pad, (uint8_t)pad);
    Bytes out(p.size());
    uint8_t prev[16];
    std::memcpy(prev, iv.data(), 16);
    for (size_t off = 0; off < p.size(); off += 16) {
        uint8_t blk[16];
        for (int i = 0; i < 16; ++i) blk[i] = p[off + i] ^ prev[i];
        enc_block(rk, blk, out.data() + off);
        std::memcpy(prev, out.data() + off, 16);
    }
    wipe(p);
    return out;
}

bool aes256_cbc_decrypt(const Bytes& key, const Bytes& iv, const Bytes& cipher, Bytes& plain) {
    plain.clear();
    if (key.size() != 32 || iv.size() != 16 || cipher.empty() || cipher.size() % 16) return false;
    uint8_t rk[240];
    expand_key(key.data(), rk);
    Bytes out(cipher.size());
    const uint8_t* prev = iv.data();
    for (size_t off = 0; off < cipher.size(); off += 16) {
        uint8_t blk[16];
        dec_block(rk, cipher.data() + off, blk);
        for (int i = 0; i < 16; ++i) out[off + i] = blk[i] ^ prev[i];
        prev = cipher.data() + off;
    }
    uint8_t pad = out.back();
    if (pad == 0 || pad > 16 || pad > out.size()) { wipe(out); return false; }
    for (size_t i = out.size() - pad; i < out.size(); ++i)
        if (out[i] != pad) { wipe(out); return false; }
    out.resize(out.size() - pad);
    plain = std::move(out);
    return true;
}

// ============================================================ encodings
std::string base64_encode(const uint8_t* d, size_t n) {
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    s.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        s += A[(v >> 18) & 63];
        s += A[(v >> 12) & 63];
        s += i + 1 < n ? A[(v >> 6) & 63] : '=';
        s += i + 2 < n ? A[v & 63] : '=';
    }
    return s;
}

bool base64_decode(std::string_view s, Bytes& out) {
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char ch : s) {
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+' || ch == '-') v = 62;
        else if (ch == '/' || ch == '_') v = 63;
        else if (ch == '=') break;
        else if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') continue;
        else return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)(acc >> bits));
        }
    }
    return true;
}

std::string hex_encode(const Bytes& b) {
    static const char* H = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) { s += H[c >> 4]; s += H[c & 15]; }
    return s;
}

bool hex_decode(std::string_view s, Bytes& out) {
    out.clear();
    std::string h(s);
    if (h.size() % 2) h = "0" + h;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < h.size(); i += 2) {
        int a = nib(h[i]), b = nib(h[i + 1]);
        if (a < 0 || b < 0) { out.clear(); return false; }
        out.push_back((uint8_t)(a << 4 | b));
    }
    return true;
}

Bytes random_bytes(size_t n) {
    Bytes b(n);
    if (!n) return b;
#ifdef _WIN32
    if (!SystemFunction036(b.data(), (ULONG)n)) b.clear();
#else
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (!f || std::fread(b.data(), 1, n, f) != n) b.clear();
    if (f) std::fclose(f);
#endif
    return b;
}

void wipe(std::string& s) {
    volatile char* p = s.data();
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
}
void wipe(Bytes& b) {
    volatile uint8_t* p = b.data();
    for (size_t i = 0; i < b.size(); ++i) p[i] = 0;
    b.clear();
}

// ============================================================ big numbers (Montgomery)
namespace {
using Limbs = std::vector<uint32_t>;  // little-endian

Limbs from_be(const Bytes& b, size_t k) {
    Limbs r(k, 0);
    size_t n = b.size();
    for (size_t i = 0; i < n && i / 4 < k; ++i) r[i / 4] |= (uint32_t)b[n - 1 - i] << (8 * (i % 4));
    return r;
}
Bytes to_be(const Limbs& a, size_t nbytes) {
    Bytes out(nbytes, 0);
    for (size_t i = 0; i < nbytes; ++i) {
        size_t limb = i / 4;
        out[nbytes - 1 - i] = limb < a.size() ? (uint8_t)(a[limb] >> (8 * (i % 4))) : 0;
    }
    return out;
}
// a >= b ; a has k or k+1 limbs, b has k limbs
bool geq(const Limbs& a, const Limbs& b) {
    for (size_t i = a.size(); i-- > 0;) {
        uint32_t bi = i < b.size() ? b[i] : 0;
        if (a[i] != bi) return a[i] > bi;
    }
    return true;
}
void sub_in_place(Limbs& a, const Limbs& b) {
    int64_t borrow = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        int64_t d = (int64_t)a[i] - (i < b.size() ? b[i] : 0) - borrow;
        borrow = d < 0;
        a[i] = (uint32_t)(d + (borrow ? (int64_t)1 << 32 : 0));
    }
}
Limbs mont_mul(const Limbs& a, const Limbs& b, const Limbs& n, uint32_t n0inv) {
    size_t k = n.size();
    std::vector<uint32_t> t(k + 2, 0);
    for (size_t i = 0; i < k; ++i) {
        uint64_t c = 0;
        for (size_t j = 0; j < k; ++j) {
            uint64_t s = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + c;
            t[j] = (uint32_t)s;
            c = s >> 32;
        }
        uint64_t s = (uint64_t)t[k] + c;
        t[k] = (uint32_t)s;
        t[k + 1] = (uint32_t)(s >> 32);
        uint32_t m = t[0] * n0inv;
        s = (uint64_t)t[0] + (uint64_t)m * n[0];
        c = s >> 32;
        for (size_t j = 1; j < k; ++j) {
            s = (uint64_t)t[j] + (uint64_t)m * n[j] + c;
            t[j - 1] = (uint32_t)s;
            c = s >> 32;
        }
        s = (uint64_t)t[k] + c;
        t[k - 1] = (uint32_t)s;
        t[k] = t[k + 1] + (uint32_t)(s >> 32);
    }
    Limbs r(t.begin(), t.begin() + k + 1);
    if (geq(r, n)) sub_in_place(r, n);
    r.resize(k);
    return r;
}
}  // namespace

Bytes modexp(const Bytes& base, const Bytes& exp, const Bytes& mod_in) {
    Bytes mod = mod_in;
    while (!mod.empty() && mod.front() == 0) mod.erase(mod.begin());
    if (mod.empty() || !(mod.back() & 1)) return {};
    size_t nbytes = mod.size();
    size_t k = (nbytes + 3) / 4;
    {
        Bytes b = base;
        while (!b.empty() && b.front() == 0) b.erase(b.begin());
        if (b.size() > k * 4) return {};
    }
    Limbs n = from_be(mod, k);
    uint32_t x = n[0];
    for (int i = 0; i < 5; ++i) x *= 2 - n[0] * x;
    uint32_t n0inv = (uint32_t)(0u - x);
    // R^2 mod n by doubling
    Limbs r2(k + 1, 0);
    r2[0] = 1;
    for (size_t i = 0; i < 64 * k; ++i) {
        uint32_t carry = 0;
        for (size_t j = 0; j < k + 1; ++j) {
            uint32_t nc = r2[j] >> 31;
            r2[j] = (r2[j] << 1) | carry;
            carry = nc;
        }
        if (geq(r2, n)) sub_in_place(r2, n);
    }
    r2.resize(k);
    Limbs one(k, 0);
    one[0] = 1;
    Limbs bm = mont_mul(from_be(base, k), r2, n, n0inv);
    Limbs acc = mont_mul(one, r2, n, n0inv);  // R mod n == 1 in Montgomery form
    bool started = false;
    for (uint8_t byte : exp) {
        for (int bit = 7; bit >= 0; --bit) {
            if (started) acc = mont_mul(acc, acc, n, n0inv);
            if ((byte >> bit) & 1) {
                acc = mont_mul(acc, bm, n, n0inv);
                started = true;
            }
        }
    }
    acc = mont_mul(acc, one, n, n0inv);
    return to_be(acc, nbytes);
}

Bytes rsa_pkcs1_encrypt(const std::string& modulus_hex, const std::string& exponent_hex, const Bytes& message, const Bytes& rnd) {
    Bytes mod, e;
    if (!hex_decode(modulus_hex, mod) || !hex_decode(exponent_hex, e)) return {};
    while (!mod.empty() && mod.front() == 0) mod.erase(mod.begin());
    size_t k = mod.size();
    if (k < 64 || message.size() + 11 > k) return {};
    size_t ps_len = k - message.size() - 3;
    Bytes em;
    em.reserve(k);
    em.push_back(0x00);
    em.push_back(0x02);
    Bytes pool = rnd;
    size_t used = 0;
    while (em.size() < 2 + ps_len) {
        if (used >= pool.size()) {
            if (!rnd.empty()) return {};  // test supplied too few bytes
            pool = random_bytes(ps_len * 2);
            used = 0;
            if (pool.empty()) return {};
        }
        uint8_t b = pool[used++];
        if (b != 0) em.push_back(b);
    }
    em.push_back(0x00);
    em.insert(em.end(), message.begin(), message.end());
    Bytes c = modexp(em, e, mod);
    wipe(em);
    return c;
}

}  // namespace vs::crypto
