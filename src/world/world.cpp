#include "world/world.hpp"

#include "world/rig.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>

namespace alia::world {

glm::quat from_ypr(float yaw, float pitch, float roll) {
    return glm::quat_cast(glm::eulerAngleYXZ(yaw, pitch, roll));
}

void to_ypr(const glm::quat& q, float* yaw, float* pitch, float* roll) {
    glm::extractEulerAngleYXZ(glm::mat4_cast(q), *yaw, *pitch, *roll);
}

World::World() = default;

Entity World::spawn() {
    Entity e = reg_.create();
    reg_.emplace<TransformComponent>(e);
    reg_.emplace<PrevTransformComponent>(e);
    reg_.emplace<HierarchyComponent>(e);
    return e;
}

void World::detach(Entity e) {
    auto& h = reg_.get<HierarchyComponent>(e);
    if (h.parent != entt::null && reg_.valid(h.parent)) {
        auto& siblings = reg_.get<HierarchyComponent>(h.parent).children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), e), siblings.end());
    }
    h.parent = entt::null;
}

void World::destroy(Entity e) {
    if (!reg_.valid(e)) return;
    detach(e);
    std::vector<Entity> stack{e};
    while (!stack.empty()) {
        Entity x = stack.back();
        stack.pop_back();
        for (Entity c : reg_.get<HierarchyComponent>(x).children) stack.push_back(c);
        if (x == camera_) camera_ = entt::null;
        reg_.destroy(x);
    }
}

void World::clear() {  // the scene; loaded rigs stay (they are resources)
    reg_.clear();
    camera_ = entt::null;
}

size_t World::count() const { return reg_.view<TransformComponent>().size(); }

namespace {

// A matrix of rotation, uniform-ish scale and translation back into a transform (no shear).
TransformComponent decompose(const glm::mat4& m) {
    TransformComponent t;
    t.position = glm::vec3(m[3]);
    glm::mat3 r(m);
    t.scale = {glm::length(r[0]), glm::length(r[1]), glm::length(r[2])};
    if (glm::determinant(r) < 0) t.scale.x = -t.scale.x;  // a mirror: put it on x
    for (int k = 0; k < 3; k++) r[k] /= t.scale[k];
    t.rotation = glm::normalize(glm::quat_cast(r));
    return t;
}

}  // namespace

bool World::set_parent(Entity e, Entity parent, bool keep_world) {
    if (!reg_.valid(e) || (parent != entt::null && !reg_.valid(parent))) return false;
    for (Entity p = parent; p != entt::null; p = reg_.get<HierarchyComponent>(p).parent)
        if (p == e) return false;  // would make a cycle
    glm::mat4 now(1.0f), before(1.0f);
    if (keep_world) {
        pose_changed();
        Memo m1, m0;
        now = world_at(e, 1.0f, m1);
        before = world_at(e, 0.0f, m0);  // the previous step's world transform
    }
    detach(e);
    reg_.get<HierarchyComponent>(e).parent = parent;
    if (parent != entt::null) reg_.get<HierarchyComponent>(parent).children.push_back(e);
    if (keep_world) {
        Memo m1, m0;
        const glm::mat4 p_now = parent == entt::null ? glm::mat4(1.0f) : world_at(parent, 1.0f, m1);
        const glm::mat4 p_before = parent == entt::null ? glm::mat4(1.0f) : world_at(parent, 0.0f, m0);
        transform(e) = decompose(glm::inverse(p_now) * now);
        reg_.get<PrevTransformComponent>(e).t = decompose(glm::inverse(p_before) * before);
    }
    return true;
}

bool World::active_in_hierarchy(Entity e) const {
    for (Entity x = e; x != entt::null; x = reg_.get<HierarchyComponent>(x).parent)
        if (!reg_.get<HierarchyComponent>(x).active) return false;
    return true;
}

Entity World::parent(Entity e) const { return reg_.get<HierarchyComponent>(e).parent; }

void World::teleport(Entity e) { reg_.get<PrevTransformComponent>(e).t = reg_.get<TransformComponent>(e); }

