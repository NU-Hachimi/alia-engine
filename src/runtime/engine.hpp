// L6: the engine as a game sees it. Owns the surface (window or headless), the GPU device
// and the handheld pipeline; each frame the game fills a draw list and calls end_frame().
//
//   Engine e; e.open(config, &err);
//   while (e.poll()) {
//       ... read e.input(), update the game ...
//       e.frame() = {...};  e.draw(instance) ...;  e.hud_text(...);
//       e.end_frame();      // render, final pass, present
//   }
//
// Games normally don't write this loop: runtime::run() (app.hpp) owns it, with a fixed logic
// step, and opens the engine from the cart manifest and the run options.
#pragma once

#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "audio/device.hpp"
#include "audio/mixer.hpp"
#include "core/image.hpp"
#include "runtime/actions.hpp"
#include "runtime/lens.hpp"
#include "platform/surface.hpp"
#include "render/final_pass.hpp"
#include "render/handheld.hpp"
#include "world/world.hpp"

namespace alia::runtime {

struct Config {
    std::string title = "alia-engine";
    std::string app_id = "pad.alia.engine";
    int width = 1280, height = 720;          // window (or headless output) size
    int internal_w = 640, internal_h = 360;  // scene resolution
    int rotation = 0;                        // final pass rotation, clockwise degrees
    bool headless = false;
    bool vsync = true;
    // The platform's rules (the Alia Pad SDK's alia.runtime; the Python module fills them in when the SDK is there):
    std::string keymap;              // the PC keyboard as a gamepad (platform/keymap.hpp); ALIA_KEYMAP overrides
    int audio_frames = 512;          // [audio] game_quantum: the buffer the output stream asks for
    std::string audio_role = "Game"; // [audio] role: the stream's role for the system's mixer
};

struct FrameTiming {
    double cpu_ms = 0;      // end_frame(), all of it
    double submit_ms = 0;   // end_frame() up to the swap: building and submitting the frame
    double present_ms = 0;  // the swap itself (waits for vsync in a window)
    double frame_ms = 0;    // between the last two end_frame() calls
};

class Engine {
public:
    Engine();
    ~Engine();
    bool open(const Config& cfg, std::string* error);
    void close();
    bool is_open() const { return bool(dev_); }

    // Events and input; false once the user asked to quit. With carry_pressed, button presses
    // add up across polls until clear_pressed() (the runtime clears them after each logic
    // step, so a press lands in exactly one step even when a frame runs zero or two steps).
    bool poll(bool carry_pressed = false);
    void clear_pressed() { input_.pressed = 0; }
    // Sound (docs/api.md §7): the mixer always exists; the device only in a window (headless runs,
    // tests and tools over adb stay silent and can mix() by hand).
    audio::Mixer& mixer() { return mixer_; }
    const audio::Device& audio_device() const { return audio_device_; }
    uint32_t load_sound(const std::string& path, std::string* error);  // a WAV, relative to the cart's content/
    uint32_t load_sound(const std::vector<uint8_t>& wav, std::string* error);
    // Named actions bound to buttons and sticks (actions.hpp); read them against input().
    ActionMap& actions() { return actions_; }
    // The gamepad's motors (low, high 0..1) for ms; false when it can't (see platform::Surface).
    bool rumble(float low, float high, int ms) { return surface_ && surface_->rumble(low, high, ms); }
    const platform::InputState& input() const { return input_; }
    // The handheld's camera, the lens (lens.hpp, docs/camera.md): updated in poll(), drawn by end_frame().
    Lens& lens() { return lens_; }
    void close_lens() { if (dev_) lens_.close(*dev_); }

    uint32_t create_material(const render::Material& m, std::string* error) { return pipeline_.create_material(m, error); }
    bool update_material(uint32_t id, const render::Material& m, std::string* error) {
        return pipeline_.update_material(id, m, error);
    }
    uint32_t create_mesh(const render::MeshDesc& desc, std::string* error);
    bool update_mesh(uint32_t id, const render::MeshDesc& desc, std::string* error);
    void destroy_mesh(uint32_t id);
    // Everything in a resource pack (docs/formats.md): materials first, then the meshes whose parts
    // name them; name -> id for each. Relative paths are taken from the cart's content directory
    // (asset::content_path). On an error no mesh is left behind (materials are: they are small and
    // have no destroy).
    bool load_pack(const std::string& path, std::map<std::string, uint32_t>* meshes,
                   std::map<std::string, uint32_t>* materials, std::string* error);
    // Every rig in a pack -> world rig ids by name (World::add_rig). Rigs name their meshes; spawning
    // resolves them against every mesh loaded by name so far (mesh_named).
    bool load_rigs(const std::string& path, std::map<std::string, uint32_t>* out, std::string* error);
    bool load_rigs(std::vector<uint8_t> pack_bytes, std::map<std::string, uint32_t>* out, std::string* error);
    uint32_t mesh_named(const std::string& name) const {
        auto it = mesh_names_.find(name);
        return it == mesh_names_.end() ? 0 : it->second;
    }
    bool load_meshes(const std::string& path, std::map<std::string, uint32_t>* out, std::string* error) {
        std::map<std::string, uint32_t> materials;
        return load_pack(path, out, &materials, error);
    }
    void set_palette(std::span<const uint8_t> rgb);

