#include "audio/device.hpp"

namespace alia::audio {

struct Device::Impl {};

Device::Device() : impl_(std::make_shared<Impl>()) {}
Device::~Device() = default;
bool Device::open(Mixer&, std::string* error) {
    *error = "built without SDL: no audio";
    return false;
}
void Device::close() {}
bool Device::is_open() const { return false; }
DeviceStats Device::stats() const { return {}; }

}  // namespace alia::audio
