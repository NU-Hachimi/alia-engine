// The audio output (the Alia Pad cart docs): one SDL3 audio stream, 48 kHz stereo float, asking
// for 512-frame buffers; its callback pulls blocks from the Mixer on SDL's audio thread. Without
// SDL (ALIA_WITH_SDL off) or with no audio device, open() fails and the game runs silent.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "audio/mixer.hpp"

namespace alia::audio {

struct DeviceStats {
    int rate = 0, sample_frames = 0;  // what the device gave us
    uint64_t callbacks = 0;
    uint64_t late = 0;      // callbacks that came more than 1.5 buffers after the last one: likely dropouts
    double mix_ms_max = 0;  // the longest mix() since open
};

class Device {
public:
    static constexpr int kSampleFrames = 512;  // ~10.7 ms at 48 kHz

    Device();
    ~Device();
    bool open(Mixer& mixer, std::string* error);
    void close();
    bool is_open() const;
    DeviceStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alia::audio
