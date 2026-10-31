// L7: the alia_engine Python module (docs/architecture.md §2). A thin layer over
// runtime::Engine; geometry goes in as bytes (array('f', ...).tobytes(), struct.pack), and
// the frame is one draw list per end_frame(), so Python crosses into C++ a few dozen times
// per frame rather than per vertex.
// nanobind (Python.h) first: it defines feature macros the system headers must see.
#include <nanobind/nanobind.h>
#include <nanobind/stl/array.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include <cstring>
#include <stdexcept>

#include "core/log.hpp"
#include "runtime/app.hpp"
#include "runtime/engine.hpp"
#include "runtime/locale.hpp"
#include "runtime/saves.hpp"
#include "world/rig.hpp"

namespace nb = nanobind;
using namespace nb::literals;
using namespace alia;

namespace {

template <class T>
std::span<const T> view(const nb::bytes& b, const char* what) {
    if (b.size() % sizeof(T)) throw nb::value_error((std::string(what) + ": size is not a multiple of the element").c_str());
    return {reinterpret_cast<const T*>(b.data()), b.size() / sizeof(T)};
}

struct Mesh {
    nb::bytes positions, indices;
    std::optional<nb::bytes> tri_palette, vertex_rgb;
    bool double_sided = false;
    std::vector<render::Submesh> submeshes;

    render::MeshDesc desc() const {
        render::MeshDesc d;
        d.positions = view<float>(positions, "positions");
        d.indices = view<uint16_t>(indices, "indices");
        if (tri_palette) d.tri_palette = view<uint8_t>(*tri_palette, "tri_palette");
        if (vertex_rgb) d.vertex_rgb = view<uint8_t>(*vertex_rgb, "vertex_rgb");
        d.double_sided = double_sided;
        d.submeshes = submeshes;
        return d;
    }
};

// The platform's rules, from the Alia Pad SDK (its Python package `alia`: on the handheld, in the simulator, and in
// `alia run`'s layout): [audio] for the output stream, [keyboard] for the PC keyboard (platform/keymap.hpp's format).
// Without the SDK (or an older one without these), the engine's defaults; a bad value is skipped with a warning.
std::string names(nb::handle v) {
    if (nb::isinstance<nb::str>(v)) return nb::cast<std::string>(v);
    std::string out;
    for (nb::handle k : v) out += (out.empty() ? "" : ",") + nb::cast<std::string>(k);
    return out;
}

void platform_rules(runtime::Config& c) {
    nb::object rt;
    try {
        rt = nb::module_::import_("alia.runtime");
    } catch (nb::python_error&) {
        ALIA_INFO("no Alia Pad SDK (alia.runtime): the engine's defaults for sound and the keyboard");
        return;
    }
    try {
        if (nb::hasattr(rt, "audio")) {
            nb::dict a = nb::cast<nb::dict>(rt.attr("audio")());
            if (a.contains("game_quantum")) c.audio_frames = nb::cast<int>(a["game_quantum"]);
            if (a.contains("role")) c.audio_role = nb::cast<std::string>(a["role"]);
        }
        if (nb::hasattr(rt, "keyboard")) {
            nb::dict k = nb::cast<nb::dict>(rt.attr("keyboard")());
            std::string map;
            auto add = [&](const std::string& name, nb::handle keys) { map += (map.empty() ? "" : ";") + name + "=" + names(keys); };
            for (const char* b : {"south", "east", "west", "north", "l1", "r1", "l2", "r2", "l3", "r3", "select", "start",
                                  "up", "down", "left", "right"})
                if (k.contains(b)) add(b, k[b]);
            for (auto [stick, prefix] : {std::pair{"left_stick", "l"}, std::pair{"right_stick", "r"}}) {
                if (!k.contains(stick)) continue;
                nb::dict d = nb::cast<nb::dict>(k[stick]);
                for (auto [dir, axis] : {std::pair{"left", "x-"}, std::pair{"right", "x+"}, std::pair{"up", "y-"}, std::pair{"down", "y+"}})
                    if (d.contains(dir)) add(std::string(prefix) + axis, d[dir]);
            }
            if (k.contains("half_tilt")) add("half", k["half_tilt"]);
            c.keymap = map;
        }
    } catch (const std::exception& e) {
        ALIA_WARN("the Alia Pad SDK's alia.runtime: %s; the engine's defaults where it failed", e.what());
    }
}

class PyEngine {
public:
    explicit PyEngine(runtime::Config c) {
        platform_rules(c);
        std::string err;
        if (!e_.open(c, &err)) throw std::runtime_error("alia_engine: " + err);
    }

    PyEngine(const std::string& title, const std::string& app_id, std::pair<int, int> size,
             std::pair<int, int> internal, int rotate, bool headless, bool vsync) {
        runtime::Config c;
        c.title = title;
        c.app_id = app_id;
        c.width = size.first;
        c.height = size.second;
        c.internal_w = internal.first;
        c.internal_h = internal.second;
        c.rotation = rotate;
        c.headless = headless;
        c.vsync = vsync;
        platform_rules(c);
        std::string err;
        if (!e_.open(c, &err)) throw std::runtime_error("alia_engine: " + err);
    }

    runtime::Engine& get() {
        if (!e_.is_open()) throw std::runtime_error("alia_engine: the engine is closed");
        return e_;
    }

    uint32_t create_material(const render::Material& m) {
        std::string err;
        uint32_t id = get().create_material(m, &err);
        if (!id) throw nb::value_error(err.c_str());
        return id;
    }

    uint32_t create_mesh(const Mesh& m) {
        std::string err;
        uint32_t id = get().create_mesh(m.desc(), &err);
        if (!id) throw nb::value_error(err.c_str());
        return id;
    }

    void update_mesh(uint32_t id, const Mesh& m) {
        std::string err;
        if (!get().update_mesh(id, m.desc(), &err)) throw nb::value_error(err.c_str());
    }

    runtime::Engine e_;
};

// A Python object with init / fixed_update / render / shutdown (init and shutdown optional).
class PyGame final : public runtime::Game {
public:
    PyGame(nb::object game, nb::object engine) : g_(std::move(game)), e_(std::move(engine)) {}
    void init(runtime::Engine&) override { call("init"); }
    void fixed_update(runtime::Engine&, double dt) override { g_.attr("fixed_update")(e_, dt); }
    void render(runtime::Engine&, double alpha) override { g_.attr("render")(e_, alpha); }
    void shutdown(runtime::Engine&) override { call("shutdown"); }

private:
    void call(const char* name) {
        if (nb::hasattr(g_, name)) g_.attr(name)(e_);
    }
    nb::object g_, e_;
};

// World handles (docs/api.md §3): a Python object holds the engine object (keeps it alive) and an
// entity id; using a destroyed one raises.
struct PyNode {
    nb::object engine_obj;
    world::Entity entity;

