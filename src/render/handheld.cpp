#include "render/handheld.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "font5x7.h"

namespace alia::render {
namespace {

struct Vertex {
    float pos[3];
    float normal[3];
    uint8_t color[4];  // a = 0: palette index in r; a = 255: rgb
    float uv[2];       // textured parts
};

struct HudVertex {
    float x, y, u, v;
    uint8_t color[4];
};

// Shared by both stages of the mesh pipeline, so the precision must match: highp everywhere.
#define MESH_PARAMS R"(
layout(std140) uniform Params {
    mat4 u_view_proj;
    vec4 u_model[3];   // rows of the 3x4 model matrix
    vec4 u_eye;        // xyz: eye
    vec4 u_forward;    // xyz: view direction; w: tint palette index, -1 for none
    vec4 u_light;      // xyz: direction the light travels; w: ambient
    vec4 u_material;   // x: diffuse; y: fade; z: 1 = ground fog applies; w: emissive
    vec4 u_fog;        // x: near; y: full; z: steps (0: off)
    vec4 u_fog_color;
    vec4 u_ground;     // x: amount; y: top; z: ramp; w: opacity
};
)"

// The vertex stage, per draw (the model, tint and fade in Params) or instanced (in attributes 3-6:
// the model's rows, then x: tint palette index or -1, y: fade).
#define MESH_VS_BODY R"(
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec4 a_color;
#ifdef INSTANCED
layout(location = 3) in vec4 a_row0;
layout(location = 4) in vec4 a_row1;
layout(location = 5) in vec4 a_row2;
layout(location = 6) in vec4 a_extra;
#define ROW0 a_row0
#define ROW1 a_row1
#define ROW2 a_row2
#define TINT a_extra.x
#define FADE a_extra.y
#else
#define ROW0 u_model[0]
#define ROW1 u_model[1]
#define ROW2 u_model[2]
#define TINT u_forward.w
#define FADE u_material.y
#endif
uniform sampler2D u_tex0;  // palette, 32x1
out vec3 v_color;
out float v_depth;
#ifdef TEXTURED
layout(location = 7) in vec2 a_uv;
out vec2 v_uv;
out float v_shade;  // the lighting, applied per pixel to the texture's colour
out float v_k;      // fade and ground fog
#endif
void main() {
    vec4 p = vec4(a_pos, 1.0);
    vec3 w = vec3(dot(ROW0, p), dot(ROW1, p), dot(ROW2, p));
    vec3 n = vec3(dot(ROW0.xyz, a_normal), dot(ROW1.xyz, a_normal), dot(ROW2.xyz, a_normal));
    n = n / max(length(n), 1e-6);
    gl_Position = u_view_proj * vec4(w, 1.0);
    vec3 c;
#ifdef TEXTURED
    c = TINT >= 0.0 ? texelFetch(u_tex0, ivec2(int(TINT), 0), 0).rgb : a_color.rgb;  // what the texture is multiplied by
#else
    if (a_color.a < 0.5) {
        int index = TINT >= 0.0 ? int(TINT) : int(a_color.r * 255.0 + 0.5);
        c = texelFetch(u_tex0, ivec2(index, 0), 0).rgb;
    } else {
        c = a_color.rgb;
    }
#endif
    // Fade (and ground fog) blend towards the fog colour and flatten the lighting, so a
    // fade of 1 is exactly the fog colour.
    float k = FADE;
    if (u_material.z > 0.5 && u_ground.x > 0.0) {
        float low = clamp(1.0 - w.y / u_ground.y, 0.0, 1.0);
        float ramp = min(length(w - u_eye.xyz) / u_ground.z, 1.0);
        k = max(k, u_ground.x * low * low * ramp);
    }
    float ambient = u_light.w * (1.0 - k) + k;
    float diffuse = u_material.x * (1.0 - k);
    float shade = ambient + diffuse * max(0.0, -dot(n, u_light.xyz));
#ifdef TEXTURED
    v_color = c;
    v_uv = a_uv;
    v_shade = shade;
    v_k = k;
#else
    c = mix(c, u_fog_color.rgb, k);
    vec3 lit = min(c * shade, vec3(1.0));
    v_color = mix(lit, c, u_material.w);  // emissive: towards the unlit colour
#endif
    v_depth = dot(w - u_eye.xyz, u_forward.xyz);
}
)"

const char* kMeshVs = "#version 300 es\nprecision highp float;\n" MESH_PARAMS MESH_VS_BODY;
const char* kMeshInstancedVs = "#version 300 es\n#define INSTANCED 1\nprecision highp float;\n" MESH_PARAMS MESH_VS_BODY;
const char* kTexMeshVs = "#version 300 es\n#define TEXTURED 1\nprecision highp float;\n" MESH_PARAMS MESH_VS_BODY;
const char* kTexMeshInstancedVs =
    "#version 300 es\n#define TEXTURED 1\n#define INSTANCED 1\nprecision highp float;\n" MESH_PARAMS MESH_VS_BODY;

struct InstanceData {  // attributes 3-6 of the instanced mesh pipeline
    float model[12];
    float tint, fade, unused[2];
};

const char* kMeshFs = "#version 300 es\nprecision highp float;\n" MESH_PARAMS R"(
in vec3 v_color;
in float v_depth;
out vec4 o_color;
void main() {
    vec3 c = v_color;
    if (u_fog.z > 0.0) {
        float t = clamp((v_depth - u_fog.x) / max(u_fog.y - u_fog.x, 1e-3), 0.0, 1.0);
        t = floor(t * u_fog.z + 0.5) / u_fog.z;  // banded, not smooth
        c = mix(c, u_fog_color.rgb, t);
    }
    o_color = vec4(c, u_ground.w);
}
)";

