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

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "audio/device.hpp"
#include "audio/mixer.hpp"
#include "runtime/actions.hpp"
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
    std::vector<render::Instance> instances_;
    std::vector<render::HudCmd> hud_;
    FrameTiming timing_;
    double last_end_ = 0;
};

}  // namespace alia::runtime
