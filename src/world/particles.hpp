// L5 particles (docs/api.md §6): emitters simulated in C++ at the fixed step, drawn as mesh
// instances (the handheld pipeline batches the ones sharing a mesh into one instanced draw).
//
// A ParticleDef is the template (World::add_particles, like a rig); an emitter is a node with an
// EmitterComponent that spawns from it: `rate` per second and/or burst(). Each emitter has its own
// random generator, so headless runs and tests reproduce. Particles keep their previous position
// and are blended between steps like every other node.
#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "render/handheld.hpp"

namespace alia::world {

struct Range {
    float lo = 0, hi = 0;
};

struct Key {  // a curve point over a particle's life: t in 0..1
    float t = 0, value = 0;
};

struct ParticleDef {
    uint32_t mesh = 0;
    Range lifetime{1, 1};       // seconds
    Range speed{0, 0};          // m/s along the shape's direction
    glm::vec3 velocity{0.0f};   // m/s added to every particle (emitter frame), e.g. a lift
    Range size{1, 1};           // scale at birth
    Range rotation{0, 0};       // yaw (rad) at birth
    Range spin{0, 0};           // rad/s about the particle's own vertical axis
    float gravity = 0;          // x 9.8 m/s^2, down
    float drag = 0;             // per second: v *= max(0, 1 - drag dt)
    uint32_t max_particles = 64;
    bool local = false;         // particles move with the emitter (else they stay where they were born)
    enum Shape : uint8_t { Point, Sphere, Hemisphere, Cone, Ring } shape = Point;
    float radius = 0;           // sphere, hemisphere, ring: where they start; cone: the base disc
    float angle = 0.5f;         // cone: half angle (rad) around the emitter's +Y
    std::vector<Key> size_over_life;  // multiplies size; empty: 1
    std::vector<Key> fade_over_life;  // fade into the fog colour (1: gone); empty: 0
    uint8_t tint = 0;                 // palette index replacing the mesh's palette colours (0: keep), unless a burst gives one
    std::vector<Key> tint_over_life;  // palette index from t on (steps, not blended); empty: the tint above
    enum Ground : uint8_t { Through, Stop, Bounce, Die } ground = Through;  // on World's heightmap
    float bounce = 0.4f;        // Bounce: kept fraction of the vertical speed
    render::Light light = render::Light::Material;
    bool ground_fog = true;
};

struct Particle {
    glm::vec3 position, previous, velocity;  // emitter frame when the def is local, else world
    float age, life, size, angle, previous_angle, spin;
    uint8_t tint;
    bool resting;  // stopped on the ground
};

struct EmitterComponent {
    uint32_t def = 0;           // World::add_particles id
    float rate = 0;             // particles per second
    float time_scale = 1;       // slow motion, hit stop (0)
    float owed = 0;             // fraction of a particle carried to the next step
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    std::vector<Particle> particles;
};

}  // namespace alia::world