// Textured parts: the texture (slot 1; uv's v from the top, RHI textures have their first row at v = 1) times
// the vertex stage's colour, then the same fade, lighting, emissive and fog as the untextured path.
// u_fog.w = 1: cutout (alpha below a half isn't drawn).
const char* kTexMeshFs = "#version 300 es\nprecision highp float;\n" MESH_PARAMS R"(
uniform sampler2D u_tex1;
in vec3 v_color;
in float v_depth;
in vec2 v_uv;
in float v_shade;
in float v_k;
out vec4 o_color;
void main() {
    vec4 t = texture(u_tex1, vec2(v_uv.x, 1.0 - v_uv.y));
    if (u_fog.w > 0.5 && t.a < 0.5) discard;
    vec3 c = mix(t.rgb * v_color, u_fog_color.rgb, v_k);
    c = mix(min(c * v_shade, vec3(1.0)), c, u_material.w);
    if (u_fog.z > 0.0) {
        float f = clamp((v_depth - u_fog.x) / max(u_fog.y - u_fog.x, 1e-3), 0.0, 1.0);
        f = floor(f * u_fog.z + 0.5) / u_fog.z;
        c = mix(c, u_fog_color.rgb, f);
    }
    o_color = vec4(c, u_ground.w * t.a);
}
)";

// Full-screen triangle; the sky and the overlay share it.
#define FULLSCREEN_VS R"(#version 300 es
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)"

const char* kSkyFs = R"(#version 300 es
precision highp float;
layout(std140) uniform Params {
    vec4 u_top;
    vec4 u_horizon;
    vec4 u_sky;  // x: camera pitch; y: focal length in pixels; z: span; w: half height
};
out vec4 o_color;
void main() {
    float elevation = u_sky.x + atan((gl_FragCoord.y - u_sky.w) / u_sky.y);
    o_color = vec4(mix(u_horizon.rgb, u_top.rgb, clamp(elevation / u_sky.z, 0.0, 1.0)), 1.0);
}
)";

const char* kOverlayFs = R"(#version 300 es
precision mediump float;
layout(std140) uniform Params { vec4 u_color; };
out vec4 o_color;
void main() { o_color = u_color; }
)";

// The camera's image behind the scene: q (0..1 over the scene, top-left origin) * scale + offset is
// where in the image; outside it, black (Fit's bars).
const char* kLensBgFs = R"(#version 300 es
precision highp float;
layout(std140) uniform Params { vec4 u_map; vec4 u_scene; };  // xy: scale, zw: offset; u_scene.xy: scene size
uniform sampler2D u_tex0;
out vec4 o_color;
void main() {
    vec2 q = vec2(gl_FragCoord.x, u_scene.y - gl_FragCoord.y) / u_scene.xy;
    vec2 uv = q * u_map.xy + u_map.zw;
    bool inside = all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
    o_color = inside ? vec4(texture(u_tex0, vec2(uv.x, 1.0 - uv.y)).rgb, 1.0) : vec4(0.0, 0.0, 0.0, 1.0);
}
)";

const char* kHudImageFs = R"(#version 300 es
precision mediump float;
uniform sampler2D u_tex0;  // an image (a texture, the camera's)
in vec2 v_uv;
in vec4 v_color;
out vec4 o_color;
void main() { o_color = texture(u_tex0, v_uv) * v_color; }
)";

const char* kHudVs = R"(#version 300 es
precision highp float;
layout(std140) uniform Params { vec4 u_size; };  // xy: scene size in pixels; zw: font atlas size
layout(location = 0) in vec4 a_pos_uv;
layout(location = 1) in vec4 a_color;
out vec2 v_uv;
out vec4 v_color;
void main() {
    gl_Position = vec4(a_pos_uv.x / u_size.x * 2.0 - 1.0, 1.0 - a_pos_uv.y / u_size.y * 2.0, 0.0, 1.0);
    v_uv = vec2(a_pos_uv.z, u_size.w - a_pos_uv.w) / u_size.zw;  // RHI textures: first row at v = 1
    v_color = a_color;
}
)";

const char* kHudFs = R"(#version 300 es
precision mediump float;
uniform sampler2D u_tex0;  // font atlas: white glyphs, coverage in alpha
in vec2 v_uv;
in vec4 v_color;
out vec4 o_color;
void main() { o_color = vec4(v_color.rgb, v_color.a * texture(u_tex0, v_uv).a); }
)";

// Font atlas: 16 x 6 cells of 6 x 8 texels; cell i holds character 0x20 + i, the last cell
// (index 95) is solid and used for rectangles.
constexpr int kCellW = 6, kCellH = 8, kAtlasCols = 16, kAtlasW = kCellW * 16, kAtlasH = kCellH * 6;
constexpr int kSolidCell = 95;

std::vector<uint8_t> font_atlas() {
    using namespace glcdfont;
    std::vector<uint8_t> px(size_t(kAtlasW) * kAtlasH * 4, 0);
    auto set = [&](int x, int y) {
        uint8_t* p = &px[(size_t(y) * kAtlasW + x) * 4];
        p[0] = p[1] = p[2] = p[3] = 255;
    };
    for (int ch = kFontFirst; ch <= kFontLast; ch++) {
        int cell = ch - kFontFirst, cx = (cell % kAtlasCols) * kCellW, cy = (cell / kAtlasCols) * kCellH;
        const uint8_t* g = &kFont5x7[cell * kFontW];
        for (int col = 0; col < kFontW; col++)
            for (int row = 0; row < kFontH; row++)
                if (g[col] & (1 << row)) set(cx + col, cy + row);
    }
    int sx = (kSolidCell % kAtlasCols) * kCellW, sy = (kSolidCell / kAtlasCols) * kCellH;
    for (int y = 0; y < kCellH; y++)
        for (int x = 0; x < kCellW; x++) set(sx + x, sy + y);
    return px;
}

void rgb(uint32_t c, float out[4], float a = 1) {
    out[0] = float((c >> 16) & 0xFF) / 255.0f;
    out[1] = float((c >> 8) & 0xFF) / 255.0f;
    out[2] = float(c & 0xFF) / 255.0f;
    out[3] = a;
}

