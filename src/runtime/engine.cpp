#include "runtime/engine.hpp"
#include "runtime/saves.hpp"

#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstdio>

#include "asset/pack.hpp"
#include "core/image.hpp"
#include "core/log.hpp"

namespace alia::runtime {
namespace {

double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

namespace {  // textures and photos
constexpr int kMaxTextureSide = 2048;
constexpr int kMaxPhotoSide = 2048;
int clamp_side(int max_side) { return max_side <= 0 ? kMaxTextureSide : std::min(max_side, kMaxTextureSide); }
}  // namespace

Engine::Engine() = default;
Engine::~Engine() { close(); }

bool Engine::open(const Config& cfg, std::string* error) {
    close();
    cfg_ = cfg;
    saves::use_default(cfg_.app_id);  // off the device: a save directory of its own
    platform::SurfaceDesc sd;
    sd.width = cfg.width;
    sd.height = cfg.height;
    sd.title = cfg_.title.c_str();
    sd.app_id = cfg_.app_id.c_str();
    sd.vsync = cfg.vsync;
    sd.keymap = cfg_.keymap.c_str();
    surface_ = cfg.headless ? platform::create_headless_surface(sd, error) : platform::create_window_surface(sd, error);
    if (!surface_) return false;
    if (!cfg.headless) {
        std::string why;
        if (!audio_device_.open(mixer_, {cfg_.audio_frames, cfg_.audio_role}, &why)) ALIA_INFO("no sound: %s", why.c_str());
    }
    dev_ = rhi::create_gles_device();
    if (!pipeline_.init(*dev_, cfg.internal_w, cfg.internal_h, error) || !final_.init(*dev_, error)) {
        close();
        return false;
    }
    if (!surface_->has_default_framebuffer()) {
        out_tex_ = dev_->create_texture({cfg.width, cfg.height, rhi::Format::RGBA8, rhi::Filter::Nearest, nullptr});
        out_target_ = dev_->create_target({out_tex_, {}});
    }
    ALIA_INFO("%s", describe().c_str());
    return true;
}

uint32_t Engine::load_sound(const std::vector<uint8_t>& wav, std::string* error) {
    audio::Sound s;
    if (!audio::parse_wav(wav.data(), wav.size(), &s, error)) return 0;
    return mixer_.add_sound(std::move(s));
}

uint32_t Engine::load_sound(const std::string& path, std::string* error) {
    const std::string p = asset::content_path(path);
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) {
        *error = p + ": cannot open";
        return 0;
    }
    std::vector<uint8_t> bytes;
    uint8_t chunk[65536];
    for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) bytes.insert(bytes.end(), chunk, chunk + n);
    std::fclose(f);
    uint32_t id = load_sound(bytes, error);
    if (!id) *error = p + ": " + *error;
    return id;
}

void Engine::close() {
    audio_device_.close();
    for (auto& [id, t] : textures_)
        if (t.pending.valid()) t.pending.wait();  // a decode still running holds no engine state, but let it end
    if (dev_) {
        for (auto& [id, t] : textures_)
            if (t.handle) dev_->destroy(t.handle);
        lens_.shutdown(*dev_);
        pipeline_.shutdown(*dev_);
        if (out_target_) dev_->destroy(out_target_);
        if (out_tex_) dev_->destroy(out_tex_);
        out_target_ = {};
        out_tex_ = {};
    }
    final_ = {};
    dev_.reset();
    surface_.reset();
    instances_.clear();
    hud_.clear();
    input_ = {};
    world_ = world::World();  // the scene and the loaded rigs
    textures_.clear();
    texture_bytes_ = 0;
    for (auto& [id, c] : composites_) {
        if (c.decode.valid()) c.decode.wait();
        if (c.encode.valid()) c.encode.wait();
    }
    composites_.clear();
    mesh_names_.clear();
    alpha_ = 1.0f;
}

