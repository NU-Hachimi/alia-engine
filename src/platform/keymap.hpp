// The PC keyboard as a gamepad (direct runs, not in a cart). The layout is the platform's, not the engine's: `alia run`
// reads it from the Alia Pad SDK (alia.runtime.keyboard()) and hands it over as ALIA_KEYMAP, "name=Key[,Key];...",
// with SDL key names. The names are the buttons (south east west north l1 r1 l2 r2 l3 r3 select start up down left
// right), the stick directions (lx- lx+ ly- ly+ rx- rx+ ry- ry+; "-" is left / up) and half (held: the sticks at half
// tilt). An entry is split at its first "=", so "start==" is the "=" key. Without ALIA_KEYMAP, kMinimalKeymap.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace alia::platform {

inline constexpr const char* kMinimalKeymap = "east=Space;start=Return;lx-=A;lx+=D;ly-=W;ly+=S";

struct Keymap {
    std::vector<std::pair<uint32_t, uint32_t>> buttons;  // SDL keycode -> Button bits (input.hpp)
    std::vector<int> axis[8];                           // SDL scancodes: lx- lx+ ly- ly+ rx- rx+ ry- ry+
    std::vector<int> half;                              // SDL scancodes held for half tilt
};

// What it can't read (an unknown name or key) is skipped and listed in *skipped.
Keymap parse_keymap(const std::string& text, std::string* skipped);

}  // namespace alia::platform
