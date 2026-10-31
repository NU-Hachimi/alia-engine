// L5: the world (docs/api.md §3). Persistent objects in an EnTT registry: the game creates them
// once and changes what changes; the engine draws them every frame, interpolated between the
// last two logic steps. Component names follow StellarAlia's Components.hpp, so scenes can be
// exported later without conversion.
//
// Interpolation works on local transforms: at the start of each logic step every Transform is
// copied to PrevTransform; drawing blends Prev -> Transform by alpha (position and scale
// linearly, rotation by slerp) and only then composes the hierarchy, so children follow their
// parents without drifting. teleport() (and spawning) skips the blend for that step.
//
// Activity (docs/api.md §3.1, Unity's activeSelf / activeInHierarchy): a node is in the world when it
// and every ancestor are active. Out of the world it isn't drawn, its animator neither advances nor
// poses, its collider answers no query. Turning a parent off never changes its children's own flag.
// A node that wasn't in the world at the start of a step isn't blended in that step (no slide).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "asset/pack.hpp"
#include "render/handheld.hpp"
#include "world/particles.hpp"

namespace alia::world {

using Entity = entt::entity;

struct TagComponent {
    std::string name;
};

struct HierarchyComponent {
    Entity parent = entt::null;
    std::vector<Entity> children;
    bool active = true;         // its own flag
    bool live_at_step = true;   // in the world (it and its ancestors active) when the step began
};

struct TransformComponent {  // local, relative to the parent
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};  // identity (w, x, y, z)
    glm::vec3 scale{1.0f};
};

struct PrevTransformComponent {  // the local transform at the start of the current logic step
    TransformComponent t;
};

struct MeshRendererComponent {
    uint32_t mesh = 0;
    // Instance overrides (docs/api.md §2), as in StellarAlia's MaterialOverrideComponent.
    uint8_t tint = 0;
    float fade = 0;
    render::Light light = render::Light::Material;
    bool ground_fog = true;
    bool visible = true;
};

struct CameraComponent {
    float fov_x = 1.309f;  // horizontal, radians (StellarAlia's is vertical: converted on export)
    float near_plane = 0.5f, far_plane = 120.0f;
};

// Collision (docs/api.md §5): a sphere or capsule in the node's frame; it follows the node's
// current (not interpolated) world transform, its radius scaled by the largest axis scale.
struct ColliderComponent {
    enum Shape : uint8_t { Sphere = 1, Capsule = 2 } shape = Sphere;
    glm::vec3 a{0.0f}, b{0.0f};  // sphere centre in a; capsule a - b
    float r = 0.5f;
    uint32_t layers = 1;  // queries pass a mask; a collider answers when they share a bit
    uint32_t user = 0;    // for the game (Megalith: the slot index of a part)
    int priority = 0;     // wins ties between hits at the same distance (armour over the leg under it)
    bool enabled = true;
};

struct RayHit {
    Entity node = entt::null;
    float distance = 0;
    glm::vec3 point{0.0f};
    uint32_t layers = 0, user = 0;
    int priority = 0;
};

struct WorldCollider {  // a collider in world space, for the game's own geometry (aim assist ...)
    Entity node;
    ColliderComponent::Shape shape;
    glm::vec3 a, b;
    float r;
    uint32_t layers, user;
    int priority;
};

// A rig instance (StellarAlia: AnimatorComponent): on the rig's root node; the bones are child
// nodes, posed every logic step (and whenever the animation or phase changed).
struct FiredEvent {
    int anim = 0, index = 0;  // rig.anims[anim].events[index]
};

// Rigid IK (docs/api.md §4.1): two analytic constraints applied after the animation pose, before
// anything is composed, in creation order; they write the bones' local rotations, so the result
// interpolates like the animation.
//   LookAt: turn `bone` so its `axis` (bone frame) points at the target, by the shortest arc,
//           at most max_angle away from the animated direction.
//   TwoBone: `bone` (upper) and `lower` (its child) bend so the end point (`end`, in the lower
//           bone's frame) reaches the target; `pole` (rig frame) picks the bending plane. Out of
//           reach, the chain stretches towards it.
struct IkConstraint {
    enum Kind : uint8_t { LookAt = 1, TwoBone = 2 } kind = LookAt;
    int id = 0;
    int bone = 0, lower = -1;
    glm::vec3 axis{0, 0, 1}, end{0, 0, 0}, pole{0, 0, 1};
    float max_angle = 3.14159265f;
    Entity target = entt::null;  // a node, followed every step; or, when null, the point below
    glm::vec3 point{0.0f};
    float weight = 1;
    bool enabled = true;
};

struct AnimatorComponent {
    uint32_t rig = 0;   // World::add_rig id
    int anim = 0;
    float phase = 0;    // 0..256 (looping) or 0..255
    float speed = 0;    // phase units per second; 0: the game sets phase itself
    std::vector<Entity> bones, slots;
    std::vector<std::pair<std::string, Entity>> attach;  // created when first asked for
    std::vector<FiredEvent> fired;  // events crossed in the last logic step
    std::vector<IkConstraint> ik;
    int next_ik = 1;
    int posed_anim = -1;            // what the bones show
    float posed_phase = -1;
};

// Rotation from yaw (about +Y), pitch (about +X), roll (about +Z), applied Ry * Rx * Rz like
// Megalith's objects: a positive pitch tips the +Z nose down.
glm::quat from_ypr(float yaw, float pitch, float roll);
void to_ypr(const glm::quat& q, float* yaw, float* pitch, float* roll);

class World {
public:
    World();

    Entity spawn();
    void destroy(Entity e);  // and its children
    bool valid(Entity e) const { return reg_.valid(e); }
    void clear();
    size_t count() const;

