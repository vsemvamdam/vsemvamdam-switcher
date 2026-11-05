// vsemvamdam-switcher — small RGBA image helpers (portable)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace vs::img {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    bool ok() const { return w > 0 && h > 0 && rgba.size() == (size_t)w * h * 4; }
};

bool decode(const std::string& bytes, Image& out);   // PNG/JPG/BMP/GIF(first frame)
bool load_file(const std::string& path, Image& out);
Image resize(const Image& src, int w, int h);        // area-average downscale / bilinear upscale
Image crop_square(const Image& src);                 // center crop
void round_corners(Image& im, float radius_frac);    // alpha mask, radius as fraction of size (0.5 = circle)
std::vector<uint8_t> encode_png(const Image& im);
std::vector<uint8_t> make_ico(int w, int h, const std::vector<uint8_t>& rgba);  // sizes 256,64,48,32,16 (PNG entries)
// Monogram placeholder avatar: colored square with initial letters drawn as blocky glyph-free circle pattern.
Image placeholder(uint32_t seed, int size, bool dark = false);

}  // namespace vs::img
