#include "runtime/lens.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "alia/camera.h"
#include "core/log.hpp"

namespace alia::runtime {
namespace {

// The developing pass: one full-screen triangle into the image, the SDK's reference shader per pixel.
const char* kDevelopVs = R"(#version 300 es
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kDevelopHead = R"(#version 300 es
precision highp float;
precision highp int;
precision highp sampler2D;
layout(std140) uniform Params {
    ivec4 u_raw;   // xy: the raw frame's size; zw: where red sits in the 2x2 cell
    ivec4 u_out;   // xy: the image's size; z: turn; w: 1 = one colour per 2x2 cell (the image at half the frame or less)
    vec4 u_wb;     // xy: white balance (red, blue); z: black level 0..1
    vec4 u_ccm0, u_ccm1, u_ccm2;  // the colour matrix, rows
};
uniform sampler2D u_tex0;  // the raw frame, R8
uniform sampler2D u_tex1;  // colour shading, 16 x 12
uniform sampler2D u_tex2;  // tone curve, 1024 x 1
out vec4 o_color;
)";

const char* kDevelopGlsl =
#include "alia_camera_glsl.inc"
    ;

const char* kDevelopMain = R"(
void main() {
    // The image is an RHI render target: its top row is GL's last.
    vec2 uv = vec2(gl_FragCoord.x / float(u_out.x), 1.0 - gl_FragCoord.y / float(u_out.y));
    ivec2 p = alia_camera_pixel(uv, u_raw.xy, u_out.z);
    mat3 ccm = transpose(mat3(u_ccm0.xyz, u_ccm1.xyz, u_ccm2.xyz));
    vec3 c = u_out.w == 1 ? alia_camera_rgb_cell(u_tex0, u_raw.xy, u_raw.zw, u_wb.z, u_wb.xy, u_tex1, u_tex2, ccm, p / 2)
                          : alia_camera_rgb(u_tex0, u_raw.xy, u_raw.zw, u_wb.z, u_wb.xy, u_tex1, u_tex2, ccm, p);
    o_color = vec4(c, 1.0);
}
)";

constexpr size_t kPhotosKept = 8;

struct DevelopParams {
    int32_t raw[4];
    int32_t out[4];
    float wb[4];
    float ccm[3][4];
};

Lens::State state_of(uint32_t code) {
    switch (code) {
    case CAM_ERR_DENIED: return Lens::State::Denied;
    case CAM_ERR_REVOKED: return Lens::State::Revoked;
    case CAM_ERR_BUSY: return Lens::State::Busy;
    case CAM_ERR_UNAVAILABLE: return Lens::State::Unavailable;
    case CAM_ERR_SUSPENDED: return Lens::State::Suspended;
    default: return Lens::State::Failed;
    }
}

}  // namespace

struct Lens::Impl {
    alia_camera c;
    bool connected = false;
};

const char* Lens::name(State s) {
    switch (s) {
    case State::Closed: return "closed";
    case State::Starting: return "starting";
    case State::Pending: return "pending";
    case State::Running: return "running";
    case State::Paused: return "paused";
    case State::Denied: return "denied";
    case State::Revoked: return "revoked";
    case State::Busy: return "busy";
    case State::Unavailable: return "unavailable";
    case State::Suspended: return "suspended";
    case State::Failed: return "failed";
    }
    return "?";
}

Lens::Lens() : impl_(std::make_unique<Impl>()) {}
Lens::~Lens() {
    if (impl_->connected) alia_camera_close(&impl_->c);  // GL objects went with the device
}

void Lens::open(int width, int height, bool full_fov) {
    if (impl_->connected) return;  // already open (or opening): close() first to ask for another size
    want_w_ = std::max(width, 1), want_h_ = std::max(height, 1), want_full_ = full_fov;
    message_.clear();
    // ALIA_CAMERA_SOCKET: another service (the simulator's, tests); otherwise the SDK's default.
    const char* path = std::getenv("ALIA_CAMERA_SOCKET");
    int r = alia_camera_connect(&impl_->c, path && *path ? path : nullptr);
    if (r < 0) {
        state_ = State::Unavailable;
        message_ = std::string("no camera service: ") + std::strerror(-r);
        return;
    }
    impl_->connected = true;
    r = alia_camera_open(&impl_->c, uint32_t(want_w_), uint32_t(want_h_), full_fov);
    if (r < 0) {
        alia_camera_close(&impl_->c);
        impl_->connected = false;
        state_ = State::Unavailable;
        message_ = std::string("the camera service: ") + std::strerror(-r);
        return;
    }
    state_ = State::Starting;
}

