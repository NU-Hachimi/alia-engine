#include "audio/device.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "core/log.hpp"

namespace alia::audio {

// The state the opening thread and the audio callback use: shared, so a Device closed (or destroyed) while the device
// is still opening leaves it to the thread, which closes the stream if the open ever returns.
struct Device::Impl {
    Mixer* mixer = nullptr;
    std::atomic<SDL_AudioStream*> stream{nullptr};
    std::vector<float> buffer;
    mutable std::mutex lock;  // stats, opening, abandoned
    DeviceStats stats;
    double last_ms = -1;
    bool opening = false;     // the thread is inside SDL_OpenAudioDeviceStream
    bool abandoned = false;   // closed meanwhile: the thread closes whatever it gets

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
        std::lock_guard<std::mutex> g(self->lock);
        auto& s = self->stats;
        const double period = 1000.0 * std::max(s.sample_frames, 1) / std::max(s.rate, 1);
        if (self->last_ms >= 0 && t0 - self->last_ms > 1.5 * period) s.late++;
        self->last_ms = t0;
        s.callbacks++;
        s.mix_ms_max = std::max(s.mix_ms_max, t1 - t0);
    }

    // On its own thread: SDL_OpenAudioDeviceStream can block for good (seen with PipeWire's "Dummy Output" when a PC
    // has no sound card), and the game must not wait for it. The stream opens paused; it starts here once it's ours.
    static void open_stream(std::shared_ptr<Impl> self) {
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kRate};
        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, callback, self.get());
        std::unique_lock<std::mutex> g(self->lock);
        self->opening = false;
        if (self->abandoned) {
            g.unlock();
            if (stream) SDL_DestroyAudioStream(stream);
            return;
        }
        if (!stream) {
            ALIA_INFO("no sound: SDL audio device: %s", SDL_GetError());
            return;
        }
        SDL_AudioSpec got{};
        int frames = 0;
        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &got, &frames);
        self->stats = {};
        self->stats.rate = got.freq;
        self->stats.sample_frames = frames;
        self->last_ms = -1;
        self->stream = stream;
        g.unlock();
        SDL_ResumeAudioStreamDevice(stream);
    }
};

Device::Device() : impl_(std::make_shared<Impl>()) {}
Device::~Device() { close(); }

bool Device::open(Mixer& mixer, const OutputParams& params, std::string* error) {
    close();
    // The platform's asks (the Alia Pad SDK's [audio]); the mixer always mixes 48 kHz stereo, SDL converts if needed.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, std::to_string(params.frames).c_str());
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, params.role.c_str());
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        *error = std::string("SDL audio: ") + SDL_GetError();
        return false;
    }
    impl_->mixer = &mixer;
    impl_->opening = true;
    std::thread(Impl::open_stream, impl_).detach();  // the game starts silent; sound comes once the device opens
    return true;
}

void Device::close() {
    std::unique_lock<std::mutex> g(impl_->lock);
    if (impl_->opening) {  // still inside SDL: leave the stream to the thread, and SDL's audio as it is
        impl_->abandoned = true;
        g.unlock();
        ALIA_WARN("the audio device never opened; leaving it");
        impl_ = std::make_shared<Impl>();  // a fresh state for a later open()
        return;
    }
    SDL_AudioStream* stream = impl_->stream.exchange(nullptr);
    g.unlock();
    if (!stream) return;
    SDL_DestroyAudioStream(stream);  // stops the callback before it returns
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool Device::is_open() const { return impl_->stream.load() != nullptr; }

DeviceStats Device::stats() const {
    std::lock_guard<std::mutex> g(impl_->lock);
    return impl_->stats;
}

}  // namespace alia::audio
