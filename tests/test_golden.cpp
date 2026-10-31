// Golden image test: render the E0 test scene headless and compare with tests/golden/.
//   test_golden            compare (fails on mismatch, writes the actual image next to the build)
//   test_golden --update   rewrite the golden images (review the diff before committing!)
//
// Different GPUs rasterise triangle edges slightly differently, so a small number of pixels
// may differ by a few levels; flat areas, the checker, the frame and the orientation marker
// must match.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/image.hpp"
#include "core/log.hpp"
#include "platform/surface.hpp"
#include "render/final_pass.hpp"
#include "render/test_scene.hpp"

using namespace alia;

int main(int argc, char** argv) {
    bool update = argc > 1 && std::strcmp(argv[1], "--update") == 0;
    std::string err;
    platform::SurfaceDesc sd;
    sd.width = sd.height = 1280;
    sd.title = "golden";
    sd.vsync = false;
    auto surface = platform::create_headless_surface(sd, &err);
    if (!surface) {
        std::printf("SKIP: no headless EGL (%s)\n", err.c_str());
        return 77;
    }
    auto dev = rhi::create_gles_device();
    std::printf("renderer: %s\n", dev->describe().c_str());
    render::TestScene scene;
    render::FinalPass final_pass;
    if (!scene.init(*dev, &err) || !final_pass.init(*dev, &err)) {
        std::printf("FAIL: %s\n", err.c_str());
        return 1;
    }
    scene.render(*dev, 0.0f);

    struct Case { const char* name; int w, h, rotation; };
    const Case cases[] = {{"e0_landscape_1280x720", 1280, 720, 0}, {"e0_panel_720x1280_rot90", 720, 1280, 90}};
    int failures = 0;
    for (const auto& c : cases) {
        auto tex = dev->create_texture({c.w, c.h, rhi::Format::RGBA8, rhi::Filter::Nearest, nullptr});
        auto target = dev->create_target({tex, {}});
        final_pass.draw(*dev, scene.color(), render::TestScene::kWidth, render::TestScene::kHeight, target, c.w, c.h,
                        c.rotation);
        Image img{c.w, c.h, std::vector<uint8_t>(size_t(c.w) * c.h * 4)};
        dev->read_pixels(target, c.w, c.h, img.rgba.data());
        // ALIA_GOLDEN_DIR in the environment overrides the build-time path (cross-built tests
        // run on the board, where the source tree isn't at the PC's path).
        const char* env_dir = std::getenv("ALIA_GOLDEN_DIR");
        std::string golden = std::string(env_dir ? env_dir : ALIA_GOLDEN_DIR) + "/" + c.name + ".png";
        if (update) {
            write_png(golden, img);
            std::printf("updated %s\n", golden.c_str());
            continue;
        }
        Image ref;
        if (!read_png(golden, &ref)) {
            std::printf("FAIL %s: no golden image (run with --update)\n", c.name);
            failures++;
            continue;
        }
        auto d = compare(img, ref, 8);
        int allowed = c.w * c.h / 200;  // 0.5%: edge pixels of the triangles
        bool ok = d.same_size && d.differing_pixels <= allowed;
        std::printf("%s %s: %d pixels differ by > 8 levels (allowed %d), max delta %d\n", ok ? "ok  " : "FAIL",
                    c.name, d.differing_pixels, allowed, d.max_channel_delta);
        if (!ok) {
            write_png(std::string(c.name) + ".actual.png", img);
            failures++;
        }
    }
    return failures != 0;
}
