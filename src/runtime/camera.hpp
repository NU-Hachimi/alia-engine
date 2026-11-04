// The handheld's camera for games (docs/camera.md): the Alia Pad cart SDK's camera client
// (third_party/alia-sdk/alia/camera.h), and the engine's side of it: raw Bayer frames imported as
// textures (dmabuf, or a copy where that can't be done), developed on the GPU with the SDK's reference
// shader into one RGBA image the scene and the HUD draw from, the buffers handed back once the GPU is
// done with them, and photos.
//
// Everything happens in update(), once per display frame, on the render thread: no thread, never blocks.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rhi/rhi.hpp"

namespace alia::runtime {

class Camera {
public:
    enum class State { Closed, Starting, Pending, Running, Paused, Denied, Revoked, Busy, Unavailable, Suspended, Failed };
    static const char* name(State s);

    struct Mode {
        int width = 0, height = 0, fps = 0, turn = 0;  // the raw frame; turn: 0 / 90 clockwise
        std::string order;                             // "RGGB", "BGGR", ...
        float fx = 0, fy = 0, cx = 0, cy = 0, k1 = 0, k2 = 0, p1 = 0, p2 = 0, k3 = 0;  // raw pixels, before turn
        bool measured = false;                         // false: the module's typical values
        uint32_t line_time_ns = 0;
    };
    struct Frame {
        uint32_t sequence = 0;
        uint64_t timestamp_ns = 0;  // CLOCK_MONOTONIC, end of exposure
        uint32_t exposure_us = 0;
        float gain = 0, wb_red = 0, wb_blue = 0, colour_temperature = 0;
    };
    struct Photo {
        bool done = false;
        std::vector<uint8_t> jpeg;  // upright, the stream's field of view
        std::string error;          // done and no jpeg: why
    };

    Camera();
    ~Camera();
    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;

    // Ask for the camera: the size the game draws it at (the image is that big or a bit bigger, in the
    // camera's own aspect), full_fov false allows a centre crop. Never blocks; state() says how it goes.
    void open(int width, int height, bool full_fov);
    void close(rhi::Device& dev);  // also: the sensor powers down
    // Read what arrived, develop the newest frame, hand back buffers the GPU is done with.
    void update(rhi::Device& dev);
    void shutdown(rhi::Device& dev) { close(dev); }

    State state() const { return state_; }
    const std::string& message() const { return message_; }
    // The developed image (RGBA8, Linear; RHI convention: top row at v = 1); {} until the first frame.
    rhi::TextureH image() const { return have_image_ ? out_tex_ : rhi::TextureH{}; }
    int image_width() const { return out_w_; }
    int image_height() const { return out_h_; }
    const std::optional<Mode>& mode() const { return mode_; }
    const std::optional<Frame>& frame() const { return frame_; }
    bool zero_copy() const { return import_ == Import::InPlace; }  // the frames are read in place (dmabuf), not copied

    // Take a photo; the id to look for in photo(). Only while the stream is open: otherwise the photo
    // is done at once, with an error.
    uint32_t take_photo();
    const Photo* photo(uint32_t id) const;  // the newest 8 are kept

private:
    struct Impl;
    void fail(State s, const std::string& message, rhi::Device& dev);
    void disconnect(rhi::Device& dev);
    bool rebuild(rhi::Device& dev);  // a new MODE: new textures for its buffers and size
    void develop(rhi::Device& dev, int index, uint32_t params);
    rhi::TextureH buffer(rhi::Device& dev, int index);
    void finish_photo(uint32_t id, std::vector<uint8_t> jpeg, std::string error);

    std::unique_ptr<Impl> impl_;
    State state_ = State::Closed;
    std::string message_;
    int want_w_ = 0, want_h_ = 0;
    bool want_full_ = true;
    std::optional<Mode> mode_;
    std::optional<Frame> frame_;
    uint32_t generation_ = 0;  // the client's mode_generation the textures were made for
    enum class Import { Unknown, InPlace, Copy } import_ = Import::Unknown;
    rhi::PipelineH develop_;
    rhi::TextureH out_tex_, shade_tex_, tone_tex_, upload_tex_;
    rhi::TargetH out_target_;
    int out_w_ = 0, out_h_ = 0;
    bool have_image_ = false;
    uint32_t params_id_ = 0;
    bool have_params_ = false;
    std::vector<rhi::TextureH> raw_;     // per buffer index (zero copy)
    std::vector<uint8_t*> mapped_;       // per buffer index (copy path): mmap'd
    std::vector<size_t> mapped_size_;
    struct Held {
        int index;
        rhi::FenceH fence;
    };
    std::vector<Held> held_;
    uint32_t next_photo_ = 1, photo_in_flight_ = 0;
    std::vector<std::pair<uint32_t, Photo>> photos_;
};

}  // namespace alia::runtime