void Lens::disconnect(rhi::Device& dev) {
    for (const Held& h : held_)
        if (h.fence) dev.destroy(h.fence);
    held_.clear();
    for (rhi::TextureH& t : raw_)
        if (t) dev.destroy(t);
    raw_.clear();
    for (size_t i = 0; i < mapped_.size(); i++)
        if (mapped_[i]) munmap(mapped_[i], mapped_size_[i]);
    mapped_.clear();
    mapped_size_.clear();
    if (impl_->connected) alia_camera_close(&impl_->c);
    impl_->connected = false;
    generation_ = 0;
    have_params_ = false;
    if (photo_in_flight_) finish_photo(photo_in_flight_, {}, "the camera closed");
}

void Lens::close(rhi::Device& dev) {
    disconnect(dev);
    for (rhi::TextureH* t : {&out_tex_, &shade_tex_, &tone_tex_, &upload_tex_})
        if (*t) dev.destroy(*t), *t = {};
    if (out_target_) dev.destroy(out_target_), out_target_ = {};
    if (develop_) dev.destroy(develop_), develop_ = {};
    have_image_ = false;
    out_w_ = out_h_ = 0;
    mode_.reset();
    frame_.reset();
    state_ = State::Closed;
    message_.clear();
}

void Lens::fail(State s, const std::string& message, rhi::Device& dev) {
    disconnect(dev);
    state_ = s;
    message_ = message;
    have_image_ = false;  // a stopped camera shows nothing (REVOKED: at once)
    ALIA_INFO("camera: %s%s%s", name(s), message.empty() ? "" : ": ", message.c_str());
}

bool Lens::rebuild(rhi::Device& dev) {
    alia_camera& c = impl_->c;
    const cam_mode& m = c.mode;
    for (const Held& h : held_)  // the client dropped its buffers: nothing to hand back
        if (h.fence) dev.destroy(h.fence);
    held_.clear();
    for (rhi::TextureH& t : raw_)
        if (t) dev.destroy(t);
    for (size_t i = 0; i < mapped_.size(); i++)
        if (mapped_[i]) munmap(mapped_[i], mapped_size_[i]);
    raw_.assign(m.nbuf, {});
    mapped_.assign(m.nbuf, nullptr);
    mapped_size_.assign(m.nbuf, 0);
    generation_ = c.mode_generation;

    Mode mode;
    mode.width = int(m.width), mode.height = int(m.height), mode.fps = int(m.fps), mode.turn = int(m.turn);
    mode.order.assign(m.order, strnlen(m.order, sizeof m.order));
    mode.fx = m.fx, mode.fy = m.fy, mode.cx = m.cx, mode.cy = m.cy;
    mode.k1 = m.k1, mode.k2 = m.k2, mode.p1 = m.p1, mode.p2 = m.p2, mode.k3 = m.k3;
    mode.measured = m.intrinsics_measured != 0;
    mode.line_time_ns = m.line_time_ns;
    mode_ = mode;

    // The image: the camera's aspect (after turn), just big enough to cover the size asked for, never
    // bigger than the frame itself (developing can only lose detail, not add it).
    const int tw = m.turn == 90 ? int(m.height) : int(m.width), th = m.turn == 90 ? int(m.width) : int(m.height);
    const double s = std::min(1.0, std::max(double(want_w_) / tw, double(want_h_) / th));
    const int w = std::max(1, int(std::lround(tw * s))), h = std::max(1, int(std::lround(th * s)));
    if (w != out_w_ || h != out_h_) {
        if (out_target_) dev.destroy(out_target_);
        if (out_tex_) dev.destroy(out_tex_);
        out_tex_ = dev.create_texture({w, h, rhi::Format::RGBA8, rhi::Filter::Linear, nullptr});
        out_target_ = dev.create_target({out_tex_, {}});
        out_w_ = w, out_h_ = h;
        have_image_ = false;
    }
    if (!develop_) {
        std::string src = std::string(kDevelopHead) + kDevelopGlsl + kDevelopMain, err;
        rhi::PipelineDesc d;
        d.vertex_source = kDevelopVs;
        d.fragment_source = src.c_str();
        d.name = "camera-develop";
        develop_ = dev.create_pipeline(d, &err);
        if (!develop_) {
            fail(State::Failed, "the developing shader: " + err, dev);
            return false;
        }
        shade_tex_ = dev.create_texture({16, 12, rhi::Format::RGBA16F, rhi::Filter::Linear, nullptr});
        tone_tex_ = dev.create_texture({1024, 1, rhi::Format::R16F, rhi::Filter::Linear, nullptr});
    }
    if (upload_tex_) dev.destroy(upload_tex_), upload_tex_ = {};
    import_ = Import::Unknown;  // decided by the first buffer (buffer())
    ALIA_INFO("camera: %ux%u %s at %u fps, turn %u, image %dx%d", m.width, m.height, mode.order.c_str(), m.fps, m.turn,
              out_w_, out_h_);
    return true;
}