    render::FrameDesc& frame() { return frame_; }
    // The world (docs/api.md §3): drawn every end_frame(), interpolated by alpha (the runtime sets
    // it before render(); 1 = the current state, which is what tools without the runtime get).
    world::World& world() { return world_; }
    void set_alpha(float alpha) { alpha_ = alpha; }
    void draw(const render::Instance& in) { instances_.push_back(in); }
    void hud_rect(float x, float y, float w, float h, uint32_t color);
    void hud_text(float x, float y, std::string text, uint32_t color, float scale, uint32_t shadow);
    // The camera's image in a HUD box: fit false crops it to fill the box, true shows all of it, centred
    // (black while there is none); color multiplies it (0xRRGGBBAA).
    void hud_lens(float x, float y, float w, float h, bool fit, uint32_t color);

    // Photos (docs/camera.md): the lens's photo alone, or composited like a game's photo mode: the frame
    // shown when it was asked for, rendered again at width x height with the full-resolution photo in place
    // of the live camera image; scene false leaves the 3D world out (the photo fills the background), hud
    // false the HUD. Asynchronous: photo(id) until done.
    uint32_t take_photo(bool scene, bool hud, int width, int height);
    const Lens::Photo* photo(uint32_t id) const;

    // Textures (docs/textures.md): ids from 1; kLensTexture is the lens's image. A texture loading in
    // the background (load_texture_async) has ready false until poll() uploads it, or an error.
    static constexpr uint32_t kLensTexture = 0xFFFFFFFFu;
    struct TextureInfo {
        int width = 0, height = 0;
        bool linear = false, repeat = false, ready = false;  // repeat: tiles outside 0..1 (else the edge)
        std::string error;
    };
    // A file relative to the cart's content/ (asset::content_path), whole.
    static bool read_content(const std::string& path, std::vector<uint8_t>* out, std::string* error);
    uint32_t load_texture(const std::vector<uint8_t>& png_or_jpeg, int max_side, bool linear, std::string* error,
                          bool repeat = false);
    uint32_t load_texture_async(std::vector<uint8_t> png_or_jpeg, int max_side, bool linear, bool repeat = false);
    uint32_t create_texture(int width, int height, const uint8_t* rgba, bool linear, std::string* error, bool repeat = false);
    bool update_texture(uint32_t id, const uint8_t* rgba, size_t size, std::string* error);
    void destroy_texture(uint32_t id);
    const TextureInfo* texture_info(uint32_t id) const;
    size_t texture_bytes() const { return texture_bytes_; }  // RGBA8 on the GPU, all textures
    // A texture in a HUD box (the same rules as hud_lens); src: the part of it (0..1, top-left origin).
    // Nothing while it isn't loaded.
    void hud_image(uint32_t texture, float x, float y, float w, float h, bool fit, uint32_t color,
                   std::array<float, 4> src = {0, 0, 1, 1});
    void end_frame();

    // Waits until the GPU has finished everything submitted so far (benchmarks: a 1-pixel read-back).
    void finish();

    // Writes the last frame as PNG: the output (headless), or the scene target (windows, or
    // scene_only).
    bool screenshot(const std::string& png_path, bool scene_only, std::string* error);

    int scene_width() const { return pipeline_.width(); }
    int scene_height() const { return pipeline_.height(); }
    const render::PipelineStats& stats() const { return pipeline_.stats(); }
    const FrameTiming& timing() const { return timing_; }
    std::string describe() const;

private:
    Config cfg_;
    std::unique_ptr<platform::Surface> surface_;
    ActionMap actions_;
    audio::Mixer mixer_;
    audio::Device audio_device_;
    std::unique_ptr<rhi::Device> dev_;
    render::HandheldPipeline pipeline_;
    render::FinalPass final_;
    rhi::TextureH out_tex_;  // headless output
    rhi::TargetH out_target_;
    render::FrameDesc frame_;
    world::World world_;
    std::map<std::string, uint32_t> mesh_names_;  // every mesh loaded from a pack, by name
    float alpha_ = 1.0f;
    platform::InputState input_;
    Lens lens_;
    struct Tex {
        TextureInfo info;
        rhi::TextureH handle;
        std::future<std::pair<Image, std::string>> pending;  // load_texture_async: decoding on another thread (or why not)
    };
    std::map<uint32_t, Tex> textures_;
    uint32_t next_texture_ = 1;
    size_t texture_bytes_ = 0;
    void upload(uint32_t id, Tex& t, const Image& img);
    struct Composite {
        bool scene = true, hud = false, snapshot_pending = true;
        int width = 0, height = 0;
        render::FrameDesc frame;
        std::vector<render::Instance> instances;
        std::vector<render::HudCmd> hud_cmds;
        std::future<std::pair<Image, std::string>> decode;
        std::future<std::vector<uint8_t>> encode;
        Lens::Photo result;
    };
    std::map<uint32_t, Composite> composites_;
    void update_composites();
    void render_composite(Composite& c, const Image& photo);
    void push_image(render::HudCmd c, rhi::TextureH tex, int tw, int th, bool linear, bool fit);
    std::vector<render::Instance> instances_;
    std::vector<render::HudCmd> hud_;
    FrameTiming timing_;
    double last_end_ = 0;
};

}  // namespace alia::runtime
