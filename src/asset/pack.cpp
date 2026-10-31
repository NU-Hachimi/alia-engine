#include "asset/pack.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace alia::asset {
namespace {

constexpr char kMagic[4] = {'A', 'P', 'A', 'K'};
constexpr uint16_t kVersion = 1;
constexpr size_t kHeader = 16;  // magic, u16 version, u16 reserved, u32 count, u32 directory offset

// Little-endian reads with bounds checks; `at` advances.
struct Reader {
    const uint8_t* p;
    size_t n, at = 0;
    bool ok = true;

    bool need(size_t k) {
        if (!ok || k > n || at > n - k) ok = false;
        return ok;
    }
    uint8_t u8() { return need(1) ? p[at++] : 0; }
    float f32() {
        uint32_t v = u32();
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    std::string str(size_t len) {
        if (!need(len)) return {};
        std::string s(reinterpret_cast<const char*>(p + at), len);
        at += len;
        return s;
    }
    uint16_t u16() {
        if (!need(2)) return 0;
        uint16_t v = uint16_t(p[at] | (p[at + 1] << 8));
        at += 2;
        return v;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = uint32_t(p[at]) | (uint32_t(p[at + 1]) << 8) | (uint32_t(p[at + 2]) << 16) | (uint32_t(p[at + 3]) << 24);
        at += 4;
        return v;
    }
    template <class T>
    bool array(std::vector<T>& out, size_t count) {
        if (count > n / sizeof(T) || !need(count * sizeof(T))) return false;
        out.resize(count);
        std::memcpy(out.data(), p + at, count * sizeof(T));  // the board and the PC are both little-endian
        at += count * sizeof(T);
        return true;
    }
};

bool fail(std::string* error, const std::string& what) {
    *error = what;
    return false;
}

}  // namespace

bool Pack::open(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail(error, "cannot open " + path);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!load(std::move(bytes), error)) return fail(error, path + ": " + *error);
    return true;
}

bool Pack::load(std::vector<uint8_t> bytes, std::string* error) {
    bytes_ = std::move(bytes);
    entries_.clear();
    if (bytes_.size() < kHeader || std::memcmp(bytes_.data(), kMagic, 4) != 0) return fail(error, "not a resource pack");
    Reader r{bytes_.data(), bytes_.size(), 4};
    const uint16_t version = r.u16();
    r.u16();
    const uint32_t count = r.u32(), dir = r.u32();
    if (version != kVersion) return fail(error, "pack version " + std::to_string(version) + " (this engine reads 1)");
    if (dir < kHeader || dir > bytes_.size()) return fail(error, "directory outside the file");
    r.at = dir;
    for (uint32_t i = 0; i < count && r.ok; i++) {
        Entry e;
        e.type = EntryType(r.u8());
        r.u8();  // flags, unused in version 1
        const uint16_t name_len = r.u16();
        e.offset = r.u32();
        e.size = r.u32();
        if (!r.need(name_len)) break;
        e.name.assign(reinterpret_cast<const char*>(bytes_.data() + r.at), name_len);
        r.at += name_len;
        if (e.offset < kHeader || e.offset > dir || e.size > dir - e.offset) return fail(error, "entry " + e.name + " outside the data");
        entries_.push_back(std::move(e));
    }
    if (!r.ok) return fail(error, "truncated directory");
    return true;
}

const Entry* Pack::find(EntryType type, const std::string& name) const {
    for (const Entry& e : entries_)
        if (e.type == type && e.name == name) return &e;
    return nullptr;
}

bool Pack::read_mesh(const Entry& e, MeshData* out, std::string* error) const {
    if (e.type != EntryType::Mesh) return fail(error, e.name + ": not a mesh");
    Reader r{bytes_.data() + e.offset, e.size};
    const uint32_t nv = r.u32(), nt = r.u32();
    const uint8_t mode = r.u8(), flags = r.u8();
    r.u16();
    if (!r.ok || nv == 0 || nv > 65535 || nt == 0) return fail(error, e.name + ": bad mesh header");
    if (mode > 3) return fail(error, e.name + ": unknown colour mode " + std::to_string(mode));
    *out = MeshData{};
    out->double_sided = flags & 1;
    bool ok = r.array(out->positions, size_t(nv) * 3) && r.array(out->indices, size_t(nt) * 3);
    if (mode & 1) ok = ok && r.array(out->tri_palette, nt);
    if (mode & 2) ok = ok && r.array(out->vertex_rgb, size_t(nv) * 3);
    if (ok && (flags & 2)) {  // the part table
        const uint32_t parts = r.u32();
        for (uint32_t i = 0; i < parts && r.ok; i++) {
            SubmeshData sub;
            sub.first_tri = r.u32();
            sub.tri_count = r.u32();
            sub.material = r.str(r.u16());
            if (sub.tri_count == 0 || sub.first_tri > nt || sub.tri_count > nt - sub.first_tri)
                return fail(error, e.name + ": submesh outside the triangles");
            out->submeshes.push_back(std::move(sub));
        }
        ok = r.ok && parts > 0;
    }
    if (!ok) return fail(error, e.name + ": truncated");
    for (uint16_t i : out->indices)
        if (i >= nv) return fail(error, e.name + ": index out of range");
    return true;
}

