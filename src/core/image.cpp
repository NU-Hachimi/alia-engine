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

}  // namespace alia