void rgba(uint32_t c, uint8_t out[4]) {
    out[0] = uint8_t(c >> 24);
    out[1] = uint8_t(c >> 16);
    out[2] = uint8_t(c >> 8);
    out[3] = uint8_t(c);
}

}  // namespace

bool HandheldPipeline::init(rhi::Device& dev, int width, int height, std::string* error) {
    w_ = width;
    h_ = height;
    color_ = dev.create_texture({w_, h_, rhi::Format::RGBA8, rhi::Filter::Nearest, nullptr});
    depth_ = dev.create_texture({w_, h_, rhi::Format::Depth24, rhi::Filter::Nearest, nullptr});
    target_ = dev.create_target({color_, depth_});
    // Until the game sets its own (set_palette), a usable default (handheld.hpp kDefaultPalette).
    std::vector<uint8_t> first(kPaletteSize * 4, 255);
    for (int i = 0; i < kPaletteSize; i++)
        for (int c = 0; c < 3; c++) first[size_t(i) * 4 + size_t(c)] = kDefaultPalette[i * 3 + c];
    palette_ = dev.create_texture({kPaletteSize, 1, rhi::Format::RGBA8, rhi::Filter::Nearest, first.data()});
    std::vector<uint8_t> atlas = font_atlas();
    font_ = dev.create_texture({kAtlasW, kAtlasH, rhi::Format::RGBA8, rhi::Filter::Nearest, atlas.data()});

    rhi::PipelineDesc mesh;
    mesh.vertex_source = kMeshVs;
    mesh.fragment_source = kMeshFs;
    mesh.attribs[0] = {0, rhi::AttribType::Float3, offsetof(Vertex, pos)};
    mesh.attribs[1] = {1, rhi::AttribType::Float3, offsetof(Vertex, normal)};
    mesh.attribs[2] = {2, rhi::AttribType::UNorm8x4, offsetof(Vertex, color)};
    mesh.attrib_count = 3;
    mesh.stride = sizeof(Vertex);
    mesh.depth_test = mesh.depth_write = true;
    mesh.cull_back = true;
    mesh.name = "mesh";
    mesh_ = dev.create_pipeline(mesh, error);
    mesh.cull_back = false;
    mesh.name = "mesh-two-sided";
    if (mesh_) mesh_two_sided_ = dev.create_pipeline(mesh, error);
    // Translucent parts: drawn after everything opaque, back to front, testing depth but not writing it.
    mesh.alpha_blend = true;
    mesh.depth_write = false;
    mesh.name = "mesh-blend-two-sided";
    if (mesh_two_sided_) blend_two_sided_ = dev.create_pipeline(mesh, error);
    mesh.cull_back = true;
    mesh.name = "mesh-blend";
    if (blend_two_sided_) blend_ = dev.create_pipeline(mesh, error);

    rhi::PipelineDesc inst = mesh;  // opaque only: translucent parts are sorted one by one
    inst.vertex_source = kMeshInstancedVs;
    inst.alpha_blend = false;
    inst.depth_write = true;
    for (int i = 0; i < 4; i++)
        inst.attribs[3 + i] = {uint32_t(3 + i), rhi::AttribType::Float4, uint32_t(16 * i), true};
    inst.attrib_count = 7;
    inst.instance_stride = sizeof(InstanceData);
    inst.name = "mesh-instanced";
    if (blend_) instanced_ = dev.create_pipeline(inst, error);
    inst.cull_back = false;
    inst.name = "mesh-instanced-two-sided";
    if (instanced_) instanced_two_sided_ = dev.create_pipeline(inst, error);

    // The textured six: the same states, the TEXTURED shaders, a uv attribute (location 7).
    rhi::PipelineDesc tex = mesh;  // blend, culled (as left above): start from opaque
    tex.vertex_source = kTexMeshVs;
    tex.fragment_source = kTexMeshFs;
    tex.attribs[3] = {7, rhi::AttribType::Float2, offsetof(Vertex, uv)};
    tex.attrib_count = 4;
    tex.alpha_blend = false;
    tex.depth_write = true;
    tex.cull_back = true;
    tex.name = "tex-mesh";
    if (instanced_two_sided_) tex_mesh_ = dev.create_pipeline(tex, error);
    tex.cull_back = false;
    tex.name = "tex-mesh-two-sided";
    if (tex_mesh_) tex_mesh_two_sided_ = dev.create_pipeline(tex, error);
    tex.alpha_blend = true;
    tex.depth_write = false;
    tex.name = "tex-blend-two-sided";
    if (tex_mesh_two_sided_) tex_blend_two_sided_ = dev.create_pipeline(tex, error);
    tex.cull_back = true;
    tex.name = "tex-blend";
    if (tex_blend_two_sided_) tex_blend_ = dev.create_pipeline(tex, error);
    rhi::PipelineDesc tex_inst = inst;
    tex_inst.vertex_source = kTexMeshInstancedVs;
    tex_inst.fragment_source = kTexMeshFs;
    tex_inst.attribs[7] = {7, rhi::AttribType::Float2, offsetof(Vertex, uv)};
    tex_inst.attrib_count = 8;
    tex_inst.cull_back = true;
    tex_inst.name = "tex-instanced";
    if (tex_blend_) tex_instanced_ = dev.create_pipeline(tex_inst, error);
    tex_inst.cull_back = false;
    tex_inst.name = "tex-instanced-two-sided";
    if (tex_instanced_) tex_instanced_two_sided_ = dev.create_pipeline(tex_inst, error);
    const uint8_t magenta[4] = {255, 0, 255, 255}, black[4] = {0, 0, 0, 255};
    missing_ = dev.create_texture({1, 1, rhi::Format::RGBA8, rhi::Filter::Nearest, magenta});
    black_ = dev.create_texture({1, 1, rhi::Format::RGBA8, rhi::Filter::Nearest, black});

    rhi::PipelineDesc sky;
    sky.vertex_source = FULLSCREEN_VS;
    sky.fragment_source = kSkyFs;
    sky.name = "sky";
    if (tex_instanced_two_sided_) sky_ = dev.create_pipeline(sky, error);

    rhi::PipelineDesc overlay;
    overlay.vertex_source = FULLSCREEN_VS;
    overlay.fragment_source = kOverlayFs;
    overlay.alpha_blend = true;
    overlay.name = "overlay";
    if (sky_) overlay_ = dev.create_pipeline(overlay, error);

    rhi::PipelineDesc hud;
    hud.vertex_source = kHudVs;
    hud.fragment_source = kHudFs;
    hud.attribs[0] = {0, rhi::AttribType::Float4, offsetof(HudVertex, x)};
    hud.attribs[1] = {1, rhi::AttribType::UNorm8x4, offsetof(HudVertex, color)};
    hud.attrib_count = 2;
    hud.stride = sizeof(HudVertex);
    hud.alpha_blend = true;
    hud.name = "hud";
    if (overlay_) hud_ = dev.create_pipeline(hud, error);
    hud.fragment_source = kHudImageFs;
    hud.name = "hud-image";
    if (hud_) hud_image_ = dev.create_pipeline(hud, error);

    rhi::PipelineDesc camera_bg;
    camera_bg.vertex_source = FULLSCREEN_VS;
    camera_bg.fragment_source = kLensBgFs;
    camera_bg.name = "lens-background";
    if (hud_image_) lens_bg_ = dev.create_pipeline(camera_bg, error);
    return bool(lens_bg_);
}

