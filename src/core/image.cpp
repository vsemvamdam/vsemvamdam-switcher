#include "image.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include "fsx.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace vs::img {

bool decode(const std::string& bytes, Image& out) {
    int w, h, n;
    unsigned char* p = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), (int)bytes.size(), &w, &h, &n, 4);
    if (!p) return false;
    out.w = w; out.h = h;
    out.rgba.assign(p, p + (size_t)w * h * 4);
    stbi_image_free(p);
    return true;
}

bool load_file(const std::string& path, Image& out) {
    std::string data;
    if (!fsx::read_file(path, data)) return false;
    return decode(data, out);
}

Image resize(const Image& src, int w, int h) {
    Image dst;
    if (!src.ok() || w <= 0 || h <= 0) return dst;
    dst.w = w; dst.h = h;
    dst.rgba.assign((size_t)w * h * 4, 0);
    const double sx = (double)src.w / w, sy = (double)src.h / h;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double acc[4] = {0, 0, 0, 0}, wsum = 0;
            if (sx >= 1.0 && sy >= 1.0) {
                // area average, premultiplied by alpha to avoid dark fringes
                double x0 = x * sx, x1 = (x + 1) * sx, y0 = y * sy, y1 = (y + 1) * sy;
                for (int yy = (int)y0; yy < (int)std::ceil(y1) && yy < src.h; ++yy) {
                    double wy = std::min(y1, yy + 1.0) - std::max(y0, (double)yy);
                    for (int xx = (int)x0; xx < (int)std::ceil(x1) && xx < src.w; ++xx) {
                        double wx = std::min(x1, xx + 1.0) - std::max(x0, (double)xx);
                        double ww = wx * wy;
                        const uint8_t* p = &src.rgba[((size_t)yy * src.w + xx) * 4];
                        double a = p[3] / 255.0;
                        acc[0] += p[0] * a * ww; acc[1] += p[1] * a * ww; acc[2] += p[2] * a * ww; acc[3] += p[3] * ww;
                        wsum += ww;
                    }
                }
            } else {
                double fx = (x + 0.5) * sx - 0.5, fy = (y + 0.5) * sy - 0.5;
                int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
                double tx = fx - ix, ty = fy - iy;
                for (int k = 0; k < 4; ++k) {
                    int xx = std::clamp(ix + (k & 1), 0, src.w - 1), yy = std::clamp(iy + (k >> 1), 0, src.h - 1);
                    double ww = ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty);
                    const uint8_t* p = &src.rgba[((size_t)yy * src.w + xx) * 4];
                    double a = p[3] / 255.0;
                    acc[0] += p[0] * a * ww; acc[1] += p[1] * a * ww; acc[2] += p[2] * a * ww; acc[3] += p[3] * ww;
                    wsum += ww;
                }
            }
            uint8_t* d = &dst.rgba[((size_t)y * w + x) * 4];
            if (wsum > 0) {
                double a = acc[3] / wsum;
                d[3] = (uint8_t)std::clamp(a + 0.5, 0.0, 255.0);
                double am = a / 255.0 * wsum;
                if (am > 1e-9)
                    for (int c = 0; c < 3; ++c) d[c] = (uint8_t)std::clamp(acc[c] / am + 0.5, 0.0, 255.0);
            }
        }
    }
    return dst;
}

Image crop_square(const Image& src) {
    if (!src.ok() || src.w == src.h) return src;
    int s = std::min(src.w, src.h);
    int ox = (src.w - s) / 2, oy = (src.h - s) / 2;
    Image d;
    d.w = d.h = s;
    d.rgba.resize((size_t)s * s * 4);
    for (int y = 0; y < s; ++y) memcpy(&d.rgba[(size_t)y * s * 4], &src.rgba[((size_t)(y + oy) * src.w + ox) * 4], (size_t)s * 4);
    return d;
}

void round_corners(Image& im, float frac) {
    if (!im.ok()) return;
    float r = frac * std::min(im.w, im.h);
    for (int y = 0; y < im.h; ++y) {
        for (int x = 0; x < im.w; ++x) {
            float px = x + 0.5f, py = y + 0.5f;
            float cx = std::clamp(px, r, im.w - r), cy = std::clamp(py, r, im.h - r);
            float dx = px - cx, dy = py - cy;
            float d = std::sqrt(dx * dx + dy * dy);
            float cov = std::clamp(r - d + 0.5f, 0.0f, 1.0f);
            uint8_t& a = im.rgba[((size_t)y * im.w + x) * 4 + 3];
            a = (uint8_t)(a * cov);
        }
    }
}

static void png_write_cb(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
}

std::vector<uint8_t> encode_png(const Image& im) {
    std::vector<uint8_t> out;
    if (!im.ok()) return out;
    stbi_write_png_to_func(png_write_cb, &out, im.w, im.h, 4, im.rgba.data(), im.w * 4);
    return out;
}

std::vector<uint8_t> make_ico(int w, int h, const std::vector<uint8_t>& rgba) {
    Image src{w, h, rgba};
    if (!src.ok()) return {};
    src = crop_square(src);
    const int sizes[] = {256, 64, 48, 32, 24, 16};
    std::vector<std::vector<uint8_t>> pngs;
    for (int s : sizes) pngs.push_back(encode_png(resize(src, s, s)));
    std::vector<uint8_t> f;
    auto u16 = [&](uint16_t v) { f.push_back(v & 0xFF); f.push_back(v >> 8); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) f.push_back((v >> (8 * i)) & 0xFF); };
    u16(0); u16(1); u16((uint16_t)pngs.size());
    uint32_t offset = 6 + 16 * (uint32_t)pngs.size();
    for (size_t i = 0; i < pngs.size(); ++i) {
        int s = sizes[i];
        f.push_back(s >= 256 ? 0 : (uint8_t)s);
        f.push_back(s >= 256 ? 0 : (uint8_t)s);
        f.push_back(0); f.push_back(0);
        u16(1); u16(32);
        u32((uint32_t)pngs[i].size());
        u32(offset);
        offset += (uint32_t)pngs[i].size();
    }
    for (auto& p : pngs) f.insert(f.end(), p.begin(), p.end());
    return f;
}

Image placeholder(uint32_t seed, int size, bool dark) {
    // Soft tile in a calm palette (muted, darker variant for the dark theme)
    static const uint32_t pal[] = {0xD9CFC1, 0xC9D3CC, 0xCAD2DB, 0xDACBCF, 0xD6D3C4, 0xC8C9D6, 0xD3C6BA, 0xBFCFC9};
    static const uint32_t pal_dark[] = {0x3B3631, 0x313833, 0x30363D, 0x3A3135, 0x37352E, 0x323340, 0x3A3029, 0x2E3835};
    uint32_t c = dark ? pal_dark[seed % 8] : pal[seed % (sizeof(pal) / sizeof(pal[0]))];
    Image im;
    im.w = im.h = size;
    im.rgba.resize((size_t)size * size * 4);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float t = (float)(x + y) / (2.0f * size);
            uint8_t* p = &im.rgba[((size_t)y * size + x) * 4];
            p[0] = (uint8_t)(((c >> 16) & 0xFF) * (1.0f - 0.10f * t));
            p[1] = (uint8_t)(((c >> 8) & 0xFF) * (1.0f - 0.10f * t));
            p[2] = (uint8_t)((c & 0xFF) * (1.0f - 0.10f * t));
            p[3] = 255;
        }
    return im;
}

}  // namespace vs::img
