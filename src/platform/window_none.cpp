// Built without SDL (ALIA_WITH_SDL=OFF): headless only, e.g. board benchmarks and CI.
#include "platform/surface.hpp"

namespace alia::platform {

std::unique_ptr<Surface> create_window_surface(const SurfaceDesc&, std::string* error) {
    *error = "this build has no window support (configured with ALIA_WITH_SDL=OFF)";
    return nullptr;
}

}  // namespace alia::platform
