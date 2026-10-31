#include "audio/device.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>

namespace alia::audio {

struct Device::Impl {
    Mixer* mixer = nullptr;
    SDL_AudioStream* stream = nullptr;
    std::vector<float> buffer;
    mutable std::mutex stats_lock;
    DeviceStats stats;
    double last_ms = -1;

    static double now_ms() {
        using namespace std::chrono;
        return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
    }

    static void SDLCALL callback(void* user, SDL_AudioStream* stream, int additional, int /*total*/) {
        auto* self = static_cast<Impl*>(user);
        const int frames = additional / int(2 * sizeof(float));
        if (frames <= 0) return;
        const double t0 = now_ms();
        self->buffer.resize(size_t(frames) * 2);
        self->mixer->mix(self->buffer.data(), frames);
        SDL_PutAudioStreamData(stream, self->buffer.data(), frames * int(2 * sizeof(float)));
        const double t1 = now_ms();
        std::lock_guard<std::mutex> g(self->stats_lock);
        auto& s = self->stats;
        const double period = 1000.0 * std::max(s.sample_frames, 1) / std::max(s.rate, 1);
        if (self->last_ms >= 0 && t0 - self->last_ms > 1.5 * period) s.late++;
        self->last_ms = t0;
        s.callbacks++;
        s.mix_ms_max = std::max(s.mix_ms_max, t1 - t0);
    }
};

Device::Device() : impl_(std::make_unique<Impl>()) {}
Device::~Device() { close(); }

bool Device::open(Mixer& mixer, std::string* error) {
    close();
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");  // about 10.7 ms (cart.md §4: 20-30 ms in all)
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        *error = std::string("SDL audio: ") + SDL_GetError();
        return false;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kRate};
    impl_->mixer = &mixer;
    impl_->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Impl::callback, impl_.get());
    if (!impl_->stream) {
        *error = std::string("SDL audio device: ") + SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    SDL_AudioSpec got{};
    int frames = 0;
    SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(impl_->stream), &got, &frames);
    {
        std::lock_guard<std::mutex> g(impl_->stats_lock);
        impl_->stats = {};
        impl_->stats.rate = got.freq;
        impl_->stats.sample_frames = frames;
        impl_->last_ms = -1;
    }
    SDL_ResumeAudioStreamDevice(impl_->stream);
    return true;
}

void Device::close() {
    if (!impl_->stream) return;
    SDL_DestroyAudioStream(impl_->stream);  // stops the callback before it returns
    impl_->stream = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool Device::is_open() const { return impl_->stream != nullptr; }

DeviceStats Device::stats() const {
    std::lock_guard<std::mutex> g(impl_->stats_lock);
    return impl_->stats;
}

}  // namespace alia::audio
