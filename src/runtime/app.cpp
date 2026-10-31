#include "runtime/app.hpp"
#include "runtime/locale.hpp"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "core/log.hpp"

namespace alia::runtime {
namespace {

std::atomic<bool> g_stop{false};
constexpr double kPauseGap = 0.25;  // s between polls that counts as a pause (see the loop)

extern "C" void on_sigterm(int) { g_stop = true; }

// Restores the previous SIGTERM / SIGINT handlers when the loop ends, however it ends.
struct SignalGuard {
    using Handler = void (*)(int);
    Handler term, intr;
    SignalGuard() : term(std::signal(SIGTERM, on_sigterm)), intr(std::signal(SIGINT, on_sigterm)) { g_stop = false; }
    ~SignalGuard() {
        std::signal(SIGTERM, term);
        std::signal(SIGINT, intr);
    }
};

bool parse_size(const std::string& text, int* w, int* h) {
    int a = 0, b = 0;
    char extra = 0;
    if (std::sscanf(text.c_str(), "%dx%d%c", &a, &b, &extra) != 2 || a <= 0 || b <= 0 || a > 8192 || b > 8192) return false;
    *w = a;
    *h = b;
    return true;
}

bool parse_rotation(const std::string& text, int* out) {
    int r = std::atoi(text.c_str());
    if (text.empty() || (r != 0 && r != 90 && r != 180 && r != 270)) return false;
    *out = r;
    return true;
}

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? v : nullptr;
}

bool truthy(const char* v) { return v && std::string(v) != "0"; }

double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// Strips a TOML basic string: "value" -> value. Returns false for anything else.
bool quoted(const std::string& v, std::string* out) {
    if (v.size() < 2 || v.front() != '"' || v.back() != '"' || v.rfind("\"\"\"", 0) == 0) return false;
    *out = v.substr(1, v.size() - 2);
    return true;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

}  // namespace

bool read_manifest(const std::string& path, Manifest* out) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line, section;
    bool in_multiline = false;  // inside a """ ... """ string (descriptions)
    while (std::getline(in, line)) {
        if (in_multiline) {
            if (line.find("\"\"\"") != std::string::npos) in_multiline = false;
            continue;
        }
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.front() == '[') {
            section = trim(t.substr(1, t.find(']') - 1));
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(t.substr(0, eq)), value = trim(t.substr(eq + 1));
        if (value.rfind("\"\"\"", 0) == 0) {
            in_multiline = value.find("\"\"\"", 3) == std::string::npos;
            continue;
        }
        if (size_t hash = value.find(" #"); hash != std::string::npos && value.front() == '"' &&
                                              value.find('"', 1) < hash)
            value = trim(value.substr(0, hash));  // a trailing comment after the string
        std::string v;
        if (!quoted(value, &v)) continue;
        if (section == "cart" && key == "id") out->id = v;
        else if (section == "cart" && key == "title") out->title = v;
        else if (section == "cart" && key == "title_zh") out->titles.try_emplace("zh", v);  // the old form
        else if (section.rfind("locale.", 0) == 0 && key == "title") out->titles[section.substr(7)] = v;
        else if (section == "display" && key == "render") out->render = v;
    }
    return true;
}

std::string Manifest::title_for(const std::string& language) const {
    std::vector<std::string> tags;
    for (const auto& [tag, t] : titles) tags.push_back(tag);
    // choose_language falls back to English and then any tag; a cart's own title comes before those.
    const std::string tag = choose_language(tags, language);
    if (!tag.empty()) {
        const std::string lang = language.substr(0, language.find('-')), got = tag.substr(0, tag.find('-'));
        auto same = [](std::string a, std::string b) {
            for (char& c : a) c = char(std::tolower(static_cast<unsigned char>(c)));
            for (char& c : b) c = char(std::tolower(static_cast<unsigned char>(c)));
            return a == b;
        };
        if (same(got, lang)) return titles.at(tag);
    }
    return title;
}

