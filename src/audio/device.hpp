// The audio output (the Alia Pad cart docs): one SDL3 audio stream, 48 kHz stereo float, asking
// for 512-frame buffers; its callback pulls blocks from the Mixer on SDL's audio thread. open()
// doesn't wait for the device: it opens on its own thread (opening can block for good on some
// PCs), and the game is silent until then. Without SDL (ALIA_WITH_SDL off) or with no audio
// device, the game runs silent.
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

// What the platform asks of a game's output stream (the Alia Pad SDK's [audio]).
struct OutputParams {
    int frames = 512;              // game_quantum: buffer size asked for (~10.7 ms at 48 kHz)
    std::string role = "Game";     // the stream's role for the system's mixer (PipeWire media.role)
};

class Device {
public:
    Device();
    ~Device();
    bool open(Mixer& mixer, const OutputParams& params, std::string* error);
    void close();
    bool is_open() const;
    DeviceStats stats() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;  // shared with the thread that opens the device
};

}  // namespace alia::audio
