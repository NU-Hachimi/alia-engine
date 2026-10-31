// The keyboard layout handed over as ALIA_KEYMAP (platform/keymap.hpp).
#include <SDL3/SDL.h>

#include <cstdio>

#include "platform/input.hpp"
#include "platform/keymap.hpp"

using namespace alia::platform;

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

static uint32_t bits_of(const Keymap& m, SDL_Keycode key) {
    uint32_t bits = 0;
    for (const auto& [k, b] : m.buttons)
        if (k == uint32_t(key)) bits |= b;
    return bits;
}

int main() {
    std::string skipped;
    // The platform's layout as alia run writes it from alia.runtime.keyboard() (start has "=", select "-").
    Keymap m = parse_keymap("south=Z;east=X;north=C;west=V;l2=Q;start=Return,Keypad Enter,=;select=Backspace,-;"
                            "up=Up;lx-=A;lx+=D;ly-=W;ly+=S;rx-=J;rx+=L;ry-=I;ry+=K;half=Left Shift,Right Shift", &skipped);
    check(skipped.empty(), "the platform's layout reads without skipping anything");
    check(bits_of(m, SDLK_Z) == kSouth && bits_of(m, SDLK_X) == kEast && bits_of(m, SDLK_Q) == kL2 && bits_of(m, SDLK_UP) == kUp,
          "buttons by SDL key name");
    check(bits_of(m, SDLK_RETURN) == kStart && bits_of(m, SDLK_KP_ENTER) == kStart && bits_of(m, SDLK_EQUALS) == kStart &&
              bits_of(m, SDLK_MINUS) == kSelect,
          "several keys for one button, \"=\" and \"-\" among them");
    check(m.axis[0].size() == 1 && m.axis[0][0] == SDL_SCANCODE_A && m.axis[7][0] == SDL_SCANCODE_K && m.half.size() == 2,
          "stick directions and half tilt as scancodes");
    skipped.clear();
    Keymap bad = parse_keymap("south=Z;bogus=Q;east=NotAKey;oops;;", &skipped);
    check(bad.buttons.size() == 1 && skipped.find("bogus") != std::string::npos && skipped.find("NotAKey") != std::string::npos &&
              skipped.find("oops") != std::string::npos,
          "an unknown name or key is skipped and reported; the rest still works");
    skipped.clear();
    Keymap minimal = parse_keymap(kMinimalKeymap, &skipped);
    check(skipped.empty() && bits_of(minimal, SDLK_SPACE) == kEast && bits_of(minimal, SDLK_RETURN) == kStart &&
              minimal.axis[2][0] == SDL_SCANCODE_W,
          "the minimal fallback: WASD, Space, Enter");
    return failures != 0;
}