void HandheldPipeline::shutdown(rhi::Device& dev) {
    for (Mesh& m : meshes_)
        if (m.vb) dev.destroy(m.vb);
    meshes_.clear();
    if (hud_vb_) dev.destroy(hud_vb_);
    if (instance_vb_) dev.destroy(instance_vb_);
    for (rhi::PipelineH p : {sky_, mesh_, mesh_two_sided_, blend_, blend_two_sided_, overlay_, hud_, instanced_,
                             instanced_two_sided_, lens_bg_, hud_image_, tex_mesh_, tex_mesh_two_sided_, tex_blend_,
                             tex_blend_two_sided_, tex_instanced_, tex_instanced_two_sided_})
        if (p) dev.destroy(p);
    lens_ = {};
    if (target_) dev.destroy(target_);
    bound_textures_.clear();
    for (rhi::TextureH t : {color_, depth_, palette_, font_, missing_, black_})
        if (t) dev.destroy(t);
    *this = HandheldPipeline{};
}

void HandheldPipeline::bind_texture(uint32_t id, rhi::TextureH texture, bool linear) {
    for (BoundTexture& b : bound_textures_)
        if (b.id == id) {
            b.texture = texture, b.linear = linear;
            return;
        }
    bound_textures_.push_back({id, texture, linear});
}

bool HandheldPipeline::check_material(const Material& m, std::string* error) {
    if (m.light != Light::Sun && m.light != Light::Character) {
        *error = "material: light must be sun or character (unlit is a shading)";
        return false;
    }
    if (m.palette_index >= kPaletteSize || m.emissive < 0 || m.emissive > 1 || m.opacity < 0 || m.opacity > 1) {
        *error = "material: palette_index 0..31, emissive and opacity 0..1";
        return false;
    }
    return true;
}

uint32_t HandheldPipeline::create_material(const Material& m, std::string* error) {
    if (!check_material(m, error)) return 0;
    for (uint32_t id = 1; id < materials_.size(); id++)  // a destroyed one's slot first
        if (!material_live_[id]) {
            materials_[id] = m;
            material_live_[id] = true;
            return id;
        }
    materials_.push_back(m);
    material_live_.push_back(true);
    return uint32_t(materials_.size() - 1);
}

bool HandheldPipeline::destroy_material(uint32_t id, std::string* error) {
    if (!has_material(id)) {
        *error = "material: unknown id (or the default material, 0)";
        return false;
    }
    for (size_t k = 0; k < meshes_.size(); k++)
        if (meshes_[k].live)
            for (const Part& part : meshes_[k].parts)
                if (part.material == id) {
                    *error = "material " + std::to_string(id) + ": mesh " + std::to_string(k + 1) + " still uses it (destroy the mesh first)";
                    return false;
                }
    materials_[id] = Material{};
    material_live_[id] = false;
    return true;
}

bool HandheldPipeline::update_material(uint32_t id, const Material& m, std::string* error) {
    if (!has_material(id)) {
        *error = "material: unknown id";
        return false;
    }
    if (!check_material(m, error)) return false;
    materials_[id] = m;  // shading, color and palette_index only reach meshes built after this
    return true;
}