    world::World& w() const {
        PyEngine& e = nb::cast<PyEngine&>(engine_obj);
        world::World& wd = e.get().world();
        if (!wd.valid(entity)) throw std::runtime_error("alia_engine: this node was destroyed");
        return wd;
    }
    world::TransformComponent& t() const { return w().transform(entity); }
    world::MeshRendererComponent& r() const { return w().renderer(entity); }
};

struct PyCamera : PyNode {};

struct PyRigNode : PyNode {
    world::AnimatorComponent& a() const {
        world::AnimatorComponent* an = w().animator(entity);
        if (!an) throw std::runtime_error("alia_engine: not a rig");
        return *an;
    }
};

struct PyAnimator {
    PyRigNode node;
};

int bone_index(const asset::RigData& rig, nb::handle which) {
    if (nb::isinstance<nb::str>(which)) {
        std::string name = nb::cast<std::string>(which);
        for (size_t i = 0; i < rig.bones.size(); i++)
            if (rig.bones[i].name == name) return int(i);
        throw nb::value_error(("alia_engine: no bone " + name).c_str());
    }
    int i = nb::cast<int>(which);
    if (i < 0 || i >= int(rig.bones.size())) throw nb::value_error("alia_engine: no such bone");
    return i;
}

struct PyIk {  // one IK constraint on a rig (docs/api.md §4.1)
    PyRigNode node;
    int id;
    world::IkConstraint& c() const {
        for (auto& k : node.a().ik)
            if (k.id == id) return k;
        throw std::runtime_error("alia_engine: this constraint was removed");
    }
};

// A target: a node (followed) or a point.
void set_target(world::IkConstraint& c, nb::handle target) {
    if (nb::isinstance<PyNode>(target)) {
        c.target = nb::cast<PyNode&>(target).entity;
        return;
    }
    auto p = nb::cast<std::array<float, 3>>(target);
    c.target = entt::null;
    c.point = {p[0], p[1], p[2]};
}

struct PyRig {
    nb::object engine_obj;
    uint32_t id;
    const asset::RigData& data() const { return *nb::cast<PyEngine&>(engine_obj).get().world().rig(id); }
    int anim_index(nb::handle anim) const {
        const auto& anims = data().anims;
        if (nb::isinstance<nb::str>(anim)) {
            std::string name = nb::cast<std::string>(anim);
            for (size_t i = 0; i < anims.size(); i++)
                if (anims[i].name == name) return int(i);
            throw nb::value_error(("alia_engine: no animation " + name).c_str());
        }
        int i = nb::cast<int>(anim);
        if (i < 0 || i >= int(anims.size())) throw nb::value_error("alia_engine: no such animation");
        return i;
    }
};

nb::dict event_dict(const asset::RigData& rig, int anim, int index) {
    const asset::RigEventData& ev = rig.anims[size_t(anim)].events[size_t(index)];
    nb::dict d;
    d["type"] = ev.type;
    d["phase"] = ev.phase;
    d["anim"] = rig.anims[size_t(anim)].name;
    for (auto& [k, v] : ev.strings) d[k.c_str()] = v;
    for (auto& [k, v] : ev.numbers) d[k.c_str()] = v;
    return d;
}

void for_subtree(world::World& w, world::Entity e, const std::function<void(world::Entity)>& f) {
    f(e);
    for (world::Entity c : w.registry().get<world::HierarchyComponent>(e).children) for_subtree(w, c, f);
}

struct PyWorld {
    nb::object engine_obj;
    world::World& w() const { return nb::cast<PyEngine&>(engine_obj).get().world(); }
};

glm::vec3 vec3_of(nb::handle v, const char* what) {
    if (nb::isinstance<nb::float_>(v) || nb::isinstance<nb::int_>(v)) {
        float f = nb::cast<float>(v);
        return glm::vec3(f);
    }
    auto a = nb::cast<std::array<float, 3>>(v);
    (void)what;
    return {a[0], a[1], a[2]};
}

std::tuple<float, float, float> tuple3(const glm::vec3& v) { return {v.x, v.y, v.z}; }

// Particles (docs/api.md §6): a template made by Engine.particles(), emitters are nodes.
struct PyParticles {
    nb::object engine_obj;
    uint32_t id;
};

struct PyEmitter : PyNode {
    world::EmitterComponent& em() const {
        world::EmitterComponent* e = w().emitter(entity);
        if (!e) throw std::runtime_error("alia_engine: not an emitter");
        return *e;
    }
};

world::Range range_of(nb::handle v) {  // a number, or (lo, hi)
    if (nb::isinstance<nb::float_>(v) || nb::isinstance<nb::int_>(v)) {
        float f = nb::cast<float>(v);
        return {f, f};
    }
    auto p = nb::cast<std::pair<float, float>>(v);
    return {p.first, p.second};
}

std::vector<world::Key> keys_of(nb::handle v) {  // [(t, value), ...]
    std::vector<world::Key> out;
    if (v.is_none()) return out;
    for (auto [t, value] : nb::cast<std::vector<std::pair<float, float>>>(v)) out.push_back({t, value});
    return out;
}

// Input actions (docs/api.md §7): button names as ae exports them, for bindings() / load().
constexpr std::pair<const char*, uint32_t> kButtonNames[] = {
    {"SOUTH", platform::kSouth}, {"EAST", platform::kEast}, {"WEST", platform::kWest}, {"NORTH", platform::kNorth},
    {"L1", platform::kL1}, {"R1", platform::kR1}, {"L2", platform::kL2}, {"R2", platform::kR2},
    {"L3", platform::kL3}, {"R3", platform::kR3}, {"SELECT", platform::kSelect}, {"START", platform::kStart},
    {"UP", platform::kUp}, {"DOWN", platform::kDown}, {"LEFT", platform::kLeft}, {"RIGHT", platform::kRight}};

uint32_t button_of(nb::handle b) {  // ae.SOUTH (a number) or "SOUTH"
    if (nb::isinstance<nb::str>(b)) {
        std::string n = nb::cast<std::string>(b);
        for (auto& [name, bit] : kButtonNames)
            if (n == name) return bit;
        throw nb::value_error(("alia_engine: no button " + n).c_str());
    }
    return nb::cast<uint32_t>(b);
}

struct PyActions {
    nb::object engine_obj;
    runtime::ActionMap& map() const { return nb::cast<PyEngine&>(engine_obj).get().actions(); }
    const platform::InputState& in() const { return nb::cast<PyEngine&>(engine_obj).get().input(); }
};

[[noreturn]] void raise_errno(int r, const std::string& what) {
    PyErr_SetObject(PyExc_OSError, nb::make_tuple(-r, std::string(std::strerror(-r)) + ": " + what).ptr());
    throw nb::python_error();
}

template <class E>
E choice(const std::string& name, std::initializer_list<std::pair<const char*, E>> options, const char* what) {
    for (auto& [n, e] : options)
        if (name == n) return e;
    throw nb::value_error(("alia_engine: no " + std::string(what) + " " + name).c_str());
}

// Sound (docs/api.md §7).
struct PyAudio {
    nb::object engine_obj;
    runtime::Engine& e() const { return nb::cast<PyEngine&>(engine_obj).get(); }
};
struct PySound {
    nb::object engine_obj;
    uint32_t id;
};
struct PyVoice {
    nb::object engine_obj;
    uint32_t id;
    audio::Mixer& m() const { return nb::cast<PyEngine&>(engine_obj).get().mixer(); }
};

audio::PlayParams play_params(float volume, float pan, float pitch, bool loop, double loop_start, double end, float fade_in,
                              const std::string& bus) {
    audio::PlayParams p;
    p.volume = volume, p.pan = pan, p.pitch = pitch, p.loop = loop, p.loop_start = loop_start, p.end = end, p.fade_in = fade_in;
    p.bus = choice<audio::Bus>(bus, {{"effects", audio::Bus::Effects}, {"music", audio::Bus::Music}}, "bus");
    return p;
}

}  // namespace

