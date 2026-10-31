// L1: where frames go. A Surface owns the GL ES context and, for windows, the window.
//
//   window   — SDL3 (Wayland or X11); has a default framebuffer that present() shows.
//   headless — surfaceless EGL: a context with no framebuffer at all. Render into an
//              offscreen target and read it back (benchmarks on the board, golden tests, CI).
//
// Later: a "hosted" surface where an embedding app owns the context and
// hands us a framebuffer to draw into (docs/architecture.md §4).
#pragma once

#include <memory>
#include <string>

#include "platform/input.hpp"

namespace alia::platform {

struct SurfaceDesc {
    int width = 1280;
    int height = 720;
    const char* title = "alia-engine";
    // Wayland app_id / X11 class: lets the compositor pick this window out (sway rules,
    // moving it to its own workspace on the board).
    const char* app_id = "pad.alia.engine";
    bool vsync = true;
};

class Surface {
public:
    virtual ~Surface() = default;

    // Size of the default framebuffer in pixels (headless: the size it was created with).
    virtual int width() const = 0;
    virtual int height() const = 0;
    // False for headless: there is no framebuffer 0 to draw into.
    virtual bool has_default_framebuffer() const = 0;
    // Pump events and refresh input(); false once the user asked to quit (window closed,
    // Esc on the keyboard).
    virtual bool poll_events() = 0;
    // Controller state from the last poll_events(); always empty for headless surfaces.
    virtual const InputState& input() const {
        static const InputState none;
        return none;
    }
    // Rumble the gamepad: low and high frequency motors 0..1 for `ms`. False when there is
    // nothing to rumble (headless, no gamepad, or a gamepad without motors: Alia Pad until the
    // motor hardware arrives; the Alia Pad cart docs).
    virtual bool rumble(float low, float high, int ms) { (void)low, (void)high, (void)ms; return false; }
    virtual void present() = 0;
    virtual std::string describe() const = 0;  // backend + GL renderer, for logs
};

std::unique_ptr<Surface> create_window_surface(const SurfaceDesc& desc, std::string* error);
std::unique_ptr<Surface> create_headless_surface(const SurfaceDesc& desc, std::string* error);

}  // namespace alia::platform
