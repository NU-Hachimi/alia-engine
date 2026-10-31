#include "runtime/actions.hpp"

#include <algorithm>
#include <cmath>

namespace alia::runtime {

bool ActionMap::held(const std::string& name, const platform::InputState& in) const {
    auto it = buttons_.find(name);
    return it != buttons_.end() && (in.held & it->second);
}

bool ActionMap::pressed(const std::string& name, const platform::InputState& in) const {
    auto it = buttons_.find(name);
    return it != buttons_.end() && (in.pressed & it->second);
}

glm::vec2 ActionMap::axis(const std::string& name, const platform::InputState& in) const {
    auto it = axes_.find(name);
    if (it == axes_.end()) return glm::vec2(0.0f);
    glm::vec2 v;
    switch (it->second.stick) {
    case Stick::Left: v = {in.lx, in.ly}; break;
    case Stick::Right: v = {in.rx, in.ry}; break;
    case Stick::Dpad:
        v = {float(bool(in.held & platform::kRight)) - float(bool(in.held & platform::kLeft)),
             float(bool(in.held & platform::kDown)) - float(bool(in.held & platform::kUp))};
        break;
    }
    const float len = glm::length(v), dz = std::clamp(it->second.dead_zone, 0.0f, 0.99f);
    if (len <= dz) return glm::vec2(0.0f);
    const float scaled = std::min(1.0f, (len - dz) / (1 - dz));  // past the dead zone, 0..1 again
    return v / len * scaled;
}

}  // namespace alia::runtime