RunOptions options_from_environment(const std::string& default_title, const std::string& default_app_id) {
    RunOptions o;
    o.engine.title = default_title;
    o.engine.app_id = default_app_id;
    Manifest m;
    if (const char* dir = env("ALIA_CART_DIR"); dir && read_manifest(std::string(dir) + "/cart.toml", &m)) {
        if (std::string t = m.title_for(language()); !t.empty()) o.engine.title = t;  // the window's title
        if (!m.id.empty()) o.engine.app_id = m.id;
        if (!m.render.empty() && !parse_size(m.render, &o.engine.internal_w, &o.engine.internal_h))
            ALIA_WARN("cart.toml: [display] render = \"%s\" is not WxH; using %dx%d", m.render.c_str(),
                      o.engine.internal_w, o.engine.internal_h);
    }
    if (const char* id = env("ALIA_CART_ID")) o.engine.app_id = id;  // the session's word is final
    o.engine.headless = truthy(env("ALIA_HEADLESS"));
    if (const char* v = env("ALIA_FRAMES")) o.frames = std::max(0, std::atoi(v));
    if (const char* v = env("ALIA_SCREENSHOT")) o.screenshot = v;
    o.stats = truthy(env("ALIA_STATS"));
    o.input_log = truthy(env("ALIA_INPUT_LOG"));
    if (truthy(env("ALIA_NO_INTERPOLATE"))) o.interpolate = false;
    if (const char* v = env("ALIA_DISPLAY_SIZE"); v && !parse_size(v, &o.engine.width, &o.engine.height))
        ALIA_WARN("ALIA_DISPLAY_SIZE=%s is not WxH", v);
    if (const char* v = env("ALIA_DISPLAY_ROTATION"); v && !parse_rotation(v, &o.engine.rotation))
        ALIA_WARN("ALIA_DISPLAY_ROTATION=%s is not 0/90/180/270", v);
    if (const char* v = env("ALIA_RENDER"); v && !parse_size(v, &o.engine.internal_w, &o.engine.internal_h))
        ALIA_WARN("ALIA_RENDER=%s is not WxH", v);
    return o;
}

bool apply_flags(RunOptions& o, std::vector<std::string>& args, std::string* error) {
    std::vector<std::string> rest;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        auto value = [&](std::string* out) {
            if (i + 1 >= args.size()) {
                *error = a + " needs a value";
                return false;
            }
            *out = args[++i];
            return true;
        };
        std::string v;
        if (a == "--headless") o.engine.headless = true;
        else if (a == "--stats") o.stats = true;
        else if (a == "--input-log") o.input_log = true;
        else if (a == "--no-interpolate") o.interpolate = false;
        else if (a == "--frames") {
            if (!value(&v)) return false;
            o.frames = std::max(0, std::atoi(v.c_str()));
        } else if (a == "--screenshot") {
            if (!value(&o.screenshot)) return false;
        } else if (a == "--size") {
            if (!value(&v) || !parse_size(v, &o.engine.width, &o.engine.height)) return *error = "--size WxH", false;
        } else if (a == "--render") {
            if (!value(&v) || !parse_size(v, &o.engine.internal_w, &o.engine.internal_h)) return *error = "--render WxH", false;
        } else if (a == "--rotate") {
            if (!value(&v) || !parse_rotation(v, &o.engine.rotation)) return *error = "--rotate 0|90|180|270", false;
        } else {
            rest.push_back(a);
        }
    }
    args = rest;
    return true;
}