Entity World::camera() {
    if (camera_ == entt::null) {
        camera_ = spawn();
        reg_.emplace<CameraComponent>(camera_);
        reg_.emplace<TagComponent>(camera_, "camera");
    }
    return camera_;
}

uint32_t World::add_rig(asset::RigData rig) {
    rigs_.push_back(std::move(rig));
    return uint32_t(rigs_.size());
}

Entity World::spawn_rig(uint32_t id, const std::function<uint32_t(const std::string&)>& mesh_of) {
    const asset::RigData* r = rig(id);
    if (!r) return entt::null;
    Entity root = spawn();
    AnimatorComponent a;
    a.rig = id;
    for (const asset::RigBoneData& b : r->bones) {
        Entity e = spawn();
        set_parent(e, b.parent < 0 ? root : a.bones[size_t(b.parent)]);
        transform(e).position = {b.offset[0], b.offset[1], b.offset[2]};
        if (uint32_t mesh = b.mesh.empty() ? 0 : mesh_of(b.mesh)) renderer(e).mesh = mesh;
        tag(e).name = b.name;
        a.bones.push_back(e);
        for (const asset::ColliderData& c : b.colliders) {  // one child node per collider
            Entity ce = spawn();
            set_parent(ce, e);
            reg_.emplace<ColliderComponent>(ce, ColliderComponent::Shape(c.shape), glm::vec3(c.a[0], c.a[1], c.a[2]),
                                            glm::vec3(c.b[0], c.b[1], c.b[2]), c.r, c.layers, c.user, c.priority, true);
        }
    }
    for (const asset::RigSlotData& s : r->slots) {
        Entity e = spawn();
        set_parent(e, a.bones[size_t(s.bone)]);
        auto& t = transform(e);
        t.position = {s.offset[0], s.offset[1], s.offset[2]};
        t.rotation = euler_degrees(s.rot[0], s.rot[1], s.rot[2]);
        renderer(e);  // empty until the game fits a variant
        if (const asset::ColliderData& c = s.collider; c.shape)  // on the slot node; off until a variant is fitted
            reg_.emplace<ColliderComponent>(e, ColliderComponent::Shape(c.shape), glm::vec3(c.a[0], c.a[1], c.a[2]),
                                            glm::vec3(c.b[0], c.b[1], c.b[2]), c.r, c.layers, c.user, c.priority, false);
        a.slots.push_back(e);
    }
    reg_.emplace<AnimatorComponent>(root, std::move(a));
    pose(reg_.get<AnimatorComponent>(root));
    for (Entity e : reg_.get<AnimatorComponent>(root).bones) teleport(e);
    return root;
}

Entity World::attach_point(Entity root, const std::string& name) {
    AnimatorComponent* a = animator(root);
    if (!a) return entt::null;
    for (auto& [n, e] : a->attach)
        if (n == name && reg_.valid(e)) return e;
    for (const asset::RigAttachData& p : rigs_[a->rig - 1].attach) {
        if (p.name != name) continue;
        Entity e = spawn();
        set_parent(e, a->bones[size_t(p.bone)]);
        transform(e).position = {p.offset[0], p.offset[1], p.offset[2]};
        tag(e).name = name;
        a->attach.emplace_back(name, e);
        return e;
    }
    return entt::null;
}

void World::pose(AnimatorComponent& a) {
    static thread_local std::vector<LocalPose> local;
    const asset::RigData& r = rigs_[a.rig - 1];
    if (!pose_local(r, a.anim, a.phase, local)) return;
    for (size_t b = 0; b < a.bones.size() && b < local.size(); b++) {
        if (!reg_.valid(a.bones[b])) continue;
        auto& t = transform(a.bones[b]);
        t.position = local[b].position;
        t.rotation = local[b].rotation;
    }
    a.posed_anim = a.anim;
    a.posed_phase = a.phase;
    if (!a.ik.empty()) {
        Entity root = reg_.get<HierarchyComponent>(a.bones[0]).parent;
        for (const IkConstraint& c : a.ik)
            if (c.enabled && c.weight > 0) solve(root, a, c);
    }
}

