// L5 world (world/world.hpp): hierarchy, interpolation, the camera. No GPU needed.
#include <cmath>
#include <cstdio>
#include <vector>

#include "world/world.hpp"

using namespace alia;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

// Translation of the instance with this mesh in the collected list (row-major 3x4: t is column 3).
const render::Instance* find(const std::vector<render::Instance>& list, uint32_t mesh) {
    for (const auto& in : list)
        if (in.mesh == mesh) return &in;
    return nullptr;
}

}  // namespace

int main() {
    world::World w;
    render::FrameDesc frame;
    std::vector<render::Instance> list;

    auto parent = w.spawn(), child = w.spawn(), grandchild = w.spawn(), other = w.spawn();
    check(w.count() == 4, "four entities");
    check(w.set_parent(child, parent) && w.set_parent(grandchild, child), "parenting");
    check(!w.set_parent(parent, grandchild) && !w.set_parent(parent, parent), "cycles refused");
    w.renderer(parent).mesh = 1;
    w.renderer(child).mesh = 2;
    w.renderer(grandchild).mesh = 3;
    w.transform(parent).position = {10, 0, 0};
    w.transform(parent).rotation = world::from_ypr(float(M_PI) / 2, 0, 0);  // +Z turns to +X
    w.transform(child).position = {0, 0, 2};
    w.transform(grandchild).scale = glm::vec3(3.0f);
    w.collect(1, list, frame);
    const render::Instance* c = find(list, 2);
    check(list.size() == 3 && c && near(c->model[3], 12) && near(c->model[7], 0) && near(c->model[11], 0),
          "child at parent position + parent rotation * local offset");
    const render::Instance* g = find(list, 3);
    check(g && near(g->model[3], 12) && near(std::hypot(g->model[0], g->model[4], g->model[8]), 3),
          "grandchild inherits position, keeps its own scale");
    check(near(w.world_matrix(child)[3][0], 12), "world_matrix agrees");

    // Interpolation: the step starts, the game moves things, the display is halfway.
    w.begin_step();
    w.transform(parent).position = {20, 0, 0};
    w.transform(other).position = {0, 0, 0};
    w.renderer(other).mesh = 4;
    list.clear();
    w.collect(0.5f, list, frame);
    check(near(find(list, 1)->model[3], 15), "position blended halfway");
    check(near(find(list, 2)->model[3], 17), "the child follows its blended parent");
    w.transform(other).position = {100, 0, 0};
    w.teleport(other);
    list.clear();
    w.collect(0.25f, list, frame);
    check(near(find(list, 4)->model[3], 100), "teleport: no blend from where it was");
    w.begin_step();
    w.transform(parent).rotation = world::from_ypr(float(M_PI) / 2 + 1.0f, 0, 0);
    list.clear();
    w.collect(0.5f, list, frame);
    float yaw, pitch, roll;
    const float* r = find(list, 1)->model.data();  // the draw order is the registry's, not spawn order
    glm::vec3 dir = glm::mat3(glm::vec3(r[0], r[4], r[8]), glm::vec3(r[1], r[5], r[9]), glm::vec3(r[2], r[6], r[10])) *
                    glm::vec3(0, 0, 1);
    check(near(std::atan2(dir.x, dir.z), float(M_PI) / 2 + 0.5f, 1e-3f), "rotation slerped halfway");
    world::to_ypr(world::from_ypr(0.3f, -0.4f, 0.2f), &yaw, &pitch, &roll);
    check(near(yaw, 0.3f) && near(pitch, -0.4f) && near(roll, 0.2f), "yaw/pitch/roll round trip");

    // Hidden and mesh-less entities are not drawn; destroying a parent takes its children.
    w.renderer(other).visible = false;
    list.clear();
    w.collect(1, list, frame);
    check(!find(list, 4), "invisible: not drawn");
    w.destroy(child);
    check(!w.valid(child) && !w.valid(grandchild) && w.valid(parent) && w.count() == 2, "destroy takes the children");
    w.begin_step();  // the parent's remaining child list is clean
    check(w.registry().get<world::HierarchyComponent>(parent).children.empty(), "the parent forgot the child");

    // The camera: an entity whose world transform becomes the frame's eye, yaw and pitch.
    check(!w.camera_active(), "no camera until asked for");
    auto cam = w.camera();
    w.transform(cam).position = {1, 2, 3};
    w.transform(cam).rotation = world::from_ypr(0.7f, 0.2f, 0);  // pitch > 0 tips the nose down
    list.clear();
    w.collect(1, list, frame);
    check(w.camera_active() && near(frame.eye[0], 1) && near(frame.eye[1], 2) && near(frame.eye[2], 3), "camera eye");
    check(near(frame.yaw, 0.7f) && near(frame.pitch, -0.2f), "camera yaw; frame pitch is up-positive");
    w.set_parent(cam, parent);  // the camera rides along
    list.clear();
    w.collect(1, list, frame);
    check(!near(frame.eye[0], 1), "a camera with a parent moves with it");
    w.clear();
    check(w.count() == 0 && !w.camera_active(), "clear");

    // Rigs: a body with an arm; "wave" turns the arm 0 -> 90 degrees about Z over two keys (no loop),
    // "spin" loops the body about Y over 4 keys with events at phases 0 and 128.
    asset::RigData rig;
    rig.name = "test";
    rig.bones = {{"body", -1, {0, 1, 0}, "body_mesh"}, {"arm", 0, {1, 0, 0}, "arm_mesh"}};
    rig.slots = {{1, {0.5f, 0, 0}, {0, 0, 0}, {"", "hat"}}};
    rig.attach = {{"hand", 1, {1, 0, 0}}};
    asset::RigAnimData wave{"wave", false, 2, {}, {{}, {0, 0, 0, 0, 0, 90}}, {}};
    asset::RigAnimData spin{"spin", true, 4, {0, 0.5f, 0, 0.5f}, {{0, 0, 0, 0, 90, 0, 0, 180, 0, 0, 270, 0}, {}},
                            {{0, "step", {}, {}}, {128, "step", {{"foot", "left"}}, {{"n", 2}}}}};
    rig.anims = {wave, spin};
    const uint32_t rig_id = w.add_rig(rig);
    auto meshes = [](const std::string& name) -> uint32_t { return name == "body_mesh" ? 10 : name == "arm_mesh" ? 11 : 0; };
    auto root = w.spawn_rig(rig_id, meshes);
    world::AnimatorComponent* an = w.animator(root);
    check(an && an->bones.size() == 2 && an->slots.size() == 1, "spawn_rig: a node per bone and slot");
    check(w.renderer(an->bones[0]).mesh == 10 && w.renderer(an->bones[1]).mesh == 11 && w.renderer(an->slots[0]).mesh == 0,
          "bone meshes resolved by name; slots start empty");
    an->anim = 0;
    an->phase = 255;  // the end of "wave": the arm turned 90 degrees, so the hand (1 m along the arm) points up
    list.clear();
    w.collect(1, list, frame);
    auto hand = w.attach_point(root, "hand");
    glm::mat4 hm = w.world_matrix(hand);
    check(near(hm[3][0], 1) && near(hm[3][1], 2) && near(hm[3][2], 0), "posed by collect(); attach point follows its bone");
    check(w.attach_point(root, "hand") == hand && w.attach_point(root, "nope") == entt::null, "attach points: one node each");
    // The engine plays "spin" itself: 64 phase units per second, 4 steps of 0.5 s -> phases 32 .. 128.
    an = w.animator(root);
    an->anim = 1;
    an->phase = 0;
    an->speed = 64;
    w.update_animators(0.5f);  // a new animation: the event at phase 0 fires
    check(an->fired.size() == 1 && an->fired[0].index == 0, "a new animation fires the events at its start");
    w.update_animators(0.5f);  // 32 -> 64
    check(an->fired.empty(), "no event between 32 and 64");
    w.update_animators(1.0f);  // 64 -> 128
    check(an->fired.size() == 1 && an->fired[0].index == 1, "the event at 128 fires when crossed");
    w.update_animators(2.0f);  // 128 -> 256 = 0 (wraps)
    check(near(an->phase, 0) && an->fired.size() == 1 && an->fired[0].index == 0, "loop wraps; the event at 0 fires again");
    // Interpolation between two posed steps: halfway between 0 and 90 degrees of body yaw.
    w.begin_step();
    an->speed = 0;
    an->phase = 64;  // key 1: 90 degrees
    w.update_animators(0.1f);
    list.clear();
    w.collect(0.5f, list, frame);
    const float* b = find(list, 10)->model.data();
    check(near(std::atan2(b[2], b[0]), float(M_PI) / 4, 1e-3f), "bones interpolate their local rotations");
    check(near(b[7], 1.25f), "root_y in the root bone's offset, blended too (1.0 at phase 0, 1.5 at 64)");
    w.destroy(root);
    check(w.count() == 0, "destroying the rig takes its bones, slots and attach points");

    // Collision.
    using C = world::ColliderComponent;
    auto ball = w.spawn(), pill = w.spawn(), armour = w.spawn(), off = w.spawn();
    w.transform(ball).position = {0, 0, 10};
    w.registry().emplace<C>(ball, C::Sphere, glm::vec3(0), glm::vec3(0), 1.0f, 1u, 7u, 0, true);
    w.transform(pill).position = {5, 0, 10};
    w.registry().emplace<C>(pill, C::Capsule, glm::vec3(0, -2, 0), glm::vec3(0, 2, 0), 0.5f, 2u, 8u, 0, true);
    w.transform(armour).position = {0, 0, 10};  // the same sphere as `ball`, higher priority
    w.registry().emplace<C>(armour, C::Sphere, glm::vec3(0), glm::vec3(0), 1.0f, 4u, 9u, 1, true);
    w.registry().emplace<C>(off, C::Sphere, glm::vec3(0, 0, 3), glm::vec3(0), 1.0f, 1u, 10u, 0, false);
    auto hit = w.ray_cast({0, 0, 0}, {0, 0, 2}, 100, 0xFFFFFFFF);
    check(hit && near(hit->distance, 9) && hit->user == 9, "ray: nearest, the tie goes to the higher priority; the disabled one doesn't count");
    hit = w.ray_cast({0, 0, 0}, {0, 0, 1}, 100, 1u);
    check(hit && hit->user == 7 && near(hit->point[2], 9), "layers filter");
    check(!w.ray_cast({0, 0, 0}, {0, 0, 1}, 8, 1u), "max distance");
    check(!w.ray_cast({0, 0, 10}, {0, 0, 1}, 100, 1u), "a ray starting inside a shape doesn't hit it");
    hit = w.ray_cast({5, 1.5f, 0}, {0, 0, 1}, 100, 2u);
    check(hit && near(hit->distance, 9.5f) && hit->user == 8, "capsule wall");
    hit = w.ray_cast({5, 10, 10}, {0, -1, 0}, 100, 2u);
    check(hit && near(hit->distance, 7.5f), "capsule end cap");
    check(!w.ray_cast({0, 0, 0}, {0, 0, 1}, 100, 5u, {ball, armour}), "exclude");
    w.transform(ball).scale = glm::vec3(2.0f);
    hit = w.ray_cast({0, 0, 0}, {0, 0, 1}, 100, 1u);
    check(hit && near(hit->distance, 8), "the radius scales with the node");
    auto kid = w.spawn();
    w.set_parent(kid, ball);
    w.transform(kid).position = {0, 5, 0};  // 10 m up in the world (the parent's scale is 2)
    w.registry().emplace<C>(kid, C::Sphere, glm::vec3(0), glm::vec3(0), 0.5f, 16u, 11u, 0, true);
    hit = w.ray_cast({0, 10, 0}, {0, 0, 1}, 100, 16u);
    check(hit && near(hit->distance, 9), "colliders follow their parents (radius 0.5 x the parent's scale 2)");
    check(!w.ray_cast({0, 10, 0}, {0, 0, 1}, 100, 16u, {ball}), "excluding a node excludes its descendants");
    auto touching = w.overlap_sphere({0, 0, 7.5f}, 1.0f, 0xFFFFFFFF);
    check(touching.size() == 1 && touching[0].user == 7, "overlap_sphere");
    check(w.colliders(0xFFFFFFFF).size() == 4, "colliders(): the enabled ones");

    // The ground: 3 x 3 heights, 10 m apart from (-10, -10); a hill of 4 m in the middle.
    w.set_ground({0, 0, 0, 0, 4, 0, 0, 0, 0}, 3, 10, -10, -10);
    check(near(w.ground_height(0, 0), 4) && near(w.ground_height(5, 0), 2) && near(w.ground_height(-50, 0), 0),
          "ground: bilinear, clamped at the edges");
    auto landing = w.ray_ground({-10, 10, 0}, {1, -1, 0}, 100);
    check(landing && near(w.ground_height(-10 + *landing / std::sqrt(2.0f), 0), 10 - *landing / std::sqrt(2.0f), 1e-3f),
          "ray_ground lands on the surface");
    check(!w.ray_ground({0, 10, 0}, {0, 1, 0}, 100), "a ray going up misses the ground");

    // Rig colliders: on child nodes of the bones (they move with the animation); slot colliders on the
    // slot nodes, off until a variant is fitted.
    rig.bones[1].colliders = {{1, {1, 0, 0}, {1, 0, 0}, 0.3f, 32u, 5u, 0}};
    rig.slots[0].collider = {1, {0, 0, 0}, {0, 0, 0}, 0.2f, 64u, 0u, 1};
    const uint32_t rig2 = w.add_rig(rig);
    auto r2 = w.spawn_rig(rig2, meshes);
    w.animator(r2)->anim = 0;
    w.animator(r2)->phase = 255;  // the arm points up: its collider (1 m along it) is at (1, 2, 0)
    hit = w.ray_cast({-5, 2, 0}, {1, 0, 0}, 100, 32u);
    check(hit && near(hit->point[0], 0.7f), "a bone's collider follows the pose");
    check(!w.ray_cast({-5, 1, 0}, {1, 0, 0}, 100, 64u), "slot colliders start off");

    // Activity (api.md §3.1): off takes a node and its children out of the world; children keep their own flag.
    w.clear();
    auto top = w.spawn(), mid = w.spawn(), leaf = w.spawn();
    w.set_parent(mid, top);
    w.set_parent(leaf, mid);
    w.renderer(top).mesh = 1;
    w.renderer(mid).mesh = 2;
    w.renderer(leaf).mesh = 3;
    w.registry().emplace<C>(leaf, C::Sphere, glm::vec3(0), glm::vec3(0), 0.5f, 1u, 0u, 0, true);
    w.set_active(leaf, false);
    w.set_active(top, false);
    check(!w.active_in_hierarchy(mid) && w.active(mid) && !w.active(leaf), "a parent off: its children are out, their flags kept");
    list.clear();
    w.collect(1.0f, list, frame);
    check(list.empty() && w.colliders(1u).empty(), "out of the world: not drawn, no colliders");
    w.set_active(top, true);
    list.clear();
    w.collect(1.0f, list, frame);
    check(list.size() == 2 && !find(list, 3) && w.colliders(1u).empty(), "back on: the child that was off stays off");
    w.set_active(leaf, true);
    w.renderer(mid).fade = 1.0f;
    list.clear();
    w.collect(1.0f, list, frame);
    check(list.size() == 2 && !find(list, 2) && w.colliders(1u).size() == 1, "fade 1 is not drawn; the collider stays");
    // Back in the world: no slide from where it was when it left.
    w.renderer(mid).fade = 0.0f;
    w.set_active(top, false);
    w.begin_step();
    w.transform(top).position = {10, 0, 0};
    w.set_active(top, true);
    list.clear();
    w.collect(0.0f, list, frame);
    check(near(find(list, 1)->model[3], 10), "turned back on this step: drawn where it is, not blended");

    // Parenting that keeps the world transform (api.md §3.2).
    w.clear();
    auto horse = w.spawn(), saddle = w.spawn(), rider = w.spawn();
    w.set_parent(saddle, horse);
    w.transform(horse).position = {5, 0, 0};
    w.transform(horse).rotation = world::from_ypr(1.0f, 0, 0);
    w.transform(saddle).position = {0, 1, 0};
    w.transform(rider).position = {5, 3, 2};
    w.begin_step();
    w.transform(rider).position = {5, 3, 3};  // it moved this step
    check(w.set_parent(rider, saddle, true), "attach keeping the world transform");
    glm::mat4 m = w.world_matrix(rider);
    check(near(m[3][0], 5) && near(m[3][1], 3) && near(m[3][2], 3), "it stays where it was");
    list.clear();
    w.renderer(rider).mesh = 4;
    w.collect(0.5f, list, frame);
    const auto* half = find(list, 4);
    check(half && near(half->model[3], 5) && near(half->model[7], 3) && near(half->model[11], 2.5f),
          "and still interpolates the step (halfway between z 2 and 3)");
    w.transform(horse).position = {6, 0, 0};
    m = w.world_matrix(rider);
    check(near(m[3][0], 6) && near(m[3][2], 3), "then moves with its new parent");
    check(w.set_parent(rider, entt::null, true) && near(w.world_matrix(rider)[3][0], 6), "detach keeping the world transform");

    // Rigid IK (api.md §4.1): a shoulder 2 m up, an upper arm and a forearm of 1 m hanging down, a head.
    w.clear();
    asset::RigData body;
    body.name = "body";
    body.bones = {{"base", -1, {0, 0, 0}, ""}, {"upper", 0, {0, 2, 0}, ""}, {"lower", 1, {0, -1, 0}, ""}, {"head", 0, {0, 3, 0}, ""}};
    body.anims = {{"rest", true, 1, {}, {}, {}}};
    auto person = w.spawn_rig(w.add_rig(body), [](const std::string&) { return 0u; });
    auto* pa = w.animator(person);
    auto at = [&](int bone, glm::vec3 local = glm::vec3(0)) { return glm::vec3(w.world_matrix(pa->bones[size_t(bone)]) * glm::vec4(local, 1)); };
    world::IkConstraint reach;
    reach.kind = world::IkConstraint::TwoBone;
    reach.bone = 1;
    reach.lower = 2;
    reach.end = {0, -1, 0};
    reach.pole = {0, 0, 1};
    reach.point = {1.2f, 2.5f, 0.5f};
    pa->ik = {reach};
    w.update_animators(0);
    glm::vec3 shoulder = at(1), elbow = at(2), hand_at = at(2, {0, -1, 0});
    check(glm::length(hand_at - reach.point) < 1e-3f, "two_bone: the end reaches the target");
    check(near(glm::length(elbow - shoulder), 1) && near(glm::length(hand_at - elbow), 1), "two_bone: the bones keep their lengths");
    const glm::vec3 reach_dir = glm::normalize(reach.point - shoulder);
    const glm::vec3 bend = elbow - shoulder - reach_dir * glm::dot(elbow - shoulder, reach_dir);
    check(glm::dot(bend, glm::vec3(0, 0, 1) - reach_dir * reach_dir.z) > 0, "two_bone: the elbow bends towards the pole");
    pa->ik[0].point = {5, 2, 0};
    w.update_animators(0);
    check(glm::length(at(2, {0, -1, 0}) - glm::vec3(2, 2, 0)) < 1e-2f, "two_bone: out of reach, it stretches towards the target");
    world::IkConstraint look;
    look.bone = 3;
    look.axis = {0, 0, 1};
    look.point = {5, 3, 0};
    pa->ik = {look};
    w.update_animators(0);
    glm::vec3 facing = at(3, {0, 0, 1}) - at(3);
    check(near(facing.x, 1, 1e-3f) && near(facing.z, 0, 1e-3f), "look_at: the axis points at the target");
    pa->ik[0].max_angle = glm::radians(30.0f);
    w.update_animators(0);
    facing = at(3, {0, 0, 1}) - at(3);
    check(near(std::atan2(facing.x, facing.z), glm::radians(30.0f), 1e-3f), "look_at: no further than max_angle");
    pa->ik[0].max_angle = 3.14159265f;
    pa->ik[0].weight = 0.5f;
    w.update_animators(0);
    facing = at(3, {0, 0, 1}) - at(3);
    check(near(std::atan2(facing.x, facing.z), glm::radians(45.0f), 1e-3f), "weight blends from the animated pose");
    pa->ik[0].weight = 1;
    auto bird = w.spawn();
    w.transform(bird).position = {0, 3, -5};
    pa->ik[0].target = bird;
    w.update_animators(0);
    facing = at(3, {0, 0, 1}) - at(3);
    check(near(facing.z, -1, 1e-3f), "look_at follows a node; turning all the way round works");

    // Particles (api.md §6).
    w.clear();
    w.set_ground({0, 0, 0, 0}, 2, 100, -50, -50);  // flat at y = 0
    world::ParticleDef sparks;
    sparks.mesh = 7;
    sparks.lifetime = {1.0f, 1.0f};
    sparks.speed = {2, 2};
    sparks.gravity = 1;
    sparks.max_particles = 16;
    sparks.ground = world::ParticleDef::Stop;
    sparks.fade_over_life = {{0, 0}, {1, 1}};
    sparks.tint_over_life = {{0.5f, 9}};
    const uint32_t sparks_id = w.add_particles(sparks);
    auto fx = w.spawn_emitter(sparks_id, 1);
    w.transform(fx).position = {0, 1, 0};
    w.burst(fx, 10);
    check(w.emitter(fx)->particles.size() == 10, "burst(10): ten particles");
    w.burst(fx, 10);
    check(w.emitter(fx)->particles.size() == 16, "max_particles: the oldest make room");
    bool from_emitter = true;
    for (const auto& q : w.emitter(fx)->particles) from_emitter = from_emitter && near(glm::length(q.position - glm::vec3(0, 1, 0)), 0) && near(glm::length(q.velocity), 2);
    check(from_emitter, "a point shape: from the emitter, at the speed asked");
    for (int k = 0; k < 30; k++) w.update_particles(1.0f / 60);  // half a second
    bool fell = true, landed = true;
    for (const auto& q : w.emitter(fx)->particles) {
        fell = fell && (q.velocity.y < 2.0f - 9.8f * 0.49f || q.resting);
        landed = landed && q.position.y >= -1e-4f;
    }
    check(fell && landed, "gravity pulls them down; Stop keeps them on the ground");
    list.clear();
    w.collect(1.0f, list, frame);
    bool tinted = list.size() == 16;
    for (const auto& in : list) tinted = tinted && in.mesh == 7 && in.tint == 9 && near(in.fade, 0.5f, 0.02f);
    check(tinted, "drawn as instances: tint from tint_over_life at t 0.5, fade from fade_over_life");
    for (int k = 0; k < 31; k++) w.update_particles(1.0f / 60);
    check(w.emitter(fx)->particles.empty(), "they die at the end of their lifetime");
    // Determinism: the same seed, the same particles.
    auto a1 = w.spawn_emitter(sparks_id, 42), a2 = w.spawn_emitter(sparks_id, 42), a3 = w.spawn_emitter(sparks_id, 43);
    w.burst(a1, 5);
    w.burst(a2, 5);
    w.burst(a3, 5);
    check(w.emitter(a1)->particles[4].velocity == w.emitter(a2)->particles[4].velocity &&
              w.emitter(a1)->particles[4].velocity != w.emitter(a3)->particles[4].velocity,
          "each emitter has its own seed: the same seed reproduces");
    // Rate, local space, bursts at a point with a tint, time scale, activity.
    world::ParticleDef smoke;
    smoke.mesh = 8;
    smoke.lifetime = {10, 10};
    smoke.velocity = {0, 1, 0};
    smoke.local = true;
    smoke.max_particles = 100;
    auto chimney = w.spawn_emitter(w.add_particles(smoke), 3);
    w.emitter(chimney)->rate = 20;
    for (int k = 0; k < 60; k++) w.update_particles(1.0f / 60);
    check(w.emitter(chimney)->particles.size() == 20, "rate: 20 a second");
    w.transform(chimney).position = {5, 0, 0};
    list.clear();
    w.collect(1.0f, list, frame);
    bool moved = true;
    for (const auto& in : list)
        if (in.mesh == 8) moved = moved && in.model[3] > 4.9f;
    check(moved, "local: the particles move with the emitter");
    auto spot = glm::vec3(0, 5, 20);
    w.burst(a1, 1, &spot, 3);
    check(w.emitter(a1)->particles.back().position == spot && w.emitter(a1)->particles.back().tint == 3, "burst at a point, with a tint");
    w.emitter(a1)->time_scale = 0;
    const glm::vec3 frozen = w.emitter(a1)->particles.back().position;
    w.update_particles(0.5f);
    check(w.emitter(a1)->particles.back().position == frozen, "time_scale 0 holds them (hit stop)");
    w.set_active(chimney, false);
    const size_t smoke_count = w.emitter(chimney)->particles.size();
    w.update_particles(1.0f);
    list.clear();
    w.collect(1.0f, list, frame);
    bool none = true;
    for (const auto& in : list) none = none && in.mesh != 8;
    check(none && w.emitter(chimney)->particles.size() == smoke_count, "an inactive emitter is neither drawn nor simulated");
    return failures != 0;
}