bool HandheldPipeline::build(rhi::Device& dev, Mesh& m, const MeshDesc& d, std::string* error) {
    auto fail = [&](const std::string& what) {
        *error = "mesh: " + what;
        return false;
    };
    const size_t nverts = d.positions.size() / 3, ntris = d.indices.size() / 3;
    const bool has_palette = !d.tri_palette.empty(), has_rgb = !d.vertex_rgb.empty();
    if (d.positions.size() % 3 || d.indices.size() % 3 || ntris == 0)
        return fail("positions must be xyz triples and indices whole triangles");
    if (has_palette && d.tri_palette.size() != ntris) return fail("tri_palette needs one entry per triangle");
    if (has_rgb && d.vertex_rgb.size() != nverts * 3) return fail("vertex_rgb needs one RGB per vertex");
    const bool has_uv = !d.uv.empty();
    if (has_uv && d.uv.size() != nverts * 2) return fail("uv needs one u, v per vertex");
    for (uint16_t i : d.indices)
        if (i >= nverts) return fail("index out of range");

    std::vector<Submesh> subs(d.submeshes.begin(), d.submeshes.end());
    if (subs.empty()) subs.push_back({0, uint32_t(ntris), 0});
    // What each part is baked with: the default material picks from the mesh's colours.
    std::vector<Material> baked;
    for (const Submesh& sub : subs) {
        if (sub.tri_count == 0 || sub.first_tri > ntris || sub.tri_count > ntris - sub.first_tri)
            return fail("submesh outside the triangles");
        if (sub.material && !has_material(sub.material)) return fail("unknown material " + std::to_string(sub.material));
        Material mat = materials_[sub.material];
        if (sub.material == 0) {
            if (!has_palette && !has_rgb) return fail("give tri_palette or vertex_rgb (or submeshes with materials)");
            mat.shading = has_palette ? Shading::Flat : Shading::Smooth;
            mat.color = has_palette ? ColorSource::Palette : ColorSource::Vertex;
        }
        if (mat.color == ColorSource::Palette && !has_palette && mat.palette_index == 0)
            return fail("a palette material without palette_index needs the mesh's tri_palette");
        if (mat.color == ColorSource::Vertex && !has_rgb) return fail("a vertex-colour material needs the mesh's vertex_rgb");
        if (mat.color == ColorSource::Texture && !has_uv) return fail("a textured material needs the mesh's uv");
        baked.push_back(mat);
    }

    auto pos = [&](size_t i) { return glm::vec3(d.positions[3 * i], d.positions[3 * i + 1], d.positions[3 * i + 2]); };
    std::vector<glm::vec3> smooth(nverts, glm::vec3(0));  // area-weighted vertex normals
    for (size_t t = 0; t < ntris; t++) {
        const uint16_t* f = &d.indices[3 * t];
        glm::vec3 n = glm::cross(pos(f[1]) - pos(f[0]), pos(f[2]) - pos(f[0]));
        for (int k = 0; k < 3; k++) smooth[f[k]] += n;
    }
    std::vector<Vertex> verts;
    std::vector<Part> parts;
    for (size_t s_i = 0; s_i < subs.size(); s_i++) {
        const Submesh& sub = subs[s_i];
        const Material& mat = baked[s_i];
        parts.push_back({uint32_t(verts.size()), sub.tri_count * 3, sub.material});
        for (size_t t = sub.first_tri; t < sub.first_tri + sub.tri_count; t++) {
            const uint16_t* f = &d.indices[3 * t];
            glm::vec3 face = glm::cross(pos(f[1]) - pos(f[0]), pos(f[2]) - pos(f[0]));
            float len = glm::length(face);
            face = len > 1e-12f ? face / len : glm::vec3(0, 1, 0);
            for (int k = 0; k < 3; k++) {
                Vertex v{};
                glm::vec3 p = pos(f[k]), n = mat.shading == Shading::Smooth ? smooth[f[k]] : face;
                float nl = glm::length(n);
                n = nl > 1e-12f ? n / nl : glm::vec3(0, 1, 0);
                std::memcpy(v.pos, glm::value_ptr(p), sizeof v.pos);
                std::memcpy(v.normal, glm::value_ptr(n), sizeof v.normal);
                if (has_uv) std::memcpy(v.uv, &d.uv[size_t(f[k]) * 2], sizeof v.uv);
                if (mat.color == ColorSource::Texture) {
                    v.color[0] = v.color[1] = v.color[2] = v.color[3] = 255;  // the texture as it is (tints multiply)
                } else if (mat.color == ColorSource::Palette) {
                    v.color[0] = uint8_t((mat.palette_index ? mat.palette_index : d.tri_palette[t]) % kPaletteSize);
                } else {
                    std::memcpy(v.color, &d.vertex_rgb[size_t(f[k]) * 3], 3);
                    v.color[3] = 255;
                }
                verts.push_back(v);
            }
        }
    }
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (size_t i = 0; i < nverts; i++) {
        lo = glm::min(lo, pos(i));
        hi = glm::max(hi, pos(i));
    }
    glm::vec3 c = (lo + hi) * 0.5f;
    float r = 0;
    for (size_t i = 0; i < nverts; i++) r = std::max(r, glm::length(pos(i) - c));

    const size_t bytes = verts.size() * sizeof(Vertex);
    if (m.vb && verts.size() <= m.capacity) {
        dev.update_buffer(m.vb, verts.data(), bytes);
    } else {
        if (m.vb) dev.destroy(m.vb);
        // A replaced mesh is likely to be replaced again (ropes, trails): make it dynamic.
        m.vb = dev.create_buffer({rhi::BufferUsage::Vertex, bytes, verts.data(), m.live});
        m.capacity = uint32_t(verts.size());
    }
    m.vertex_count = uint32_t(verts.size());
    std::memcpy(m.center, glm::value_ptr(c), sizeof m.center);
    m.radius = r;
    m.double_sided = d.double_sided;
    m.parts = std::move(parts);
    m.live = true;
    return true;
}

uint32_t HandheldPipeline::create_mesh(rhi::Device& dev, const MeshDesc& desc, std::string* error) {
    Mesh m;
    if (!build(dev, m, desc, error)) return 0;
    meshes_.push_back(m);
    return uint32_t(meshes_.size());
}

bool HandheldPipeline::update_mesh(rhi::Device& dev, uint32_t id, const MeshDesc& desc, std::string* error) {
    if (!has_mesh(id)) {
        *error = "mesh: unknown id";
        return false;
    }
    return build(dev, meshes_[id - 1], desc, error);
}

void HandheldPipeline::destroy_mesh(rhi::Device& dev, uint32_t id) {
    if (!has_mesh(id)) return;
    Mesh& m = meshes_[id - 1];
    dev.destroy(m.vb);
    m = Mesh{};
}

bool HandheldPipeline::has_mesh(uint32_t id) const { return id >= 1 && id <= meshes_.size() && meshes_[id - 1].live; }

void HandheldPipeline::set_palette(rhi::Device& dev, std::span<const uint8_t> rgb_in) {
    std::vector<uint8_t> px(kPaletteSize * 4, 0);
    for (size_t i = 0; i < kPaletteSize && 3 * i + 2 < rgb_in.size(); i++) {
        std::memcpy(&px[4 * i], &rgb_in[3 * i], 3);
        px[4 * i + 3] = 255;
    }
    dev.update_texture(palette_, px.data());
}