// Buffer `index` ready to develop (its BUFFER may come in a later poll than the MODE): read in place
// when the GPU can (the handheld), otherwise mapped and copied into one texture per frame (simulators,
// some PC drivers). The raw frame's texture, or {}.
rhi::TextureH Lens::buffer(rhi::Device& dev, int index) {
    alia_camera& c = impl_->c;
    const cam_mode& m = c.mode;
    const size_t i = size_t(index);
    if (i >= raw_.size() || c.buffer_fd[i] < 0) return {};
    if (import_ != Import::Copy && !raw_[i]) {
        std::string why;
        raw_[i] = dev.import_dmabuf({c.buffer_fd[i], int(m.width), int(m.height), m.stride, m.offset, m.modifier}, &why);
        if (raw_[i]) {
            if (import_ == Import::Unknown) ALIA_INFO("camera: frames read in place (dmabuf)");
            import_ = Import::InPlace;
        } else {
            ALIA_INFO("camera: frames copied (the dmabuf import: %s)", why.c_str());
            for (rhi::TextureH& t : raw_)
                if (t) dev.destroy(t), t = {};
            import_ = Import::Copy;
        }
    }
    if (import_ == Import::InPlace) return raw_[i];
    if (!mapped_[i]) {
        const size_t size = size_t(m.offset) + size_t(m.stride) * m.height;
        void* p = mmap(nullptr, size, PROT_READ, MAP_SHARED, c.buffer_fd[i], 0);
        if (p == MAP_FAILED) {
            fail(State::Failed, "a camera buffer can be neither imported nor mapped", dev);
            return {};
        }
        mapped_[i] = static_cast<uint8_t*>(p), mapped_size_[i] = size;
    }
    if (!upload_tex_)
        upload_tex_ = dev.create_texture({int(m.width), int(m.height), rhi::Format::R8, rhi::Filter::Nearest, nullptr});
    dev.update_data_texture(upload_tex_, mapped_[i] + m.offset, m.stride);
    return upload_tex_;
}

void Lens::develop(rhi::Device& dev, int index, uint32_t params) {
    alia_camera& c = impl_->c;
    const cam_mode& m = c.mode;
    if (const cam_params* p = alia_camera_params_of(&c, &c.current); p && (!have_params_ || params != params_id_)) {
        float shade[12 * 16 * 4];
        for (int i = 0; i < 12 * 16; i++) {
            const float* s = &p->shading[0][0][0] + i * 3;
            shade[i * 4 + 0] = s[0], shade[i * 4 + 1] = s[1], shade[i * 4 + 2] = s[2], shade[i * 4 + 3] = 1;
        }
        dev.update_data_texture(shade_tex_, shade);
        dev.update_data_texture(tone_tex_, p->tone);
        params_id_ = params, have_params_ = true;
    }
    if (!have_params_) return;  // the protocol sends PARAMS first; nothing to develop with until then
    const cam_params* p = alia_camera_params_of(&c, &c.current);
    rhi::TextureH raw = buffer(dev, index);
    if (!raw) return;

    DevelopParams u{};
    u.raw[0] = int(m.width), u.raw[1] = int(m.height);
    const char* order = m.order;  // where red sits in the 2x2 cell
    u.raw[2] = std::strncmp(order, "GRBG", 4) == 0 || std::strncmp(order, "BGGR", 4) == 0 ? 1 : 0;
    u.raw[3] = std::strncmp(order, "GBRG", 4) == 0 || std::strncmp(order, "BGGR", 4) == 0 ? 1 : 0;
    u.out[0] = out_w_, u.out[1] = out_h_, u.out[2] = int(m.turn);
    // At half the frame's size or less every output pixel covers a whole Bayer cell: the SDK's cheaper path
    // (4 reads, no interpolation: less than half the GPU time; the handheld's 640x481 from 1640x1232).
    const int raw_w = m.turn == 90 ? int(m.height) : int(m.width);
    u.out[3] = out_w_ * 2 <= raw_w ? 1 : 0;
    u.wb[0] = c.current.wb_red, u.wb[1] = c.current.wb_blue, u.wb[2] = float(m.black_level) / 255.0f;
    static const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const float* ccm = p ? p->ccm : identity;
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) u.ccm[r][k] = ccm[r * 3 + k];

    rhi::PassDesc pass;
    pass.target = out_target_;
    pass.width = out_w_, pass.height = out_h_;
    pass.color_load = rhi::LoadOp::DontCare;  // every pixel is written
    pass.name = "camera";
    dev.begin_pass(pass);
    dev.set_pipeline(develop_);
    dev.set_texture(0, raw);
    dev.set_texture(1, shade_tex_);
    dev.set_texture(2, tone_tex_);
    dev.set_uniforms(&u, sizeof u);
    dev.draw(3);
    dev.end_pass();
    have_image_ = true;
}