NB_MODULE(alia_engine, m) {
    m.doc() = "Alia Pad engine: window and input, a handheld 3D pipeline (palette meshes, banded fog, HUD).";
    m.attr("__version__") = ALIA_VERSION;  // CMakeLists.txt project(VERSION)

    // Positional buttons (platform/input.hpp). Alia Pad labels: A = EAST, B = SOUTH, X = NORTH, Y = WEST.
    m.attr("SOUTH") = uint32_t(platform::kSouth);
    m.attr("EAST") = uint32_t(platform::kEast);
    m.attr("WEST") = uint32_t(platform::kWest);
    m.attr("NORTH") = uint32_t(platform::kNorth);
    m.attr("L1") = uint32_t(platform::kL1);
    m.attr("R1") = uint32_t(platform::kR1);
    m.attr("L2") = uint32_t(platform::kL2);
    m.attr("R2") = uint32_t(platform::kR2);
    m.attr("L3") = uint32_t(platform::kL3);
    m.attr("R3") = uint32_t(platform::kR3);
    m.attr("SELECT") = uint32_t(platform::kSelect);
    m.attr("START") = uint32_t(platform::kStart);
    m.attr("UP") = uint32_t(platform::kUp);
    m.attr("DOWN") = uint32_t(platform::kDown);
    m.attr("LEFT") = uint32_t(platform::kLeft);
    m.attr("RIGHT") = uint32_t(platform::kRight);

    nb::enum_<render::Light>(m, "Light")
        .value("SUN", render::Light::Sun)
        .value("CHARACTER", render::Light::Character)
        .value("UNLIT", render::Light::Unlit)
        .value("MATERIAL", render::Light::Material);
    nb::enum_<render::Shading>(m, "Shading")
        .value("FLAT", render::Shading::Flat)
        .value("SMOOTH", render::Shading::Smooth)
        .value("UNLIT", render::Shading::Unlit);
    nb::enum_<render::ColorSource>(m, "ColorSource")
        .value("PALETTE", render::ColorSource::Palette)
        .value("VERTEX", render::ColorSource::Vertex);
    nb::enum_<render::AlphaMode>(m, "AlphaMode")
        .value("OPAQUE", render::AlphaMode::Opaque)
        .value("BLEND", render::AlphaMode::Blend);

    nb::class_<render::Material>(m, "Material",
                                 "The handheld shading model (docs/api.md §2). shading, color and palette_index are "
                                 "baked into a mesh when it is built; the rest applies at every draw.")
        .def(
            "__init__",
            [](render::Material* self, render::Shading shading, render::ColorSource color, uint8_t palette_index,
               render::Light light, float ambient, float diffuse, float emissive, render::AlphaMode alpha_mode,
               float opacity, bool double_sided, bool fog, bool ground_fog) {
                new (self) render::Material{shading, color,      palette_index, light,        ambient, diffuse,
                                            emissive, alpha_mode, opacity,      double_sided, fog,     ground_fog};
            },
            "shading"_a = render::Shading::Flat, "color"_a = render::ColorSource::Palette, "palette_index"_a = 0,
            "light"_a = render::Light::Sun, "ambient"_a = -1.0f, "diffuse"_a = -1.0f, "emissive"_a = 0.0f,
            "alpha_mode"_a = render::AlphaMode::Opaque, "opacity"_a = 1.0f, "double_sided"_a = false, "fog"_a = true,
            "ground_fog"_a = true)
        .def_rw("shading", &render::Material::shading)
        .def_rw("color", &render::Material::color)
        .def_rw("palette_index", &render::Material::palette_index)
        .def_rw("light", &render::Material::light)
        .def_rw("ambient", &render::Material::ambient)
        .def_rw("diffuse", &render::Material::diffuse)
        .def_rw("emissive", &render::Material::emissive)
        .def_rw("alpha_mode", &render::Material::alpha_mode)
        .def_rw("opacity", &render::Material::opacity)
        .def_rw("double_sided", &render::Material::double_sided)
        .def_rw("fog", &render::Material::fog)
        .def_rw("ground_fog", &render::Material::ground_fog);

    nb::class_<platform::InputState>(m, "Input")
        .def_ro("gamepad", &platform::InputState::gamepad)
        .def_ro("held", &platform::InputState::held)
        .def_ro("pressed", &platform::InputState::pressed)
        .def_ro("lx", &platform::InputState::lx)
        .def_ro("ly", &platform::InputState::ly)
        .def_ro("rx", &platform::InputState::rx)
        .def_ro("ry", &platform::InputState::ry)
        .def_ro("lt", &platform::InputState::lt)
        .def_ro("rt", &platform::InputState::rt);

    nb::class_<render::DirLight>(m, "DirLight")
        .def_rw("travel", &render::DirLight::travel)
        .def_rw("ambient", &render::DirLight::ambient)
        .def_rw("diffuse", &render::DirLight::diffuse);

    // Arrays come back as copies: assign whole tuples (f.eye = (x, y, z)), not elements.
    nb::class_<render::FrameDesc>(m, "Frame")
        .def_rw("eye", &render::FrameDesc::eye)
        .def_rw("yaw", &render::FrameDesc::yaw)
        .def_rw("pitch", &render::FrameDesc::pitch)
        .def_rw("fov_x", &render::FrameDesc::fov_x)
        .def_rw("near", &render::FrameDesc::near_plane)
        .def_rw("far", &render::FrameDesc::far_plane)
        .def_rw("sky_top", &render::FrameDesc::sky_top)
        .def_rw("sky_horizon", &render::FrameDesc::sky_horizon)
        .def_rw("sky_span", &render::FrameDesc::sky_span)
        .def_rw("fog_color", &render::FrameDesc::fog_color)
        .def_rw("fog_near", &render::FrameDesc::fog_near)
        .def_rw("fog_full", &render::FrameDesc::fog_full)
        .def_rw("fog_steps", &render::FrameDesc::fog_steps)
        .def_rw("ground_fog", &render::FrameDesc::ground_fog)
        .def_rw("ground_top", &render::FrameDesc::ground_top)
        .def_rw("ground_ramp", &render::FrameDesc::ground_ramp)
        .def_rw("sun", &render::FrameDesc::sun)
        .def_rw("character", &render::FrameDesc::character)
        .def_rw("overlay", &render::FrameDesc::overlay);

    nb::class_<Mesh>(m, "Mesh",
                     "Geometry: positions (float32 xyz), indices (uint16 triangles, CCW front faces), the colours "
                     "materials can use (tri_palette: uint8 per triangle; vertex_rgb: 3 x uint8 per vertex) and "
                     "submeshes [(first_tri, tri_count, material id)]. Without submeshes: one part with the "
                     "default material (tri_palette -> flat palette colours, else smooth vertex colours).")
        .def(
            "__init__",
            [](Mesh* self, nb::bytes positions, nb::bytes indices, std::optional<nb::bytes> tri_palette,
               std::optional<nb::bytes> vertex_rgb, bool double_sided,
               const std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>& submeshes) {
                new (self) Mesh{positions, indices, tri_palette, vertex_rgb, double_sided, {}};
                for (auto [first, count, material] : submeshes) self->submeshes.push_back({first, count, material});
            },
            "positions"_a, "indices"_a, "tri_palette"_a = nb::none(), "vertex_rgb"_a = nb::none(),
            "double_sided"_a = false, "submeshes"_a = std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>{});

    m.def(
        "run",
        [](nb::object game, std::optional<std::vector<std::string>> argv, const std::string& title, double fixed_hz,
           const std::string& app_id) {
            runtime::RunOptions o = runtime::options_from_environment(title, app_id);
            o.fixed_hz = fixed_hz;
            if (argv) {
                std::vector<std::string> args = *argv;
                std::string err;
                if (!runtime::apply_flags(o, args, &err)) throw nb::value_error(err.c_str());
            }
            if (o.input_log) setenv("ALIA_INPUT_LOG", "1", 1);  // read when the window opens
            nb::object engine = nb::cast(new PyEngine(o.engine), nb::rv_policy::take_ownership);
            PyGame adapter(game, engine);
            runtime::run_open(nb::cast<PyEngine&>(engine).e_, adapter, o);
            nb::cast<PyEngine&>(engine).e_.close();
        },
        "game"_a, "argv"_a = nb::none(), "title"_a = "alia-engine", "fixed_hz"_a = 60.0, "app_id"_a = "pad.alia.engine",
        "Run a game: an object with init(engine), fixed_update(engine, dt), render(engine, alpha) and "
        "shutdown(engine). The runtime owns the loop and the display (architecture §2.1); argv: the "
        "common flags (--headless, --frames N, --screenshot PATH, --stats, --input-log, --size, --render, "
        "--rotate) are taken from it, anything else is the game's.");

    m.def("strip_runtime_flags",
          [](std::vector<std::string> args) {
              runtime::RunOptions o;
              std::string err;
              if (!runtime::apply_flags(o, args, &err)) throw nb::value_error(err.c_str());
              return args;
          },
          "args"_a, "The arguments that are left for the game once the runtime's common flags are taken out.");

    // ---- the world (docs/api.md §3) ----
    auto ypr = [](PyNode& n) {
        float y, p, r;
        world::to_ypr(n.t().rotation, &y, &p, &r);
        return std::array<float, 3>{y, p, r};
    };
    nb::class_<PyNode>(m, "Node",
                       "A persistent object in the world: create with world.spawn(), change what changes, "
                       "destroy() when done. Positions are relative to the parent.")
        .def_prop_rw("position", [](PyNode& n) { return tuple3(n.t().position); },
                     [](PyNode& n, std::array<float, 3> v) { n.t().position = {v[0], v[1], v[2]}; })
        .def_prop_rw("rotation",
                     [](PyNode& n) {
                         const glm::quat& q = n.t().rotation;
                         return std::tuple<float, float, float, float>{q.x, q.y, q.z, q.w};
                     },
                     [](PyNode& n, std::array<float, 4> q) { n.t().rotation = glm::normalize(glm::quat(q[3], q[0], q[1], q[2])); },
                     "Quaternion (x, y, z, w).")
        .def_prop_rw("yaw", [ypr](PyNode& n) { return ypr(n)[0]; },
                     [ypr](PyNode& n, float v) {
                         auto a = ypr(n);
                         n.t().rotation = world::from_ypr(v, a[1], a[2]);
                     },
                     "About +Y (radians); yaw 0 faces +Z.")
        .def_prop_rw("pitch", [ypr](PyNode& n) { return ypr(n)[1]; },
                     [ypr](PyNode& n, float v) {
                         auto a = ypr(n);
                         n.t().rotation = world::from_ypr(a[0], v, a[2]);
                     },
                     "About +X after the yaw; positive tips the +Z nose down (like Megalith's objects).")
        .def_prop_rw("roll", [ypr](PyNode& n) { return ypr(n)[2]; },
                     [ypr](PyNode& n, float v) {
                         auto a = ypr(n);
                         n.t().rotation = world::from_ypr(a[0], a[1], v);
                     })
        .def("set_rotation", [](PyNode& n, float yaw, float pitch, float roll) { n.t().rotation = world::from_ypr(yaw, pitch, roll); },
             "yaw"_a, "pitch"_a = 0.0f, "roll"_a = 0.0f, "Ry(yaw) * Rx(pitch) * Rz(roll) in one go.")
        .def_prop_rw("scale", [](PyNode& n) { return tuple3(n.t().scale); },
                     [](PyNode& n, nb::object v) { n.t().scale = vec3_of(v, "scale"); }, "A number or (x, y, z).")
        .def_prop_rw("mesh", [](PyNode& n) { return n.r().mesh; }, [](PyNode& n, uint32_t v) { n.r().mesh = v; })
        .def_prop_rw("visible", [](PyNode& n) { return n.r().visible; }, [](PyNode& n, bool v) { n.r().visible = v; })
        .def_prop_rw("tint", [](PyNode& n) { return n.r().tint; }, [](PyNode& n, uint8_t v) { n.r().tint = v; })
        .def_prop_rw("fade", [](PyNode& n) { return n.r().fade; }, [](PyNode& n, float v) { n.r().fade = v; })
        .def_prop_rw("light", [](PyNode& n) { return n.r().light; }, [](PyNode& n, render::Light v) { n.r().light = v; })
        .def_prop_rw("ground_fog", [](PyNode& n) { return n.r().ground_fog; },
                     [](PyNode& n, bool v) { n.r().ground_fog = v; })
        .def_prop_rw("name", [](PyNode& n) { return n.w().tag(n.entity).name; },
                     [](PyNode& n, std::string v) { n.w().tag(n.entity).name = std::move(v); })
        .def_prop_rw(
            "parent",
            [](PyNode& n) -> nb::object {
                world::Entity p = n.w().parent(n.entity);
                if (p == entt::null) return nb::none();
                return nb::cast(PyNode{n.engine_obj, p});
            },
            [](PyNode& n, std::optional<PyNode*> p) {
                world::Entity pe = p && *p ? (*p)->entity : world::Entity(entt::null);
                if (!n.w().set_parent(n.entity, pe)) throw nb::value_error("alia_engine: that parent would make a cycle");
            },
            nb::arg("parent").none(), "Another node, or None (keeps the local transform: see set_parent).")
        .def(
            "set_parent",
            [](PyNode& n, std::optional<PyNode*> p, bool keep_world) {
                world::Entity pe = p && *p ? (*p)->entity : world::Entity(entt::null);
                if (!n.w().set_parent(n.entity, pe, keep_world))
                    throw nb::value_error("alia_engine: that parent would make a cycle");
            },
            nb::arg("parent").none(), "keep_world"_a = false,
            "Attach to another node (or None: the world root). keep_world: stay where it is in the world (and "
            "keep interpolating); otherwise keep the local transform and move with the new parent.")
        .def_prop_rw("active", [](PyNode& n) { return n.w().active(n.entity); },
                     [](PyNode& n, bool v) { n.w().set_active(n.entity, v); },
                     "Its own flag. Off: it and its children leave the world (not drawn, animators frozen, "
                     "colliders off); children keep their own flags.")
        .def_prop_ro("active_in_hierarchy", [](PyNode& n) { return n.w().active_in_hierarchy(n.entity); },
                     "In the world: it and every ancestor are active.")
        .def_prop_ro("world_position", [](PyNode& n) {
            glm::mat4 m = n.w().world_matrix(n.entity);
            return tuple3(glm::vec3(m[3]));
        })
        .def_prop_ro("alive", [](PyNode& n) { return nb::cast<PyEngine&>(n.engine_obj).get().world().valid(n.entity); })
        .def_prop_rw(
            "collider",
            [](PyNode& n) -> nb::object {
                auto* c = n.w().registry().try_get<world::ColliderComponent>(n.entity);
                return c ? nb::cast(*c) : nb::none();
            },
            [](PyNode& n, std::optional<world::ColliderComponent> c) {
                if (c) n.w().registry().emplace_or_replace<world::ColliderComponent>(n.entity, *c);
                else n.w().registry().remove<world::ColliderComponent>(n.entity);
            },
            nb::arg("collider").none(), "ae.sphere(...) / ae.capsule(...) in the node's frame, or None (a copy when read).")
        .def_prop_rw(
            "collider_enabled",
            [](PyNode& n) {
                auto* c = n.w().registry().try_get<world::ColliderComponent>(n.entity);
                return c && c->enabled;
            },
            [](PyNode& n, bool v) {
                if (auto* c = n.w().registry().try_get<world::ColliderComponent>(n.entity)) c->enabled = v;
            })
        .def("teleport", [](PyNode& n) { for_subtree(n.w(), n.entity, [&](world::Entity e) { n.w().teleport(e); }); },
             "Don't blend from the previous step's transform, for it and its descendants (after a jump, so "
             "it doesn't slide there).")
        .def(
            "set_subtree",
            [](PyNode& n, std::optional<uint8_t> tint, std::optional<float> fade, std::optional<render::Light> light,
               std::optional<bool> visible, std::optional<bool> ground_fog) {
                world::World& w = n.w();
                for_subtree(w, n.entity, [&](world::Entity e) {
                    if (!w.has_renderer(e)) return;
                    auto& r = w.renderer(e);
                    if (tint) r.tint = *tint;
                    if (fade) r.fade = *fade;
                    if (light) r.light = *light;
                    if (visible) r.visible = *visible;
                    if (ground_fog) r.ground_fog = *ground_fog;
                });
            },
            "tint"_a = nb::none(), "fade"_a = nb::none(), "light"_a = nb::none(), "visible"_a = nb::none(),
            "ground_fog"_a = nb::none(), "Set these on it and every descendant that draws (a whole rig: fade, light ...).")
        .def("destroy", [](PyNode& n) { n.w().destroy(n.entity); }, "Remove it and its children.")
        .def("__eq__", [](PyNode& a, PyNode& b) { return a.entity == b.entity; })
        .def("__hash__", [](PyNode& n) { return uint32_t(entt::to_integral(n.entity)); });

    nb::class_<PyCamera, PyNode>(m, "Camera",
                                 "The world's camera: a node (so it can have a parent). Once the game touches "
                                 "world.camera it drives the frame's eye, yaw, pitch, fov_x, near and far.")
        .def_prop_rw("yaw", [ypr](PyCamera& c) { return ypr(c)[0]; },
                     [ypr](PyCamera& c, float v) { c.t().rotation = world::from_ypr(v, ypr(c)[1], 0); },
                     "yaw 0 faces +Z.")
        .def_prop_rw("pitch", [ypr](PyCamera& c) { return -ypr(c)[1]; },
                     [ypr](PyCamera& c, float v) { c.t().rotation = world::from_ypr(ypr(c)[0], -v, 0); },
                     "Positive looks up (like the frame's pitch).")
        .def_prop_rw("fov_x", [](PyCamera& c) { return c.w().registry().get<world::CameraComponent>(c.entity).fov_x; },
                     [](PyCamera& c, float v) { c.w().registry().get<world::CameraComponent>(c.entity).fov_x = v; })
        .def_prop_rw("near", [](PyCamera& c) { return c.w().registry().get<world::CameraComponent>(c.entity).near_plane; },
                     [](PyCamera& c, float v) { c.w().registry().get<world::CameraComponent>(c.entity).near_plane = v; })
        .def_prop_rw("far", [](PyCamera& c) { return c.w().registry().get<world::CameraComponent>(c.entity).far_plane; },
                     [](PyCamera& c, float v) { c.w().registry().get<world::CameraComponent>(c.entity).far_plane = v; });

    // ---- collision (docs/api.md §5) ----
    nb::enum_<world::ColliderComponent::Shape>(m, "Shape")
        .value("SPHERE", world::ColliderComponent::Sphere)
        .value("CAPSULE", world::ColliderComponent::Capsule);
    nb::class_<world::ColliderComponent>(m, "Collider", "A sphere (centre a) or capsule (a - b) in its node's frame.")
        .def_rw("shape", &world::ColliderComponent::shape)
        .def_prop_rw("a", [](world::ColliderComponent& c) { return tuple3(c.a); },
                     [](world::ColliderComponent& c, std::array<float, 3> v) { c.a = {v[0], v[1], v[2]}; })
        .def_prop_rw("b", [](world::ColliderComponent& c) { return tuple3(c.b); },
                     [](world::ColliderComponent& c, std::array<float, 3> v) { c.b = {v[0], v[1], v[2]}; })
        .def_rw("r", &world::ColliderComponent::r)
        .def_rw("layers", &world::ColliderComponent::layers)
        .def_rw("user", &world::ColliderComponent::user)
        .def_rw("priority", &world::ColliderComponent::priority)
        .def_rw("enabled", &world::ColliderComponent::enabled);
    m.def("sphere",
          [](std::array<float, 3> c, float r, uint32_t layers, uint32_t user, int priority) {
              return world::ColliderComponent{world::ColliderComponent::Sphere, {c[0], c[1], c[2]}, {c[0], c[1], c[2]}, r,
                                              layers, user, priority, true};
          },
          "centre"_a, "r"_a, "layers"_a = 1u, "user"_a = 0u, "priority"_a = 0);
    m.def("capsule",
          [](std::array<float, 3> a, std::array<float, 3> b, float r, uint32_t layers, uint32_t user, int priority) {
              return world::ColliderComponent{world::ColliderComponent::Capsule, {a[0], a[1], a[2]}, {b[0], b[1], b[2]}, r,
                                              layers, user, priority, true};
          },
          "a"_a, "b"_a, "r"_a, "layers"_a = 1u, "user"_a = 0u, "priority"_a = 0);

    struct PyHit {
        PyNode node;
        float distance;
        std::tuple<float, float, float> point;
        uint32_t layers, user;
    };
    nb::class_<PyHit>(m, "Hit")
        .def_ro("node", &PyHit::node)
        .def_ro("distance", &PyHit::distance)
        .def_ro("point", &PyHit::point)
        .def_ro("layers", &PyHit::layers)
        .def_ro("user", &PyHit::user)
        .def("__repr__", [](PyHit& h) { return "Hit(distance=" + std::to_string(h.distance) + ", user=" + std::to_string(h.user) + ")"; });
    struct PyWorldCollider {
        PyNode node;
        world::ColliderComponent::Shape shape;
        std::tuple<float, float, float> a, b;
        float r;
        uint32_t layers, user;
        int priority;
    };
    nb::class_<PyWorldCollider>(m, "WorldCollider", "A collider in world space (world.colliders()).")
        .def_ro("node", &PyWorldCollider::node)
        .def_ro("shape", &PyWorldCollider::shape)
        .def_ro("a", &PyWorldCollider::a)
        .def_ro("b", &PyWorldCollider::b)
        .def_ro("r", &PyWorldCollider::r)
        .def_ro("layers", &PyWorldCollider::layers)
        .def_ro("user", &PyWorldCollider::user)
        .def_ro("priority", &PyWorldCollider::priority)
        .def_prop_ro("centre", [](PyWorldCollider& c) {
            auto [ax, ay, az] = c.a;
            auto [bx, by, bz] = c.b;
            return std::tuple<float, float, float>{(ax + bx) / 2, (ay + by) / 2, (az + bz) / 2};
        });

    nb::class_<PyRig>(m, "Rig", "A rigid rig loaded with engine.load_rigs() (docs/api.md §4).")
        .def_prop_ro("name", [](PyRig& r) { return r.data().name; })
        .def_prop_ro("anims", [](PyRig& r) {
            std::vector<std::string> v;
            for (const auto& a : r.data().anims) v.push_back(a.name);
            return v;
        })
        .def_prop_ro("bones", [](PyRig& r) {
            std::vector<std::string> v;
            for (const auto& b : r.data().bones) v.push_back(b.name);
            return v;
        })
        .def_prop_ro("slots", [](PyRig& r) { return r.data().slots.size(); })
        .def_prop_ro("attach", [](PyRig& r) {
            std::vector<std::string> v;
            for (const auto& a : r.data().attach) v.push_back(a.name);
            return v;
        })
        .def("anim_index", [](PyRig& r, nb::handle a) { return r.anim_index(a); }, "anim"_a)
        .def("events", [](PyRig& r, nb::handle a) {
            int i = r.anim_index(a);
            nb::list out;
            for (size_t k = 0; k < r.data().anims[size_t(i)].events.size(); k++) out.append(event_dict(r.data(), i, int(k)));
            return out;
        }, "anim"_a)
        .def("pose",
             [](PyRig& r, nb::handle a, float phase) {
                 std::vector<glm::mat4> rig_space;
                 world::pose_rig_space(r.data(), r.anim_index(a), phase, rig_space);
                 nb::list out;
                 for (const glm::mat4& w : rig_space) {
                     out.append(nb::make_tuple(w[0][0], w[1][0], w[2][0], w[3][0], w[0][1], w[1][1], w[2][1], w[3][1],
                                               w[0][2], w[1][2], w[2][2], w[3][2]));
                 }
                 return out;
             },
             "anim"_a, "phase"_a,
             "Rig-space transform of every bone, row-major 3x4 (12 numbers): the C++ pose, for tests and for "
             "games that still compute colliders themselves.")
        .def("attach_position",
             [](PyRig& r, const std::string& name, nb::handle a, float phase) {
                 const asset::RigData& d = r.data();
                 auto p = std::find_if(d.attach.begin(), d.attach.end(), [&](const auto& x) { return x.name == name; });
                 if (p == d.attach.end()) throw nb::value_error(("alia_engine: no attach point " + name).c_str());
                 std::vector<glm::mat4> rig_space;
                 world::pose_rig_space(d, r.anim_index(a), phase, rig_space);
                 return tuple3(glm::vec3(rig_space[size_t(p->bone)] * glm::vec4(p->offset[0], p->offset[1], p->offset[2], 1)));
             },
             "name"_a, "anim"_a, "phase"_a,
             "Where an attach point is in rig space for a pose, without spawning anything (where the hand will be).");

    nb::class_<PyAnimator>(m, "Animator")
        .def_prop_rw("anim", [](PyAnimator& a) { return a.node.a().anim; },
                     [](PyAnimator& a, nb::handle v) {
                         auto& an = a.node.a();
                         PyRig rig{a.node.engine_obj, an.rig};
                         an.anim = rig.anim_index(v);
                     },
                     "Index (or name, when setting) of the animation playing.")
        .def_prop_rw("phase", [](PyAnimator& a) { return a.node.a().phase; },
                     [](PyAnimator& a, float v) { a.node.a().phase = v; },
                     "0..256 for a loop (wraps), 0..255 otherwise; set it every step to drive the animation "
                     "from the game's own clock.")
        .def_prop_rw("speed", [](PyAnimator& a) { return a.node.a().speed; },
                     [](PyAnimator& a, float v) { a.node.a().speed = v; },
                     "Phase units per second the engine advances by itself; 0: the game sets phase.")
        .def("play",
             [](PyAnimator& a, nb::handle anim, float speed, float phase) {
                 auto& an = a.node.a();
                 PyRig rig{a.node.engine_obj, an.rig};
                 an.anim = rig.anim_index(anim);
                 an.speed = speed;
                 an.phase = phase;
             },
             "anim"_a, "speed"_a = 64.0f, "phase"_a = 0.0f, "Start an animation (name or index); speed in phase units/s.")
        .def_prop_ro("fired",
                     [](PyAnimator& a) {
                         auto& an = a.node.a();
                         const asset::RigData& rig = *a.node.w().rig(an.rig);
                         nb::list out;
                         for (const auto& f : an.fired) out.append(event_dict(rig, f.anim, f.index));
                         return out;
                     },
                     "Events crossed in the last logic step: dicts with type, phase, anim and the event's own keys.");

    nb::class_<PyActions>(m, "Actions", "Named input actions bound to buttons and sticks; read them in fixed_update.")
        .def("bind",
             [](PyActions& a, const std::string& name, nb::args buttons) {
                 uint32_t mask = 0;
                 for (nb::handle b : buttons) mask |= button_of(b);
                 a.map().bind(name, mask);
             },
             "name"_a, "buttons"_a, "Any of these buttons (ae.SOUTH or \"SOUTH\"); binding again replaces.")
        .def("bind_axis",
             [](PyActions& a, const std::string& name, const std::string& stick, float dead_zone) {
                 using S = runtime::ActionMap::Stick;
                 a.map().bind_axis(name, choice<S>(stick, {{"left", S::Left}, {"right", S::Right}, {"dpad", S::Dpad}}, "stick"),
                                   dead_zone);
             },
             "name"_a, "stick"_a, "dead_zone"_a = 0.0f, "A stick (\"left\", \"right\") or the d-pad (\"dpad\"); radial dead zone 0..1.")
        .def("unbind", [](PyActions& a, const std::string& name) { a.map().unbind(name); }, "name"_a)
        .def("clear", [](PyActions& a) { a.map().clear(); })
        .def("held", [](PyActions& a, const std::string& name) { return a.map().held(name, a.in()); }, "name"_a)
        .def("pressed", [](PyActions& a, const std::string& name) { return a.map().pressed(name, a.in()); }, "name"_a,
             "Went down since the last step (taps shorter than a step still count).")
        .def("axis",
             [](PyActions& a, const std::string& name) {
                 glm::vec2 v = a.map().axis(name, a.in());
                 return std::make_pair(v.x, v.y);
             },
             "name"_a, "(x, y), -1..1, +x right, +y down (like Input).")
        .def("bindings",
             [](PyActions& a) {
                 nb::dict out;
                 for (auto& [name, mask] : a.map().buttons()) {
                     nb::list l;
                     for (auto& [bn, bit] : kButtonNames)
                         if (mask & bit) l.append(bn);
                     out[name.c_str()] = l;
                 }
                 for (auto& [name, ax] : a.map().axes()) {
                     const char* stick = ax.stick == runtime::ActionMap::Stick::Left ? "left"
                                         : ax.stick == runtime::ActionMap::Stick::Right ? "right" : "dpad";
                     nb::dict d;
                     d["stick"] = stick;
                     d["dead_zone"] = ax.dead_zone;
                     out[name.c_str()] = d;
                 }
                 return out;
             },
             "{name: [button names] or {\"stick\", \"dead_zone\"}}: JSON-friendly, for a save.")
        .def("load",
             [](PyActions& a, nb::dict table) {
                 a.map().clear();
                 using S = runtime::ActionMap::Stick;
                 for (auto [k, v] : table) {
                     std::string name = nb::cast<std::string>(k);
                     if (nb::isinstance<nb::dict>(v)) {
                         nb::dict d = nb::cast<nb::dict>(v);
                         std::string stick = nb::cast<std::string>(d["stick"]);
                         float dz = d.contains("dead_zone") ? nb::cast<float>(d["dead_zone"]) : 0.0f;
                         a.map().bind_axis(name, choice<S>(stick, {{"left", S::Left}, {"right", S::Right}, {"dpad", S::Dpad}}, "stick"), dz);
                     } else {
                         uint32_t mask = 0;
                         for (nb::handle b : nb::cast<nb::list>(v)) mask |= button_of(b);
                         a.map().bind(name, mask);
                     }
                 }
             },
             "table"_a, "Replace every binding with a bindings() table (e.g. read back from a save).");

    nb::class_<PySound>(m, "Sound", "A sound loaded with engine.audio.load (16-bit PCM WAV).")
        .def_prop_ro("seconds", [](PySound& s) {
            const audio::Sound* d = nb::cast<PyEngine&>(s.engine_obj).get().mixer().sound(s.id);
            return d ? double(d->frames()) / d->rate : 0.0;
        })
        .def_prop_ro("rate", [](PySound& s) { return nb::cast<PyEngine&>(s.engine_obj).get().mixer().sound(s.id)->rate; })
        .def_prop_ro("channels", [](PySound& s) { return nb::cast<PyEngine&>(s.engine_obj).get().mixer().sound(s.id)->channels; });

    nb::class_<PyVoice>(m, "Voice", "One playing sound (engine.audio.play); goes stale when it ends.")
        .def_prop_ro("playing", [](PyVoice& v) { return v.m().playing(v.id); })
        .def("stop", [](PyVoice& v, float fade) { v.m().stop(v.id, fade); }, "fade"_a = 0.0f, "Now, or fading out over `fade` s.")
        .def("set_volume", [](PyVoice& v, float volume, float fade) { v.m().set_volume(v.id, volume, fade); }, "volume"_a,
             "fade"_a = 0.0f)
        .def("set_pan", [](PyVoice& v, float pan) { v.m().set_pan(v.id, pan); }, "pan"_a);

    nb::class_<PyAudio>(m, "Audio",
                        "The engine's mixer (docs/api.md §7): one 48 kHz stereo stream to the system; music and effects "
                        "buses. Headless there is no device: mix() renders by hand.")
        .def("load",
             [](PyAudio& a, nb::handle source) {
                 std::string err;
                 uint32_t id;
                 if (nb::isinstance<nb::bytes>(source)) {
                     auto b = nb::cast<nb::bytes>(source);
                     const auto* p = reinterpret_cast<const uint8_t*>(b.data());
                     id = a.e().load_sound(std::vector<uint8_t>(p, p + b.size()), &err);
                 } else {
                     id = a.e().load_sound(nb::cast<std::string>(source), &err);
                 }
                 if (!id) throw std::runtime_error("alia_engine: " + err);
                 return PySound{a.engine_obj, id};
             },
             "source"_a, "A 16-bit PCM WAV (a path, relative to the cart's content/, or its bytes), mono or stereo, any rate.")
        .def("play",
             [](PyAudio& a, PySound& s, float volume, float pan, float pitch, bool loop, double loop_start, double end,
                float fade_in, const std::string& bus) {
                 return PyVoice{a.engine_obj, a.e().mixer().play(s.id, play_params(volume, pan, pitch, loop, loop_start, end, fade_in, bus))};
             },
             "sound"_a, "volume"_a = 1.0f, "pan"_a = 0.0f, "pitch"_a = 1.0f, "loop"_a = false, "loop_start"_a = 0.0,
             "end"_a = 0.0, "fade_in"_a = 0.0f, "bus"_a = "effects",
             "Play it now -> Voice. pan -1..1; pitch: playback rate; loop_start / end in seconds (end 0: the whole sound). "
             "With all voices busy the quietest effect makes room.")
        .def("play_music",
             [](PyAudio& a, std::optional<PySound*> s, float volume, bool loop, double loop_start, double end, float fade) {
                 if (!s || !*s) {
                     a.e().mixer().stop_music(fade);
                     return;
                 }
                 a.e().mixer().play_music((*s)->id, play_params(volume, 0, 1, loop, loop_start, end, 0, "music"), fade);
             },
             nb::arg("sound").none(), "volume"_a = 1.0f, "loop"_a = true, "loop_start"_a = 0.0, "end"_a = 0.0, "fade"_a = 0.4f,
             "The music track; call it every step if you like: the same sound only changes its volume (over `fade`), "
             "another crossfades, a finished one-shot stays quiet while it is still asked for. None: silence.")
        .def("stop_music", [](PyAudio& a, float fade) { a.e().mixer().stop_music(fade); }, "fade"_a = 0.4f)
        .def_prop_rw("music_volume", [](PyAudio& a) { return a.e().mixer().bus_volume(audio::Bus::Music); },
                     [](PyAudio& a, float v) { a.e().mixer().set_bus_volume(audio::Bus::Music, v); }, "The music bus, 0..1.")
        .def_prop_rw("sfx_volume", [](PyAudio& a) { return a.e().mixer().bus_volume(audio::Bus::Effects); },
                     [](PyAudio& a, float v) { a.e().mixer().set_bus_volume(audio::Bus::Effects, v); }, "The effects bus, 0..1.")
        .def_prop_ro("voices", [](PyAudio& a) { return a.e().mixer().stats().voices; })
        .def_prop_ro("device",
                     [](PyAudio& a) -> nb::object {
                         if (!a.e().audio_device().is_open()) return nb::none();
                         audio::DeviceStats s = a.e().audio_device().stats();
                         nb::dict d;
                         d["rate"] = s.rate;
                         d["sample_frames"] = s.sample_frames;
                         d["callbacks"] = s.callbacks;
                         d["late"] = s.late;
                         d["mix_ms_max"] = s.mix_ms_max;
                         return d;
                     },
                     "None without a device; else what it gave us and how it went (late: likely dropouts).")
        .def("mix",
             [](PyAudio& a, int frames) {
                 if (a.e().audio_device().is_open()) throw std::runtime_error("alia_engine: the device is mixing (mix() is for headless)");
                 std::vector<float> out(size_t(std::max(frames, 0)) * 2);
                 a.e().mixer().mix(out.data(), int(out.size() / 2));
                 return nb::bytes(reinterpret_cast<const char*>(out.data()), out.size() * sizeof(float));
             },
             "frames"_a, "Headless: render the next `frames` (48 kHz) as interleaved stereo float32 bytes.");

    nb::class_<PyParticles>(m, "Particles", "A particle template (Engine.particles); spawn emitters from it.")
        .def_prop_ro("id", [](PyParticles& p) { return p.id; });

    nb::class_<PyEmitter, PyNode>(m, "Emitter", "A node that spawns particles from a template.")
        .def("burst",
             [](PyEmitter& n, int count, std::optional<std::array<float, 3>> at, std::optional<int> tint) {
                 n.em();
                 glm::vec3 p = at ? glm::vec3((*at)[0], (*at)[1], (*at)[2]) : glm::vec3(0.0f);
                 n.w().burst(n.entity, count, at ? &p : nullptr, tint ? *tint : -1);
             },
             "count"_a, "at"_a = nb::none(), "tint"_a = nb::none(),
             "count particles now: from the emitter, or from `at` (world space; directions still in the "
             "emitter's frame), tinted `tint` instead of the template's.")
        .def_prop_rw("rate", [](PyEmitter& n) { return n.em().rate; }, [](PyEmitter& n, float v) { n.em().rate = std::max(v, 0.0f); },
                     "Particles per second (0: bursts only).")
        .def_prop_rw("time_scale", [](PyEmitter& n) { return n.em().time_scale; },
                     [](PyEmitter& n, float v) { n.em().time_scale = std::max(v, 0.0f); }, "Slow motion; 0 holds them (hit stop).")
        .def_prop_ro("count", [](PyEmitter& n) { return n.em().particles.size(); }, "Particles alive.")
        .def("clear", [](PyEmitter& n) { n.em().particles.clear(); });

    nb::class_<PyIk>(m, "Ik", "An IK constraint on a rig (RigNode.look_at / two_bone, docs/api.md §4.1).")
        .def_prop_rw("target",
                     [](PyIk& k) -> nb::object {
                         auto& c = k.c();
                         if (c.target != entt::null) return nb::cast(PyNode{k.node.engine_obj, c.target});
                         return nb::cast(tuple3(c.point));
                     },
                     [](PyIk& k, nb::handle t) { set_target(k.c(), t); }, "A node (followed every step) or a point.")
        .def_prop_rw("weight", [](PyIk& k) { return k.c().weight; }, [](PyIk& k, float v) { k.c().weight = std::clamp(v, 0.0f, 1.0f); },
                     "0: the animation as it is, 1: the constraint fully.")
        .def_prop_rw("enabled", [](PyIk& k) { return k.c().enabled; }, [](PyIk& k, bool v) { k.c().enabled = v; })
        .def_prop_rw("max_angle", [](PyIk& k) { return k.c().max_angle; }, [](PyIk& k, float v) { k.c().max_angle = v; },
                     "look_at: radians it may turn away from the animated direction.")
        .def_prop_rw("pole", [](PyIk& k) { return tuple3(k.c().pole); },
                     [](PyIk& k, std::array<float, 3> v) { k.c().pole = {v[0], v[1], v[2]}; },
                     "two_bone: the side the joint bends towards (rig frame).")
        .def("remove", [](PyIk& k) {
            auto& ik = k.node.a().ik;
            ik.erase(std::remove_if(ik.begin(), ik.end(), [&](const auto& c) { return c.id == k.id; }), ik.end());
        });

    nb::class_<PyRigNode, PyNode>(m, "RigNode", "A rig in the world: the root node; bones, slots and attach points are its descendants.")
        .def_prop_ro("animator", [](PyRigNode& n) {
            n.a();
            return PyAnimator{n};
        })
        .def("bone",
             [](PyRigNode& n, nb::handle which) {
                 auto& an = n.a();
                 const asset::RigData& rig = *n.w().rig(an.rig);
                 if (nb::isinstance<nb::str>(which)) {
                     std::string name = nb::cast<std::string>(which);
                     for (size_t i = 0; i < rig.bones.size(); i++)
                         if (rig.bones[i].name == name) return PyNode{n.engine_obj, an.bones[i]};
                     throw nb::value_error(("alia_engine: no bone " + name).c_str());
                 }
                 size_t i = nb::cast<size_t>(which);
                 if (i >= an.bones.size()) throw nb::value_error("alia_engine: no such bone");
                 return PyNode{n.engine_obj, an.bones[i]};
             },
             "bone"_a, "A bone's node (name or index): its mesh, tint, visible ... are yours to change.")
        .def("slot",
             [](PyRigNode& n, size_t i) {
                 auto& an = n.a();
                 if (i >= an.slots.size()) throw nb::value_error("alia_engine: no such slot");
                 return PyNode{n.engine_obj, an.slots[i]};
             },
             "index"_a, "A part slot's node; set its mesh to fit a variant (mesh 0: empty).")
        .def("slot_variant",
             [](PyRigNode& n, size_t i, size_t variant) {
                 auto& an = n.a();
                 const asset::RigData& rig = *n.w().rig(an.rig);
                 if (i >= an.slots.size() || variant >= rig.slots[i].variants.size())
                     throw nb::value_error("alia_engine: no such slot or variant");
                 const std::string& mesh = rig.slots[i].variants[variant];
                 n.w().renderer(an.slots[i]).mesh = mesh.empty() ? 0 : nb::cast<PyEngine&>(n.engine_obj).get().mesh_named(mesh);
                 if (auto* c = n.w().registry().try_get<world::ColliderComponent>(an.slots[i])) c->enabled = variant != 0;
             },
             "index"_a, "variant"_a,
             "Fit the rig's own variant (0: empty) by its mesh name; the slot's collider is on with a variant, off without.")
        .def("look_at",
             [](PyRigNode& n, nb::handle bone, nb::handle target, std::array<float, 3> axis, float max_angle, float weight) {
                 auto& an = n.a();
                 world::IkConstraint c;
                 c.kind = world::IkConstraint::LookAt;
                 c.id = an.next_ik++;
                 c.bone = bone_index(*n.w().rig(an.rig), bone);
                 c.axis = {axis[0], axis[1], axis[2]};
                 c.max_angle = max_angle;
                 c.weight = std::clamp(weight, 0.0f, 1.0f);
                 set_target(c, target);
                 an.ik.push_back(c);
                 return PyIk{n, c.id};
             },
             "bone"_a, "target"_a, "axis"_a = std::array<float, 3>{0, 0, 1}, "max_angle"_a = 3.14159265f, "weight"_a = 1.0f,
             "Turn a bone so its axis (bone frame) points at the target (a node or a point), at most max_angle radians "
             "from where the animation has it.")
        .def("two_bone",
             [](PyRigNode& n, nb::handle upper, nb::handle lower, nb::handle end, nb::handle target,
                std::array<float, 3> pole, float weight) {
                 auto& an = n.a();
                 const asset::RigData& rig = *n.w().rig(an.rig);
                 world::IkConstraint c;
                 c.kind = world::IkConstraint::TwoBone;
                 c.id = an.next_ik++;
                 c.bone = bone_index(rig, upper);
                 c.lower = bone_index(rig, lower);
                 if (rig.bones[size_t(c.lower)].parent != c.bone) throw nb::value_error("alia_engine: two_bone: lower must be upper's child");
                 if (nb::isinstance<nb::str>(end)) {  // an attach point on the lower bone, or a bone hanging from it
                     std::string name = nb::cast<std::string>(end);
                     bool found = false;
                     for (const auto& p : rig.attach)
                         if (p.name == name && p.bone == c.lower) c.end = {p.offset[0], p.offset[1], p.offset[2]}, found = true;
                     for (const auto& b : rig.bones)
                         if (!found && b.name == name && b.parent == c.lower) c.end = {b.offset[0], b.offset[1], b.offset[2]}, found = true;
                     if (!found) throw nb::value_error(("alia_engine: two_bone: " + name + " is not on the lower bone").c_str());
                 } else {
                     auto p = nb::cast<std::array<float, 3>>(end);
                     c.end = {p[0], p[1], p[2]};
                 }
                 c.pole = {pole[0], pole[1], pole[2]};
                 c.weight = std::clamp(weight, 0.0f, 1.0f);
                 set_target(c, target);
                 an.ik.push_back(c);
                 return PyIk{n, c.id};
             },
             "upper"_a, "lower"_a, "end"_a, "target"_a, "pole"_a = std::array<float, 3>{0, 0, 1}, "weight"_a = 1.0f,
             "Bend upper and lower (its child) so `end` (an attach point on lower, a bone hanging from it, or a point "
             "in lower's frame) reaches the target; pole (rig frame) is the side the joint bends towards.")
        .def("colliders",
             [](PyRigNode& n, nb::handle which) {
                 auto& an = n.a();
                 const asset::RigData& rig = *n.w().rig(an.rig);
                 size_t i = 0;
                 if (nb::isinstance<nb::str>(which)) {
                     std::string name = nb::cast<std::string>(which);
                     while (i < rig.bones.size() && rig.bones[i].name != name) i++;
                 } else {
                     i = nb::cast<size_t>(which);
                 }
                 if (i >= an.bones.size()) throw nb::value_error("alia_engine: no such bone");
                 std::vector<PyNode> out;
                 for (world::Entity c : n.w().registry().get<world::HierarchyComponent>(an.bones[i]).children)
                     if (n.w().registry().all_of<world::ColliderComponent>(c) &&
                         std::find(an.slots.begin(), an.slots.end(), c) == an.slots.end())  // slots' are the slots'
                         out.push_back(PyNode{n.engine_obj, c});
                 return out;
             },
             "bone"_a, "The nodes carrying the rig's colliders on this bone (name or index).")
        .def("attach_point",
             [](PyRigNode& n, const std::string& name) {
                 world::Entity e = n.w().attach_point(n.entity, name);
                 if (e == entt::null) throw nb::value_error(("alia_engine: no attach point " + name).c_str());
                 return PyNode{n.engine_obj, e};
             },
             "name"_a, "A node on the attach point (created the first time): read its world_position, or parent things to it.");

    nb::class_<PyWorld>(m, "World")
        .def(
            "spawn",
            [](PyWorld& pw, uint32_t mesh, const std::string& name, std::optional<PyNode*> parent,
               std::array<float, 3> position, float yaw, float pitch, float roll, nb::object scale, uint8_t tint,
               float fade, render::Light light, bool ground_fog, bool visible, bool active) {
                world::World& w = pw.w();
                world::Entity e = w.spawn();
                w.set_active(e, active);
                auto& t = w.transform(e);
                t.position = {position[0], position[1], position[2]};
                t.rotation = world::from_ypr(yaw, pitch, roll);
                t.scale = vec3_of(scale, "scale");
                if (parent && *parent && !w.set_parent(e, (*parent)->entity)) {
                    w.destroy(e);
                    throw nb::value_error("alia_engine: bad parent");
                }
                if (mesh) w.renderer(e) = {mesh, tint, fade, light, ground_fog, visible};
                if (!name.empty()) w.tag(e).name = name;
                w.teleport(e);
                return PyNode{pw.engine_obj, e};
            },
            "mesh"_a = 0, "name"_a = "", "parent"_a = nb::none(), "position"_a = std::array<float, 3>{0, 0, 0},
            "yaw"_a = 0.0f, "pitch"_a = 0.0f, "roll"_a = 0.0f, "scale"_a = nb::cast(1.0f), "tint"_a = 0,
            "fade"_a = 0.0f, "light"_a = render::Light::Material, "ground_fog"_a = true, "visible"_a = true,
            "active"_a = true, "A new node (mesh 0: an empty node, e.g. a parent). Stays until destroy().")
        .def(
            "spawn_rig",
            [](PyWorld& pw, PyRig& rig, std::optional<PyNode*> parent, std::array<float, 3> position, float yaw,
               std::optional<std::map<std::string, uint32_t>> meshes) {
                world::World& w = pw.w();
                PyEngine& eng = nb::cast<PyEngine&>(pw.engine_obj);
                world::Entity root = w.spawn_rig(rig.id, [&](const std::string& name) -> uint32_t {
                    if (meshes) {
                        auto it = meshes->find(name);
                        return it == meshes->end() ? 0 : it->second;
                    }
                    return eng.get().mesh_named(name);
                });
                auto& t = w.transform(root);
                t.position = {position[0], position[1], position[2]};
                t.rotation = world::from_ypr(yaw, 0, 0);
                if (parent && *parent && !w.set_parent(root, (*parent)->entity)) throw nb::value_error("alia_engine: bad parent");
                w.teleport(root);
                return PyRigNode{{pw.engine_obj, root}};
            },
            "rig"_a, "parent"_a = nb::none(), "position"_a = std::array<float, 3>{0, 0, 0}, "yaw"_a = 0.0f,
            "meshes"_a = nb::none(),
            "A rig in the world. Bone meshes are found by name among the meshes loaded from packs (or in `meshes`).")
        .def_prop_ro("camera", [](PyWorld& pw) { return PyCamera{{pw.engine_obj, pw.w().camera()}}; })
        .def(
            "spawn_emitter",
            [](PyWorld& pw, PyParticles& def, std::optional<PyNode*> parent, std::array<float, 3> position, float yaw,
               float pitch, float rate, uint64_t seed) {
                world::World& w = pw.w();
                world::Entity e = w.spawn_emitter(def.id, seed);
                if (e == entt::null) throw nb::value_error("alia_engine: no such particle template");
                auto& t = w.transform(e);
                t.position = {position[0], position[1], position[2]};
                t.rotation = world::from_ypr(yaw, pitch, 0);
                if (parent && *parent && !w.set_parent(e, (*parent)->entity)) throw nb::value_error("alia_engine: bad parent");
                w.teleport(e);
                w.emitter(e)->rate = std::max(rate, 0.0f);
                return PyEmitter{{pw.engine_obj, e}};
            },
            "particles"_a, "parent"_a = nb::none(), "position"_a = std::array<float, 3>{0, 0, 0}, "yaw"_a = 0.0f,
            "pitch"_a = 0.0f, "rate"_a = 0.0f, "seed"_a = 0u,
            "An emitter node (follow something by giving it a parent: an attach point, a bone). Stays until destroy().")
        .def(
            "ray_cast",
            [](PyWorld& pw, std::array<float, 3> o, std::array<float, 3> d, float max_distance, uint32_t layers,
               const std::vector<PyNode*>& exclude) -> nb::object {
                std::vector<world::Entity> ex;
                for (PyNode* n : exclude) ex.push_back(n->entity);
                auto hit = pw.w().ray_cast({o[0], o[1], o[2]}, {d[0], d[1], d[2]}, max_distance, layers, ex);
                if (!hit) return nb::none();
                return nb::cast(PyHit{{pw.engine_obj, hit->node}, hit->distance, tuple3(hit->point), hit->layers, hit->user});
            },
            "origin"_a, "direction"_a, "max_distance"_a = 1e30f, "layers"_a = 0xFFFFFFFFu, "exclude"_a = std::vector<PyNode*>{},
            "Nearest enabled collider sharing a bit with `layers` -> Hit (node, distance, point, layers, user) or None. "
            "exclude: nodes whose colliders, and their descendants', don't count. Ties within 1 mm go to the higher priority.")
        .def(
            "overlap_sphere",
            [](PyWorld& pw, std::array<float, 3> c, float r, uint32_t layers) {
                std::vector<PyHit> out;
                for (auto& h : pw.w().overlap_sphere({c[0], c[1], c[2]}, r, layers))
                    out.push_back({{pw.engine_obj, h.node}, h.distance, tuple3(h.point), h.layers, h.user});
                return out;
            },
            "centre"_a, "r"_a, "layers"_a = 0xFFFFFFFFu,
            "Colliders touching the sphere -> [Hit] (distance: from the centre to their surface, 0 inside).")
        .def(
            "colliders",
            [](PyWorld& pw, uint32_t layers) {
                std::vector<PyWorldCollider> out;
                for (auto& c : pw.w().colliders(layers))
                    out.push_back({{pw.engine_obj, c.node}, c.shape, tuple3(c.a), tuple3(c.b), c.r, c.layers, c.user, c.priority});
                return out;
            },
            "layers"_a = 0xFFFFFFFFu, "Every enabled collider in world space (as it is now, not interpolated).")
        .def(
            "set_ground",
            [](PyWorld& pw, std::vector<float> heights, int n, float cell, std::array<float, 2> origin) {
                if (heights.size() != size_t(n) * size_t(n) || n < 2) throw nb::value_error("set_ground: n x n heights, n >= 2");
                pw.w().set_ground(std::move(heights), n, cell, origin[0], origin[1]);
            },
            "heights"_a, "n"_a, "cell"_a, "origin"_a,
            "The ground heightmap: n x n heights in metres (rows along z), cell metres apart from origin (x, z).")
        .def("ground_height", [](PyWorld& pw, float x, float z) { return pw.w().ground_height(x, z); }, "x"_a, "z"_a,
             "Bilinear, clamped at the edges; 0 without a ground.")
        .def(
            "ray_ground",
            [](PyWorld& pw, std::array<float, 3> o, std::array<float, 3> d, float max_distance) -> nb::object {
                auto t = pw.w().ray_ground({o[0], o[1], o[2]}, {d[0], d[1], d[2]}, max_distance);
                return t ? nb::cast(*t) : nb::none();
            },
            "origin"_a, "direction"_a, "max_distance"_a = 1e30f, "Distance to where the ray meets the ground, or None.")
        .def_prop_ro("environment",
                     [](PyWorld& pw) -> render::FrameDesc& { return nb::cast<PyEngine&>(pw.engine_obj).get().frame(); },
                     nb::rv_policy::reference_internal,
                     "Sky, fog, lights and overlay: the engine's frame (its camera fields follow world.camera).")
        .def_prop_ro("count", [](PyWorld& pw) { return pw.w().count(); })
        .def("clear", [](PyWorld& pw) { pw.w().clear(); })
        .def("begin_step", [](PyWorld& pw) { pw.w().begin_step(); },
             "What ae.run does before each fixed_update (remember transforms for interpolation): for tools and "
             "tests that drive the world without ae.run.")
        .def("end_step",
             [](PyWorld& pw, float dt) {
                 pw.w().update_animators(dt);
                 pw.w().update_particles(dt);
             },
             "dt"_a, "What ae.run does after each fixed_update: advance and pose the animators, simulate the particles.")
        .def(
            "set_transforms",
            [](PyWorld& pw, const std::vector<PyNode*>& nodes, const nb::bytes& data) {
                auto f = view<float>(data, "data");
                if (f.size() != nodes.size() * 10) throw nb::value_error("set_transforms: 10 floats per node");
                world::World& w = pw.w();
                for (size_t i = 0; i < nodes.size(); i++) {
                    if (!w.valid(nodes[i]->entity)) throw std::runtime_error("alia_engine: a node was destroyed");
                    const float* v = &f[i * 10];
                    auto& t = w.transform(nodes[i]->entity);
                    t.position = {v[0], v[1], v[2]};
                    t.rotation = glm::normalize(glm::quat(v[6], v[3], v[4], v[5]));
                    t.scale = {v[7], v[8], v[9]};
                }
            },
            "nodes"_a, "data"_a,
            "Many nodes at once: per node position (3), rotation quaternion x y z w (4), scale (3), float32.");

    m.def("language", &runtime::language,
          "The system's language as a BCP 47 tag (\"zh-CN\", \"en\"): ALIA_LANG in a cart (the Shell's setting, read at "
          "launch), the POSIX locale elsewhere, \"en\" when nothing says otherwise.");
    m.def("choose_language",
          [](const std::vector<std::string>& available, std::optional<std::string> wanted) {
              return runtime::choose_language(available, wanted ? *wanted : runtime::language());
          },
          "available"_a, "wanted"_a = nb::none(),
          "Of the languages the game has, the one to use (as the Shell picks a cart's title): the exact tag, then the "
          "language alone (zh-CN -> zh, zh-Hans), then English, then the first. \"\" when there are none.");
    m.def("localized",
          [](nb::dict table, std::optional<std::string> wanted) -> nb::object {
              std::vector<std::string> tags;
              for (auto [k, v] : table) tags.push_back(nb::cast<std::string>(k));
              std::string tag = runtime::choose_language(tags, wanted ? *wanted : runtime::language());
              return tag.empty() ? nb::none() : nb::borrow(table[tag.c_str()]);
          },
          "table"_a, "wanted"_a = nb::none(), "{language tag: text} -> the text for the language (choose_language).");

    nb::module_ save = m.def_submodule("save", "Cart saves (docs/api.md §7): the Alia Pad cart SDK's save.h. In a cart they go to the "
                                                "sandbox's /save (atomic writes, two previous versions, the cart's quota); "
                                                "elsewhere to ~/.local/share/alia-engine/saves/<app id>.");
    save.def("write",
             [](const std::string& name, nb::bytes data) {
                 if (int r = runtime::saves::write(name, data.data(), data.size()); r < 0)
                     raise_errno(r, name + " in " + (runtime::saves::dir().empty() ? "(no save directory)" : runtime::saves::dir()));
             },
             "name"_a, "data"_a, "Atomically; the two previous versions stay (versions()). Names: [A-Za-z0-9][A-Za-z0-9._-]{0,63}.")
    .def("read",
         [](const std::string& name, int version) -> nb::object {
             auto d = runtime::saves::read(name, version);
             if (!d) return nb::none();
             return nb::bytes(reinterpret_cast<const char*>(d->data()), d->size());
         },
         "name"_a, "version"_a = 0, "The newest readable version (or version 1, 2: older ones); None when there is none.")
    .def("names", &runtime::saves::names)
    .def("versions", &runtime::saves::versions, "name"_a, "Which versions exist: 0 (current), 1, 2.")
    .def("rollback",
         [](const std::string& name, int version) {
             if (int r = runtime::saves::rollback(name, version); r < 0) raise_errno(r, name);
         },
         "name"_a, "version"_a, "Make an older version current (the current one becomes version 1).")
    .def("delete",
         [](const std::string& name) {
             if (int r = runtime::saves::remove(name); r < 0) raise_errno(r, name);
         },
         "name"_a)
    .def("dir", &runtime::saves::dir, "Where saves go now (empty until an Engine opens or set_dir()).")
    .def("set_dir", &runtime::saves::set_dir, "dir"_a, "Use this directory (tools, tests); an Engine won't change it.");

    nb::class_<PyEngine>(m, "Engine")
        .def(nb::init<const std::string&, const std::string&, std::pair<int, int>, std::pair<int, int>, int, bool,
                      bool>(),
             "title"_a = "alia-engine", "app_id"_a = "pad.alia.engine", "size"_a = std::pair{1280, 720},
             "internal"_a = std::pair{640, 360}, "rotate"_a = 0, "headless"_a = false, "vsync"_a = true)
        .def("poll", [](PyEngine& e) { return e.get().poll(); },
             "Pump events and read input; False once the user asked to quit.")
        .def_prop_ro("input", [](PyEngine& e) { return e.get().input(); })
        .def("create_material", &PyEngine::create_material, "material"_a, "-> material id (0 is the default material)")
        .def("update_material",
             [](PyEngine& e, uint32_t id, const render::Material& m) {
                 std::string err;
                 if (!e.get().update_material(id, m, &err)) throw nb::value_error(err.c_str());
             },
             "id"_a, "material"_a, "Changes light, emissive, alpha, fog ... for meshes already built; shading, "
             "color and palette_index only for meshes built afterwards.")
        .def("create_mesh", &PyEngine::create_mesh, "mesh"_a)
        .def("rumble", [](PyEngine& e, float low, float high, int ms) { return e.get().rumble(low, high, ms); },
             "low"_a, "high"_a, "ms"_a,
             "Rumble the gamepad (motors 0..1) for ms; False when it can't (headless, no gamepad, no motors: "
             "Alia Pad until the motor hardware). Safe to call anyway.")
        .def_prop_ro("actions", [](nb::object self) { return PyActions{self}; }, "Named input actions (docs/api.md §7).")
        .def_prop_ro("audio", [](nb::object self) { return PyAudio{self}; }, "The mixer (docs/api.md §7).")
        .def(
            "particles",
            [](nb::object self, uint32_t mesh, nb::handle lifetime, nb::handle speed, std::array<float, 3> velocity,
               nb::handle size, nb::handle rotation, nb::handle spin, float gravity, float drag, uint32_t max_particles,
               bool local, const std::string& shape, float radius, float angle, nb::handle size_over_life,
               nb::handle fade_over_life, uint8_t tint, nb::handle tint_over_life, const std::string& ground, float bounce,
               render::Light light, bool ground_fog) {
                using D = world::ParticleDef;
                D d;
                d.mesh = mesh;
                d.lifetime = range_of(lifetime);
                d.speed = range_of(speed);
                d.velocity = {velocity[0], velocity[1], velocity[2]};
                d.size = range_of(size);
                d.rotation = range_of(rotation);
                d.spin = range_of(spin);
                d.gravity = gravity;
                d.drag = drag;
                d.max_particles = max_particles;
                d.local = local;
                d.shape = choice<D::Shape>(shape, {{"point", D::Point}, {"sphere", D::Sphere}, {"hemisphere", D::Hemisphere},
                                                   {"cone", D::Cone}, {"ring", D::Ring}}, "shape");
                d.radius = radius;
                d.angle = angle;
                d.size_over_life = keys_of(size_over_life);
                d.fade_over_life = keys_of(fade_over_life);
                d.tint = tint;
                d.tint_over_life = keys_of(tint_over_life);
                d.ground = choice<D::Ground>(ground, {{"through", D::Through}, {"stop", D::Stop}, {"bounce", D::Bounce}, {"die", D::Die}},
                                             "ground");
                d.bounce = bounce;
                d.light = light;
                d.ground_fog = ground_fog;
                return PyParticles{self, nb::cast<PyEngine&>(self).get().world().add_particles(std::move(d))};
            },
            "mesh"_a, "lifetime"_a = 1.0f, "speed"_a = 0.0f, "velocity"_a = std::array<float, 3>{0, 0, 0}, "size"_a = 1.0f,
            "rotation"_a = 0.0f, "spin"_a = 0.0f, "gravity"_a = 0.0f, "drag"_a = 0.0f, "max_particles"_a = 64u,
            "local"_a = false, "shape"_a = "point", "radius"_a = 0.0f, "angle"_a = 0.5f, "size_over_life"_a = nb::none(),
            "fade_over_life"_a = nb::none(), "tint"_a = 0, "tint_over_life"_a = nb::none(), "ground"_a = "through",
            "bounce"_a = 0.4f, "light"_a = render::Light::Material, "ground_fog"_a = true,
            "A particle template (docs/api.md §6). Ranges: a number or (lo, hi); curves: [(t, value)] over the life "
            "(t 0..1). shape: point, sphere, hemisphere, cone (angle around +Y), ring (radius, outwards); ground: "
            "through, stop, bounce, die (on the world's heightmap; world-space particles only).")
        .def("update_mesh", &PyEngine::update_mesh, "id"_a, "mesh"_a)
        .def("destroy_mesh", [](PyEngine& e, uint32_t id) { e.get().destroy_mesh(id); }, "id"_a)
        .def("load_rigs",
             [](nb::object self, nb::handle source) {
                 PyEngine& e = nb::cast<PyEngine&>(self);
                 std::map<std::string, uint32_t> rigs;
                 std::string err;
                 bool ok;
                 if (nb::isinstance<nb::bytes>(source)) {
                     auto b = nb::cast<nb::bytes>(source);
                     ok = e.get().load_rigs(std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(b.data()),
                                                                 reinterpret_cast<const uint8_t*>(b.data()) + b.size()),
                                            &rigs, &err);
                 } else {
                     ok = e.get().load_rigs(nb::cast<std::string>(source), &rigs, &err);
                 }
                 if (!ok) throw std::runtime_error("alia_engine: " + err);
                 nb::dict out;
                 for (auto& [name, id] : rigs) out[name.c_str()] = PyRig{self, id};
                 return out;
             },
             "source"_a,
             "Every rig in a resource pack (a path, or the pack's bytes, e.g. alia_pack.PackWriter().bytes() for "
             "rigs made in code) -> {name: Rig}. Load the meshes they name first (load_pack).")
        .def("load_pack",
             [](PyEngine& e, const std::string& path) {
                 std::map<std::string, uint32_t> meshes, materials;
                 std::string err;
                 if (!e.get().load_pack(path, &meshes, &materials, &err)) throw std::runtime_error("alia_engine: " + err);
                 return std::make_pair(meshes, materials);
             },
             "path"_a, "Everything in a resource pack -> ({mesh name: id}, {material name: id}); paths as load_meshes.")
        .def("load_meshes",
             [](PyEngine& e, const std::string& path) {
                 std::map<std::string, uint32_t> out;
                 std::string err;
                 if (!e.get().load_meshes(path, &out, &err)) throw std::runtime_error("alia_engine: " + err);
                 return out;
             },
             "path"_a,
             "Every mesh in a resource pack (.apak, docs/formats.md) -> {name: mesh id}. A relative path is "
             "taken from the cart's content directory ($ALIA_CART_DIR/content), or the working directory.")
        .def("set_palette",
             [](PyEngine& e, const nb::bytes& rgb) { e.get().set_palette(view<uint8_t>(rgb, "palette")); },
             "rgb"_a, "Up to 32 RGB triples (bytes).")
        .def_prop_ro("frame", [](PyEngine& e) -> render::FrameDesc& { return e.get().frame(); },
                     nb::rv_policy::reference_internal, "This frame's camera, sky, fog and lights; kept between frames.")
        .def("draw",
             [](PyEngine& e, uint32_t mesh, const std::array<float, 12>& model, uint8_t tint, render::Light light,
                float fade, bool ground_fog) {
                 e.get().draw({mesh, model, tint, light, ground_fog, fade});
             },
             "mesh"_a, "model"_a, "tint"_a = 0, "light"_a = render::Light::Material, "fade"_a = 0.0f,
             "ground_fog"_a = true,
             "Queue one mesh instance; model is a row-major 3x4 matrix (12 floats). tint, light (MATERIAL: the "
             "material's), fade and ground_fog (False wins over the material) override the materials.")
        .def("hud_rect",
             [](PyEngine& e, float x, float y, float w, float h, uint32_t color) { e.get().hud_rect(x, y, w, h, color); },
             "x"_a, "y"_a, "w"_a, "h"_a, "color"_a, "Filled rectangle in scene pixels; color is 0xRRGGBBAA.")
        .def("hud_text",
             [](PyEngine& e, float x, float y, std::string text, uint32_t color, float scale, uint32_t shadow) {
                 e.get().hud_text(x, y, std::move(text), color, scale, shadow);
             },
             "x"_a, "y"_a, "text"_a, "color"_a = 0xFFFFFFFFu, "scale"_a = 1.0f, "shadow"_a = 0u,
             "5x7 text (ASCII), 6 * scale pixels per character; shadow: 0xRRGGBBAA drop shadow, 0 for none.")
        .def("end_frame", [](PyEngine& e) { e.get().end_frame(); }, "Render the draw list and present it.")
        .def("screenshot",
             [](PyEngine& e, const std::string& path, bool scene_only) {
                 std::string err;
                 if (!e.get().screenshot(path, scene_only, &err)) throw std::runtime_error(err);
             },
             "path"_a, "scene_only"_a = false)
        .def_prop_ro("scene_size", [](PyEngine& e) { return std::pair{e.get().scene_width(), e.get().scene_height()}; })
        .def_prop_ro("stats",
                     [](PyEngine& e) {
                         const auto& s = e.get().stats();
                         const auto& t = e.get().timing();
                         nb::dict d;
                         d["instances"] = s.instances;
                         d["culled"] = s.culled;
                         d["triangles"] = s.triangles;
                         d["draws"] = s.draws;
                         d["cpu_ms"] = t.cpu_ms;
                         d["frame_ms"] = t.frame_ms;
                         d["submit_ms"] = t.submit_ms;
                         d["present_ms"] = t.present_ms;
                         return d;
                     })
        .def("describe", [](PyEngine& e) { return e.get().describe(); })
        .def_prop_ro("world", [](nb::object self) { return PyWorld{self}; }, "The world (docs/api.md §3).")
        .def("close", [](PyEngine& e) { e.e_.close(); });
}