void HandheldPipeline::render(rhi::Device& dev, const FrameDesc& f, std::span<const Instance> instances,
                              std::span<const HudCmd> hud) {
    render_into(dev, target_, w_, h_, f, instances, hud);
}

void HandheldPipeline::render_into(rhi::Device& dev, rhi::TargetH target, int W, int H, const FrameDesc& f,
                                   std::span<const Instance> instances, std::span<const HudCmd> hud) {
    stats_ = {};
    const float aspect = float(W) / float(H);
    const float fov_y = 2 * std::atan(std::tan(f.fov_x / 2) / aspect);
    const glm::vec3 eye(f.eye[0], f.eye[1], f.eye[2]);
    const glm::vec3 fwd(std::sin(f.yaw) * std::cos(f.pitch), std::sin(f.pitch), std::cos(f.yaw) * std::cos(f.pitch));
    const glm::mat4 view_proj =
        glm::perspective(fov_y, aspect, f.near_plane, f.far_plane) * glm::lookAt(eye, eye + fwd, glm::vec3(0, 1, 0));
    // Frustum planes (Gribb & Hartmann) for culling bounding spheres: dot(n, p) + d >= -r is visible.
    glm::vec4 planes[6];
    const glm::mat4 t = glm::transpose(view_proj);
    for (int i = 0; i < 3; i++) {
        planes[2 * i] = t[3] + t[i];
        planes[2 * i + 1] = t[3] - t[i];
    }
    for (glm::vec4& p : planes) p /= glm::length(glm::vec3(p));

    rhi::PassDesc pass;
    pass.target = target;
    pass.width = W;
    pass.height = H;
    pass.color_load = rhi::LoadOp::DontCare;  // the sky covers every pixel
    pass.depth_load = rhi::LoadOp::Clear;
    pass.depth_store = rhi::StoreOp::DontCare;
    pass.name = "scene";
    dev.begin_pass(pass);

    if (f.lens_background != LensBackground::Off) {
        // The image's size on the scene: Cover the smallest that fills it, Fit the largest that fits.
        float map[8] = {1, 1, -2, -2, float(W), float(H), 0, 0};  // no image: all outside, black
        if (lens_ && lens_w_ > 0 && lens_h_ > 0) {
            const float sx = float(W) / float(lens_w_), sy = float(H) / float(lens_h_);
            const float s = f.lens_background == LensBackground::Cover ? std::max(sx, sy) : std::min(sx, sy);
            map[0] = float(W) / (float(lens_w_) * s);
            map[1] = float(H) / (float(lens_h_) * s);
            map[2] = (1 - map[0]) / 2;
            map[3] = (1 - map[1]) / 2;
        }
        dev.set_pipeline(lens_bg_);
        if (lens_) dev.set_texture(0, lens_, rhi::Filter::Linear);
        else dev.set_texture(0, palette_);  // anything: nothing is sampled
        dev.set_uniforms(map, sizeof map);
        dev.draw(3);
    } else {
        float sky[12];
        rgb(f.sky_top, sky);
        rgb(f.sky_horizon, sky + 4);
        const float focal = (float(H) / 2) / std::tan(fov_y / 2);
        sky[8] = f.pitch;
        sky[9] = focal;
        sky[10] = std::max(f.sky_span, 1e-3f);
        sky[11] = float(H) / 2;
        dev.set_pipeline(sky_);
        dev.set_uniforms(sky, sizeof sky);
        dev.draw(3);
    }

    struct MeshParams {
        float view_proj[16];
        float model[12];
        float eye[4], forward[4], light[4], material[4], fog[4], fog_color[4], ground[4];
    } p{};
    std::memcpy(p.view_proj, glm::value_ptr(view_proj), sizeof p.view_proj);
    std::memcpy(p.eye, f.eye.data(), 3 * sizeof(float));
    std::memcpy(p.forward, glm::value_ptr(fwd), 3 * sizeof(float));
    p.fog[0] = f.fog_near;
    p.fog[1] = f.fog_full;
    p.fog[2] = float(std::max(f.fog_steps, 0));
    rgb(f.fog_color, p.fog_color);
    p.ground[0] = f.ground_fog;
    p.ground[1] = std::max(f.ground_top, 1e-3f);
    p.ground[2] = std::max(f.ground_ramp, 1e-3f);

    rhi::PipelineH bound;
    dev.set_texture(0, palette_);
    // A textured part's texture on slot 1 (magenta while it isn't there, black while bound to nothing); u_fog.w: cutout.
    auto bind_material_texture = [&](const Material& mat) {
        p.fog[3] = mat.alpha_mode == AlphaMode::Cutout ? 1.0f : 0.0f;
        if (mat.color != ColorSource::Texture) return;
        for (const BoundTexture& b : bound_textures_)
            if (b.id == mat.texture) {
                if (b.texture) dev.set_texture(1, b.texture, b.linear ? rhi::Filter::Linear : rhi::Filter::Nearest);
                else dev.set_texture(1, black_);
                return;
            }
        dev.set_texture(1, missing_);
    };
    auto draw_part = [&](const Mesh& m, const Part& part, const Instance& in, bool blend) {
        const Material& mat = materials_[part.material];
        const bool two_sided = m.double_sided || mat.double_sided;
        const bool textured = mat.color == ColorSource::Texture;
        rhi::PipelineH want = textured ? (blend ? (two_sided ? tex_blend_two_sided_ : tex_blend_) : (two_sided ? tex_mesh_two_sided_ : tex_mesh_))
                                       : (blend ? (two_sided ? blend_two_sided_ : blend_) : (two_sided ? mesh_two_sided_ : mesh_));
        if (want != bound) {
            dev.set_pipeline(want);
            bound = want;
        }
        bind_material_texture(mat);
        std::memcpy(p.model, in.model.data(), sizeof p.model);
        const Light light = in.light == Light::Material ? mat.light : in.light;
        const bool unlit = light == Light::Unlit || mat.shading == Shading::Unlit;
        const DirLight& l = light == Light::Character ? f.character : f.sun;
        std::memcpy(p.light, l.travel.data(), 3 * sizeof(float));
        p.light[3] = unlit ? 1.0f : (mat.ambient >= 0 ? mat.ambient : l.ambient);
        p.forward[3] = in.tint ? float(in.tint % kPaletteSize) : -1.0f;
        p.material[0] = unlit ? 0.0f : (mat.diffuse >= 0 ? mat.diffuse : l.diffuse);
        p.material[1] = std::clamp(in.fade, 0.0f, 1.0f);
        p.material[2] = in.ground_fog && mat.ground_fog ? 1.0f : 0.0f;
        p.material[3] = mat.emissive;
        p.fog[2] = mat.fog ? float(std::max(f.fog_steps, 0)) : 0.0f;
        p.ground[3] = blend ? mat.opacity : 1.0f;
        dev.set_uniforms(&p, sizeof p);
        dev.set_vertex_buffer(m.vb);
        dev.draw(part.vertex_count, part.first_vertex);
        stats_.triangles += part.vertex_count / 3;
        stats_.draws++;
    };
    struct Translucent {
        const Mesh* mesh;
        const Part* part;
        const Instance* instance;
        float depth;
    };
    std::vector<Translucent> translucent;
    // Opaque instances, grouped by what stays the same across one draw: mesh, light, ground fog.
    struct Group {
        uint32_t mesh;
        Light light;
        bool ground_fog;
        std::vector<const Instance*> members;
    };
    std::vector<Group> groups;
    std::unordered_map<uint64_t, size_t> group_of;
    for (const Instance& in : instances) {
        stats_.instances++;
        if (!has_mesh(in.mesh)) continue;
        const Mesh& m = meshes_[in.mesh - 1];
        const float* r = in.model.data();
        glm::vec3 c(r[0] * m.center[0] + r[1] * m.center[1] + r[2] * m.center[2] + r[3],
                    r[4] * m.center[0] + r[5] * m.center[1] + r[6] * m.center[2] + r[7],
                    r[8] * m.center[0] + r[9] * m.center[1] + r[10] * m.center[2] + r[11]);
        float scale = std::max({std::hypot(r[0], r[4], r[8]), std::hypot(r[1], r[5], r[9]), std::hypot(r[2], r[6], r[10])});
        float radius = m.radius * scale;
        bool visible = true;
        for (const glm::vec4& pl : planes) visible = visible && glm::dot(glm::vec3(pl), c) + pl.w >= -radius;
        if (!visible) {
            stats_.culled++;
            continue;
        }
        bool opaque = false;
        for (const Part& part : m.parts) {
            if (materials_[part.material].alpha_mode == AlphaMode::Blend)
                translucent.push_back({&m, &part, &in, glm::dot(c - eye, fwd)});
            else
                opaque = true;
        }
        if (!opaque) continue;
        const uint64_t key = uint64_t(in.mesh) << 8 | uint64_t(in.light) << 1 | uint64_t(in.ground_fog);
        auto [it, fresh] = group_of.try_emplace(key, groups.size());
        if (fresh) groups.push_back({in.mesh, in.light, in.ground_fog, {}});
        groups[it->second].members.push_back(&in);
    }
    // One instance: the per-draw path. More: their transforms, tints and fades into the instance
    // buffer (once for the frame), then one instanced draw per opaque part.
    std::vector<InstanceData> data;
    for (const Group& g : groups)
        if (g.members.size() > 1)
            for (const Instance* in : g.members) {
                InstanceData d{};
                std::memcpy(d.model, in->model.data(), sizeof d.model);
                d.tint = in->tint ? float(in->tint % kPaletteSize) : -1.0f;
                d.fade = std::clamp(in->fade, 0.0f, 1.0f);
                data.push_back(d);
            }
    if (!data.empty()) {
        const size_t bytes = data.size() * sizeof(InstanceData);
        if (bytes > instance_capacity_) {
            if (instance_vb_) dev.destroy(instance_vb_);
            instance_capacity_ = std::max(bytes, instance_capacity_ * 2);
            instance_vb_ = dev.create_buffer({rhi::BufferUsage::Vertex, instance_capacity_, nullptr, true});
        }
        dev.update_buffer(instance_vb_, data.data(), bytes);
    }
    size_t first = 0;
    for (const Group& g : groups) {
        const Mesh& m = meshes_[g.mesh - 1];
        if (g.members.size() == 1) {
            for (const Part& part : m.parts)
                if (materials_[part.material].alpha_mode != AlphaMode::Blend) draw_part(m, part, *g.members[0], false);
            continue;
        }
        for (const Part& part : m.parts) {
            const Material& mat = materials_[part.material];
            if (mat.alpha_mode == AlphaMode::Blend) continue;
            const bool two_sided = m.double_sided || mat.double_sided;
            rhi::PipelineH want = mat.color == ColorSource::Texture ? (two_sided ? tex_instanced_two_sided_ : tex_instanced_)
                                                                    : (two_sided ? instanced_two_sided_ : instanced_);
            if (want != bound) {
                dev.set_pipeline(want);
                bound = want;
            }
            bind_material_texture(mat);
            const Light light = g.light == Light::Material ? mat.light : g.light;
            const bool unlit = light == Light::Unlit || mat.shading == Shading::Unlit;
            const DirLight& l = light == Light::Character ? f.character : f.sun;
            std::memcpy(p.light, l.travel.data(), 3 * sizeof(float));
            p.light[3] = unlit ? 1.0f : (mat.ambient >= 0 ? mat.ambient : l.ambient);
            p.material[0] = unlit ? 0.0f : (mat.diffuse >= 0 ? mat.diffuse : l.diffuse);
            p.material[2] = g.ground_fog && mat.ground_fog ? 1.0f : 0.0f;
            p.material[3] = mat.emissive;
            p.fog[2] = mat.fog ? float(std::max(f.fog_steps, 0)) : 0.0f;
            p.ground[3] = 1.0f;
            dev.set_uniforms(&p, sizeof p);
            dev.set_vertex_buffer(m.vb);
            dev.set_instance_buffer(instance_vb_, first * sizeof(InstanceData));
            dev.draw_instanced(part.vertex_count, uint32_t(g.members.size()), part.first_vertex);
            stats_.triangles += part.vertex_count / 3 * uint32_t(g.members.size());
            stats_.draws++;
        }
        first += g.members.size();
    }
    // Back to front, by the instance's centre (a part of a mesh does not get its own depth).
    std::stable_sort(translucent.begin(), translucent.end(),
                     [](const Translucent& a, const Translucent& b) { return a.depth > b.depth; });
    for (const Translucent& t : translucent) draw_part(*t.mesh, *t.part, *t.instance, true);

    if (f.overlay & 0xFF) {
        float c[4];
        rgb(f.overlay >> 8, c, float(f.overlay & 0xFF) / 255.0f);
        dev.set_pipeline(overlay_);
        dev.set_uniforms(c, sizeof c);
        dev.draw(3);
    }
    draw_hud(dev, hud);
    dev.end_pass();
}