std::string Engine::describe() const {
    if (!dev_) return "closed";
    return surface_->describe() + " | " + dev_->describe() + " | scene " + std::to_string(pipeline_.width()) + "x" +
           std::to_string(pipeline_.height());
}

bool Engine::poll(bool carry_pressed) {
    if (!surface_ || !surface_->poll_events()) return false;
    const uint32_t carried = carry_pressed ? input_.pressed : 0;
    input_ = surface_->input();
    input_.pressed |= carried;
    lens_.update(*dev_);
    update_composites();
    for (auto& [id, t] : textures_) {  // background decodes that are done: onto the GPU
        if (!t.pending.valid() || t.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
        auto [img, err] = t.pending.get();
        if (img.empty()) t.info.error = err;
        else upload(id, t, img);
    }
    return true;
}

uint32_t Engine::create_mesh(const render::MeshDesc& desc, std::string* error) {
    return pipeline_.create_mesh(*dev_, desc, error);
}

bool Engine::update_mesh(uint32_t id, const render::MeshDesc& desc, std::string* error) {
    return pipeline_.update_mesh(*dev_, id, desc, error);
}

void Engine::destroy_mesh(uint32_t id) { pipeline_.destroy_mesh(*dev_, id); }

bool Engine::load_pack(const std::string& path, std::map<std::string, uint32_t>* meshes,
                       std::map<std::string, uint32_t>* materials, std::string* error) {
    asset::Pack pack;
    if (!pack.open(asset::content_path(path), error)) return false;
    std::map<std::string, uint32_t> mats, loaded;
    auto fail = [&](const std::string& name) {
        *error = path + ": " + name + ": " + *error;
        for (auto& [n, id] : loaded) pipeline_.destroy_mesh(*dev_, id);
        return false;
    };
    for (const asset::Entry& entry : pack.entries()) {
        if (entry.type != asset::EntryType::Material) continue;
        asset::MaterialData d;
        if (!pack.read_material(entry, &d, error)) return fail(entry.name);
        render::Material m{render::Shading(d.shading), render::ColorSource(d.color), d.palette_index,
                           render::Light(d.light),     d.ambient,                     d.diffuse,
                           d.emissive,                 render::AlphaMode(d.alpha_mode), d.opacity,
                           d.double_sided,             d.fog,                         d.ground_fog};
        uint32_t id = pipeline_.create_material(m, error);
        if (!id) return fail(entry.name);
        mats[entry.name] = id;
    }
    for (const asset::Entry& entry : pack.entries()) {
        if (entry.type != asset::EntryType::Mesh) continue;  // other types are for other loaders
        asset::MeshData m;
        if (!pack.read_mesh(entry, &m, error)) return fail(entry.name);
        std::vector<render::Submesh> subs;
        for (const asset::SubmeshData& sub : m.submeshes) {
            uint32_t material = 0;
            if (!sub.material.empty()) {
                auto it = mats.find(sub.material);
                if (it == mats.end()) {
                    *error = "no material " + sub.material + " in the pack";
                    return fail(entry.name);
                }
                material = it->second;
            }
            subs.push_back({sub.first_tri, sub.tri_count, material});
        }
        uint32_t id = pipeline_.create_mesh(*dev_, {m.positions, m.indices, m.tri_palette, m.vertex_rgb, m.double_sided, subs, m.uv}, error);
        if (!id) return fail(entry.name);
        loaded[entry.name] = id;
    }
    meshes->insert(loaded.begin(), loaded.end());
    materials->insert(mats.begin(), mats.end());
    for (auto& [name, id] : loaded) mesh_names_[name] = id;
    return true;
}

static bool add_rigs(world::World& w, const asset::Pack& pack, std::map<std::string, uint32_t>* out, std::string* error) {
    std::map<std::string, uint32_t> rigs;
    for (const asset::Entry& entry : pack.entries()) {
        if (entry.type != asset::EntryType::Rig) continue;
        asset::RigData r;
        if (!pack.read_rig(entry, &r, error)) return false;
        rigs[entry.name] = w.add_rig(std::move(r));
    }
    out->insert(rigs.begin(), rigs.end());
    return true;
}

bool Engine::load_rigs(const std::string& path, std::map<std::string, uint32_t>* out, std::string* error) {
    asset::Pack pack;
    if (!pack.open(asset::content_path(path), error)) return false;
    if (!add_rigs(world_, pack, out, error)) {
        *error = path + ": " + *error;
        return false;
    }
    return true;
}

bool Engine::load_rigs(std::vector<uint8_t> bytes, std::map<std::string, uint32_t>* out, std::string* error) {
    asset::Pack pack;
    return pack.load(std::move(bytes), error) && add_rigs(world_, pack, out, error);
}

void Engine::set_palette(std::span<const uint8_t> rgb) { pipeline_.set_palette(*dev_, rgb); }

void Engine::hud_rect(float x, float y, float w, float h, uint32_t color) {
    hud_.push_back({render::HudCmd::Rect, x, y, w, h, color, 0, {}});
}

void Engine::hud_text(float x, float y, std::string text, uint32_t color, float scale, uint32_t shadow) {
    hud_.push_back({render::HudCmd::Text, x, y, scale, 0, color, shadow, std::move(text)});
}

// An image in a box: fit shows all of it (the box shrunk to its aspect, centred), otherwise it's cropped
// to fill the box; c's u0..v1 is the part of the texture to start from.
void Engine::push_image(render::HudCmd c, rhi::TextureH tex, int tw, int th, bool linear, bool fit) {
    c.kind = render::HudCmd::Image;
    c.texture = tex, c.tex_w = tw, c.tex_h = th, c.linear = linear;
    const float iw = float(tw) * (c.u1 - c.u0), ih = float(th) * (c.v1 - c.v0);
    if (iw > 0 && ih > 0 && c.w > 0 && c.h > 0) {
        const float box = c.w / c.h, image = iw / ih;
        if (fit) {
            const float nw = image > box ? c.w : c.h * image, nh = image > box ? c.w / image : c.h;
            c.x += (c.w - nw) / 2, c.y += (c.h - nh) / 2, c.w = nw, c.h = nh;
        } else if (image > box) {  // crop the sides
            const float cut = (c.u1 - c.u0) * (1 - box / image) / 2;
            c.u0 += cut, c.u1 -= cut;
        } else {  // crop top and bottom
            const float cut = (c.v1 - c.v0) * (1 - image / box) / 2;
            c.v0 += cut, c.v1 -= cut;
        }
    }
    hud_.push_back(std::move(c));
}

void Engine::hud_lens(float x, float y, float w, float h, bool fit, uint32_t color) {
    if (!lens_.image()) {  // no image yet: a black box of the same size
        hud_rect(x, y, w, h, 0x000000FFu & (color | 0xFFFFFF00u));
        return;
    }
    render::HudCmd c{render::HudCmd::Image, x, y, w, h, color, 0, {}};
    push_image(std::move(c), lens_.image(), lens_.image_width(), lens_.image_height(), true, fit);
}

void Engine::hud_image(uint32_t id, float x, float y, float w, float h, bool fit, uint32_t color, std::array<float, 4> src) {
    if (id == kLensTexture) {
        if (src == std::array<float, 4>{0, 0, 1, 1}) return hud_lens(x, y, w, h, fit, color);
        if (!lens_.image()) return;
    }
    render::HudCmd c{render::HudCmd::Image, x, y, w, h, color, 0, {}};
    c.u0 = src[0], c.v0 = src[1], c.u1 = src[2], c.v1 = src[3];
    if (id == kLensTexture)
        return push_image(std::move(c), lens_.image(), lens_.image_width(), lens_.image_height(), true, fit);
    auto it = textures_.find(id);
    if (it == textures_.end() || !it->second.handle) return;
    const TextureInfo& i = it->second.info;
    push_image(std::move(c), it->second.handle, i.width, i.height, i.linear, fit);
}

void Engine::upload(uint32_t id, Tex& t, const Image& img) {
    if (t.handle) {
        dev_->destroy(t.handle);
        texture_bytes_ -= size_t(t.info.width) * t.info.height * 4;
    }
    t.handle = dev_->create_texture({img.width, img.height, rhi::Format::RGBA8,
                                     t.info.linear ? rhi::Filter::Linear : rhi::Filter::Nearest, img.rgba.data(),
                                     t.info.repeat ? rhi::Wrap::Repeat : rhi::Wrap::Clamp});
    t.info.width = img.width, t.info.height = img.height, t.info.ready = true, t.info.error.clear();
    texture_bytes_ += size_t(img.width) * img.height * 4;
    pipeline_.bind_texture(id, t.handle, t.info.linear);  // materials that name it
}


uint32_t Engine::take_photo(bool scene, bool hud, int width, int height) {
    const uint32_t id = lens_.take_photo();
    if (!scene && !hud) return id;  // the lens's photo as it is
    for (auto it = composites_.begin(); composites_.size() >= 8 && it != composites_.end();)  // the newest few kept
        it = it->second.result.done ? composites_.erase(it) : std::next(it);
    Composite& c = composites_[id];
    c.scene = scene, c.hud = hud;
    c.width = std::clamp(width, 16, kMaxPhotoSide), c.height = std::clamp(height, 16, kMaxPhotoSide);
    return id;
}

const Lens::Photo* Engine::photo(uint32_t id) const {
    auto it = composites_.find(id);
    if (it == composites_.end()) return lens_.photo(id);
    return &it->second.result;
}

void Engine::update_composites() {
    for (auto it = composites_.begin(); it != composites_.end(); ++it) {
        Composite& c = it->second;
        if (c.result.done) continue;
        if (c.encode.valid()) {
            if (c.encode.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
            c.result = {true, c.encode.get(), ""};
            if (c.result.jpeg.empty()) c.result.error = "the photo could not be encoded";
            continue;
        }
        if (c.decode.valid()) {
            if (c.snapshot_pending || c.decode.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
            auto [img, err] = c.decode.get();
            if (img.empty()) {
                c.result = {true, {}, "the camera's photo: " + err};
                continue;
            }
            render_composite(c, img);
            continue;
        }
        const Lens::Photo* p = lens_.photo(it->first);
        if (!p || !p->done) continue;
        if (p->jpeg.empty()) {
            c.result = {true, {}, p->error};
            continue;
        }
        // Decoded on another thread (a full-resolution photo: about a second on the handheld), just big
        // enough to cover the photo's size.
        c.decode = std::async(std::launch::async, [jpeg = p->jpeg, w = c.width, h = c.height]() {
            Image img;
            std::string err;
            if (!decode_image(jpeg.data(), jpeg.size(), &img, &err)) return std::pair{Image{}, err};
            const double s = std::max(double(w) / img.width, double(h) / img.height);
            const int side = int(std::ceil(std::max(img.width, img.height) * std::min(1.0, s)));
            return std::pair{shrink(img, side), std::string()};
        });
    }
}

void Engine::render_composite(Composite& c, const Image& photo) {
    rhi::TextureH ptex = dev_->create_texture({photo.width, photo.height, rhi::Format::RGBA8, rhi::Filter::Linear, photo.rgba.data()});
    rhi::TextureH color = dev_->create_texture({c.width, c.height, rhi::Format::RGBA8, rhi::Filter::Nearest, nullptr});
    rhi::TextureH depth = dev_->create_texture({c.width, c.height, rhi::Format::Depth24, rhi::Filter::Nearest, nullptr});
    rhi::TargetH target = dev_->create_target({color, depth});
    // The photo takes the live camera image's place everywhere the frame used it.
    const rhi::TextureH live = lens_.image();
    for (render::HudCmd& h : c.hud_cmds)
        if (h.kind == render::HudCmd::Image && live && h.texture == live) h.texture = ptex, h.tex_w = photo.width, h.tex_h = photo.height;
    render::FrameDesc f = c.frame;
    std::span<const render::Instance> instances = c.instances;
    if (!c.scene) {  // only the photo (and the HUD): it fills the background
        f.lens_background = render::LensBackground::Cover;
        f.overlay = 0;
        instances = {};
    }
    std::span<const render::HudCmd> hud = c.hud ? std::span<const render::HudCmd>(c.hud_cmds) : std::span<const render::HudCmd>();
    pipeline_.set_lens_image(ptex, photo.width, photo.height);
    pipeline_.bind_texture(kLensTexture, ptex, true);
    pipeline_.render_into(*dev_, target, c.width, c.height, f, instances, hud);
    pipeline_.set_lens_image(lens_.image(), lens_.image_width(), lens_.image_height());
    pipeline_.bind_texture(kLensTexture, lens_.image(), true);
    Image out;
    out.width = c.width, out.height = c.height;
    out.rgba.resize(size_t(c.width) * c.height * 4);
    dev_->read_pixels(target, c.width, c.height, out.rgba.data());
    dev_->destroy(target);
    for (rhi::TextureH t : {ptex, color, depth}) dev_->destroy(t);
    c.instances.clear(), c.hud_cmds.clear();
    c.encode = std::async(std::launch::async, [out = std::move(out)]() { return encode_jpeg(out, 92); });
}

bool Engine::read_content(const std::string& path, std::vector<uint8_t>* out, std::string* error) {
    const std::string p = asset::content_path(path);
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) {
        *error = p + ": cannot open";
        return false;
    }
    out->clear();
    uint8_t chunk[65536];
    for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) out->insert(out->end(), chunk, chunk + n);
    std::fclose(f);
    return true;
}

uint32_t Engine::load_texture(const std::vector<uint8_t>& bytes, int max_side, bool linear, std::string* error, bool repeat) {
    Image img;
    if (!decode_image(bytes.data(), bytes.size(), &img, error)) return 0;
    const uint32_t id = next_texture_++;
    Tex& t = textures_[id];
    t.info.linear = linear, t.info.repeat = repeat;
    upload(id, t, shrink(img, clamp_side(max_side)));
    return id;
}

uint32_t Engine::load_texture_async(std::vector<uint8_t> bytes, int max_side, bool linear, bool repeat) {
    const uint32_t id = next_texture_++;
    Tex& t = textures_[id];
    t.info.linear = linear, t.info.repeat = repeat;
    const int side = clamp_side(max_side);
    t.pending = std::async(std::launch::async, [bytes = std::move(bytes), side]() {
        Image img;
        std::string err;
        if (!decode_image(bytes.data(), bytes.size(), &img, &err)) return std::pair{Image{}, err};
        return std::pair{shrink(img, side), std::string()};
    });
    return id;
}

uint32_t Engine::create_texture(int width, int height, const uint8_t* rgba, bool linear, std::string* error, bool repeat) {
    if (width <= 0 || height <= 0 || width > kMaxTextureSide || height > kMaxTextureSide) {
        *error = "texture size: 1.." + std::to_string(kMaxTextureSide) + " on each side";
        return 0;
    }
    Image img;
    img.width = width, img.height = height;
    img.rgba.assign(rgba, rgba + size_t(width) * height * 4);
    const uint32_t id = next_texture_++;
    Tex& t = textures_[id];
    t.info.linear = linear, t.info.repeat = repeat;
    upload(id, t, img);
    return id;
}

bool Engine::update_texture(uint32_t id, const uint8_t* rgba, size_t size, std::string* error) {
    auto it = textures_.find(id);
    if (it == textures_.end() || !it->second.handle) {
        *error = "no such texture (or not loaded yet)";
        return false;
    }
    const TextureInfo& i = it->second.info;
    if (size != size_t(i.width) * i.height * 4) {
        *error = "update: width * height * 4 bytes of RGBA";
        return false;
    }
    dev_->update_texture(it->second.handle, rgba);
    return true;
}

bool Engine::update_texture_rect(uint32_t id, int x, int y, int w, int h, const uint8_t* rgba, size_t size,
                                 std::string* error) {
    auto it = textures_.find(id);
    if (it == textures_.end() || !it->second.handle) {
        *error = "no such texture (or not loaded yet)";
        return false;
    }
    const TextureInfo& i = it->second.info;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > i.width || y + h > i.height) {
        *error = "update: the rect must lie inside the texture";
        return false;
    }
    if (size != size_t(w) * h * 4) {
        *error = "update: w * h * 4 bytes of RGBA for the rect";
        return false;
    }
    dev_->update_texture_rect(it->second.handle, x, y, w, h, rgba);
    return true;
}

