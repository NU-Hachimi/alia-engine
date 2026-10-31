// E2 benchmark (docs/architecture.md §6): synthetic loads through the real handheld pipeline,
// headless, to measure the board's budget. Each case renders N frames without vsync and
// waits for the GPU at the end, so ms/frame is what the GPU (or the CPU feeding it) costs.
//
//   alia-bench                 all cases, a table on stdout
//   alia-bench --frames 300    frames per case (default 200)
//   alia-bench --only tris     cases whose name contains the text
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.hpp"
#include "runtime/engine.hpp"

using namespace alia;

namespace {

// Subdivided icosahedron-like sphere: a lat/long grid, flat shaded, palette colours.
render::MeshDesc sphere(int rings, int segments, std::vector<float>& pos, std::vector<uint16_t>& idx,
                        std::vector<uint8_t>& pal) {
    pos.clear();
    idx.clear();
    pal.clear();
    for (int r = 0; r <= rings; r++) {
        float v = float(M_PI) * r / rings;
        for (int s = 0; s <= segments; s++) {
            float u = 2 * float(M_PI) * s / segments;
            pos.insert(pos.end(), {std::sin(v) * std::cos(u), std::cos(v), std::sin(v) * std::sin(u)});
        }
    }
    for (int r = 0; r < rings; r++)
        for (int s = 0; s < segments; s++) {
            uint16_t a = uint16_t(r * (segments + 1) + s), b = uint16_t(a + 1), c = uint16_t(a + segments + 1),
                     d = uint16_t(c + 1);
            idx.insert(idx.end(), {a, b, c, b, d, c});
            pal.insert(pal.end(), {uint8_t(1 + (r + s) % 3), uint8_t(1 + (r + s) % 3)});
        }
    return {pos, idx, pal, {}};
}

struct Case {
    std::string name;
    int internal_w = 640, internal_h = 360, out_w = 1280, out_h = 720, rotation = 0;
    int instances = 0;       // spheres
    int mesh = 0;            // 0: small sphere (~200 tris), 1: big sphere (~2000 tris)
    int overdraw = 0;        // full-screen quads drawn back to front (fill rate)
    int glyphs = 0;          // HUD characters
    bool fog = true;
};

double run(runtime::Engine& e, const Case& c, uint32_t small, uint32_t big, uint32_t quad, int frames,
           uint32_t* tris) {
    auto frame = [&](int n) {
        render::FrameDesc& f = e.frame();
        f.eye = {0, 0, -12};
        f.yaw = 0.0f + 0.001f * float(n);  // a tiny change per frame, so nothing can be cached
        f.pitch = 0;
        f.fog_steps = c.fog ? 16 : 0;
        f.fog_near = 10;
        f.fog_full = 60;
        f.far_plane = 80;
        const int side = int(std::ceil(std::sqrt(double(std::max(c.instances, 1)))));
        for (int i = 0; i < c.instances; i++) {
            float x = (float(i % side) - float(side) / 2) * 18.0f / float(side), y = (float(i / side) - float(side) / 2) * 10.0f / float(side);
            float s = 7.0f / float(side);
            e.draw({c.mesh ? big : small, {s, 0, 0, x, 0, s, 0, y, 0, 0, s, 0}});
        }
        // Back to front, each layer nearer than the last: every layer is shaded (no early-z rejection).
        for (int k = 0; k < c.overdraw; k++) {
            float z = 20.0f - float(k) * 15.0f / std::max(c.overdraw, 1);
            e.draw({quad, {40, 0, 0, 0, 0, 25, 0, 0, 0, 0, 1, z}, uint8_t(1 + k % 3), render::Light::Unlit});
        }
        for (int g = 0; g < c.glyphs; g += 50) e.hud_text(4, 4 + float(g) / 50 * 8, std::string(50, 'A'), 0xFFFFFFFF, 1, 0);
        e.end_frame();
    };
    frame(0);  // warm-up: shader compile, buffer uploads
    e.finish();
    *tris = e.stats().triangles;
    auto t0 = std::chrono::steady_clock::now();
    for (int n = 1; n <= frames; n++) frame(n);
    e.finish();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / frames;
}

}  // namespace

