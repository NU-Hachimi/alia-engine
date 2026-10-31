// L0: RGBA8 images in memory and PNG I/O (screenshots, golden tests).
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

// Per-channel comparison: pixels differing by more than `tolerance` in any channel.
struct ImageDiff {
    bool same_size = false;
    int differing_pixels = 0;
    int max_channel_delta = 0;
};
ImageDiff compare(const Image& a, const Image& b, int tolerance);

}  // namespace alia