namespace {

glm::quat rotation_of(const glm::mat4& m) {  // the rotation in a matrix with (positive) scale
    glm::mat3 r(m);
    for (int k = 0; k < 3; k++) r[k] = glm::normalize(r[k]);
    return glm::normalize(glm::quat_cast(r));
}

// The shortest rotation taking direction `from` to `to` (both non-zero), limited to max_angle.
glm::quat arc(glm::vec3 from, glm::vec3 to, float max_angle = 3.14159265f) {
    from = glm::normalize(from);
    to = glm::normalize(to);
    const float c = std::clamp(glm::dot(from, to), -1.0f, 1.0f);
    float angle = std::acos(c);
    glm::vec3 axis = glm::cross(from, to);
    if (glm::length(axis) < 1e-6f) {
        if (c > 0) return glm::quat(1, 0, 0, 0);
        axis = glm::cross(from, std::fabs(from.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0));  // opposite: any perpendicular
    }
    return glm::angleAxis(std::min(angle, max_angle), glm::normalize(axis));
}

}  // namespace

void World::solve(Entity root, AnimatorComponent& a, const IkConstraint& c) {
    if (c.bone < 0 || size_t(c.bone) >= a.bones.size()) return;
    glm::vec3 target = c.point;
    if (c.target != entt::null) {
        if (!reg_.valid(c.target)) return;
        Memo memo;
        target = glm::vec3(world_at(c.target, 1.0f, memo)[3]);
    }
    const glm::quat id(1, 0, 0, 0);
    // Turn a bone in world space by `delta`, keeping its parent: local' = parent^-1 * delta * world.
    auto turn = [&](Entity bone, const glm::quat& delta) {
        Memo memo;
        const glm::quat world = rotation_of(world_at(bone, 1.0f, memo));
        Entity parent = reg_.get<HierarchyComponent>(bone).parent;
        const glm::quat parent_rot = parent == entt::null ? id : rotation_of(world_at(parent, 1.0f, memo));
        transform(bone).rotation = glm::normalize(glm::inverse(parent_rot) * glm::slerp(id, delta, c.weight) * world);
    };
    auto origin = [&](Entity e, glm::vec3 local = glm::vec3(0)) {
        Memo memo;
        return glm::vec3(world_at(e, 1.0f, memo) * glm::vec4(local, 1));
    };
    const Entity upper = a.bones[size_t(c.bone)];
    if (c.kind == IkConstraint::LookAt) {
        Memo memo;
        const glm::mat4 m = world_at(upper, 1.0f, memo);
        const glm::vec3 to = target - glm::vec3(m[3]), axis = glm::vec3(m * glm::vec4(c.axis, 0));
        if (glm::length(to) > 1e-5f && glm::length(axis) > 1e-6f) turn(upper, arc(axis, to, c.max_angle));
        return;
    }
    if (c.lower < 0 || size_t(c.lower) >= a.bones.size()) return;
    const Entity lower = a.bones[size_t(c.lower)];
    const glm::vec3 A = origin(upper), B = origin(lower), E = origin(lower, c.end);
    const float la = glm::length(B - A), lb = glm::length(E - B);
    glm::vec3 to = target - A;
    float d = glm::length(to);
    if (la < 1e-5f || lb < 1e-5f || d < 1e-5f) return;
    const glm::vec3 dir = to / d;
    d = std::clamp(d, std::fabs(la - lb) + 1e-4f, la + lb - 1e-4f);
    // The joint goes into the plane of the target direction and the pole, on the pole's side.
    Memo memo;
    glm::vec3 pole = glm::vec3(world_at(root, 1.0f, memo) * glm::vec4(c.pole, 0));
    pole -= dir * glm::dot(pole, dir);
    if (glm::length(pole) < 1e-5f) pole = B - A - dir * glm::dot(B - A, dir);  // pole along the reach: keep the current bend
    if (glm::length(pole) < 1e-5f) pole = glm::cross(dir, std::fabs(dir.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0));
    pole = glm::normalize(pole);
    const float alpha = std::acos(std::clamp((la * la + d * d - lb * lb) / (2 * la * d), -1.0f, 1.0f));
    const glm::vec3 joint = A + la * (std::cos(alpha) * dir + std::sin(alpha) * pole);
    turn(upper, arc(B - A, joint - A));
    const glm::vec3 B2 = origin(lower), E2 = origin(lower, c.end);
    turn(lower, arc(E2 - B2, A + dir * d - B2));
}