void run_open(Engine& e, Game& game, const RunOptions& opt) {
    SignalGuard signals;
    const double step = 1.0 / std::max(opt.fixed_hz, 1.0);
    const bool simulated = opt.engine.headless;  // headless: one step per frame, reproducible
    struct Shutdown {
        Engine& e;
        Game& g;
        bool armed = true;
        ~Shutdown() {  // the game threw: still let it clean up, but don't throw while unwinding
            if (!armed) return;
            try {
                g.shutdown(e);
            } catch (...) {
            }
        }
    } shutdown{e, game};
    game.init(e);

    double acc = 0, last = now_ms() / 1000;
    // Stats over the last second (docs/stats-protocol.md).
    double window_start = now_ms(), update_ms = 0, render_ms = 0, present_ms = 0, worst_ms = 0, prev_end = 0;
    int frames_in_window = 0, steps_in_window = 0, dropped = 0;
    for (int n = 0; !g_stop && (opt.frames == 0 || n < opt.frames); n++) {
        if (!e.poll(true)) break;
        double t = now_ms() / 1000;
        // A gap longer than kPauseGap is the cart having been frozen (the system suspends carts with
        // SIGSTOP, and the Shell draws its own "paused" overlay) or a long stall: that time is dropped,
        // not caught up, so the game resumes exactly where it stopped. Shorter hitches catch up at most
        // max_steps steps.
        const double gap = t - last;
        if (simulated) acc += step;
        else if (gap > kPauseGap) dropped++;
        else acc += std::min(gap, step * opt.max_steps), dropped += gap > step * opt.max_steps;
        last = t;

        const double t0 = now_ms();
        int steps = 0;
        while (acc >= step - 1e-9 && steps < opt.max_steps) {
            e.world().begin_step();
            game.fixed_update(e, step);
            e.world().update_animators(float(step));
            e.world().update_particles(float(step));
            e.clear_pressed();
            acc -= step;
            steps++;
        }
        const double t1 = now_ms();
        // Rendering runs one step behind the logic and blends towards it: alpha 0 is the state before
        // the last step, 1 the state after it.
        const double alpha = simulated || !opt.interpolate ? 1.0 : std::clamp(acc / step, 0.0, 1.0);
        e.set_alpha(float(alpha));
        game.render(e, alpha);
        e.end_frame();
        const double t2 = now_ms();

        update_ms += t1 - t0;
        render_ms += t2 - t1 - e.timing().present_ms;
        present_ms += e.timing().present_ms;
        if (prev_end > 0) worst_ms = std::max(worst_ms, t2 - prev_end);
        prev_end = t2;
        frames_in_window++;
        steps_in_window += steps;
        if (opt.stats && t2 - window_start >= 1000) {
            const auto& s = e.stats();
            const double secs = (t2 - window_start) / 1000;
            const audio::DeviceStats a = e.audio_device().stats();
            std::printf("alia-stats {\"fps\": %.1f, \"steps\": %d, \"update_ms\": %.2f, \"render_ms\": %.2f, "
                        "\"present_ms\": %.2f, \"worst_frame_ms\": %.2f, \"late\": %d, \"instances\": %u, "
                        "\"culled\": %u, \"triangles\": %u, \"voices\": %u, \"audio_late\": %llu}\n",
                        frames_in_window / secs, steps_in_window, update_ms / frames_in_window,
                        render_ms / frames_in_window, present_ms / frames_in_window, worst_ms, dropped, s.instances,
                        s.culled, s.triangles, e.mixer().stats().voices, (unsigned long long)a.late);
            std::fflush(stdout);
            window_start = t2;
            update_ms = render_ms = present_ms = worst_ms = 0;
            frames_in_window = steps_in_window = dropped = 0;
        }
    }
    shutdown.armed = false;
    game.shutdown(e);
    if (!opt.screenshot.empty()) {
        std::string err;
        if (!e.screenshot(opt.screenshot, false, &err)) ALIA_WARN("%s", err.c_str());
    }
}

bool run(Game& game, const RunOptions& opt, std::string* error) {
    if (opt.input_log) setenv("ALIA_INPUT_LOG", "1", 1);  // read when the window opens
    Engine e;
    if (!e.open(opt.engine, error)) return false;
    run_open(e, game, opt);
    e.close();
    return true;
}

}  // namespace alia::runtime
