#include "render/test_scene.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace alia::render {
namespace {

struct Vertex {
    float x, y, z;
    uint8_t r, g, b, a;
    float u, v;
};

const char* kFlatVs = R"(#version 300 es
layout(std140) uniform Params { vec4 u_xform; };  // xy scale, zw offset (NDC)
layout(location = 0) in vec3 a_0;
layout(location = 1) in vec4 a_1;
out vec4 v_color;
void main() {
    gl_Position = vec4(a_0.xy * u_xform.xy + u_xform.zw, a_0.z, 1.0);
    v_color = a_1;
}
)";
const char* kFlatFs = R"(#version 300 es
precision mediump float;
in vec4 v_color;
out vec4 o_color;
void main() { o_color = v_color; }
)";
const char* kTexVs = R"(#version 300 es
layout(location = 0) in vec3 a_0;
layout(location = 2) in vec2 a_2;
out vec2 v_uv;
void main() { gl_Position = vec4(a_0, 1.0); v_uv = a_2; }
)";
const char* kTexFs = R"(#version 300 es
precision mediump float;
uniform sampler2D u_tex0;
in vec2 v_uv;
out vec4 o_color;
void main() { o_color = texture(u_tex0, v_uv); }
)";

// Pixel rect (top-left origin, in the 640x360 scene) -> two triangles in NDC.
// Positions are given in the 640x360 design space; sx/sy scale to the actual resolution
// (the 1-pixel frame stays 1 pixel by passing exact edges).
float g_sx = 1, g_sy = 1, g_w = TestScene::kWidth, g_h = TestScene::kHeight;
void quad(std::vector<Vertex>& out, float px, float py, float pw, float ph, uint8_t r, uint8_t g, uint8_t b,
          float z = 0.9f, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1) {
    auto nx = [](float x) { return x / g_w * 2 - 1; };
    auto ny = [](float y) { return 1 - y / g_h * 2; };
    float x0 = nx(px), x1 = nx(px + pw), y0 = ny(py), y1 = ny(py + ph);
    Vertex a{x0, y0, z, r, g, b, 255, u0, v1}, bb{x1, y0, z, r, g, b, 255, u1, v1};
    Vertex c{x1, y1, z, r, g, b, 255, u1, v0}, d{x0, y1, z, r, g, b, 255, u0, v0};
    out.insert(out.end(), {a, bb, c, a, c, d});
}

}  // namespace