void World::update_animators(float dt) {
    for (auto [e, a] : reg_.view<AnimatorComponent>().each()) {
        const asset::RigData& r = rigs_[a.rig - 1];
        a.fired.clear();
        if (!active_in_hierarchy(e)) continue;  // out of the world: frozen where it was
        if (a.anim < 0 || a.anim >= int(r.anims.size())) continue;
        const asset::RigAnimData& anim = r.anims[size_t(a.anim)];
        if (a.speed != 0) {
            a.phase += a.speed * dt;
            a.phase = anim.loop ? std::fmod(std::fmod(a.phase, 256.0f) + 256.0f, 256.0f) : std::clamp(a.phase, 0.0f, 255.0f);
        }
        // Events crossed since the bones were last posed: (from, to], wrapping for loops; a new
        // animation starts from just before 0.
        const float from = a.posed_anim == a.anim ? a.posed_phase : -1.0f, to = a.phase;
        for (size_t k = 0; k < anim.events.size(); k++) {
            const float p = anim.events[k].phase;
            const bool crossed = to >= from ? (p > from && p <= to) : (anim.loop && (p > from || p <= to));
            if (crossed && from != to) a.fired.push_back({a.anim, int(k)});
        }
        pose(a);
    }
}

void World::begin_step() {
    for (auto [e, t, prev] : reg_.view<TransformComponent, PrevTransformComponent>().each()) prev.t = t;
    for (auto [e, h] : reg_.view<HierarchyComponent>().each()) h.live_at_step = active_in_hierarchy(e);
}

TransformComponent World::blended(Entity e, float alpha) const {
    const auto& cur = reg_.get<TransformComponent>(e);
    if (alpha >= 1.0f || !reg_.get<HierarchyComponent>(e).live_at_step) return cur;  // just back: no slide
    const auto& prev = reg_.get<PrevTransformComponent>(e).t;
    return {glm::mix(prev.position, cur.position, alpha), glm::slerp(prev.rotation, cur.rotation, alpha),
            glm::mix(prev.scale, cur.scale, alpha)};
}

static glm::mat4 compose(const TransformComponent& t) {
    return glm::scale(glm::translate(glm::mat4(1.0f), t.position) * glm::mat4_cast(t.rotation), t.scale);
}

glm::mat4 World::world_at(Entity e, float alpha, Memo& memo) {
    // Walk up to the first ancestor already computed (or the root), then back down.
    std::vector<Entity> chain;
    glm::mat4 m(1.0f);
    for (Entity x = e; x != entt::null; x = reg_.get<HierarchyComponent>(x).parent) {
        auto hit = memo.find(entt::to_integral(x));
        if (hit != memo.end()) {
            m = hit->second;
            break;
        }
        chain.push_back(x);
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        m = m * compose(blended(*it, alpha));
        // Only parents are worth remembering (leaves are asked for once).
        if (!reg_.get<HierarchyComponent>(*it).children.empty()) memo.emplace(entt::to_integral(*it), m);
    }
    return m;
}

