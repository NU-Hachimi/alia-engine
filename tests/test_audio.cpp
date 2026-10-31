// Audio mixer (audio/mixer.hpp): no device, the test calls mix() itself.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "audio/mixer.hpp"

using namespace alia;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

audio::Sound tone(float hz, int rate, double seconds, int channels = 1, float level = 0.5f) {
    audio::Sound s;
    s.rate = rate;
    s.channels = channels;
    const size_t frames = size_t(seconds * rate);
    for (size_t i = 0; i < frames; i++)
        for (int c = 0; c < channels; c++) s.samples.push_back(int16_t(level * 32767 * std::sin(6.2831853 * hz * double(i) / rate)));
    return s;
}

std::vector<float> run(audio::Mixer& m, int frames) {
    std::vector<float> out(size_t(frames) * 2);
    m.mix(out.data(), frames);
    return out;
}

float peak(const std::vector<float>& v, int channel, size_t from = 0) {
    float p = 0;
    for (size_t i = from * 2 + size_t(channel); i < v.size(); i += 2) p = std::max(p, std::fabs(v[i]));
    return p;
}

std::vector<uint8_t> wav(const audio::Sound& s) {  // a RIFF WAVE of it
    auto put32 = [](std::vector<uint8_t>& b, uint32_t v) { for (int k = 0; k < 4; k++) b.push_back(uint8_t(v >> (8 * k))); };
    auto put16 = [](std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v)), b.push_back(uint8_t(v >> 8)); };
    std::vector<uint8_t> b = {'R', 'I', 'F', 'F'};
    put32(b, uint32_t(36 + s.samples.size() * 2));
    for (char c : std::string("WAVEfmt ")) b.push_back(uint8_t(c));
    put32(b, 16), put16(b, 1), put16(b, uint16_t(s.channels)), put32(b, uint32_t(s.rate));
    put32(b, uint32_t(s.rate * s.channels * 2)), put16(b, uint16_t(s.channels * 2)), put16(b, 16);
    for (char c : std::string("data")) b.push_back(uint8_t(c));
    put32(b, uint32_t(s.samples.size() * 2));
    for (int16_t v : s.samples) put16(b, uint16_t(v));
    return b;
}

}  // namespace

int main() {
    audio::Sound parsed;
    std::string err;
    const audio::Sound src = tone(440, 22050, 0.1, 2);
    check(audio::parse_wav(wav(src).data(), wav(src).size(), &parsed, &err) && parsed.samples == src.samples &&
              parsed.rate == 22050 && parsed.channels == 2,
          "parse_wav: 16-bit PCM, stereo, its own rate");
    const uint8_t junk[] = "RIFF0000WAVX";
    check(!audio::parse_wav(junk, sizeof junk, &parsed, &err), "not a WAV: refused");

    audio::Mixer m;
    // 1 kHz recorded at 24 kHz plays at 1 kHz at 48 kHz: count the rising zero crossings in 0.1 s.
    const uint32_t beep = m.add_sound(tone(1000, 24000, 1.0));
    uint32_t v = m.play(beep, {});
    auto out = run(m, 4800);
    int rising = 0;
    for (size_t f = 1; f < 4800; f++) rising += out[2 * (f - 1)] < 0 && out[2 * f] >= 0;
    check(rising >= 99 && rising <= 101, "resampled to 48 kHz: the pitch stays");
    check(std::fabs(peak(out, 0) - 0.5f * 0.7071f) < 0.01f && std::fabs(peak(out, 0) - peak(out, 1)) < 1e-4f,
          "a mono sound in the centre: constant power (0.707 each side)");
    m.stop(v);
    m.play(beep, {.pan = 1});
    out = run(m, 480);
    check(peak(out, 0) < 1e-3f && peak(out, 1) > 0.49f, "pan 1: all on the right");

    audio::Mixer m2;
    const uint32_t short_ = m2.add_sound(tone(500, 48000, 0.01));  // 480 frames
    v = m2.play(short_, {});
    run(m2, 1000);
    check(!m2.playing(v), "a sound that isn't looping ends; its handle goes stale");
    v = m2.play(short_, {.loop = true});
    out = run(m2, 2000);
    check(m2.playing(v) && peak(out, 0, 1500) > 0.3f, "a loop goes on");
    m2.stop(v, 0.01f);
    run(m2, 300);
    check(m2.playing(v), "stop with a fade: still sounding while it fades");
    run(m2, 300);
    check(!m2.playing(v), "and gone after it");
    v = m2.play(short_, {.loop = true, .fade_in = 0.01f});
    out = run(m2, 480);
    check(peak(out, 0, 0) > 0 && std::fabs(out[2 * 10]) < std::fabs(0.5f * 0.7071f) * 0.05f + 1e-3f, "fade_in starts from silence");
    m2.stop(v);
    m2.set_bus_volume(audio::Bus::Effects, 0.5f);
    m2.play(short_, {.loop = true});
    out = run(m2, 480);
    check(std::fabs(peak(out, 0) - 0.25f * 0.7071f) < 0.01f, "the effects bus volume");

    audio::Mixer m3;  // music: one track, the same again only changes its volume, a new one crossfades
    const uint32_t a = m3.add_sound(tone(300, 48000, 2.0)), b = m3.add_sound(tone(600, 48000, 2.0));
    m3.play_music(a, {.loop = true}, 0.0f);
    run(m3, 480);
    m3.play_music(a, {.volume = 0.5f, .loop = true}, 0.0f);
    check(m3.stats().voices == 1 && m3.music() == a, "the same track again: no restart, one voice");
    m3.play_music(b, {.loop = true}, 0.05f);
    run(m3, 1200);
    check(m3.stats().voices == 2, "a new track: both sound while they crossfade");
    run(m3, 2400);
    check(m3.stats().voices == 1 && m3.music() == b, "then only the new one");
    const uint32_t jingle = m3.add_sound(tone(800, 48000, 0.02));
    m3.play_music(jingle, {}, 0.0f);
    run(m3, 2400 + 4800);
    m3.play_music(jingle, {}, 0.0f);
    out = run(m3, 480);
    check(peak(out, 0) == 0 && m3.stats().voices == 0, "a finished one-shot stays silent while it is still asked for");

    audio::Mixer m4;  // full: the quietest effect makes room, never the music
    const uint32_t hum = m4.add_sound(tone(200, 48000, 1.0));
    m4.play_music(hum, {.volume = 0.01f, .loop = true}, 0.0f);
    for (int k = 0; k < audio::kVoices - 1; k++) m4.play(hum, {.volume = 0.2f + 0.01f * float(k), .loop = true});
    uint32_t extra = m4.play(hum, {.volume = 0.9f, .loop = true});
    check(extra != 0 && m4.stats().stolen == 1 && m4.stats().voices == uint32_t(audio::kVoices) && m4.music() == hum,
          "the pool full: the quietest effect makes room, the (quieter) music stays");
    return failures != 0;
}