void Engine::destroy_texture(uint32_t id) {
    auto it = textures_.find(id);
    if (it == textures_.end()) return;
    if (it->second.pending.valid()) it->second.pending.wait();
    if (it->second.handle) {
        dev_->destroy(it->second.handle);
        texture_bytes_ -= size_t(it->second.info.width) * it->second.info.height * 4;
    }
    pipeline_.unbind_texture(id);  // materials that name it draw white
    textures_.erase(it);
}

const Engine::TextureInfo* Engine::texture_info(uint32_t id) const {
    auto it = textures_.find(id);
    return it == textures_.end() ? nullptr : &it->second.info;
}

void Engine::end_frame() {
    const double t0 = now_ms();
    world_.collect(alpha_, instances_, frame_);
    for (auto& [id, c] : composites_)  // photos asked for since the last frame: this frame is the one they show
        if (c.snapshot_pending) {
            c.frame = frame_, c.instances = instances_, c.hud_cmds = hud_;
            c.snapshot_pending = false;
        }
    pipeline_.set_lens_image(lens_.image(), lens_.image_width(), lens_.image_height());
    pipeline_.bind_texture(kLensTexture, lens_.image(), true);  // materials showing the camera
    pipeline_.render(*dev_, frame_, instances_, hud_);
    const bool window = surface_->has_default_framebuffer();
    const int ow = window ? surface_->width() : cfg_.width, oh = window ? surface_->height() : cfg_.height;
    final_.draw(*dev_, pipeline_.color(), pipeline_.width(), pipeline_.height(), out_target_, ow, oh, cfg_.rotation);
    const double t_swap = now_ms();
    surface_->present();
    timing_.present_ms = now_ms() - t_swap;
    timing_.submit_ms = t_swap - t0;
    dev_->take_stats();
    instances_.clear();
    hud_.clear();
    const double t1 = now_ms();
    timing_.cpu_ms = t1 - t0;
    timing_.frame_ms = last_end_ > 0 ? t1 - last_end_ : 0;
    last_end_ = t1;
}

void Engine::finish() {
    uint8_t px[4];
    dev_->read_pixels(out_target_ ? out_target_ : pipeline_.target(), 1, 1, px);
}

bool Engine::screenshot(const std::string& png_path, bool scene_only, std::string* error) {
    // A window's framebuffer cannot be read back after present, so windows give the scene.
    const bool output = !scene_only && out_target_;
    Image img;
    img.width = output ? cfg_.width : pipeline_.width();
    img.height = output ? cfg_.height : pipeline_.height();
    img.rgba.resize(size_t(img.width) * img.height * 4);
    dev_->read_pixels(output ? out_target_ : pipeline_.target(), img.width, img.height, img.rgba.data());
    if (!write_png(png_path, img)) {
        *error = "cannot write " + png_path;
        return false;
    }
    return true;
}

}  // namespace alia::runtime
