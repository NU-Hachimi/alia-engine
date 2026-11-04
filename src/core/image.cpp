#include "core/image.hpp"

#include <algorithm>
#include <cstdlib>

#include "stb_image.h"
#include "stb_image_write.h"

namespace alia {

bool write_png(const std::string& path, const Image& img) {
    return stbi_write_png(path.c_str(), img.width, img.height, 4, img.rgba.data(), img.width * 4) != 0;
}

bool read_png(const std::string& path, Image* img) {
    int w, h, n;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!data) return false;
    img->width = w;
    img->height = h;
    img->rgba.assign(data, data + size_t(w) * h * 4);
    stbi_image_free(data);
    return true;
}

ImageDiff compare(const Image& a, const Image& b, int tolerance) {
    ImageDiff d;
    d.same_size = a.width == b.width && a.height == b.height;
    if (!d.same_size) return d;
    for (size_t i = 0; i < a.rgba.size(); i += 4) {
        int worst = 0;
        for (int c = 0; c < 4; c++) worst = std::max(worst, std::abs(int(a.rgba[i + c]) - int(b.rgba[i + c])));
        d.max_channel_delta = std::max(d.max_channel_delta, worst);
        if (worst > tolerance) d.differing_pixels++;
    }
    return d;
}

bool decode_image(const uint8_t* data, size_t size, Image* img, std::string* error) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(data, int(size), &w, &h, &n, 4);
    if (!px) {
        *error = std::string("not a PNG or JPEG: ") + stbi_failure_reason();
        return false;
    }
    img->width = w, img->height = h;
    img->rgba.assign(px, px + size_t(w) * h * 4);
    stbi_image_free(px);
    return true;
}

Image shrink(const Image& img, int max_side) {
    const int longest = std::max(img.width, img.height);
    if (max_side <= 0 || longest <= max_side) return img;
    Image out;
    out.width = std::max(1, int(int64_t(img.width) * max_side / longest));
    out.height = std::max(1, int(int64_t(img.height) * max_side / longest));
    out.rgba.resize(size_t(out.width) * out.height * 4);
    // Each output pixel averages the source pixels its area covers (whole pixels; good enough to view photos).
    for (int y = 0; y < out.height; y++) {
        const int y0 = int(int64_t(y) * img.height / out.height), y1 = std::max(y0 + 1, int(int64_t(y + 1) * img.height / out.height));
        for (int x = 0; x < out.width; x++) {
            const int x0 = int(int64_t(x) * img.width / out.width), x1 = std::max(x0 + 1, int(int64_t(x + 1) * img.width / out.width));
            uint32_t sum[4] = {0, 0, 0, 0};
            for (int sy = y0; sy < y1; sy++) {
                const uint8_t* row = &img.rgba[(size_t(sy) * img.width + x0) * 4];
                for (int sx = x0; sx < x1; sx++, row += 4)
                    for (int c = 0; c < 4; c++) sum[c] += row[c];
            }
            const uint32_t count = uint32_t((y1 - y0) * (x1 - x0));
            uint8_t* o = &out.rgba[(size_t(y) * out.width + x) * 4];
            for (int c = 0; c < 4; c++) o[c] = uint8_t((sum[c] + count / 2) / count);
        }
    }
    return out;
}

}  // namespace alia