void Lens::update(rhi::Device& dev) {
    if (!impl_->connected) return;
    alia_camera& c = impl_->c;
    // Buffers the GPU is done with go back first, so the camera can fill them again.
    for (auto it = held_.begin(); it != held_.end();) {
        if (!it->fence || dev.signaled(it->fence)) {
            if (it->fence) dev.destroy(it->fence);
            alia_camera_release(&c, it->index);
            it = held_.erase(it);
        } else {
            ++it;
        }
    }
    const int r = alia_camera_poll(&c);
    if (r == -EPROTO) return fail(state_of(c.error.code), c.error.message, dev);
    if (r < 0) return fail(State::Failed, r == -EPIPE ? "the camera service hung up" : std::strerror(-r), dev);
    if (c.error.code && c.error.request) {  // a photo's error: the stream goes on
        if (c.error.request == photo_in_flight_) finish_photo(photo_in_flight_, {}, c.error.message);
        c.error.code = 0;
    }
    if (c.photo.fd >= 0) {
        std::vector<uint8_t> jpeg(c.photo.size);
        size_t got = 0;
        while (got < jpeg.size()) {
            ssize_t n = pread(c.photo.fd, jpeg.data() + got, jpeg.size() - got, off_t(got));
            if (n <= 0) break;
            got += size_t(n);
        }
        ::close(c.photo.fd);
        c.photo.fd = -1;
        if (c.photo.request == photo_in_flight_)
            got == jpeg.size() ? finish_photo(photo_in_flight_, std::move(jpeg), "")
                               : finish_photo(photo_in_flight_, {}, "the photo could not be read");
    }
    if (c.mode_generation && c.mode_generation != generation_ && !rebuild(dev)) return;
    if (generation_ && held_.size() < CAM_MAX_HELD) {
        if (const alia_camera_frame* f = alia_camera_next(&c)) {
            develop(dev, f->index, f->params);
            frame_ = Frame{f->sequence, f->timestamp_ns, f->exposure_us, f->gain, f->wb_red, f->wb_blue, f->colour_temperature};
            // A copied frame is free at once; one read in place only after the GPU has developed it.
            if (impl_->connected)
                held_.push_back({f->index, import_ == Import::InPlace ? dev.fence() : rhi::FenceH{}});
        }
    }
    state_ = c.pending ? State::Pending : c.paused ? State::Paused : have_image_ ? State::Running : State::Starting;
}

uint32_t Lens::take_photo() {
    const uint32_t id = next_photo_++;
    // The newest few are kept (a game reads its photo within a few frames); older finished ones go.
    while (photos_.size() >= kPhotosKept) {
        auto it = std::find_if(photos_.begin(), photos_.end(), [](const auto& e) { return e.second.done; });
        if (it == photos_.end()) break;
        photos_.erase(it);
    }
    photos_.push_back({id, {}});
    if (!impl_->connected || !generation_)
        finish_photo(id, {}, std::string("the camera is ") + name(state_));
    else if (photo_in_flight_)
        finish_photo(id, {}, "a photo is already being taken");
    else if (int r = alia_camera_photo(&impl_->c, id); r < 0)
        finish_photo(id, {}, std::strerror(-r));
    else
        photo_in_flight_ = id;
    return id;
}

void Lens::finish_photo(uint32_t id, std::vector<uint8_t> jpeg, std::string error) {
    if (id == photo_in_flight_) photo_in_flight_ = 0;
    for (auto& [pid, p] : photos_)
        if (pid == id) p = {true, std::move(jpeg), std::move(error)};
}

const Lens::Photo* Lens::photo(uint32_t id) const {
    for (const auto& [pid, p] : photos_)
        if (pid == id) return &p;
    return nullptr;
}

}  // namespace alia::runtime
