#include "audio/mixer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace alia::audio {
namespace {

uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

}  // namespace

bool parse_wav(const uint8_t* d, size_t size, Sound* out, std::string* error) {
    auto fail = [&](const char* why) {
        *error = why;
        return false;
    };
    if (size < 12 || std::memcmp(d, "RIFF", 4) || std::memcmp(d + 8, "WAVE", 4)) return fail("not a RIFF WAVE file");
    int channels = 0, bits = 0, rate = 0;
    const uint8_t* pcm = nullptr;
    size_t pcm_size = 0;
    for (size_t at = 12; at + 8 <= size;) {
        const uint32_t len = u32(d + at + 4);
        const uint8_t* body = d + at + 8;
        if (len > size - at - 8) return fail("a chunk runs past the end of the file");
        if (!std::memcmp(d + at, "fmt ", 4) && len >= 16) {
            if (u16(body) != 1) return fail("only PCM WAV files (format 1)");
            channels = u16(body + 2);
            rate = int(u32(body + 4));
            bits = u16(body + 14);
        } else if (!std::memcmp(d + at, "data", 4)) {
            pcm = body;
            pcm_size = len;
        }
        at += 8 + len + (len & 1);
    }
    if (!pcm || !channels) return fail("no fmt or data chunk");
    if (bits != 16) return fail("only 16-bit samples");
    if (channels > 2) return fail("only mono or stereo");
    if (rate < 4000 || rate > 192000) return fail("sample rate out of range");
    out->channels = channels;
    out->rate = rate;
    out->samples.resize(pcm_size / 2 / size_t(channels) * size_t(channels));
    for (size_t i = 0; i < out->samples.size(); i++) out->samples[i] = int16_t(u16(pcm + 2 * i));
    return true;
}

uint32_t Mixer::add_sound(Sound s) {
    std::lock_guard<std::mutex> g(lock_);
    sounds_.push_back(std::move(s));
    return uint32_t(sounds_.size());
}

float Mixer::ramp_for(float from, float to, float seconds) {
    if (seconds <= 0) return 0;  // at once
    return std::fabs(to - from) / (seconds * float(kRate));
}

Mixer::Voice* Mixer::find(uint32_t h) {
    const int slot = int(h & 0xFF) - 1;
    if (slot < 0 || slot >= kVoices) return nullptr;
    Voice& v = voices_[slot];
    return v.active && v.generation == (h >> 8) ? &v : nullptr;
}

const Mixer::Voice* Mixer::find(uint32_t h) const { return const_cast<Mixer*>(this)->find(h); }

uint32_t Mixer::play(uint32_t id, const PlayParams& p) {
    std::lock_guard<std::mutex> g(lock_);
    const Sound* s = id >= 1 && id <= sounds_.size() ? &sounds_[id - 1] : nullptr;
    if (!s || s->frames() == 0) return 0;
    int slot = -1;
    for (int i = 0; i < kVoices && slot < 0; i++)
        if (!voices_[i].active) slot = i;
    if (slot < 0) {  // full: the quietest effect makes room (never the music)
        float quietest = 1e30f;
        for (int i = 0; i < kVoices; i++) {
            const Voice& v = voices_[i];
            const float loud = v.gain * bus_[int(v.bus)];
            if (v.bus == Bus::Effects && loud < quietest) quietest = loud, slot = i;
        }
        if (slot < 0) return 0;
        stats_.stolen++;
    }
    Voice& v = voices_[slot];
    const uint32_t generation = (v.generation + 1) & 0xFFFFFF;
    v = Voice{};
    v.generation = generation;
    v.sound = id;
    v.step = double(s->rate) / kRate * std::max(p.pitch, 0.01f);
    const double frames = double(s->frames());
    v.end = p.end > 0 ? std::min(frames, p.end * s->rate) : frames;
    v.loop_start = std::clamp(p.loop_start * s->rate, 0.0, std::max(0.0, v.end - 1));
    v.loop = p.loop;
    v.target = std::max(p.volume, 0.0f);
    v.gain = p.fade_in > 0 ? 0 : v.target;
    v.ramp = ramp_for(v.gain, v.target, p.fade_in);
    v.pan = std::clamp(p.pan, -1.0f, 1.0f);
    v.bus = p.bus;
    v.active = true;
    return handle(slot);
}