namespace {

// Megalith's collide.py, in C++: distances along a unit ray, None when the ray starts inside.
std::optional<float> ray_sphere(glm::vec3 o, glm::vec3 d, glm::vec3 c, float r) {
    glm::vec3 oc = o - c;
    float b = glm::dot(oc, d), h = b * b - (glm::dot(oc, oc) - r * r);
    if (h < 0) return std::nullopt;
    float t = -b - std::sqrt(h);
    return t >= 0 ? std::optional<float>(t) : std::nullopt;
}

std::optional<float> ray_capsule(glm::vec3 o, glm::vec3 d, glm::vec3 a, glm::vec3 b, float r) {
    glm::vec3 ba = b - a, oa = o - a;
    float baba = glm::dot(ba, ba), bard = glm::dot(ba, d), baoa = glm::dot(ba, oa);
    float rdoa = glm::dot(d, oa), oaoa = glm::dot(oa, oa);
    float k2 = baba - bard * bard;
    glm::vec3 end;
    if (k2 > 1e-9f) {  // not parallel to the axis: the cylinder wall first
        float k1 = baba * rdoa - baoa * bard, k0 = baba * oaoa - baoa * baoa - r * r * baba;
        float h = k1 * k1 - k2 * k0;
        if (h < 0) return std::nullopt;
        float t = (-k1 - std::sqrt(h)) / k2, y = baoa + t * bard;
        if (y > 0 && y < baba) return t >= 0 ? std::optional<float>(t) : std::nullopt;
        end = y <= 0 ? a : b;
    } else {
        end = baoa + bard < 0 ? a : b;
    }
    return ray_sphere(o, d, end, r);
}

float segment_distance(glm::vec3 p, glm::vec3 a, glm::vec3 b) {
    glm::vec3 ab = b - a;
    float len2 = glm::dot(ab, ab);
    float t = len2 > 1e-12f ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return glm::length(p - (a + ab * t));
}

}  // namespace

std::vector<WorldCollider> World::colliders(uint32_t layers) {
    pose_changed();
    Memo memo;
    std::vector<WorldCollider> out;
    for (auto [e, c] : reg_.view<ColliderComponent>().each()) {
        if (!c.enabled || !(c.layers & layers) || !active_in_hierarchy(e)) continue;
        const glm::mat4 m = world_at(e, 1.0f, memo);
        const float scale = std::max({glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2]))});
        const glm::vec3 a(m * glm::vec4(c.a, 1)), b(m * glm::vec4(c.shape == ColliderComponent::Capsule ? c.b : c.a, 1));
        out.push_back({e, c.shape, a, b, c.r * scale, c.layers, c.user, c.priority});
    }
    return out;
}

std::optional<RayHit> World::ray_cast(glm::vec3 o, glm::vec3 dir, float max_distance, uint32_t layers,
                                      const std::vector<Entity>& exclude) {
    const float len = glm::length(dir);
    if (len < 1e-12f) return std::nullopt;
    const glm::vec3 d = dir / len;
    auto excluded = [&](Entity e) {
        for (Entity x = e; x != entt::null; x = reg_.get<HierarchyComponent>(x).parent)
            if (std::find(exclude.begin(), exclude.end(), x) != exclude.end()) return true;
        return false;
    };
    std::optional<RayHit> best;
    for (const WorldCollider& c : colliders(layers)) {
        if (!exclude.empty() && excluded(c.node)) continue;
        std::optional<float> t = c.shape == ColliderComponent::Sphere ? ray_sphere(o, d, c.a, c.r) : ray_capsule(o, d, c.a, c.b, c.r);
        if (!t || *t > max_distance) continue;
        if (!best || *t < best->distance - 1e-3f || (std::fabs(*t - best->distance) <= 1e-3f && c.priority > best->priority))
            best = RayHit{c.node, *t, o + d * *t, c.layers, c.user, c.priority};
    }
    return best;
}

std::vector<RayHit> World::overlap_sphere(glm::vec3 centre, float r, uint32_t layers) {
    std::vector<RayHit> out;
    for (const WorldCollider& c : colliders(layers)) {
        float dist = c.shape == ColliderComponent::Sphere ? glm::length(centre - c.a) : segment_distance(centre, c.a, c.b);
        if (dist <= r + c.r) out.push_back({c.node, std::max(0.0f, dist - c.r), centre, c.layers, c.user, c.priority});
    }
    return out;
}

