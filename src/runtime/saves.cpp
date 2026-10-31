#include "runtime/saves.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <cstdlib>

#include "alia/save.h"

namespace alia::runtime::saves {
namespace {

bool chosen = false;  // set_dir() or use_default() ran

int make_dirs(const std::string& path) {  // mkdir -p
    for (size_t i = 1; i <= path.size(); i++) {
        if (i < path.size() && path[i] != '/') continue;
        if (mkdir(path.substr(0, i).c_str(), 0755) && errno != EEXIST) return -errno;
    }
    return 0;
}

}  // namespace

void set_dir(const std::string& d) {
    make_dirs(d);  // as use_default does: a directory that isn't there yet is made
    alia_save_set_dir(d.c_str());
    chosen = true;
}

void use_default(const std::string& app_id) {
    if (chosen || alia_save_dir()) return;  // the session's ALIA_SAVE_DIR, or the game's own choice
    const char* xdg = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    std::string base = xdg && *xdg ? xdg : std::string(home ? home : "/tmp") + "/.local/share";
    std::string d = base + "/alia-engine/saves/" + app_id;
    if (make_dirs(d) == 0) set_dir(d);
}

std::string dir() {
    const char* d = alia_save_dir();
    return d ? d : "";
}

int write(const std::string& name, const void* data, size_t size) { return alia_save_write(name.c_str(), data, size); }

std::optional<std::vector<uint8_t>> read(const std::string& name, int version) {
    for (int attempt = 0; attempt < 4; attempt++) {  // retried if the file grew between stat and read
        long long size;
        if (version == 0) {
            size = alia_save_size(name.c_str());  // the newest readable version
        } else {
            const char* d = alia_save_dir();
            struct stat st;
            std::string p = std::string(d ? d : "") + "/" + name + ".~" + std::to_string(version) + "~";
            size = d && stat(p.c_str(), &st) == 0 ? (long long)st.st_size : -ENOENT;
        }
        if (size < 0) return std::nullopt;
        std::vector<uint8_t> buf(size_t(size) + 1);
        long long got = alia_save_read_version(name.c_str(), version, buf.data(), buf.size());
        if (got >= 0) {
            buf.resize(size_t(got));
            return buf;
        }
        if (got != -EMSGSIZE) return std::nullopt;
    }
    return std::nullopt;
}

std::vector<std::string> names() {
    std::vector<std::string> out;
    char list[256][ALIA_SAVE_NAME_MAX + 1];
    int n = alia_save_list(list, 256);
    for (int i = 0; i < n && i < 256; i++) out.emplace_back(list[i]);
    return out;
}

std::vector<int> versions(const std::string& name) {
    std::vector<int> out;
    const char* d = alia_save_dir();
    if (!d) return out;
    for (int v = 0; v <= ALIA_SAVE_KEEP; v++) {
        std::string p = std::string(d) + "/" + name + (v ? ".~" + std::to_string(v) + "~" : "");
        struct stat st;
        if (stat(p.c_str(), &st) == 0) out.push_back(v);
    }
    return out;
}

int rollback(const std::string& name, int version) { return alia_save_rollback(name.c_str(), version); }

int remove(const std::string& name) { return alia_save_delete(name.c_str()); }

}  // namespace alia::runtime::saves
