// E0 demo: the test scene through the whole stack (platform -> rhi -> render).
//
//   alia-e0-demo                         window, animated
//   alia-e0-demo --rotate 90             as it would go to the portrait DSI panel
//   alia-e0-demo --headless --out a.png  surfaceless EGL, one frame to a PNG
//   alia-e0-demo --headless --frames 600 benchmark: N frames offscreen, prints timing
#include <chrono>
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
    bool headless = false;
    const char* out_png = nullptr;
    int rotation = 0, frames = 0, w = 1280, h = 720, iw = 640, ih = 360;
    render::Scaling scaling = render::Scaling::Auto;
    for (int i = 1; i < argc; i++) {
        auto arg = [&](const char* name) { return std::strcmp(argv[i], name) == 0 && i + 1 < argc; };
        if (std::strcmp(argv[i], "--headless") == 0) headless = true;
        else if (arg("--out")) out_png = argv[++i];
        else if (arg("--rotate")) rotation = std::atoi(argv[++i]);
        else if (arg("--frames")) frames = std::atoi(argv[++i]);
        else if (arg("--size")) std::sscanf(argv[++i], "%dx%d", &w, &h);
        else if (arg("--internal")) std::sscanf(argv[++i], "%dx%d", &iw, &ih);
        else if (arg("--scaling")) {
            const char* m = argv[++i];
            scaling = !std::strcmp(m, "integer") ? render::Scaling::Integer
                      : !std::strcmp(m, "sharp") ? render::Scaling::SharpBilinear : render::Scaling::Auto;
        }
        else {
            std::printf("usage: %s [--headless] [--out file.png] [--rotate 0|90|180|270] [--frames N] [--size WxH]\n"
                        "       [--internal WxH (default 640x360)] [--scaling auto|integer|sharp]\n", argv[0]);
            return 2;
        }
    }
    if (rotation == 90 || rotation == 270) std::swap(w, h);  // portrait output by default when rotated

    std::string err;
    platform::SurfaceDesc sd;
    sd.width = w;
    sd.height = h;
    sd.title = "alia-engine E0";
    sd.app_id = "alia-e0-demo";
    auto surface = headless ? platform::create_headless_surface(sd, &err) : platform::create_window_surface(sd, &err);
    if (!surface) {
        ALIA_ERROR("%s", err.c_str());
        return 1;
    }
    auto dev = rhi::create_gles_device();
    ALIA_INFO("%s | %s | internal %dx%d", surface->describe().c_str(), dev->describe().c_str(), iw, ih);

    render::TestScene scene;
    render::FinalPass final_pass;
    if (!scene.init(*dev, &err, iw, ih) || !final_pass.init(*dev, &err)) {
        ALIA_ERROR("%s", err.c_str());
        return 1;
    }

    // Headless has no framebuffer 0: the final pass goes into an offscreen output target.
    rhi::TextureH out_tex;
    rhi::TargetH out_target;
    if (!surface->has_default_framebuffer()) {
        out_tex = dev->create_texture({w, h, rhi::Format::RGBA8, rhi::Filter::Nearest, nullptr});
        out_target = dev->create_target({out_tex, {}});
    }

    int total = frames > 0 ? frames : (headless ? 1 : -1);
    auto t0 = std::chrono::steady_clock::now();
    rhi::FrameStats last{};
    for (int n = 0; total < 0 || n < total; n++) {
        if (!surface->poll_events()) break;
        float t = headless ? 0.0f : std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
        scene.render(*dev, t);
        int ow = surface->has_default_framebuffer() ? surface->width() : w;
        int oh = surface->has_default_framebuffer() ? surface->height() : h;
        final_pass.draw(*dev, scene.color(), scene.width(), scene.height(), out_target, ow, oh, rotation, scaling);
        surface->present();
        last = dev->take_stats();
    }
    if (frames > 0) {
        Image probe;  // a read-back forces the GPU to finish, so the timing is honest
        probe.rgba.resize(4);
        dev->read_pixels(out_target, 1, 1, probe.rgba.data());
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%d frames in %.3f s: %.2f ms/frame (%.0f fps); per frame %u passes, %u draws, %u triangles\n",
                    frames, s, 1000 * s / frames, frames / s, last.passes, last.draws, last.triangles);
    }
    if (out_png) {
        Image img;
        img.width = w;
        img.height = h;
        img.rgba.resize(size_t(w) * h * 4);
        dev->read_pixels(out_target, w, h, img.rgba.data());
        if (!write_png(out_png, img)) {
            ALIA_ERROR("cannot write %s", out_png);
            return 1;
        }
        ALIA_INFO("wrote %s (%dx%d)", out_png, w, h);
    }
    return 0;
}
