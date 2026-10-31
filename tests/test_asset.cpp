// L4 resource packs (asset/pack.hpp, docs/formats.md). The packs here are built byte by byte, not
// with tools/alia_pack.py, so the reader is checked against the format as written down; the
// Python test (test_python.py) then checks the writer against this reader.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "asset/pack.hpp"

using namespace alia;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

struct Bytes {
    std::vector<uint8_t> v;
    void u8(uint32_t x) { v.push_back(uint8_t(x)); }
    void u16(uint32_t x) { u8(x), u8(x >> 8); }
    void u32(uint32_t x) { u16(x), u16(x >> 16); }
    void f32(float f) {
        uint32_t x;
        std::memcpy(&x, &f, 4);
        u32(x);
    }
    void str(const std::string& s) { v.insert(v.end(), s.begin(), s.end()); }
    void pad4() {
        while (v.size() % 4) u8(0);
    }
};

struct Item {
    uint8_t type;
    std::string name;
    std::vector<uint8_t> payload;
};

std::vector<uint8_t> pack(const std::vector<Item>& items, uint16_t version = 1) {
    Bytes b;
    b.str("APAK");
    b.u16(version);
    b.u16(0);
    b.u32(uint32_t(items.size()));
    b.u32(0);  // directory offset, patched below
    std::vector<uint32_t> offsets;
    for (const Item& it : items) {
        offsets.push_back(uint32_t(b.v.size()));
        b.v.insert(b.v.end(), it.payload.begin(), it.payload.end());
        b.pad4();
    }
    const uint32_t dir = uint32_t(b.v.size());
    std::memcpy(&b.v[12], &dir, 4);
    for (size_t i = 0; i < items.size(); i++) {
        b.u8(items[i].type);
        b.u8(0);
        b.u16(uint32_t(items[i].name.size()));
        b.u32(offsets[i]);
        b.u32(uint32_t(items[i].payload.size()));
        b.str(items[i].name);
    }
    return b.v;
}

// A triangle (mode 1: palette) or a quad (mode 2: vertex rgb).
std::vector<uint8_t> mesh(uint8_t mode, bool double_sided = false, uint16_t bad_index = 0) {
    Bytes b;
    const int nv = mode == 1 ? 3 : 4, nt = mode == 1 ? 1 : 2;
    b.u32(nv);
    b.u32(nt);
    b.u8(mode);
    b.u8(double_sided ? 1 : 0);
    b.u16(0);
    const float quad[] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    for (int i = 0; i < nv * 3; i++) b.f32(quad[i]);
    const uint16_t idx[] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < nt * 3; i++) b.u16(bad_index && i == 2 ? bad_index : idx[i]);
    if (mode == 1) b.u8(7);
    else
        for (int i = 0; i < nv * 3; i++) b.u8(uint32_t(10 * i));
    return b.v;
}

}  // namespace

