// L4: resource packs (.apak, docs/formats.md). One file of named, typed entries, written on the
// PC by tools/alia_pack.py and read here. The whole file is read into memory (packs are small;
// streaming can come when a game needs it). Entries are validated when they are read: a broken
// pack gives an error, never a crash.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace alia::asset {

enum class EntryType : uint8_t { Mesh = 1, Material = 2, Rig = 3 };

struct Entry {
    EntryType type;
    std::string name;
    uint32_t offset = 0, size = 0;  // within the file
};

// A mesh as stored: the same shape as render::MeshDesc, owning its arrays; parts name their
// materials (resolved against the pack's material entries when loading).
struct SubmeshData {
    uint32_t first_tri = 0, tri_count = 0;
    std::string material;  // empty: the default material
};

struct MeshData {
    std::vector<float> positions;     // xyz
    std::vector<uint16_t> indices;    // triangles
    std::vector<uint8_t> tri_palette; // one palette index per triangle, or empty
    std::vector<uint8_t> vertex_rgb;  // RGB per vertex, or empty
    bool double_sided = false;
    std::vector<SubmeshData> submeshes;  // empty: one part, default material
};

// A material as stored (docs/formats.md type 2); the enum values are render::Shading,
// render::ColorSource, render::Light and render::AlphaMode.
struct MaterialData {
    uint8_t shading = 0, color = 0, palette_index = 0, light = 0, alpha_mode = 0;
    float ambient = -1, diffuse = -1, emissive = 0, opacity = 1;
    bool double_sided = false, fog = true, ground_fog = true;
};

// A rigid rig (docs/formats.md type 3): bones in parent-first order, part slots, attach points
// and keyframed animations. Angles are degrees, applied Ry * Rx * Rz; meshes are named.
struct ColliderData {  // in the bone's (or slot's) frame
    uint8_t shape = 0;    // 0 none, 1 sphere (a = centre), 2 capsule (a - b)
    float a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
    float r = 0;
    uint32_t layers = 1, user = 0;
    int priority = 0;
};
struct RigBoneData {
    std::string name;
    int parent = -1;  // index of an earlier bone, -1 for the root
    float offset[3] = {0, 0, 0};
    std::string mesh;  // empty: none
    std::vector<ColliderData> colliders = {};
};
struct RigSlotData {
    int bone = 0;
    float offset[3] = {0, 0, 0}, rot[3] = {0, 0, 0};
    std::vector<std::string> variants;  // mesh names; "" = empty (variant 0 always is)
    ColliderData collider = {};         // shape 0: none
};
struct RigAttachData {
    std::string name;
    int bone = 0;
    float offset[3] = {0, 0, 0};
};
struct RigEventData {
    uint8_t phase = 0;
    std::string type;
    std::vector<std::pair<std::string, std::string>> strings;  // e.g. bone = "head"
    std::vector<std::pair<std::string, float>> numbers;        // e.g. r = 3.0
};
struct RigAnimData {
    std::string name;
    bool loop = true;
    int keys = 1;
    std::vector<float> root_y;  // keys values added to the root bone's offset y, or empty
    std::vector<std::vector<float>> tracks;  // per bone: keys x 3 angles, or empty (rest pose)
    std::vector<RigEventData> events;
};
struct RigData {
    std::string name;
    std::vector<RigBoneData> bones;
    std::vector<RigSlotData> slots;
    std::vector<RigAttachData> attach;
    std::vector<RigAnimData> anims;
};

class Pack {
public:
    bool open(const std::string& path, std::string* error);
    bool load(std::vector<uint8_t> bytes, std::string* error);  // the same, from memory

    const std::vector<Entry>& entries() const { return entries_; }
    const Entry* find(EntryType type, const std::string& name) const;
    bool read_mesh(const Entry& e, MeshData* out, std::string* error) const;
    bool read_material(const Entry& e, MaterialData* out, std::string* error) const;
    bool read_rig(const Entry& e, RigData* out, std::string* error) const;

private:
    std::vector<uint8_t> bytes_;
    std::vector<Entry> entries_;
};

// Where a cart's files are: an absolute path stays as it is; a relative one is taken from the
// cart's content directory ($ALIA_CART_DIR/content), or from the working directory when there is
// no cart (development).
std::string content_path(const std::string& path);

}  // namespace alia::asset
