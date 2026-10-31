#include "runtime/locale.hpp"

#include <cctype>
#include <cstdlib>

namespace alia::runtime {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string primary(const std::string& tag) { return lower(tag.substr(0, tag.find('-'))); }

// "zh_CN.UTF-8@euro" -> "zh-CN"; "C", "POSIX" and "" -> "".
std::string from_posix(std::string v) {
    v = v.substr(0, v.find_first_of(".@"));
    if (v.empty() || v == "C" || v == "POSIX") return "";
    for (char& c : v)
        if (c == '_') c = '-';
    return v;
}

}  // namespace

std::string language() {
    if (const char* a = std::getenv("ALIA_LANG"); a && *a) return a;
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"})  // POSIX precedence
        if (const char* v = std::getenv(name); v && *v) {
            std::string tag = from_posix(v);
            return tag.empty() ? "en" : tag;
        }
    return "en";
}

std::string choose_language(const std::vector<std::string>& available, const std::string& wanted) {
    for (const auto& a : available)
        if (lower(a) == lower(wanted)) return a;  // zh-CN for zh-CN
    for (const auto& a : available)
        if (lower(a) == primary(wanted)) return a;  // zh for zh-CN
    for (const auto& a : available)
        if (primary(a) == primary(wanted)) return a;  // zh-Hans for zh-CN
    for (const auto& a : available)
        if (primary(a) == "en") return a;
    return available.empty() ? "" : available.front();
}

}  // namespace alia::runtime
