// L0: minimal logging. printf-style, to stderr, with a level prefix.
#pragma once

namespace alia::log {

enum class Level { Debug, Info, Warn, Error };

void set_level(Level min);
void write(Level level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

}  // namespace alia::log

#define ALIA_DEBUG(...) ::alia::log::write(::alia::log::Level::Debug, __VA_ARGS__)
#define ALIA_INFO(...) ::alia::log::write(::alia::log::Level::Info, __VA_ARGS__)
#define ALIA_WARN(...) ::alia::log::write(::alia::log::Level::Warn, __VA_ARGS__)
#define ALIA_ERROR(...) ::alia::log::write(::alia::log::Level::Error, __VA_ARGS__)