void Mixer::stop(uint32_t h, float fade) {
    std::lock_guard<std::mutex> g(lock_);
    if (Voice* v = find(h)) {
        if (fade <= 0) {
            v->active = false;
            return;
        }
        v->target = 0;
        v->ramp = ramp_for(v->gain, 0, fade);
        v->stopping = true;
    }
}

bool Mixer::playing(uint32_t h) const {
    std::lock_guard<std::mutex> g(lock_);
    return find(h) != nullptr;
}

void Mixer::set_volume(uint32_t h, float volume, float fade) {
    std::lock_guard<std::mutex> g(lock_);
    if (Voice* v = find(h); v && !v->stopping) {
        v->target = std::max(volume, 0.0f);
        v->ramp = ramp_for(v->gain, v->target, fade);
        if (v->ramp == 0) v->gain = v->target;
    }
}

void Mixer::set_pan(uint32_t h, float pan) {
    std::lock_guard<std::mutex> g(lock_);
    if (Voice* v = find(h)) v->pan = std::clamp(pan, -1.0f, 1.0f);
}

void Mixer::play_music(uint32_t sound, PlayParams p, float fade) {
    if (sound == music_sound_) {  // the same track: only its volume (if it still plays)
        set_volume(music_voice_, p.volume, fade);
        return;
    }
    stop(music_voice_, fade);
    p.bus = Bus::Music;
    p.fade_in = fade;
    music_voice_ = sound ? play(sound, p) : 0;
    music_sound_ = sound;
}

void Mixer::stop_music(float fade) {
    stop(music_voice_, fade);
    music_voice_ = music_sound_ = 0;
}

void Mixer::set_bus_volume(Bus b, float v) {
    std::lock_guard<std::mutex> g(lock_);
    bus_[int(b)] = std::max(v, 0.0f);
}

float Mixer::bus_volume(Bus b) const {
    std::lock_guard<std::mutex> g(lock_);
    return bus_[int(b)];
}

MixerStats Mixer::stats() const {
    std::lock_guard<std::mutex> g(lock_);
    MixerStats s = stats_;
    s.voices = 0;
    for (const Voice& v : voices_) s.voices += v.active;
    return s;
}

void Mixer::mix(float* out, int frames) {
    std::lock_guard<std::mutex> g(lock_);
    std::fill(out, out + 2 * frames, 0.0f);
    for (Voice& v : voices_) {
        if (!v.active) continue;
        const Sound& s = sounds_[v.sound - 1];
        const int16_t* pcm = s.samples.data();
        const int ch = s.channels;
        const float bus = bus_[int(v.bus)];
        // Constant power for mono (centre: both at 0.707); balance for stereo.
        const float a = (v.pan + 1) * 0.78539816f;
        const float mono_l = std::cos(a), mono_r = std::sin(a);
        const float bal_l = std::min(1.0f, 1 - v.pan), bal_r = std::min(1.0f, 1 + v.pan);
        for (int f = 0; f < frames; f++) {
            if (v.pos >= v.end) {
                if (!v.loop) {
                    v.active = false;
                    break;
                }
                v.pos = v.loop_start + (v.pos - v.end);
            }
            const size_t i = size_t(v.pos);
            const float t = float(v.pos - double(i));
            size_t j = i + 1;  // the next frame, wrapping into the loop
            if (double(j) >= v.end) j = v.loop ? size_t(v.loop_start) : i;
            if (v.ramp > 0) {
                v.gain = v.gain < v.target ? std::min(v.target, v.gain + v.ramp) : std::max(v.target, v.gain - v.ramp);
                if (v.gain == v.target) v.ramp = 0;
            }
            const float k = v.gain * bus / 32768.0f;
            if (ch == 1) {
                const float x = (float(pcm[i]) + (float(pcm[j]) - float(pcm[i])) * t) * k;
                out[2 * f] += x * mono_l;
                out[2 * f + 1] += x * mono_r;
            } else {
                const float l = float(pcm[2 * i]) + (float(pcm[2 * j]) - float(pcm[2 * i])) * t;
                const float r = float(pcm[2 * i + 1]) + (float(pcm[2 * j + 1]) - float(pcm[2 * i + 1])) * t;
                out[2 * f] += l * k * bal_l;
                out[2 * f + 1] += r * k * bal_r;
            }
            v.pos += v.step;
        }
        if (v.stopping && v.gain <= 0) v.active = false;
    }
    for (int k = 0; k < 2 * frames; k++) out[k] = std::clamp(out[k], -1.0f, 1.0f);
    stats_.frames += uint64_t(frames);
}

}  // namespace alia::audio
