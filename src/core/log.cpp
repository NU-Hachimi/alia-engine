#include "core/log.hpp"

#include <cstdarg>
#include <cstdio>

namespace alia::log {
namespace {
Level g_min = Level::Info;
const char* const kPrefix[] = {"debug", "info", "warn", "error"};
}  // namespace

void set_level(Level min) { g_min = min; }

void write(Level level, const char* fmt, ...) {
    if (level < g_min) return;
    std::fprintf(stderr, "[alia %s] ", kPrefix[int(level)]);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

}  // namespace alia::log
