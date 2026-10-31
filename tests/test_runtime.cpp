// L6 runtime (runtime/app.hpp): manifest reading, where each option comes from, and the loop
// (headless, simulated clock: one fixed step per frame).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "runtime/actions.hpp"
#include "runtime/app.hpp"
#include "runtime/locale.hpp"

using namespace alia;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

struct Counting final : runtime::Game {
    int inits = 0, steps = 0, renders = 0, shutdowns = 0;
    double dt = 0, alpha = -1;
    void init(runtime::Engine&) override { inits++; }
    void fixed_update(runtime::Engine&, double d) override {
        steps++;
        dt = d;
    }
    void render(runtime::Engine& e, double a) override {
        renders++;
        alpha = a;
        e.hud_rect(0, 0, 8, 8, 0xFFFFFFFF);
    }
    void shutdown(runtime::Engine&) override { shutdowns++; }
};

}  // namespace

int main() {
    char tmpl[] = "/tmp/alia-test-cart-XXXXXX";  // the fixture lives outside the source tree
    const std::string dir = mkdtemp(tmpl);
    {
        std::ofstream m(dir + "/cart.toml");
        m << "# a comment\n[cart]\nid = \"com.example.test\"   # trailing comment\ntitle = \"Test Cart\"\n"
             "description = \"\"\"Several lines, with\ntitle = \"not this\" inside\n\"\"\"\n"
             "[locale.zh]\ntitle = \"测试卡带\"\n[locale.ja]\ntitle = \"テスト\"\n"
             "[runtime]\ntype = \"python\"\nentry = \"main.py\"\n[display]\nrender = \"480x320\"\n";
    }
    runtime::Manifest man;
    check(runtime::read_manifest(dir + "/cart.toml", &man), "manifest read");
    check(man.id == "com.example.test" && man.title == "Test Cart" && man.render == "480x320",
          "id, title (not the one inside the description) and render");
    check(man.title_for("zh-CN") == "测试卡带" && man.title_for("ja") == "テスト" && man.title_for("fr") == "Test Cart" &&
              man.title_for("en") == "Test Cart",
          "[locale.<tag>] titles: the exact tag, then the language, then [cart] title");
    {
        std::ofstream m(dir + "/old.toml");
        m << "[cart]\nid = \"x\"\ntitle = \"Old\"\ntitle_zh = \"旧\"\n";
    }
    runtime::Manifest old;
    check(runtime::read_manifest(dir + "/old.toml", &old) && old.title_for("zh-CN") == "旧", "title_zh still counts as [locale.zh]");
    check(!runtime::read_manifest(dir + "/missing.toml", &man), "missing manifest is false");
    unsetenv("ALIA_CART_DIR");
    unsetenv("ALIA_CART_ID");
    check(runtime::options_from_environment("T", "com.example.mine").engine.app_id == "com.example.mine",
          "outside a cart the game's own app id (it names the save directory)");

    // Manifest, then environment, then flags.
    setenv("ALIA_CART_DIR", dir.c_str(), 1);
    unsetenv("ALIA_CART_ID");
    setenv("ALIA_DISPLAY_SIZE", "800x600", 1);
    runtime::RunOptions o = runtime::options_from_environment("fallback");
    check(o.engine.title == "Test Cart" && o.engine.app_id == "com.example.test", "title and app id from the manifest");
    check(o.engine.internal_w == 480 && o.engine.internal_h == 320, "internal resolution from [display] render");
    check(o.engine.width == 800 && o.engine.height == 600, "ALIA_DISPLAY_SIZE override");
    setenv("ALIA_CART_ID", "com.example.session", 1);
    check(runtime::options_from_environment("x").engine.app_id == "com.example.session", "ALIA_CART_ID wins");
    unsetenv("ALIA_CART_DIR");
    unsetenv("ALIA_CART_ID");
    unsetenv("ALIA_DISPLAY_SIZE");
    check(runtime::options_from_environment("fallback").engine.title == "fallback", "no manifest: the game's title");

    std::vector<std::string> args = {"--demo", "--headless", "--frames", "12", "--render", "320x180", "level2"};
    std::string err;
    check(runtime::apply_flags(o, args, &err) && o.engine.headless && o.frames == 12 && o.engine.internal_w == 320,
          "flags applied");
    check(args == std::vector<std::string>({"--demo", "level2"}), "the game's own arguments are left");
    std::vector<std::string> bad = {"--rotate", "45"};
    check(!runtime::apply_flags(o, bad, &err) && !err.empty(), "bad rotation refused");

    // The loop, headless.
    runtime::RunOptions r;
    r.engine.headless = true;
    r.frames = 30;
    r.fixed_hz = 50;
    Counting g;
    if (!runtime::run(g, r, &err)) {
        std::printf("SKIP: no headless EGL (%s)\n", err.c_str());
        return failures ? 1 : 77;
    }
    check(g.inits == 1 && g.shutdowns == 1, "init and shutdown once");
    check(g.renders == 30 && g.steps == 30, "headless: one fixed step per rendered frame");
    check(g.dt > 0.0199 && g.dt < 0.0201 && g.alpha == 1.0, "dt is the fixed step; alpha 1 (the current state) with the simulated clock");

    // Input actions (api.md §7).
    runtime::ActionMap map;
    map.bind("hook", platform::kNorth | platform::kR1);
    map.bind_axis("move", runtime::ActionMap::Stick::Left, 0.2f);
    map.bind_axis("menu", runtime::ActionMap::Stick::Dpad);
    platform::InputState in;
    in.held = in.pressed = platform::kR1;
    check(map.held("hook", in) && map.pressed("hook", in) && !map.held("shoot", in), "a button action: any of its buttons");
    in.lx = 0.1f;
    check(map.axis("move", in) == glm::vec2(0.0f), "inside the dead zone: nothing");
    in.lx = 0.6f;
    check(std::fabs(map.axis("move", in).x - 0.5f) < 1e-5f, "past it: rescaled to reach 1 at the edge");
    in.held = platform::kUp | platform::kRight;
    check(map.axis("menu", in) == glm::vec2(1, -1) / std::sqrt(2.0f), "the d-pad as an axis (+y down)");
    map.bind("move", platform::kSouth);
    check(map.axes().count("move") == 0 && map.buttons().count("move") == 1, "binding again replaces");

    // Language (api.md §7).
    setenv("ALIA_LANG", "zh-CN", 1);
    check(runtime::language() == "zh-CN", "language: ALIA_LANG in a cart");
    unsetenv("ALIA_LANG");
    setenv("LC_ALL", "", 1);
    setenv("LC_MESSAGES", "", 1);
    setenv("LANG", "zh_TW.UTF-8", 1);
    check(runtime::language() == "zh-TW", "elsewhere the POSIX locale, as a BCP 47 tag");
    setenv("LANG", "C", 1);
    check(runtime::language() == "en", "C / POSIX: English");
    check(runtime::choose_language({"en", "zh"}, "zh-CN") == "zh" && runtime::choose_language({"en", "zh-Hans"}, "zh-CN") == "zh-Hans" &&
              runtime::choose_language({"ja", "en"}, "fr") == "en" && runtime::choose_language({"ja"}, "fr") == "ja" &&
              runtime::choose_language({"zh-CN", "zh"}, "ZH-cn") == "zh-CN",
          "choose_language: exact, then the language, then English, then the first");
    return failures != 0;
}