    // false (nothing changed) when the parent is the entity itself or one of its descendants.
    // keep_world: the node stays where it is (its local transform, and the previous step's, are
    // re-expressed under the new parent, so the step still interpolates); otherwise the local
    // transform is kept and the node moves with its new parent. Exact for rotations and uniform
    // scales; a non-uniformly scaled, rotated parent would need a shear and is approximated.
    bool set_parent(Entity e, Entity parent, bool keep_world = false);
    Entity parent(Entity e) const;
    void set_active(Entity e, bool active) { reg_.get<HierarchyComponent>(e).active = active; }
    bool active(Entity e) const { return reg_.get<HierarchyComponent>(e).active; }
    bool active_in_hierarchy(Entity e) const;

    TransformComponent& transform(Entity e) { return reg_.get<TransformComponent>(e); }
    MeshRendererComponent& renderer(Entity e) { return reg_.get_or_emplace<MeshRendererComponent>(e); }
    bool has_renderer(Entity e) const { return reg_.all_of<MeshRendererComponent>(e); }
    TagComponent& tag(Entity e) { return reg_.get_or_emplace<TagComponent>(e); }
    void teleport(Entity e);           // this step: no blend from the previous transform
    glm::mat4 world_matrix(Entity e);  // current (not interpolated); poses changed animators first

    // The camera: an entity (so it can have a parent) with a CameraComponent. The world's camera
    // drives the frame once the game has touched it (camera()).
    Entity camera();
    bool camera_active() const { return camera_ != entt::null; }

    // Rigs (docs/api.md §4). add_rig keeps the data (ids from 1); spawn_rig builds a root node
    // with an AnimatorComponent, one child node per bone (meshes named by the rig, resolved by
    // mesh_of; 0: none) and one per slot (variant 0, i.e. empty, until the game picks one).
    uint32_t add_rig(asset::RigData rig);
    const asset::RigData* rig(uint32_t id) const { return id >= 1 && id <= rigs_.size() ? &rigs_[id - 1] : nullptr; }
    Entity spawn_rig(uint32_t rig, const std::function<uint32_t(const std::string&)>& mesh_of);
    AnimatorComponent* animator(Entity root) { return reg_.try_get<AnimatorComponent>(root); }
    Entity attach_point(Entity root, const std::string& name);  // entt::null if the rig has none by that name
    // Advance playing animators by dt, collect the events crossed, pose the bones (the runtime calls
    // this after the game's fixed_update). collect() also poses animators whose phase changed.
    void update_animators(float dt);

    // Particles (docs/api.md §6). add_particles keeps a template (ids from 1); an emitter is a node
    // spawning from one, by `rate` and by burst(): n particles now, from the emitter or from `at`
    // (world space), tinted `tint` (-1: the template's). update_particles runs after the animators.
    uint32_t add_particles(ParticleDef def);
    const ParticleDef* particle_def(uint32_t id) const { return id >= 1 && id <= particle_defs_.size() ? &particle_defs_[id - 1] : nullptr; }
    Entity spawn_emitter(uint32_t def, uint64_t seed);
    EmitterComponent* emitter(Entity e) { return reg_.try_get<EmitterComponent>(e); }
    void burst(Entity e, int n, const glm::vec3* at = nullptr, int tint = -1);
    void update_particles(float dt);

    // Collision queries against enabled colliders sharing a bit with `layers`. A ray starting
    // inside a shape doesn't hit it. exclude: nodes whose colliders (and their descendants') don't
    // count. Ties within 1 mm go to the higher priority.
    std::optional<RayHit> ray_cast(glm::vec3 origin, glm::vec3 dir, float max_distance, uint32_t layers,
                                   const std::vector<Entity>& exclude = {});
    std::vector<RayHit> overlap_sphere(glm::vec3 centre, float r, uint32_t layers);
    std::vector<WorldCollider> colliders(uint32_t layers);
    // The ground: a heightmap of n x n heights (metres, row-major, z rows), `cell` metres apart from
    // (origin_x, origin_z); heights are bilinear and clamped at the edges (Megalith's terrain_height).
    void set_ground(std::vector<float> heights, int n, float cell, float origin_x, float origin_z);
    bool has_ground() const { return ground_n_ >= 2; }
    float ground_height(float x, float z) const;  // 0 without a ground
    std::optional<float> ray_ground(glm::vec3 origin, glm::vec3 dir, float max_distance) const;

    // Runtime hooks.
    void begin_step();  // PrevTransform = Transform, for every entity
    // Interpolated draw list for alpha in 0..1, appended to `out`; the camera goes into `frame`.
    // Not drawn: nodes out of the world, invisible ones, and those faded all the way (fade >= 1).
    void collect(float alpha, std::vector<render::Instance>& out, render::FrameDesc& frame);

    entt::registry& registry() { return reg_; }

private:
    using Memo = std::unordered_map<uint32_t, glm::mat4>;  // world matrices of parents, per collect()
    TransformComponent blended(Entity e, float alpha) const;
    glm::mat4 world_at(Entity e, float alpha, Memo& memo);
    void detach(Entity e);
    void pose(AnimatorComponent& a);
    void solve(Entity root, AnimatorComponent& a, const IkConstraint& c);
    void emit(Entity e, EmitterComponent& em, const ParticleDef& d, int n, const glm::vec3* at, int tint);
    void collect_particles(float alpha, std::vector<render::Instance>& out, Memo& memo);
    void pose_changed();  // pose every animator whose animation or phase changed since it was posed

    entt::registry reg_;
    std::vector<asset::RigData> rigs_;
    std::vector<ParticleDef> particle_defs_;
    std::vector<float> ground_;
    int ground_n_ = 0;
    float ground_cell_ = 1, ground_x_ = 0, ground_z_ = 0;
    Entity camera_ = entt::null;
};

}  // namespace alia::world
