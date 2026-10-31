// Pure-logic tests for L3 (no GPU needed).
#include <cstdio>

#include "render/final_pass.hpp"

static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

using alia::render::fit_any;
using alia::render::fit_integer;
using alia::render::Rect;

int main() {
    int s = 0;
    // 640x360 into 1280x720: exactly 2x, no borders.
    CHECK((fit_integer(640, 360, 1280, 720, 0, &s) == Rect{0, 0, 1280, 720}) && s == 2);
    // Into 1920x1080 (TV): exactly 3x.
    CHECK((fit_integer(640, 360, 1920, 1080, 0, &s) == Rect{0, 0, 1920, 1080}) && s == 3);
    // Rotated onto the 720x1280 portrait panel: 360x640 displayed, 2x.
    CHECK((fit_integer(640, 360, 720, 1280, 90, &s) == Rect{0, 0, 720, 1280}) && s == 2);
    CHECK((fit_integer(640, 360, 720, 1280, 270) == Rect{0, 0, 720, 1280}));
    // A window that isn't a multiple: 1300x800 -> 2x, centred with borders.
    CHECK((fit_integer(640, 360, 1300, 800, 0, &s) == Rect{10, 40, 1280, 720}) && s == 2);
    // Too small for 1x: still 1x (cropped), never 0.
    CHECK((fit_integer(640, 360, 320, 200, 0, &s).width == 640) && s == 1);
    // Fractional fits: 854x480 onto 1280x720 fills it (1.5x); onto the rotated panel too.
    {  // 854x480 is not exactly 16:9 (1.779), so it fills the width and is 719 high
        Rect r = fit_any(854, 480, 1280, 720, 0);
        CHECK(r.width == 1280 && (r.height == 719 || r.height == 720));
    }
    CHECK((fit_any(960, 540, 1280, 720, 0) == Rect{0, 0, 1280, 720}));
    CHECK((fit_any(640, 360, 720, 1280, 90) == Rect{0, 0, 720, 1280}));
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures != 0;
}
