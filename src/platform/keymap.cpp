#include "platform/keymap.hpp"

#include <SDL3/SDL.h>

#include "platform/input.hpp"

namespace alia::platform {
namespace {

constexpr std::pair<const char*, uint32_t> kButtonNames[] = {
    {"south", kSouth}, {"east", kEast}, {"west", kWest}, {"north", kNorth}, {"l1", kL1}, {"r1", kR1},
    {"l2", kL2}, {"r2", kR2}, {"l3", kL3}, {"r3", kR3}, {"select", kSelect}, {"start", kStart},
    {"up", kUp}, {"down", kDown}, {"left", kLeft}, {"right", kRight},
};
constexpr const char* kAxisNames[8] = {"lx-", "lx+", "ly-", "ly+", "rx-", "rx+", "ry-", "ry+"};

// The parts of `text` between `sep`s (empty ones too).
std::vector<std::string> split(const std::string& text, char sep) {
    std::vector<std::string> out;
    size_t at = 0;
    while (true) {
        size_t end = text.find(sep, at);
        out.push_back(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (end == std::string::npos) return out;
        at = end + 1;
    }
}

}  // namespace

Keymap parse_keymap(const std::string& text, std::string* skipped) {
    Keymap map;
    for (const std::string& entry : split(text, ';')) {
        if (entry.empty()) continue;
        size_t eq = entry.find('=');
        if (eq == std::string::npos) { *skipped += " " + entry; continue; }
        std::string name = entry.substr(0, eq);
        bool button = false, axis = false;
        for (const auto& [n, bits] : kButtonNames) button = button || name == n;
        for (const char* n : kAxisNames) axis = axis || name == n;
        if (!button && !axis && name != "half") { *skipped += " " + name; continue; }
        for (const std::string& key : split(entry.substr(eq + 1), ',')) {
            SDL_Keycode code = key.empty() ? SDLK_UNKNOWN : SDL_GetKeyFromName(key.c_str());
            if (code == SDLK_UNKNOWN) { *skipped += " " + name + "=" + key; continue; }
            int sc = int(SDL_GetScancodeFromKey(code, nullptr));
            for (const auto& [n, bits] : kButtonNames)
                if (name == n) map.buttons.push_back({uint32_t(code), bits});
            for (int i = 0; i < 8; ++i)
                if (name == kAxisNames[i]) map.axis[i].push_back(sc);
            if (name == "half") map.half.push_back(sc);
        }
    }
    return map;
}

}  // namespace alia::platform