void World::set_ground(std::vector<float> heights, int n, float cell, float origin_x, float origin_z) {
    if (n < 2 || heights.size() != size_t(n) * size_t(n)) {
        ground_.clear();
        ground_n_ = 0;
        return;
    }
    ground_ = std::move(heights);
    ground_n_ = n;
    ground_cell_ = cell;
    ground_x_ = origin_x;
    ground_z_ = origin_z;
}

float World::ground_height(float x, float z) const {
    if (ground_n_ < 2) return 0;
    const int n = ground_n_;
    float gx = std::clamp((x - ground_x_) / ground_cell_, 0.0f, float(n - 1));
    float gz = std::clamp((z - ground_z_) / ground_cell_, 0.0f, float(n - 1));
    int i = std::min(int(gx), n - 2), j = std::min(int(gz), n - 2);
    float fx = gx - float(i), fz = gz - float(j);
    const float* row = &ground_[size_t(j) * size_t(n) + size_t(i)];
    float top = row[0] + (row[1] - row[0]) * fx, bottom = row[n] + (row[n + 1] - row[n]) * fx;
    return top + (bottom - top) * fz;
}

std::optional<float> World::ray_ground(glm::vec3 o, glm::vec3 dir, float max_distance) const {
    const float len = glm::length(dir);
    if (ground_n_ < 2 || len < 1e-12f) return std::nullopt;
    const glm::vec3 d = dir / len;
    auto above = [&](float t) {
        glm::vec3 p = o + d * t;
        return p.y - ground_height(p.x, p.z);
    };
    if (above(0) <= 0) return 0.0f;
    const float step = ground_cell_ / 4;  // march, then bisect the step that went below
    for (float t0 = 0; t0 < max_distance; t0 += step) {
        float t1 = std::min(t0 + step, max_distance);
        if (above(t1) > 0) continue;
        for (int k = 0; k < 20; k++) {
            float mid = (t0 + t1) / 2;
            (above(mid) > 0 ? t0 : t1) = mid;
        }
        return t1;
    }
    return std::nullopt;
}

void World::pose_changed() {
    for (auto [e, a] : reg_.view<AnimatorComponent>().each())
        if ((a.anim != a.posed_anim || a.phase != a.posed_phase) && active_in_hierarchy(e)) pose(a);
}

glm::mat4 World::world_matrix(Entity e) {
    pose_changed();  // a bone's world position must reflect the animation set this step
    Memo memo;
    return world_at(e, 1.0f, memo);
}

void World::collect(float alpha, std::vector<render::Instance>& out, render::FrameDesc& frame) {
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    pose_changed();  // animators changed outside a logic step (tools, init)
    Memo memo;
    for (auto [e, r] : reg_.view<MeshRendererComponent>().each()) {
        if (!r.visible || !r.mesh || r.fade >= 1.0f || !active_in_hierarchy(e)) continue;
        const glm::mat4 m = world_at(e, alpha, memo);
        render::Instance in;
        in.mesh = r.mesh;
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 4; col++) in.model[size_t(row * 4 + col)] = m[col][row];  // glm is column-major
        in.tint = r.tint;
        in.light = r.light;
        in.ground_fog = r.ground_fog;
        in.fade = r.fade;
        out.push_back(in);
    }
    collect_particles(alpha, out, memo);
    if (camera_ != entt::null) {
        const glm::mat4 m = world_at(camera_, alpha, memo);
        const glm::vec3 fwd = glm::normalize(glm::vec3(m * glm::vec4(0, 0, 1, 0)));
        const auto& c = reg_.get<CameraComponent>(camera_);
        frame.eye = {m[3][0], m[3][1], m[3][2]};
        frame.yaw = std::atan2(fwd.x, fwd.z);
        frame.pitch = std::asin(std::clamp(fwd.y, -1.0f, 1.0f));
        frame.fov_x = c.fov_x;
        frame.near_plane = c.near_plane;
        frame.far_plane = c.far_plane;
    }
}

}  // namespace alia::world
