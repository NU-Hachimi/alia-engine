#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.hpp"
#include "platform/surface.hpp"

namespace alia::platform {
namespace {

struct KeyBinding {
    SDL_Keycode key;
    uint32_t button;
};
// input.hpp has the table in words.
constexpr KeyBinding kKeyButtons[] = {
    {SDLK_SPACE, kEast}, {SDLK_RETURN, kEast}, {SDLK_J, kSouth}, {SDLK_K, kNorth}, {SDLK_L, kWest},
    {SDLK_Q, kL1}, {SDLK_E, kR1}, {SDLK_F, kL3}, {SDLK_G, kR3}, {SDLK_TAB, kSelect}, {SDLK_BACKSPACE, kStart},
};

struct PadBinding {
    SDL_GamepadButton pad;
    uint32_t button;
};
constexpr PadBinding kPadButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, kSouth}, {SDL_GAMEPAD_BUTTON_EAST, kEast}, {SDL_GAMEPAD_BUTTON_WEST, kWest},
    {SDL_GAMEPAD_BUTTON_NORTH, kNorth}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kL1},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kR1}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, kL3},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kR3}, {SDL_GAMEPAD_BUTTON_BACK, kSelect}, {SDL_GAMEPAD_BUTTON_START, kStart},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, kUp}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight},
};

float axis(SDL_Gamepad* pad, SDL_GamepadAxis a) {
    return std::max(-1.0f, SDL_GetGamepadAxis(pad, a) / 32767.0f);
}

class SdlWindowSurface final : public Surface {
public:
    ~SdlWindowSurface() override {
        if (pad_) SDL_CloseGamepad(pad_);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        if (ctx_) SDL_GL_DestroyContext(ctx_);
        if (win_) SDL_DestroyWindow(win_);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    bool init(const SurfaceDesc& d, std::string* error) {
        SDL_SetHint(SDL_HINT_APP_ID, d.app_id);  // must be set before the video subsystem starts
        // On Alia Pad the system decides which app gets the gamepad, and
        // the compositor may not give our window keyboard focus: take gamepad input regardless.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        log_input_ = std::getenv("ALIA_INPUT_LOG") != nullptr;
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return fail(error, "SDL_Init");
        // GLES 3.0 everywhere: the same API and shaders on the PC as on the board's FD702.
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);  // the default framebuffer only gets the final pass
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        win_ = SDL_CreateWindow(d.title, d.width, d.height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
        if (!win_) return fail(error, "SDL_CreateWindow");
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) ALIA_WARN("no gamepad support: %s", SDL_GetError());
        ctx_ = SDL_GL_CreateContext(win_);
        if (!ctx_) return fail(error, "SDL_GL_CreateContext");
        SDL_GL_SetSwapInterval(d.vsync ? 1 : 0);
        return true;
    }

    int width() const override { int w, h; SDL_GetWindowSizeInPixels(win_, &w, &h); return w; }
    int height() const override { int w, h; SDL_GetWindowSizeInPixels(win_, &w, &h); return h; }
    bool has_default_framebuffer() const override { return true; }

    bool poll_events() override {
        uint32_t pressed = 0;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: return false;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_ESCAPE) return false;
                if (!e.key.repeat) pressed |= key_button(e.key.key);
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!pad_) {
                    pad_ = SDL_OpenGamepad(e.gdevice.which);
                    if (pad_) {
                        ALIA_INFO("gamepad: %s", SDL_GetGamepadName(pad_));
                        if (log_input_) {
                            char* mapping = SDL_GetGamepadMapping(pad_);
                            ALIA_INFO("gamepad mapping: %s", mapping ? mapping : "(none)");
                            SDL_free(mapping);
                        }
                    }
                }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (pad_ && SDL_GetGamepadID(pad_) == e.gdevice.which) {
                    SDL_CloseGamepad(pad_);
                    pad_ = nullptr;
                    ALIA_INFO("gamepad removed");
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                for (const PadBinding& b : kPadButtons)
                    if (b.pad == e.gbutton.button) pressed |= b.button;
                if (log_input_) ALIA_INFO("gamepad button %d down", int(e.gbutton.button));
                break;
            default: break;
            }
        }
        read_input(pressed);
        return true;
    }

    const InputState& input() const override { return input_; }

    void present() override { SDL_GL_SwapWindow(win_); }

    bool rumble(float low, float high, int ms) override {
        if (!pad_) return false;
        auto level = [](float v) { return Uint16(std::clamp(v, 0.0f, 1.0f) * 65535.0f); };
        return SDL_RumbleGamepad(pad_, level(low), level(high), Uint32(std::max(ms, 0)));
    }

    std::string describe() const override {
        return std::string("window (SDL ") + SDL_GetCurrentVideoDriver() + ")";
    }

private:
    static uint32_t key_button(SDL_Keycode k) {
        for (const KeyBinding& b : kKeyButtons)
            if (b.key == k) return b.button;
        return 0;
    }

    void read_input(uint32_t pressed) {
        InputState in;
        const bool* keys = SDL_GetKeyboardState(nullptr);
        auto key = [&](SDL_Scancode sc) { return keys[sc] ? 1.0f : 0.0f; };
        for (const KeyBinding& b : kKeyButtons)
            if (keys[SDL_GetScancodeFromKey(b.key, nullptr)]) in.held |= b.button;
        in.lx = key(SDL_SCANCODE_D) - key(SDL_SCANCODE_A);
        in.ly = key(SDL_SCANCODE_S) - key(SDL_SCANCODE_W);
        in.rx = key(SDL_SCANCODE_RIGHT) - key(SDL_SCANCODE_LEFT);
        in.ry = key(SDL_SCANCODE_DOWN) - key(SDL_SCANCODE_UP);
        if (pad_) {
            in.gamepad = true;
            for (const PadBinding& b : kPadButtons)
                if (SDL_GetGamepadButton(pad_, b.pad)) in.held |= b.button;
            // The stick that is pushed further wins, so the keyboard still works with a pad plugged in.
            auto merge = [](float& k, float p) { if (std::fabs(p) > std::fabs(k)) k = p; };
            merge(in.lx, axis(pad_, SDL_GAMEPAD_AXIS_LEFTX));
            merge(in.ly, axis(pad_, SDL_GAMEPAD_AXIS_LEFTY));
            merge(in.rx, axis(pad_, SDL_GAMEPAD_AXIS_RIGHTX));
            merge(in.ry, axis(pad_, SDL_GAMEPAD_AXIS_RIGHTY));
            in.lt = std::max(0.0f, axis(pad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
            in.rt = std::max(0.0f, axis(pad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
            if (in.lt > 0.5f) in.held |= kL2;
            if (in.rt > 0.5f) in.held |= kR2;
        }
        // Triggers have no button events: count their edges here.
        in.pressed = pressed | (in.held & ~input_.held & (kL2 | kR2));
        input_ = in;
    }

    static bool fail(std::string* error, const char* what) {
        *error = std::string(what) + ": " + SDL_GetError();
        return false;
    }

    SDL_Window* win_ = nullptr;
    SDL_GLContext ctx_ = nullptr;
    SDL_Gamepad* pad_ = nullptr;
    InputState input_;
    bool log_input_ = false;  // ALIA_INPUT_LOG set: log the mapping and button presses
};

}  // namespace

std::unique_ptr<Surface> create_window_surface(const SurfaceDesc& desc, std::string* error) {
    auto s = std::make_unique<SdlWindowSurface>();
    if (!s->init(desc, error)) return nullptr;
    return s;
}

}  // namespace alia::platform
