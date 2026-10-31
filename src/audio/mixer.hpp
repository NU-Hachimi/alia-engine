// Audio (docs/api.md §7): the engine mixes every sound itself and hands the system one 48 kHz
// stereo stream (the Alia Pad cart docs). This part has no device: sounds, voices, the music
// and effects buses, and mix(), which the device's callback (device.hpp) or a test calls.
//
// Sounds keep their own rate and channels (16-bit PCM) and are resampled while mixing (linear),
// so a long 22 kHz mono track stays small. Voices: a fixed pool; when it is full, the quietest
// effect makes room (never the music). Every gain change is a ramp, so nothing clicks.
// Thread safety: mix() runs on the audio thread; everything else takes the same lock briefly.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace alia::audio {

constexpr int kRate = 48000;  // the output: 48 kHz stereo float
constexpr int kVoices = 32;

struct Sound {
    std::vector<int16_t> samples;  // interleaved
    int channels = 1;
    int rate = kRate;
    size_t frames() const { return channels ? samples.size() / size_t(channels) : 0; }
};

// A RIFF WAV of 16-bit PCM, mono or stereo, any rate; false (and why) for anything else.
bool parse_wav(const uint8_t* data, size_t size, Sound* out, std::string* error);

enum class Bus : uint8_t { Music, Effects };

struct PlayParams {
    float volume = 1;     // 0..
    float pan = 0;        // -1 left .. 1 right (constant power for mono sounds, balance for stereo)
    float pitch = 1;      // playback rate
    bool loop = false;
    double loop_start = 0;  // seconds: where a loop goes back to
    double end = 0;         // seconds: where it ends (or loops); 0: the end of the sound
    float fade_in = 0;      // seconds from silence
    Bus bus = Bus::Effects;
};

struct MixerStats {
    uint32_t voices = 0;     // playing now
    uint64_t frames = 0;     // mixed so far
    uint32_t stolen = 0;     // voices that made room for a new one
};

class Mixer {
public:
    uint32_t add_sound(Sound s);  // ids from 1
    const Sound* sound(uint32_t id) const { return id >= 1 && id <= sounds_.size() ? &sounds_[id - 1] : nullptr; }

    // A voice handle (0: the sound doesn't exist). Handles go stale when the voice ends.
    uint32_t play(uint32_t sound, const PlayParams& p);
    void stop(uint32_t voice, float fade = 0);
    bool playing(uint32_t voice) const;
    void set_volume(uint32_t voice, float volume, float fade = 0);
    void set_pan(uint32_t voice, float pan);

    // One music track at a time. The same sound again only changes the volume (ramped over
    // `fade`); another one fades the old one out while it fades in. A track that has ended
    // stays silent while it is still the one asked for (a jingle doesn't repeat).
    void play_music(uint32_t sound, PlayParams p, float fade);
    void stop_music(float fade);
    uint32_t music() const { return music_sound_; }

    void set_bus_volume(Bus b, float v);
    float bus_volume(Bus b) const;

    // Interleaved stereo float, `frames` of it: the voices summed, buses applied, clamped to -1..1.
    void mix(float* out, int frames);
    MixerStats stats() const;

private:
    struct Voice {
        uint32_t sound = 0, generation = 0;
        double pos = 0, step = 1;            // in the sound's frames
        double loop_start = 0, end = 0;      // frames
        float gain = 0, target = 0, ramp = 0;  // ramp: gain change per output frame
        float pan = 0;
        bool loop = false, active = false, stopping = false;  // stopping: ends when the gain reaches 0
        Bus bus = Bus::Effects;
    };
    static float ramp_for(float from, float to, float seconds);
    uint32_t handle(int slot) const { return (voices_[size_t(slot)].generation << 8) | uint32_t(slot + 1); }
    Voice* find(uint32_t handle);
    const Voice* find(uint32_t handle) const;

    mutable std::mutex lock_;
    std::vector<Sound> sounds_;
    Voice voices_[kVoices];
    uint32_t music_sound_ = 0, music_voice_ = 0;
    float bus_[2] = {1, 1};
    MixerStats stats_;
};

}  // namespace alia::audio