void HandheldPipeline::draw_hud(rhi::Device& dev, std::span<const HudCmd> hud) {
    std::vector<HudVertex> v;
    auto quad = [&](float x, float y, float w, float h, int cell, int u0, int v0, int uw, int vh, uint32_t color) {
        float cu = float((cell % kAtlasCols) * kCellW + u0), cv = float((cell / kAtlasCols) * kCellH + v0);
        HudVertex a{x, y, cu, cv, {}}, b{x + w, y, cu + uw, cv, {}}, c{x + w, y + h, cu + uw, cv + vh, {}},
            d{x, y + h, cu, cv + vh, {}};
        for (HudVertex* q : {&a, &b, &c, &d}) rgba(color, q->color);
        v.insert(v.end(), {a, b, c, a, c, d});
    };
    // Runs of quads drawn with one texture: the font atlas ({}), or an image (HudCmd::Image).
    struct Run {
        size_t first;
        rhi::TextureH texture;
        int w, h;
        bool linear;
    };
    std::vector<Run> runs;
    auto run = [&](rhi::TextureH t, int w, int h, bool linear) {
        if (runs.empty() || runs.back().texture != t || runs.back().linear != linear) runs.push_back({v.size(), t, w, h, linear});
    };
    using namespace glcdfont;
    for (const HudCmd& cmd : hud) {
        if (cmd.kind == HudCmd::Image) {
            if (!cmd.texture || cmd.tex_w <= 0 || cmd.tex_h <= 0) continue;
            run(cmd.texture, cmd.tex_w, cmd.tex_h, cmd.linear);
            // Texture pixels, top-left origin, the way quad() takes atlas cells (u_size.zw: the image's size).
            const float cw = float(cmd.tex_w), ch = float(cmd.tex_h);
            HudVertex a{cmd.x, cmd.y, cmd.u0 * cw, cmd.v0 * ch, {}}, b{cmd.x + cmd.w, cmd.y, cmd.u1 * cw, cmd.v0 * ch, {}},
                c{cmd.x + cmd.w, cmd.y + cmd.h, cmd.u1 * cw, cmd.v1 * ch, {}}, d{cmd.x, cmd.y + cmd.h, cmd.u0 * cw, cmd.v1 * ch, {}};
            for (HudVertex* q : {&a, &b, &c, &d}) rgba(cmd.color, q->color);
            v.insert(v.end(), {a, b, c, a, c, d});
            continue;
        }
        run({}, kAtlasW, kAtlasH, false);
        if (cmd.kind == HudCmd::Rect) {
            quad(cmd.x, cmd.y, cmd.w, cmd.h, kSolidCell, 1, 1, 1, 1, cmd.color);  // a texel well inside the solid cell
            continue;
        }
        const float s = std::max(cmd.w, 1.0f), advance = (kFontW + 1) * s;
        for (int pass = (cmd.shadow & 0xFF) ? 0 : 1; pass < 2; pass++) {
            float x = cmd.x + (pass ? 0 : s), y = cmd.y + (pass ? 0 : s);
            for (unsigned char ch : cmd.text) {
                if (ch >= kFontFirst && ch <= kFontLast && ch != ' ')
                    quad(x, y, kFontW * s, kFontH * s, ch - kFontFirst, 0, 0, kFontW, kFontH, pass ? cmd.color : cmd.shadow);
                x += advance;
            }
        }
    }
    if (v.empty()) return;
    const size_t bytes = v.size() * sizeof(HudVertex);
    if (bytes > hud_capacity_) {
        if (hud_vb_) dev.destroy(hud_vb_);
        hud_capacity_ = std::max(bytes, hud_capacity_ * 2);
        hud_vb_ = dev.create_buffer({rhi::BufferUsage::Vertex, hud_capacity_, nullptr, true});
    }
    dev.update_buffer(hud_vb_, v.data(), bytes);
    for (size_t i = 0; i < runs.size(); i++) {
        const size_t end = i + 1 < runs.size() ? runs[i + 1].first : v.size();
        if (end == runs[i].first) continue;
        const Run& r = runs[i];
        const float size[4] = {float(w_), float(h_), float(r.w), float(r.h)};
        dev.set_pipeline(r.texture ? hud_image_ : hud_);
        if (r.texture) dev.set_texture(0, r.texture, r.linear ? rhi::Filter::Linear : rhi::Filter::Nearest);
        else dev.set_texture(0, font_);
        dev.set_uniforms(size, sizeof size);
        dev.set_vertex_buffer(hud_vb_);
        dev.draw(uint32_t(end - runs[i].first), uint32_t(runs[i].first));
    }
}

}  // namespace alia::render
