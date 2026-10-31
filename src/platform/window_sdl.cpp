#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/log.hpp"
#include "platform/keymap.hpp"
#include "platform/surface.hpp"

namespace alia::platform {
namespace {

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
        // In a cart (the handheld, or the PC simulator) the system's gamepad is the input: the simulator turns keys into
        // that gamepad, so reading the keyboard here as well would count every key twice. Direct runs read both.
        keyboard_ = std::getenv("ALIA_CART_DIR") == nullptr;
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return fail(error, "SDL_Init");
        const char* given = std::getenv("ALIA_KEYMAP");
        std::string skipped;
        keys_ = parse_keymap(given && *given ? given : kMinimalKeymap, &skipped);
        if (keyboard_ && !(given && *given))
            ALIA_INFO("keyboard: a minimal set (WASD, Space, Enter, Esc); `alia run` passes the platform's layout (ALIA_KEYMAP)");
        if (!skipped.empty()) ALIA_WARN("ALIA_KEYMAP: skipped what isn't a button or a key name:%s", skipped.c_str());
        if (log_input_ && given) ALIA_INFO("keymap: %s", given);
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
                if (!keyboard_) break;
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
    uint32_t key_button(SDL_Keycode k) const {
        uint32_t bits = 0;
        for (const auto& [key, button] : keys_.buttons)
            if (key == uint32_t(k)) bits |= button;
        return bits;
    }

    void read_input(uint32_t pressed) {
        InputState in;
        if (keyboard_) {
            const bool* keys = SDL_GetKeyboardState(nullptr);
            auto any = [&](const std::vector<int>& list) {
                return std::any_of(list.begin(), list.end(), [&](int sc) { return keys[sc]; });
            };
            const float tilt = any(keys_.half) ? 0.5f : 1.0f;
            auto dir = [&](int i) { return any(keys_.axis[i]) ? tilt : 0.0f; };
            for (const auto& [key, button] : keys_.buttons)
                if (keys[SDL_GetScancodeFromKey(SDL_Keycode(key), nullptr)]) in.held |= button;
            in.lx = dir(1) - dir(0);
            in.ly = dir(3) - dir(2);
            in.rx = dir(5) - dir(4);
            in.ry = dir(7) - dir(6);
            in.lt = in.held & kL2 ? 1.0f : 0.0f;  // the trigger keys pull the triggers all the way
            in.rt = in.held & kR2 ? 1.0f : 0.0f;
        }
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
            in.lt = std::max(in.lt, axis(pad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
            in.rt = std::max(in.rt, axis(pad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
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
    bool keyboard_ = true;    // the keyboard stands in for a gamepad (not in a cart: ALIA_CART_DIR set)
    Keymap keys_;             // which keys are which buttons and stick directions (ALIA_KEYMAP)
};

}  // namespace

std::unique_ptr<Surface> create_window_surface(const SurfaceDesc& desc, std::string* error) {
    auto s = std::make_unique<SdlWindowSurface>();
    if (!s->init(desc, error)) return nullptr;
    return s;
}

}  // namespace alia::platform
