#include "render/final_pass.hpp"

#include <algorithm>

namespace alia::render {
namespace {

// One oversized triangle covers the viewport; no vertex buffer needed.
const char* kVs = R"(#version 300 es
layout(std140) uniform Params { mat2 u_uv_rot; vec4 u_src; };  // must match the fragment stage
out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));  // (0,0) (2,0) (0,2)
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    // Rotate around the centre of the image so the scene appears rotated clockwise on screen.
    v_uv = u_uv_rot * (p - 0.5) + 0.5;
}
)";

// Sharp bilinear: snap the lookup to texel centres except within half an output pixel of a
// texel edge, so a 1.5x scale has crisp texels with a 1-pixel blend instead of uneven widths.
// With an integer scale (nearest sampler) it reduces to plain nearest sampling.
const char* kFs = R"(#version 300 es
precision highp float;
layout(std140) uniform Params { mat2 u_uv_rot; vec4 u_src; };  // u_src: texture size, scale
uniform sampler2D u_tex0;
in vec2 v_uv;
out vec4 o_color;
void main() {
    vec2 texel = v_uv * u_src.xy;
    vec2 base = floor(texel - 0.5) + 0.5;
    vec2 frac_ = texel - base;                       // 0..1 across one texel step
    vec2 k = clamp((frac_ - 0.5) * u_src.z + 0.5, 0.0, 1.0);  // sharpen by the output scale
    o_color = texture(u_tex0, (base + k) / u_src.xy);
}
)";

}  // namespace

Rect fit_integer(int scene_w, int scene_h, int out_w, int out_h, int rotation, int* scale_out) {
    bool sideways = rotation == 90 || rotation == 270;
    int disp_w = sideways ? scene_h : scene_w;
    int disp_h = sideways ? scene_w : scene_h;
    int scale = std::max(1, std::min(out_w / disp_w, out_h / disp_h));
    if (scale_out) *scale_out = scale;
    Rect r{0, 0, disp_w * scale, disp_h * scale};
    r.x = (out_w - r.width) / 2;
    r.y = (out_h - r.height) / 2;
    return r;
}

Rect fit_any(int scene_w, int scene_h, int out_w, int out_h, int rotation) {
    bool sideways = rotation == 90 || rotation == 270;
    double disp_w = sideways ? scene_h : scene_w;
    double disp_h = sideways ? scene_w : scene_h;
    double scale = std::min(out_w / disp_w, out_h / disp_h);
    Rect r{0, 0, int(disp_w * scale + 0.5), int(disp_h * scale + 0.5)};
    r.x = (out_w - r.width) / 2;
    r.y = (out_h - r.height) / 2;
    return r;
}

bool FinalPass::init(rhi::Device& dev, std::string* error) {
    rhi::PipelineDesc d;
    d.vertex_source = kVs;
    d.fragment_source = kFs;
    d.name = "final";
    pipeline_ = dev.create_pipeline(d, error);
    return bool(pipeline_);
}

void FinalPass::draw(rhi::Device& dev, rhi::TextureH scene, int scene_w, int scene_h, rhi::TargetH out, int out_w,
                     int out_h, int rotation, Scaling scaling) {
    int iscale = 1;
    Rect r = fit_integer(scene_w, scene_h, out_w, out_h, rotation, &iscale);
    Rect any = fit_any(scene_w, scene_h, out_w, out_h, rotation);
    bool integer = scaling == Scaling::Integer || (scaling == Scaling::Auto && r == any);
    if (!integer) r = any;
    bool sideways = rotation == 90 || rotation == 270;
    float scale = float(r.width) / float(sideways ? scene_h : scene_w);
    // Screen position p (0..1 inside the rect) -> scene uv. Clockwise rotation of the image
    // means counter-clockwise rotation of the lookup. std140 mat2 = two vec4-aligned columns.
    float c = 1, s = 0;
    switch (rotation) {
    case 90: c = 0; s = 1; break;
    case 180: c = -1; s = 0; break;
    case 270: c = 0; s = -1; break;
    default: break;
    }
    // mat2 columns (c, s) and (-s, c), then u_src. For integer scaling the shader's
    // sharpening is pushed to infinity, i.e. nearest.
    float params[12] = {c, s, 0, 0, -s, c, 0, 0, float(scene_w), float(scene_h), integer ? 1e6f : scale, 0};

    rhi::PassDesc pass;
    pass.target = out;
    pass.width = out_w;
    pass.height = out_h;
    pass.color_load = rhi::LoadOp::Clear;  // black borders
    pass.depth_store = rhi::StoreOp::DontCare;
    pass.name = "final";
    dev.begin_pass(pass);
    dev.set_viewport({r.x, r.y, r.width, r.height});
    dev.set_pipeline(pipeline_);
    dev.set_texture(0, scene, integer ? rhi::Filter::Nearest : rhi::Filter::Linear);
    dev.set_uniforms(params, sizeof params);
    dev.draw(3);
    dev.end_pass();
}

}  // namespace alia::render
