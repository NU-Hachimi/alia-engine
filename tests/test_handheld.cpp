// Handheld pipeline test (render/handheld.hpp): a fixed scene through every feature, headless,
// compared with tests/golden/handheld_640x360.png; plus the mesh validation rules.
//   test_handheld            compare (fails on mismatch, writes the actual image next to the build)
//   test_handheld --update   rewrite the golden image (review the diff before committing!)
//
// The scene: sky gradient; a ground grid with per-vertex colours (unlit) sinking into the ground
// fog; a row of palette cubes going into the banded distance fog (sun light); one cube under the
// character light, one tinted, one half faded; a red overlay; HUD rectangles and shadowed text.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/image.hpp"
#include "platform/surface.hpp"
#include "render/handheld.hpp"

using namespace alia;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

std::array<float, 12> at(float x, float y, float z, float s = 1) { return {s, 0, 0, x, 0, s, 0, y, 0, 0, s, z}; }

}  // namespace

int main(int argc, char** argv) {
    const bool update = argc > 1 && std::strcmp(argv[1], "--update") == 0;
    std::string err;
    platform::SurfaceDesc sd;
    sd.title = "handheld";
    sd.vsync = false;
    auto surface = platform::create_headless_surface(sd, &err);
    if (!surface) {
        std::printf("SKIP: no headless EGL (%s)\n", err.c_str());
        return 77;
    }
    auto dev = rhi::create_gles_device();
    std::printf("renderer: %s\n", dev->describe().c_str());
    render::HandheldPipeline p;
    if (!p.init(*dev, 640, 360, &err)) {
        std::printf("FAIL: %s\n", err.c_str());
        return 1;
    }
    const uint8_t palette[] = {0, 0, 0, 200, 60, 40, 60, 160, 70, 230, 210, 80, 90, 120, 220};
    p.set_palette(*dev, palette);

    // Unit cube, counter-clockwise outward faces; each face pair one palette colour.
    const float cube_pos[] = {-1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1, -1, -1, 1, 1, -1, 1, 1, 1, 1, -1, 1, 1};
    const uint16_t cube_idx[] = {0, 3, 2, 0, 2, 1, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                 2, 3, 7, 2, 7, 6, 1, 2, 6, 1, 6, 5, 0, 4, 7, 0, 7, 3};
    const uint8_t cube_pal[] = {1, 1, 1, 1, 2, 2, 3, 3, 1, 1, 2, 2};
    render::MeshDesc cube{cube_pos, cube_idx, cube_pal, {}};
    const uint32_t cube_id = p.create_mesh(*dev, cube, &err);
    check(cube_id != 0, "palette mesh created");

    // Ground: 9 x 9 vertices, 10 m apart, two alternating colours.
    std::vector<float> gpos;
    std::vector<uint8_t> grgb;
    std::vector<uint16_t> gidx;
    for (int j = 0; j < 9; j++)
        for (int i = 0; i < 9; i++) {
            gpos.insert(gpos.end(), {(i - 4) * 10.0f, 0.0f, (j - 4) * 10.0f});
            const bool odd = (i + j) % 2;
            grgb.insert(grgb.end(), {uint8_t(odd ? 70 : 95), uint8_t(odd ? 130 : 150), uint8_t(odd ? 60 : 75)});
        }
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) {
            uint16_t a = uint16_t(j * 9 + i), b = uint16_t(a + 1), c = uint16_t(a + 9), d = uint16_t(c + 1);
            gidx.insert(gidx.end(), {a, c, b, b, c, d});
        }
    render::MeshDesc ground{gpos, gidx, {}, grgb};
    const uint32_t ground_id = p.create_mesh(*dev, ground, &err);
    check(ground_id != 0, "vertex-colour mesh created");

    // Validation: bad meshes are refused with a reason.
    const uint16_t bad_idx[] = {0, 1, 99};
    check(!p.create_mesh(*dev, {cube_pos, bad_idx, std::span(cube_pal, 1), {}}, &err) && !err.empty(),
          "index out of range refused");
    check(!p.create_mesh(*dev, {cube_pos, cube_idx, cube_pal, grgb}, &err), "vertex_rgb of the wrong size refused");
    check(!p.create_mesh(*dev, {cube_pos, cube_idx, std::span(cube_pal, 3), {}}, &err), "short tri_palette refused");
    check(!p.update_mesh(*dev, 999, cube, &err), "update of an unknown mesh refused");

    render::FrameDesc f;
    f.eye = {0, 3, -8};
    f.pitch = -0.15f;
    f.fog_near = 10;
    f.fog_full = 38;
    f.far_plane = 40;
    f.ground_fog = 0.6f;
    f.sun.travel = {0.3f, -0.8f, 0.5f};
    f.character.travel = {-0.6f, -0.5f, 0.6f};
    f.overlay = 0xC0202030;
    std::vector<render::Instance> list;
    list.push_back({ground_id, at(0, 0, 0), 0, render::Light::Unlit});
    for (int k = 0; k < 6; k++) list.push_back({cube_id, at((k - 2) * 4.0f, 1, k * 6.0f)});
    list.push_back({cube_id, at(-5, 2.5f, 3, 0.8f), 0, render::Light::Character});
    list.push_back({cube_id, at(5, 2.5f, 3, 0.8f), 4, render::Light::Sun, false});
    list.push_back({cube_id, at(0, 4.5f, 8), 0, render::Light::Sun, true, 0.5f});
    list.push_back({cube_id, at(0, 1, -100)});  // behind the camera: culled
    std::vector<render::HudCmd> hud;
    hud.push_back({render::HudCmd::Rect, 10, 10, 120, 8, 0x000000A0, 0, {}});
    hud.push_back({render::HudCmd::Text, 12, 30, 2, 0, 0xFFFFFFFF, 0x000000FF, "ALIA HANDHELD 0123"});
    p.render(*dev, f, list, hud);
    check(p.stats().instances == list.size() && p.stats().culled == 1, "one instance culled");

    const char* env_dir = std::getenv("ALIA_GOLDEN_DIR");
    auto golden_check = [&](const char* name) {
        Image img{640, 360, std::vector<uint8_t>(640 * 360 * 4)};
        dev->read_pixels(p.target(), 640, 360, img.rgba.data());
        const std::string golden = std::string(env_dir ? env_dir : ALIA_GOLDEN_DIR) + "/" + name + ".png";
        if (update) {
            write_png(golden, img);
            std::printf("updated %s\n", golden.c_str());
            return;
        }
        Image ref;
        if (!read_png(golden, &ref)) {
            check(false, "golden image present (run with --update)");
            return;
        }
        auto d = compare(img, ref, 8);
        const int allowed = 640 * 360 / 200;  // 0.5%: triangle edges, fog band edges
        std::printf("%s: %d pixels differ by > 8 levels (allowed %d), max delta %d\n", name, d.differing_pixels,
                    allowed, d.max_channel_delta);
        const bool ok = d.same_size && d.differing_pixels <= allowed;
        check(ok, (std::string("image matches tests/golden/") + name + ".png").c_str());
        if (!ok) write_png(std::string(name) + ".actual.png", img);
    };
    golden_check("handheld_640x360");

    // Materials (docs/api.md §2): the same ground, then one cube per feature.
    auto material = [&](render::Material m) {
        uint32_t id = p.create_material(m, &err);
        check(id != 0, "material created");
        return id;
    };
    const uint32_t glow = material({.shading = render::Shading::Unlit, .palette_index = 4, .fog = false});
    const uint32_t glass = material({.palette_index = 2, .alpha_mode = render::AlphaMode::Blend, .opacity = 0.5f});
    const uint32_t key = material({.light = render::Light::Character, .ambient = 0.2f, .diffuse = 0.9f});
    const uint32_t half = material({.emissive = 0.5f});
    const uint32_t soft = material({.shading = render::Shading::Smooth, .color = render::ColorSource::Vertex});
    check(!p.create_material({.light = render::Light::Unlit}, &err), "light = unlit refused (it is a shading)");
    check(!p.create_material({.opacity = 2}, &err), "opacity outside 0..1 refused");
    // A cube in two parts: the first four faces with the default material, the top and bottom glowing.
    const render::Submesh parts[] = {{0, 8, 0}, {8, 4, glow}};
    const uint32_t two_part = p.create_mesh(*dev, {cube_pos, cube_idx, cube_pal, {}, false, parts}, &err);
    check(two_part != 0, "mesh with two parts created");
    const render::Submesh as_glass[] = {{0, 12, glass}}, as_key[] = {{0, 12, key}}, as_half[] = {{0, 12, half}};
    const uint32_t glass_cube = p.create_mesh(*dev, {cube_pos, cube_idx, {}, {}, false, as_glass}, &err);
    check(glass_cube != 0, "palette material with palette_index needs no tri_palette");
    const uint32_t key_cube = p.create_mesh(*dev, {cube_pos, cube_idx, cube_pal, {}, false, as_key}, &err);
    const uint32_t half_cube = p.create_mesh(*dev, {cube_pos, cube_idx, cube_pal, {}, false, as_half}, &err);
    const render::Submesh as_soft[] = {{0, 128, soft}};
    const uint32_t soft_ground = p.create_mesh(*dev, {gpos, gidx, {}, grgb, false, as_soft}, &err);
    check(key_cube && half_cube && soft_ground, "meshes with materials created");
    const render::Submesh bad_range[] = {{10, 5, 0}}, bad_material[] = {{0, 12, 999}}, needs_rgb[] = {{0, 12, soft}};
    check(!p.create_mesh(*dev, {cube_pos, cube_idx, cube_pal, {}, false, bad_range}, &err), "submesh out of range refused");
    check(!p.create_mesh(*dev, {cube_pos, cube_idx, cube_pal, {}, false, bad_material}, &err), "unknown material refused");
    check(!p.create_mesh(*dev, {cube_pos, cube_idx, cube_pal, {}, false, needs_rgb}, &err),
          "vertex-colour material without vertex_rgb refused");

    std::vector<render::Instance> mats;
    mats.push_back({soft_ground, at(0, 0, 0), 0, render::Light::Unlit});
    mats.push_back({two_part, at(-4, 1.5f, 2)});
    mats.push_back({key_cube, at(-1.3f, 1.5f, 2)});
    mats.push_back({half_cube, at(1.3f, 1.5f, 2)});
    mats.push_back({two_part, at(4, 1.5f, 30)});            // far: the glowing faces ignore the fog
    mats.push_back({glass_cube, at(0, 1.2f, -2.5f, 0.9f)}); // in front of the others, half see-through
    mats.push_back({cube_id, at(4, 1.5f, 2), 0, render::Light::Material, true, 0.0f});
    p.render(*dev, f, mats, {});
    // The two two-part cubes share the mesh, light and ground fog: one instanced draw per part.
    check(p.stats().draws == 7 && p.stats().instances == 7, "one draw per part; the same mesh twice is one instanced draw");
    golden_check("handheld_materials_640x360");
    // update_material changes what applies at draw time for meshes that already exist.
    check(p.update_material(glass, {.palette_index = 2, .alpha_mode = render::AlphaMode::Opaque}, &err) &&
              !p.update_material(999, {}, &err),
          "update_material: known id changed, unknown refused");

    // Instancing draws exactly what one draw per instance would: 40 cubes with their own tints and
    // fades, as one mesh (batched) and as 40 copies of it (each its own group).
    std::vector<render::Instance> crowd, apart;
    std::vector<uint32_t> copies;
    for (int k = 0; k < 40; k++) {
        copies.push_back(p.create_mesh(*dev, cube, &err));
        render::Instance in{cube_id, at(float(k % 8) * 1.6f - 5.6f, 0.6f + float(k / 8) * 1.2f, 6.0f + float(k % 3))};
        in.tint = uint8_t(k % 5 == 0 ? 0 : 1 + k % 7);
        in.fade = float(k % 4) * 0.2f;
        crowd.push_back(in);
        in.mesh = copies.back();
        apart.push_back(in);
    }
    auto pixels = [&](const std::vector<render::Instance>& list) {
        std::vector<uint8_t> img(640 * 360 * 4);
        p.render(*dev, f, list, {});
        dev->read_pixels(p.target(), 640, 360, img.data());
        return img;
    };
    const auto batched = pixels(crowd);
    const uint32_t batched_draws = p.stats().draws;
    const auto single = pixels(apart);
    check(batched == single && batched_draws == 1 && p.stats().draws == 40,
          "instancing: 40 instances in one draw, the same pixels as 40 draws");
    p.shutdown(*dev);
    return failures != 0;
}
