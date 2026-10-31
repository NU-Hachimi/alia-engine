// L6 input actions (docs/api.md §7; StellarAlia's InputSystem, much smaller): the game names what
// it wants ("shoot", "move") and reads those; which buttons or stick give them is a table it can
// change at run time (a remapping menu) and save. Buttons are positional (platform::Button).
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include <glm/glm.hpp>

#include "platform/input.hpp"

namespace alia::runtime {

class ActionMap {
public:
    enum class Stick : uint8_t { Left, Right, Dpad };

    // A button action: any of `buttons` (a mask of platform::Button). Binding again replaces.
    void bind(const std::string& name, uint32_t buttons) { buttons_[name] = buttons; axes_.erase(name); }
    // An axis action: a stick (or the d-pad as one), with a radial dead zone (0..1) rescaled
    // so it still reaches 1.
    void bind_axis(const std::string& name, Stick stick, float dead_zone = 0) {
        axes_[name] = {stick, dead_zone};
        buttons_.erase(name);
    }
    void unbind(const std::string& name) { buttons_.erase(name), axes_.erase(name); }
    void clear() { buttons_.clear(), axes_.clear(); }

    bool held(const std::string& name, const platform::InputState& in) const;
    bool pressed(const std::string& name, const platform::InputState& in) const;  // went down this step
    glm::vec2 axis(const std::string& name, const platform::InputState& in) const;  // +x right, +y down

    struct Axis {
        Stick stick;
        float dead_zone;
    };
    const std::map<std::string, uint32_t>& buttons() const { return buttons_; }
    const std::map<std::string, Axis>& axes() const { return axes_; }

private:
    std::map<std::string, uint32_t> buttons_;
    std::map<std::string, Axis> axes_;
};

}  // namespace alia::runtime
