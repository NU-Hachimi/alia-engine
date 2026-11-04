// L0: RGBA8 images in memory, PNG I/O (screenshots, golden tests), PNG / JPEG decoding (textures).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace alia {

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;  // top row first

    bool empty() const { return rgba.empty(); }
};

bool write_png(const std::string& path, const Image& img);
bool read_png(const std::string& path, Image* img);
// A PNG or JPEG in memory -> RGBA8; false (and why) when it isn't one.
bool decode_image(const uint8_t* data, size_t size, Image* img, std::string* error);
// Scaled down by averaging (a box filter) so the longer side is at most max_side; smaller images unchanged.
Image shrink(const Image& img, int max_side);
// JPEG bytes of an image (quality 1..100; alpha dropped).
std::vector<uint8_t> encode_jpeg(const Image& img, int quality);

// Per-channel comparison: pixels differing by more than `tolerance` in any channel.
struct ImageDiff {
    bool same_size = false;
    int differing_pixels = 0;
    int max_channel_delta = 0;
};
ImageDiff compare(const Image& a, const Image& b, int tolerance);

}  // namespace alia