bool Pack::read_material(const Entry& e, MaterialData* out, std::string* error) const {
    if (e.type != EntryType::Material) return fail(error, e.name + ": not a material");
    Reader r{bytes_.data() + e.offset, e.size};
    MaterialData m;
    m.shading = r.u8();
    m.color = r.u8();
    m.palette_index = r.u8();
    m.light = r.u8();
    m.ambient = r.f32();
    m.diffuse = r.f32();
    m.emissive = r.f32();
    m.alpha_mode = r.u8();
    const uint8_t flags = r.u8();
    r.u16();
    m.opacity = r.f32();
    if (!r.ok) return fail(error, e.name + ": truncated material");
    if (m.shading > 2 || m.color > 1 || m.light > 1 || m.alpha_mode > 1)
        return fail(error, e.name + ": unknown shading, colour, light or alpha mode");
    m.double_sided = flags & 1;
    m.fog = flags & 2;
    m.ground_fog = flags & 4;
    *out = m;
    return true;
}

bool Pack::read_rig(const Entry& e, RigData* out, std::string* error) const {
    if (e.type != EntryType::Rig) return fail(error, e.name + ": not a rig");
    Reader r{bytes_.data() + e.offset, e.size};
    RigData rig;
    rig.name = e.name;
    auto str = [&] { return r.str(r.u16()); };
    auto vec3 = [&](float* v) {
        for (int k = 0; k < 3; k++) v[k] = r.f32();
    };
    auto collider = [&](ColliderData& c) {
        c.shape = r.u8();
        vec3(c.a);
        vec3(c.b);
        c.r = r.f32();
        c.layers = r.u32();
        c.user = r.u32();
        c.priority = int8_t(r.u8());
        return c.shape <= 2;
    };
    const uint16_t nb = r.u16();
    for (uint16_t i = 0; i < nb && r.ok; i++) {
        RigBoneData b;
        b.name = str();
        b.parent = int16_t(r.u16());
        vec3(b.offset);
        b.mesh = str();
        if (b.parent < -1 || b.parent >= int(i)) return fail(error, e.name + ": bone " + b.name + " before its parent");
        for (uint8_t n = r.u8(), k = 0; k < n && r.ok; k++) {
            ColliderData c;
            if (!collider(c) || c.shape == 0) return fail(error, e.name + ": bad collider on " + b.name);
            b.colliders.push_back(c);
        }
        rig.bones.push_back(std::move(b));
    }
    if (!r.ok || nb == 0) return fail(error, e.name + ": no bones (or truncated)");
    const uint16_t ns = r.u16();
    for (uint16_t i = 0; i < ns && r.ok; i++) {
        RigSlotData s;
        s.bone = r.u16();
        vec3(s.offset);
        vec3(s.rot);
        const uint8_t nv = r.u8();
        for (uint8_t k = 0; k < nv && r.ok; k++) s.variants.push_back(str());
        if (s.bone >= int(nb)) return fail(error, e.name + ": slot on a bone that doesn't exist");
        if (!collider(s.collider)) return fail(error, e.name + ": bad slot collider");
        rig.slots.push_back(std::move(s));
    }
    const uint16_t na = r.u16();
    for (uint16_t i = 0; i < na && r.ok; i++) {
        RigAttachData a;
        a.name = str();
        a.bone = r.u16();
        vec3(a.offset);
        if (a.bone >= int(nb)) return fail(error, e.name + ": attach point on a bone that doesn't exist");
        rig.attach.push_back(std::move(a));
    }
    const uint16_t nanim = r.u16();
    for (uint16_t i = 0; i < nanim && r.ok; i++) {
        RigAnimData a;
        a.name = str();
        a.loop = r.u8();
        a.keys = r.u16();
        if (a.keys == 0) return fail(error, e.name + ": animation " + a.name + " has no keys");
        if (r.u8()) {
            a.root_y.resize(size_t(a.keys));
            for (float& y : a.root_y) y = r.f32();
        }
        a.tracks.resize(nb);
        const uint16_t nt = r.u16();
        for (uint16_t t = 0; t < nt && r.ok; t++) {
            const uint16_t bone = r.u16();
            if (bone >= nb) return fail(error, e.name + ": track for a bone that doesn't exist");
            auto& track = a.tracks[bone];
            track.resize(size_t(a.keys) * 3);
            for (float& v : track) v = r.f32();
        }
        const uint16_t nev = r.u16();
        for (uint16_t k = 0; k < nev && r.ok; k++) {
            RigEventData ev;
            ev.phase = r.u8();
            ev.type = str();
            for (uint8_t n = r.u8(), j = 0; j < n && r.ok; j++) {
                std::string key = str();
                ev.strings.emplace_back(std::move(key), str());
            }
            for (uint8_t n = r.u8(), j = 0; j < n && r.ok; j++) {
                std::string key = str();
                ev.numbers.emplace_back(std::move(key), r.f32());
            }
            a.events.push_back(std::move(ev));
        }
        rig.anims.push_back(std::move(a));
    }
    if (!r.ok) return fail(error, e.name + ": truncated rig");
    *out = std::move(rig);
    return true;
}

std::string content_path(const std::string& path) {
    if (path.empty() || path[0] == '/') return path;
    const char* cart = std::getenv("ALIA_CART_DIR");
    if (cart && *cart) return std::string(cart) + "/content/" + path;
    return path;
}

}  // namespace alia::asset