int main() {
    std::string err;
    asset::Pack p;
    check(p.load(pack({{1, "tri", mesh(1)}, {1, "quad", mesh(2, true)}, {9, "future", {1, 2, 3}}}), &err),
          "pack loads (an unknown entry type is kept, not an error)");
    check(p.entries().size() == 3, "three entries");
    const asset::Entry* tri = p.find(asset::EntryType::Mesh, "tri");
    const asset::Entry* quad = p.find(asset::EntryType::Mesh, "quad");
    check(tri && quad && !p.find(asset::EntryType::Mesh, "future"), "find by type and name");
    asset::MeshData m;
    check(tri && p.read_mesh(*tri, &m, &err) && m.positions.size() == 9 && m.indices.size() == 3 &&
              m.tri_palette == std::vector<uint8_t>{7} && m.vertex_rgb.empty() && !m.double_sided,
          "palette mesh read");
    check(quad && p.read_mesh(*quad, &m, &err) && m.positions[3] == 1.0f && m.indices.size() == 6 &&
              m.vertex_rgb.size() == 12 && m.vertex_rgb[11] == 110 && m.tri_palette.empty() && m.double_sided,
          "vertex-rgb mesh read, double-sided");

    // Broken packs give errors, not crashes.
    std::vector<uint8_t> good = pack({{1, "tri", mesh(1)}});
    std::vector<uint8_t> bad = good;
    bad[0] = 'X';
    check(!p.load(bad, &err) && !err.empty(), "wrong magic refused");
    check(!p.load(pack({{1, "tri", mesh(1)}}, 2), &err), "version 2 refused");
    bool all_refused = true;
    for (size_t n : {size_t(3), size_t(15), good.size() - 1}) {
        std::vector<uint8_t> cut(good.begin(), good.begin() + long(n));
        std::string e2;
        const bool loaded = p.load(cut, &e2);
        if (loaded && !p.entries().empty() && p.read_mesh(p.entries()[0], &m, &e2)) all_refused = false;
    }
    check(all_refused, "truncated packs refused (cut at 3, 15 and size - 1 bytes)");
    check(p.load(pack({{1, "tri", mesh(1, false, 9)}}), &err) && !p.read_mesh(p.entries()[0], &m, &err) &&
              err.find("index") != std::string::npos,
          "index out of range refused");
    std::vector<uint8_t> oob = good;
    const uint32_t huge = 0x7FFFFFFF;
    std::memcpy(&oob[oob.size() - 3 - 4], &huge, 4);  // the directory's size field of "tri"
    check(!p.load(oob, &err), "entry outside the data refused");

    // Materials (type 2) and a mesh with a part table (flags bit 1), colours "both" (mode 3).
    Bytes mat;
    mat.u8(2), mat.u8(1), mat.u8(9), mat.u8(1);  // unlit, vertex colours, palette 9, character light
    mat.f32(0.25f), mat.f32(-1), mat.f32(0.5f);  // ambient, diffuse, emissive
    mat.u8(1), mat.u8(1 | 4), mat.u16(0);        // blend; double-sided, no fog, ground fog
    mat.f32(0.75f);                              // opacity
    Bytes parted;
    parted.u32(4), parted.u32(2), parted.u8(3), parted.u8(2), parted.u16(0);
    for (float x : {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0}) parted.f32(x);
    for (uint16_t i : {0, 1, 2, 0, 2, 3}) parted.u16(i);
    parted.u8(5), parted.u8(6);                                  // tri_palette
    for (int i = 0; i < 12; i++) parted.u8(uint32_t(i));         // vertex_rgb
    parted.u32(2);                                               // two parts
    parted.u32(0), parted.u32(1), parted.u16(0);                 // default material
    parted.u32(1), parted.u32(1), parted.u16(4), parted.str("glow");
    check(p.load(pack({{2, "glow", mat.v}, {1, "rune", parted.v}}), &err), "pack with a material and parts loads");
    asset::MaterialData md;
    check(p.read_material(*p.find(asset::EntryType::Material, "glow"), &md, &err) && md.shading == 2 && md.color == 1 &&
              md.palette_index == 9 && md.light == 1 && md.ambient == 0.25f && md.diffuse == -1 && md.emissive == 0.5f &&
              md.alpha_mode == 1 && md.opacity == 0.75f && md.double_sided && !md.fog && md.ground_fog,
          "material read");
    check(p.read_mesh(*p.find(asset::EntryType::Mesh, "rune"), &m, &err) && m.tri_palette.size() == 2 &&
              m.vertex_rgb.size() == 12 && m.submeshes.size() == 2 && m.submeshes[0].material.empty() &&
              m.submeshes[1].material == "glow" && m.submeshes[1].first_tri == 1,
          "mesh with both colour sources and two parts read");
    Bytes bad_mat = mat;
    bad_mat.v[0] = 7;  // no such shading
    check(p.load(pack({{2, "x", bad_mat.v}}), &err) && !p.read_material(p.entries()[0], &md, &err), "unknown shading refused");
    Bytes bad_part = parted;
    bad_part.v[bad_part.v.size() - 4 - 2 - 8] = 9;  // the second part's tri_count: past the triangles
    check(p.load(pack({{1, "rune", bad_part.v}}), &err) && !p.read_mesh(p.entries()[0], &m, &err),
          "part outside the triangles refused");

    check(asset::content_path("/abs/x.apak") == "/abs/x.apak", "absolute paths stay");
    unsetenv("ALIA_CART_DIR");
    check(asset::content_path("gen/x.apak") == "gen/x.apak", "no cart: the working directory");
    setenv("ALIA_CART_DIR", "/cart", 1);
    check(asset::content_path("gen/x.apak") == "/cart/content/gen/x.apak", "in a cart: its content directory");
    return failures != 0;
}
