// L6: the main loop (docs/architecture.md §2.1). Games implement Game; run() owns the loop:
// a fixed logic step, rendering once per vsync, input, quitting, stats. Where frames go comes
// from the platform, never from the game:
//
//   internal resolution   cart manifest [display] render ($ALIA_CART_DIR/cart.toml)
//   output size           the window the compositor gives (sway makes every cart fullscreen)
//   pacing                vsync (swap interval 1)
//   dev and test options  environment or common flags, the same for every game:
//
//     ALIA_HEADLESS=1         --headless          surfaceless EGL, no window; simulated clock
//     ALIA_FRAMES=N           --frames N          stop after N frames
//     ALIA_SCREENSHOT=PATH    --screenshot PATH   write the last frame (PNG) when stopping
//     ALIA_STATS=1            --stats             one stats line per second (docs/stats-protocol.md)
//     ALIA_INPUT_LOG=1        --input-log         log the gamepad mapping and presses
//     ALIA_NO_INTERPOLATE=1   --no-interpolate    draw the world as of the last step (alpha = 1)
//     ALIA_DISPLAY_SIZE=WxH   --size WxH          debug: window / headless output size
//     ALIA_DISPLAY_ROTATION=D --rotate D          debug: final pass rotation (0/90/180/270)
//     ALIA_RENDER=WxH         --render WxH        debug: internal resolution
//
// Flags win over the environment, which wins over the manifest. The system's launcher sets none of the
// ALIA_DISPLAY_* / ALIA_RENDER variables: they are for testing only.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "runtime/engine.hpp"

namespace alia::runtime {

struct RunOptions {
    Config engine;            // title, app id, sizes, rotation, headless
    double fixed_hz = 60;     // logic steps per second
    int max_steps = 6;        // per rendered frame; slower than this and the game slows down. A gap over
                              // 0.25 s (the cart was frozen by the system, or stalled) is dropped, not caught up
    int frames = 0;           // 0: until quit
    std::string screenshot;   // written when the loop ends
    bool stats = false;
    bool input_log = false;
    bool interpolate = true;  // headless runs never interpolate: the simulated clock is one step per frame
};

// Defaults, then the cart manifest, then the environment. title/app_id fall back to the
// manifest's [cart] title / id, then ALIA_CART_ID.
// default_app_id: the app id outside a cart (it names the save directory and the window class); a
// cart's manifest and the session's ALIA_CART_ID come first.
RunOptions options_from_environment(const std::string& default_title, const std::string& default_app_id = "pad.alia.engine");

// Takes the common flags above out of args (in place) and applies them; leaves the game's own
// arguments. False (and *error) for a malformed value.
bool apply_flags(RunOptions& opt, std::vector<std::string>& args, std::string* error);

// The fields run() reads from a cart manifest (the Alia Pad cart docs); a small reader for the
// `key = "value"` lines it needs, not a TOML parser.
struct Manifest {
    std::string id, title, render;  // render: "640x360"
    std::map<std::string, std::string> titles;  // [locale.<tag>] title (and the old title_zh as "zh")
    // The title for a language, as the Shell picks it (the Alia Pad cart docs): the exact tag, then the
    // language alone, then [cart] title.
    std::string title_for(const std::string& language) const;
};
bool read_manifest(const std::string& path, Manifest* out);

class Game {
public:
    virtual ~Game() = default;
    virtual void init(Engine&) {}
    virtual void fixed_update(Engine&, double dt) = 0;  // input: engine.input(); pressed is per step
    // Fill the draw list. alpha: 0..1, how far the display is between the previous logic step and the
    // last one; the world is drawn at it. 1 with the simulated clock (headless) or --no-interpolate.
    virtual void render(Engine&, double alpha) = 0;
    virtual void shutdown(Engine&) {}
};

// Opens the engine with opt.engine, then runs the loop until quit, SIGTERM or opt.frames.
// Exceptions from the game propagate (after shutdown() and close()).
bool run(Game& game, const RunOptions& opt, std::string* error);

// The loop alone, on an engine that is already open (the Python module opens its own).
void run_open(Engine& e, Game& game, const RunOptions& opt);

}  // namespace alia::runtime