int main(int argc, char** argv) {
    int frames = 200;
    const char* only = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else {
            std::printf("usage: %s [--frames N] [--only TEXT]\n", argv[0]);
            return 2;
        }
    }
    log::set_level(log::Level::Warn);

    std::vector<Case> cases;
    auto add = [&](Case c) { cases.push_back(c); };
    add({"empty (sky + final pass)"});
    add({"empty, no fog", 640, 360, 1280, 720, 0, 0, 0, 0, 0, false});
    for (int n : {10, 25, 50, 100, 200}) add({"tris: " + std::to_string(n) + " x big sphere", 640, 360, 1280, 720, 0, n, 1});
    for (int n : {100, 250, 500, 1000}) add({"draws: " + std::to_string(n) + " x small sphere", 640, 360, 1280, 720, 0, n, 0});
    for (int k : {1, 4, 8, 16}) add({"fill: " + std::to_string(k) + " full-screen layers @640x360", 640, 360, 1280, 720, 0, 0, 0, k});
    for (int k : {1, 4, 8}) add({"fill: " + std::to_string(k) + " full-screen layers @1280x720", 1280, 720, 1280, 720, 0, 0, 0, k});
    add({"res: 854x480 + 25 big spheres", 854, 480, 1280, 720, 0, 25, 1});
    add({"res: 960x540 + 25 big spheres", 960, 540, 1280, 720, 0, 25, 1});
    add({"res: 1280x720 + 25 big spheres", 1280, 720, 1280, 720, 0, 25, 1});
    add({"final: 640x360 -> 720x1280 rotated", 640, 360, 720, 1280, 90});
    add({"final: 640x360 -> 1920x1080 (TV)", 640, 360, 1920, 1080, 0});
    add({"hud: 1000 glyphs", 640, 360, 1280, 720, 0, 0, 0, 0, 1000});
    add({"megalith-like: 80 x small sphere + 2 layers", 640, 360, 1280, 720, 0, 80, 0, 2, 120});

    runtime::Engine e;
    std::string err;
    std::printf("%-44s %10s %10s %8s\n", "case", "ms/frame", "fps", "tris");
    for (const Case& c : cases) {
        if (only && c.name.find(only) == std::string::npos) continue;
        runtime::Config cfg;
        cfg.headless = true;
        cfg.vsync = false;
        cfg.width = c.out_w;
        cfg.height = c.out_h;
        cfg.internal_w = c.internal_w;
        cfg.internal_h = c.internal_h;
        cfg.rotation = c.rotation;
        if (!e.open(cfg, &err)) {
            std::printf("FAIL: %s\n", err.c_str());
            return 1;
        }
        std::vector<float> pos;
        std::vector<uint16_t> idx;
        std::vector<uint8_t> pal;
        const uint8_t palette[] = {0, 0, 0, 200, 60, 40, 60, 160, 70, 230, 210, 80};
        e.set_palette(palette);
        uint32_t small = e.create_mesh(sphere(10, 10, pos, idx, pal), &err);  // 200 tris
        uint32_t big = e.create_mesh(sphere(32, 32, pos, idx, pal), &err);    // 2048 tris
        const float qp[] = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
        const uint16_t qi[] = {0, 2, 1, 0, 3, 2};  // facing the camera (-Z)
        const uint8_t qc[] = {1, 1};
        uint32_t quad = e.create_mesh({qp, qi, qc, {}}, &err);
        uint32_t tris = 0;
        double ms = run(e, c, small, big, quad, frames, &tris);
        std::printf("%-44s %10.2f %10.0f %8u\n", c.name.c_str(), ms, 1000 / ms, tris);
        std::fflush(stdout);
        e.close();
    }
    return 0;
}
