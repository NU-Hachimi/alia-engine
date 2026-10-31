// L6 language (docs/api.md §7): the system's language as a BCP 47 tag, the same way everywhere. In a
// cart the system sets ALIA_LANG from the Shell's settings (read at launch: a change shows at the next
// start); elsewhere the POSIX locale (zh_CN.UTF-8 -> zh-CN). The game picks
// among the languages it has like the Shell picks a cart's title: the exact tag, then the language
// alone, then English, then the first it has.
#pragma once

#include <string>
#include <vector>

namespace alia::runtime {

std::string language();  // e.g. "zh-CN", "en"; "en" when nothing says otherwise
std::string choose_language(const std::vector<std::string>& available, const std::string& wanted);  // "" if none

}  // namespace alia::runtime
