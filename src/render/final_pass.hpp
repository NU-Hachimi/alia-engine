// L3: the final pass. Takes the low-resolution scene (640x360) and puts it on the output:
// nearest-neighbour integer upscale, centred with black borders, optionally rotated to the
// panel's native orientation (the DSI panel is 720x1280 portrait with no rotation hardware;
// rotating here lets sway scan the buffer out directly, docs/architecture.md §4).
// Later this pass also does colour grading, dithering and draws the HUD at native resolution.
#pragma once

#include "rhi/rhi.hpp"

namespace alia::render {

struct Rect {
    int x = 0, y = 0, width = 0, height = 0;  // top-left origin
    bool operator==(const Rect&) const = default;
};

// Where the scene lands on the output. rotation: 0, 90, 180 or 270 degrees clockwise.
// The scale is the largest integer that fits (at least 1; then it may be cropped).
Rect fit_integer(int scene_w, int scene_h, int out_w, int out_h, int rotation, int* scale_out = nullptr);

// Largest aspect-preserving rect at any (fractional) scale, centred.
Rect fit_any(int scene_w, int scene_h, int out_w, int out_h, int rotation);

// How the scene is scaled onto the output. The internal resolution is a runtime choice
// (640x360 by default; 854x480, 960x540, 1280x720 ...), so the scale is not always whole.
enum class Scaling {
    Auto,          // integer nearest when the output holds a whole multiple, else sharp bilinear
    Integer,       // whole multiples only, nearest (black borders if it doesn't fill)
    SharpBilinear, // fill at a fractional scale: crisp pixels, only their edges are blended
};

class FinalPass {
public:
    bool init(rhi::Device& dev, std::string* error);
    // Draws `scene` onto `out` (TargetH{} = the surface's default framebuffer).
    void draw(rhi::Device& dev, rhi::TextureH scene, int scene_w, int scene_h, rhi::TargetH out, int out_w,
              int out_h, int rotation, Scaling scaling = Scaling::Auto);

private:
    rhi::PipelineH pipeline_;
};

}  // namespace alia::render
