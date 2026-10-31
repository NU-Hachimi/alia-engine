// L3: the handheld scene pipeline (docs/architecture.md §1, §2): one pass into the
// low-resolution scene target, then the final pass puts it on the output.
//
// Scene pass, in order:
//   1. sky: a vertical gradient by elevation (no geometry, no depth)
//   2. meshes: forward, per-vertex lighting from one of two directional lights, colours from
//      a 32-entry palette (so a new Look is one palette upload) or baked per-vertex RGB;
//      distance fog in quantized steps (the N64 / PS1 banding) and a height ("ground") fog
//   3. overlay: an optional full-screen tint (damage flash)
//   4. HUD: rectangles and 5x7 text in scene pixels, alpha blended, so it scales with the scene
//
// The game hands over a draw list each frame (camera, lights, fog, instances, HUD); this
// layer knows nothing about where it came from (ECS, a g3d frame, Python).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "rhi/rhi.hpp"

namespace alia::render {

enum class Light : uint8_t {
    Sun = 0,
    Character = 1,
    Unlit = 2,     // as an instance override: no lighting, whatever the material says
    Material = 3,  // as an instance override: the material's own light (the default)
};

// The handheld shading model (docs/api.md §2), not PBR. shading, color and palette_index are
// baked into a mesh's vertices when the mesh is built; the rest is read at every draw, so
// update_material() changes them for meshes that already exist.
enum class Shading : uint8_t { Flat, Smooth, Unlit };  // face normals / averaged normals / no lighting
enum class ColorSource : uint8_t { Palette, Vertex };  // texture: with the texture work
enum class AlphaMode : uint8_t { Opaque, Blend };      // cutout: with textures

struct Material {
    Shading shading = Shading::Flat;
    ColorSource color = ColorSource::Palette;
    uint8_t palette_index = 0;  // non-zero: the whole part in this colour (else the mesh's per-triangle indices)
    Light light = Light::Sun;   // Sun or Character
    float ambient = -1, diffuse = -1;  // negative: the light's own values
    float emissive = 0;         // 0..1: blend from lit towards the unlit colour
    AlphaMode alpha_mode = AlphaMode::Opaque;
    float opacity = 1;          // Blend only
    bool double_sided = false;
    bool fog = true, ground_fog = true;
};

// A run of triangles drawn with one material.
struct Submesh {
    uint32_t first_tri = 0, tri_count = 0;
    uint32_t material = 0;  // 0: the default material (below)
};

// Mesh geometry: positions and triangles, and the colours the materials can use: a palette
// index per triangle and/or RGB per vertex. Without submeshes the whole mesh is one part with
// the default material, which picks from what the mesh has: tri_palette -> flat, palette colours;
// vertex_rgb -> smooth, vertex colours.
struct MeshDesc {
    std::span<const float> positions;     // xyz, metres
    std::span<const uint16_t> indices;    // triangles, counter-clockwise front faces
    std::span<const uint8_t> tri_palette;
    std::span<const uint8_t> vertex_rgb;
    bool double_sided = false;            // all parts double-sided, whatever their materials say
    std::span<const Submesh> submeshes = {};  // empty: one part, default material
};

struct Instance {
    uint32_t mesh = 0;
    std::array<float, 12> model{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};  // row-major 3x4: p' = R p + t
    uint8_t tint = 0;        // palette index replacing the palette colours; 0 keeps them
    Light light = Light::Material;
    bool ground_fog = true;  // false: never sinks into the ground fog (hit flashes), whatever the material says
    float fade = 0;          // 0..1 towards the fog colour; 1 is exactly the fog colour
};

struct DirLight {
    std::array<float, 3> travel{0.35f, -0.82f, 0.45f};  // direction the light travels (away from the source); any length
    float ambient = 0.5f, diffuse = 0.5f;
};

struct HudCmd {
    enum Kind : uint8_t { Rect, Text } kind = Rect;
    float x = 0, y = 0, w = 0, h = 0;  // scene pixels, top-left origin; text: w = scale
    uint32_t color = 0xFFFFFFFF;       // 0xRRGGBBAA
    uint32_t shadow = 0;               // text: drop shadow 1 scaled pixel down-right (alpha 0 = none)
    std::string text;
};

struct FrameDesc {
    // Camera: yaw 0 faces +Z, positive pitch looks up; fov is horizontal.
    std::array<float, 3> eye{0, 2, 0};
    float yaw = 0, pitch = 0, fov_x = 1.309f, near_plane = 0.5f, far_plane = 120;
    // Sky: horizon colour at the horizon, top colour sky_span radians above it (0xRRGGBB).
    uint32_t sky_top = 0x4060A0, sky_horizon = 0xA0B0C0;
    float sky_span = 0.55f;
    // Distance fog by view depth: none before fog_near, full at fog_full, fog_steps bands (0: no fog).
    uint32_t fog_color = 0xA0B0C0;
    float fog_near = 40, fog_full = 110;
    int fog_steps = 16;
    // Ground fog: amount * low^2 * min(1, distance / ground_ramp), low = clamp(1 - y / ground_top).
    float ground_fog = 0, ground_top = 9, ground_ramp = 30;
    DirLight sun, character;
    uint32_t overlay = 0;  // 0xRRGGBBAA full-screen tint after the meshes, under the HUD
};

struct PipelineStats {
    uint32_t instances = 0, culled = 0, triangles = 0, draws = 0;
};

// The palette before a game sets its own (docs/api.md §1.2): 0 black, 1 white, then greys, the hues,
// earth, foliage and sky tones. A game with a look of its own replaces it with set_palette.
inline constexpr uint8_t kDefaultPalette[32 * 3] = {
    0, 0, 0,        255, 255, 255,  200, 200, 200,  140, 140, 140,  80, 80, 80,     35, 35, 40,     // 0-5: black, white, greys
    220, 60, 50,    240, 140, 40,   245, 210, 70,   120, 200, 70,   50, 140, 70,    60, 190, 190,   // 6-11: red, orange, yellow, lime, green, teal
    70, 150, 230,   50, 70, 170,    150, 80, 200,   230, 90, 170,   250, 170, 180,  130, 80, 50,    // 12-17: blue, navy, violet, magenta, pink, brown
    200, 160, 110,  240, 225, 190,  90, 120, 60,    60, 85, 45,     150, 140, 130,  100, 95, 105,   // 18-23: tan, cream, grass, forest, stone, slate
    170, 210, 240,  110, 150, 200,  255, 240, 150,  255, 120, 90,   120, 240, 255,  200, 255, 140,  // 24-29: sky, dusk blue, glow yellow, coral, glow cyan, glow lime
    255, 160, 240,  250, 250, 250};                                                                // 30-31: glow pink, near-white

class HandheldPipeline {
public:
    static constexpr int kPaletteSize = 32;

