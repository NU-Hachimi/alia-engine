// L3: a fixed test picture for E0. It exercises the RHI (vertex buffers, a texture, a
// depth-tested pass into a 640x360 target) and is easy to check by eye and by golden image:
//   - vertical gradient background (interpolation)
//   - an RGB triangle (attributes), drawn twice with depth so the nearer one must win
//   - an 8x8 checker in the top-left corner (texture, nearest filtering, pixel-exact scaling)
//   - a white 1-pixel frame around the edge (no off-by-one at the borders)
//   - an "F" marker top-left (orientation: you can tell flips and rotations apart)
#pragma once

#include "rhi/rhi.hpp"

namespace alia::render {

class TestScene {
public:
    static constexpr int kWidth = 640, kHeight = 360;  // default internal resolution

    // width/height: the internal resolution to render at (any size; content scales with it).
    bool init(rhi::Device& dev, std::string* error, int width = kWidth, int height = kHeight);
    // Renders into the scene target; t is seconds (0 gives the golden-image frame).
    void render(rhi::Device& dev, float t);
    rhi::TextureH color() const { return color_; }
    int width() const { return w_; }
    int height() const { return h_; }

private:
    rhi::TextureH color_, depth_, checker_;
    rhi::TargetH target_;
    rhi::PipelineH flat_, textured_;
    rhi::BufferH quads_, tris_;
    int w_ = kWidth, h_ = kHeight;
};

}  // namespace alia::render