bool TestScene::init(rhi::Device& dev, std::string* error, int width, int height) {
    w_ = width;
    h_ = height;
    g_w = float(width);
    g_h = float(height);
    g_sx = float(width) / kWidth;
    g_sy = float(height) / kHeight;
    color_ = dev.create_texture({w_, h_, rhi::Format::RGBA8, rhi::Filter::Nearest, nullptr});
    depth_ = dev.create_texture({w_, h_, rhi::Format::Depth24, rhi::Filter::Nearest, nullptr});
    target_ = dev.create_target({color_, depth_});

    uint8_t px[8 * 8 * 4];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            uint8_t v = ((x + y) & 1) ? 255 : 0;
            uint8_t* p = &px[(y * 8 + x) * 4];
            p[0] = v;
            p[1] = v;
            p[2] = v;
            p[3] = 255;
        }
    checker_ = dev.create_texture({8, 8, rhi::Format::RGBA8, rhi::Filter::Nearest, px});

    rhi::PipelineDesc flat;
    flat.vertex_source = kFlatVs;
    flat.fragment_source = kFlatFs;
    flat.attribs[0] = {0, rhi::AttribType::Float3, offsetof(Vertex, x)};
    flat.attribs[1] = {1, rhi::AttribType::UNorm8x4, offsetof(Vertex, r)};
    flat.attrib_count = 2;
    flat.stride = sizeof(Vertex);
    flat.depth_test = true;
    flat.depth_write = true;
    flat.name = "test-flat";
    flat_ = dev.create_pipeline(flat, error);
    if (!flat_) return false;

    rhi::PipelineDesc tex = flat;
    tex.vertex_source = kTexVs;
    tex.fragment_source = kTexFs;
    tex.attribs[1] = {2, rhi::AttribType::Float2, offsetof(Vertex, u)};
    tex.depth_test = false;
    tex.depth_write = false;
    tex.name = "test-textured";
    textured_ = dev.create_pipeline(tex, error);
    if (!textured_) return false;

    // Static geometry: background gradient, frame, F marker (flat); checker (textured, last 6).
    std::vector<Vertex> v;
    {  // gradient: top deep blue, bottom warm grey
        float x0 = -1, x1 = 1, y0 = -1, y1 = 1, z = 0.99f;
        Vertex bl{x0, y0, z, 90, 80, 70, 255, 0, 0}, br{x1, y0, z, 90, 80, 70, 255, 0, 0};
        Vertex tr{x1, y1, z, 20, 40, 110, 255, 0, 0}, tl{x0, y1, z, 20, 40, 110, 255, 0, 0};
        v.insert(v.end(), {bl, br, tr, bl, tr, tl});
    }
    const float W = g_w, H = g_h, sx = g_sx, sy = g_sy;
    quad(v, 0, 0, W, 1, 255, 255, 255);                   // frame (1 pixel at any resolution): top
    quad(v, 0, H - 1, W, 1, 255, 255, 255);               // bottom
    quad(v, 0, 0, 1, H, 255, 255, 255);                   // left
    quad(v, W - 1, 0, 1, H, 255, 255, 255);               // right
    quad(v, 80 * sx, 20 * sy, 6 * sx, 40 * sy, 255, 220, 0);   // F: stem
    quad(v, 80 * sx, 20 * sy, 24 * sx, 6 * sy, 255, 220, 0);   //    top bar
    quad(v, 80 * sx, 36 * sy, 16 * sx, 6 * sy, 255, 220, 0);   //    middle bar
    quad(v, 8 * sx, 8 * sy, 64 * sx, 64 * sy, 255, 255, 255, 0.5f, 0, 0, 1, 1);  // checker (8 cells)
    quads_ = dev.create_buffer({rhi::BufferUsage::Vertex, v.size() * sizeof(Vertex), v.data(), false});

    // Two triangles, rewritten each frame: a far one (z=0.6) and a near one (z=0.3).
    tris_ = dev.create_buffer({rhi::BufferUsage::Vertex, 6 * sizeof(Vertex), nullptr, true});
    return true;
}

void TestScene::render(rhi::Device& dev, float t) {
    Vertex tris[6];
    for (int k = 0; k < 2; k++) {
        float z = k == 0 ? 0.6f : 0.3f;
        float cx = k == 0 ? 0.15f : -0.05f, cy = k == 0 ? 0.05f : -0.1f;
        for (int i = 0; i < 3; i++) {
            float a = t * 0.8f * (k == 0 ? 1 : -1) + float(i) * 2.0943951f + 1.5707963f;
            uint8_t r = i == 0 ? 255 : 0, g = i == 1 ? 255 : 0, b = i == 2 ? 255 : 0;
            if (k == 0) r = g = b = 200;  // the far triangle is grey; the near RGB one must cover it
            tris[k * 3 + i] = {cx + 0.45f * std::cos(a) * 9 / 16, cy + 0.45f * std::sin(a), z, r, g, b, 255, 0, 0};
        }
    }
    dev.update_buffer(tris_, tris, sizeof tris);

    rhi::PassDesc pass;
    pass.target = target_;
    pass.clear_color[0] = 1;  // magenta: any pixel left uncovered shows up immediately
    pass.clear_color[2] = 1;
    pass.depth_store = rhi::StoreOp::DontCare;
    pass.name = "scene";
    dev.begin_pass(pass);

    const float identity[4] = {1, 1, 0, 0};
    dev.set_pipeline(flat_);
    dev.set_uniforms(identity, sizeof identity);
    dev.set_vertex_buffer(quads_);
    dev.draw(48);  // gradient (6) + frame (4 x 6) + F marker (3 x 6)
    dev.set_vertex_buffer(tris_);
    dev.draw(3, 3);  // near RGB triangle first: depth test must keep it in front
    dev.draw(3, 0);  // far grey triangle

    dev.set_pipeline(textured_);
    dev.set_texture(0, checker_);
    dev.set_vertex_buffer(quads_);
    dev.draw(6, 48);  // checker
    dev.end_pass();
}

}  // namespace alia::render
