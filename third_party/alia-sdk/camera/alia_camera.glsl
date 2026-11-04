// SPDX-License-Identifier: MIT
// Copyright (c) 2026 NU Hachimi (Alia Pad cart SDK)
//
// The reference shader for the camera's frames (protocol v2, the devkit's docs/camera.md): a cart
// that follows it gets the same colour as the handheld's own camera app.
// GLSL ES 3.00 (no #version line here: put yours first). Inputs, from the protocol:
//   raw       the frame's buffer as an R8 texture (MODE width x height), NEAREST
//   size      MODE width, height;  rpos: where red sits in the 2x2 cell (order "RGGB" -> 0,0;
//             "GRBG" -> 1,0; "GBRG" -> 0,1; "BGGR" -> 1,1)
//   black     MODE black_level / 255.0
//   wb        FRAME wb_red, wb_blue
//   shade     PARAMS shading as a 16x12 RGB float texture, LINEAR, CLAMP_TO_EDGE
//   tone_lut  PARAMS tone as a 1024x1 R float texture, LINEAR, CLAMP_TO_EDGE
//   ccm       PARAMS ccm (row-major in the message: upload transposed, or with transpose = GL_TRUE)
// Steps: bilinear demosaic; (v - black) / (1 - black); x white balance x colour shading (red,
// blue); clip to 1 (highlights stay white); x luminance shading; ccm (-> linear sRGB); clamp;
// tone (-> display-encoded). Scaling to your size happens here too: sample at your pixel's spot.

// Your output's uv (0..1, top-left origin) -> the raw pixel to develop: turned by MODE turn
// (0 or 90: clockwise from landscape screen-up, what the sensor didn't do).
ivec2 alia_camera_pixel(vec2 uv, ivec2 size, int turn) {
    vec2 t = turn == 90 ? vec2(uv.y, 1.0 - uv.x) : uv;
    return min(ivec2(t * vec2(size)), size - 1);
}

float alia_camera_px_(sampler2D raw, ivec2 size, ivec2 p) {
    return texelFetch(raw, clamp(p, ivec2(0), size - 1), 0).r;
}

// The display-ready colour of raw pixel p.
vec3 alia_camera_rgb(sampler2D raw, ivec2 size, ivec2 rpos, float black, vec2 wb, sampler2D shade,
                     sampler2D tone_lut, mat3 ccm, ivec2 p) {
    int sx = (p.x ^ rpos.x) & 1, sy = (p.y ^ rpos.y) & 1;
    float c = alia_camera_px_(raw, size, p);
    float l = alia_camera_px_(raw, size, p + ivec2(-1, 0)), r = alia_camera_px_(raw, size, p + ivec2(1, 0));
    float u = alia_camera_px_(raw, size, p + ivec2(0, -1)), d = alia_camera_px_(raw, size, p + ivec2(0, 1));
    float h = (l + r) * 0.5, v = (u + d) * 0.5, x4 = (l + r + u + d) * 0.25;
    float d4 = (alia_camera_px_(raw, size, p + ivec2(-1, -1)) + alia_camera_px_(raw, size, p + ivec2(1, -1)) +
                alia_camera_px_(raw, size, p + ivec2(-1, 1)) + alia_camera_px_(raw, size, p + ivec2(1, 1))) * 0.25;
    vec3 rgb = sx == 0 && sy == 0 ? vec3(c, x4, d4) : sx == 1 && sy == 1 ? vec3(d4, x4, c)
             : sy == 0 ? vec3(h, c, v) : vec3(v, c, h);
    vec3 sh = texture(shade, (vec2(p) + 0.5) / vec2(size)).rgb;
    rgb = min(max(rgb - black, 0.0) / (1.0 - black) * vec3(wb.x * sh.r, 1.0, wb.y * sh.b), 1.0);
    rgb = clamp(ccm * (rgb * sh.g), 0.0, 1.0) * (1023.0 / 1024.0) + 0.5 / 1024.0;
    return vec3(texture(tone_lut, vec2(rgb.r, 0.5)).r, texture(tone_lut, vec2(rgb.g, 0.5)).r,
                texture(tone_lut, vec2(rgb.b, 0.5)).r);
}
