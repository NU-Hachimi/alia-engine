// L1: controller state, one snapshot per poll_events().
//
// Buttons are positional (like evdev BTN_SOUTH/EAST/...), so games map them to labels
// themselves. On Alia Pad the labels are Nintendo-style: A = East, B = South, X = North,
// Y = West. On the PC (run directly, not in a cart) the keyboard stands in for a gamepad, in the
// Alia Pad simulator's layout (the same keys there):
//
//   WASD           left stick (Shift: half)     IJKL        right stick
//   arrow keys     D-pad                        X / Z       East (A) / South (B)
//   C / V          North (X) / West (Y)         E / Q       L1 / L2      U / O   R1 / R2
//   F / N          L3 / R3                      Enter       Start        Backspace  Select
//   Esc            quit
//
// In a cart (ALIA_CART_DIR set) the keyboard is ignored: the system's gamepad is the input.
#pragma once

#include <cstdint>

namespace alia::platform {

enum Button : uint32_t {
    kSouth = 1u << 0,
    kEast = 1u << 1,
    kWest = 1u << 2,
    kNorth = 1u << 3,
    kL1 = 1u << 4,
    kR1 = 1u << 5,
    kL2 = 1u << 6,
    kR2 = 1u << 7,
    kL3 = 1u << 8,
    kR3 = 1u << 9,
    kSelect = 1u << 10,
    kStart = 1u << 11,
    kUp = 1u << 12,
    kDown = 1u << 13,
    kLeft = 1u << 14,
    kRight = 1u << 15,
};

struct InputState {
    bool gamepad = false;     // a real gamepad is open (the keyboard always works)
    uint32_t held = 0;        // Button bits held now
    uint32_t pressed = 0;     // went down since the previous poll; taps shorter than a poll still show
    float lx = 0, ly = 0;     // left stick, -1..1, +x right, +y down (SDL convention)
    float rx = 0, ry = 0;     // right stick
    float lt = 0, rt = 0;     // triggers, 0..1
};

}  // namespace alia::platform
