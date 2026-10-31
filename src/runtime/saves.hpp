// L6 saves (docs/api.md §7): the Alia Pad cart SDK's saves (third_party/alia-sdk/alia/save.h: atomic
// writes, two previous versions, a quota). In a cart the system sets ALIA_SAVE_DIR (/save);
// elsewhere (the PC, tests, tools) the engine picks a directory per app id, so a game saves the
// same way everywhere.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace alia::runtime::saves {

// Where saves go: ALIA_SAVE_DIR if set, else what set_dir() chose. use_default() points saves at
// $XDG_DATA_HOME/alia-engine/saves/<app_id> (~/.local/share/...) unless ALIA_SAVE_DIR or set_dir()
// already decided; Engine::open calls it.
void set_dir(const std::string& dir);
void use_default(const std::string& app_id);
std::string dir();

// 0 on success, -errno on failure (save.h's conventions).
int write(const std::string& name, const void* data, size_t size);
std::optional<std::vector<uint8_t>> read(const std::string& name, int version = 0);  // 0: newest readable
std::vector<std::string> names();
std::vector<int> versions(const std::string& name);  // which of 0 (current), 1, 2 exist
int rollback(const std::string& name, int version);
int remove(const std::string& name);

}  // namespace alia::runtime::saves
