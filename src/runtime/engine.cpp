#include "runtime/engine.hpp"
#include "runtime/saves.hpp"

#include <chrono>
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
    if (dev_) {
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
        uint32_t id = pipeline_.create_mesh(*dev_, {m.positions, m.indices, m.tri_palette, m.vertex_rgb, m.double_sided, subs}, error);
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

void Engine::end_frame() {
    const double t0 = now_ms();
    world_.collect(alpha_, instances_, frame_);
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