    bool init(rhi::Device& dev, int width, int height, std::string* error);
    void shutdown(rhi::Device& dev);

    // Materials: ids start at 1; 0 is the default material (MeshDesc). A mesh's parts must name
    // materials that exist when the mesh is built.
    uint32_t create_material(const Material& m, std::string* error);
    bool update_material(uint32_t id, const Material& m, std::string* error);
    bool has_material(uint32_t id) const { return id >= 1 && id < materials_.size(); }

    // Meshes: ids start at 1. update_mesh replaces the geometry (dynamic meshes: ropes, trails).
    uint32_t create_mesh(rhi::Device& dev, const MeshDesc& desc, std::string* error);
    bool update_mesh(rhi::Device& dev, uint32_t id, const MeshDesc& desc, std::string* error);
    void destroy_mesh(rhi::Device& dev, uint32_t id);
    bool has_mesh(uint32_t id) const;

    void set_palette(rhi::Device& dev, std::span<const uint8_t> rgb);  // up to 32 RGB triples

    // Renders the scene target. Instances are frustum-culled by their bounding spheres. Opaque
    // instances of the same mesh with the same light and ground fog are drawn together, one
    // instanced draw per part (each keeps its own transform, tint and fade); translucent parts
    // are drawn one by one, back to front.
    void render(rhi::Device& dev, const FrameDesc& frame, std::span<const Instance> instances,
                std::span<const HudCmd> hud);

    rhi::TextureH color() const { return color_; }
    rhi::TargetH target() const { return target_; }
    int width() const { return w_; }
    int height() const { return h_; }
    const PipelineStats& stats() const { return stats_; }

private:
    struct Part {
        uint32_t first_vertex = 0, vertex_count = 0, material = 0;
    };
    struct Mesh {
        rhi::BufferH vb;
        uint32_t vertex_count = 0, capacity = 0;
        float center[3] = {0, 0, 0}, radius = 0;  // bounding sphere in mesh space
        bool double_sided = false;
        bool live = false;
        std::vector<Part> parts;
    };
    static bool check_material(const Material& m, std::string* error);
    bool build(rhi::Device& dev, Mesh& m, const MeshDesc& desc, std::string* error);
    void draw_hud(rhi::Device& dev, std::span<const HudCmd> hud);

    int w_ = 0, h_ = 0;
    rhi::TextureH color_, depth_, palette_, font_;
    rhi::TargetH target_;
    rhi::PipelineH sky_, mesh_, mesh_two_sided_, blend_, blend_two_sided_, overlay_, hud_;
    rhi::PipelineH instanced_, instanced_two_sided_;
    rhi::BufferH hud_vb_, instance_vb_;
    size_t instance_capacity_ = 0;
    size_t hud_capacity_ = 0;
    std::vector<Mesh> meshes_;  // index = id - 1
    std::vector<Material> materials_{Material{}};  // index = id; [0]: the default material
    PipelineStats stats_;
};

}  // namespace alia::render
