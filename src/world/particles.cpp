// L5 particles (world/particles.hpp, docs/api.md §6).
#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "world/world.hpp"

namespace alia::world {
namespace {

constexpr float kGravity = 9.8f;
constexpr float kTau = 6.28318531f;

float next01(uint64_t& s) {  // xorshift64*: one stream per emitter, so runs reproduce
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return float((s * 0x2545F4914F6CDD1Dull) >> 40) / float(1 << 24);
}

float pick(uint64_t& s, Range r) { return r.lo + (r.hi - r.lo) * next01(s); }

glm::vec3 unit(uint64_t& s) {  // uniform on the sphere
    const float z = 2 * next01(s) - 1, a = kTau * next01(s), r = std::sqrt(std::max(0.0f, 1 - z * z));
    return {r * std::cos(a), z, r * std::sin(a)};
}

float curve(const std::vector<Key>& keys, float t, float otherwise) {  // piecewise linear, held at the ends
    if (keys.empty()) return otherwise;
    if (t <= keys.front().t) return keys.front().value;
    for (size_t k = 1; k < keys.size(); k++)
        if (t <= keys[k].t) {
            const Key &a = keys[k - 1], &b = keys[k];
            return a.value + (b.value - a.value) * (b.t > a.t ? (t - a.t) / (b.t - a.t) : 1.0f);
        }
    return keys.back().value;
}

int step(const std::vector<Key>& keys, float t) {  // the last key at or before t (-1: none yet)
    int v = -1;
    for (const Key& k : keys)
        if (k.t <= t) v = int(k.value);
    return v;
}

}  // namespace

uint32_t World::add_particles(ParticleDef def) {
    std::sort(def.size_over_life.begin(), def.size_over_life.end(), [](const Key& a, const Key& b) { return a.t < b.t; });
    std::sort(def.fade_over_life.begin(), def.fade_over_life.end(), [](const Key& a, const Key& b) { return a.t < b.t; });
    std::sort(def.tint_over_life.begin(), def.tint_over_life.end(), [](const Key& a, const Key& b) { return a.t < b.t; });
    particle_defs_.push_back(std::move(def));
    return uint32_t(particle_defs_.size());
}

Entity World::spawn_emitter(uint32_t def, uint64_t seed) {
    if (!particle_def(def)) return entt::null;
    Entity e = spawn();
    EmitterComponent em;
    em.def = def;
    em.rng = seed * 0x9E3779B97F4A7C15ull + 0xD1B54A32D192ED03ull;  // never 0
    reg_.emplace<EmitterComponent>(e, std::move(em));
    return e;
}

void World::emit(Entity e, EmitterComponent& em, const ParticleDef& d, int n, const glm::vec3* at, int tint) {
    if (n <= 0 || !d.max_particles) return;
    pose_changed();
    Memo memo;
    const glm::mat4 m = world_at(e, 1.0f, memo);  // where the emitter is this step
    glm::mat3 rot(m);
    for (int k = 0; k < 3; k++) rot[k] = glm::normalize(rot[k]);
    const glm::vec3 origin = at ? *at : glm::vec3(m[3]);
    const glm::mat4 to_local = glm::inverse(m);
    for (int i = 0; i < n; i++) {
        glm::vec3 dir, offset(0.0f);
        switch (d.shape) {
        case ParticleDef::Point:
            dir = unit(em.rng);
            break;
        case ParticleDef::Sphere:
        case ParticleDef::Hemisphere:
            dir = unit(em.rng);
            if (d.shape == ParticleDef::Hemisphere) dir.y = std::fabs(dir.y);
            offset = dir * d.radius * std::cbrt(next01(em.rng));
            break;
        case ParticleDef::Cone: {
            const float c = 1 - next01(em.rng) * (1 - std::cos(d.angle)), s = std::sqrt(std::max(0.0f, 1 - c * c));
            const float a = kTau * next01(em.rng), r = d.radius * std::sqrt(next01(em.rng)), b = kTau * next01(em.rng);
            dir = {s * std::cos(a), c, s * std::sin(a)};
            offset = {r * std::cos(b), 0, r * std::sin(b)};
            break;
        }
        case ParticleDef::Ring: {
            const float a = kTau * next01(em.rng);
            dir = {std::cos(a), 0, std::sin(a)};
            offset = dir * d.radius;
            break;
        }
        }
        const glm::vec3 v = dir * pick(em.rng, d.speed) + d.velocity;  // emitter frame
        Particle p{};
        p.position = origin + rot * offset;  // world
        p.velocity = rot * v;
        if (d.local) {
            p.position = glm::vec3(to_local * glm::vec4(p.position, 1));
            p.velocity = v;
        }
        p.previous = p.position;
        p.life = std::max(pick(em.rng, d.lifetime), 1e-3f);
        p.size = pick(em.rng, d.size);
        p.angle = p.previous_angle = pick(em.rng, d.rotation);
        p.spin = pick(em.rng, d.spin);
        p.tint = uint8_t(tint >= 0 ? tint : d.tint);
        if (em.particles.size() >= d.max_particles)  // full: the oldest makes room (new effects matter more)
            em.particles.erase(em.particles.begin());
        em.particles.push_back(p);
    }
}

void World::burst(Entity e, int n, const glm::vec3* at, int tint) {
    EmitterComponent* em = emitter(e);
    if (!em) return;
    emit(e, *em, particle_defs_[em->def - 1], n, at, tint);
}

void World::update_particles(float real_dt) {
    for (auto [e, em] : reg_.view<EmitterComponent>().each()) {
        if (!active_in_hierarchy(e)) continue;  // out of the world: frozen, not drawn
        const ParticleDef& d = particle_defs_[em.def - 1];
        const float dt = real_dt * std::max(em.time_scale, 0.0f);
        for (Particle& p : em.particles) {
            p.previous = p.position;
            p.previous_angle = p.angle;
            p.age += dt;
            if (p.resting) continue;
            p.velocity.y -= d.gravity * kGravity * dt;
            p.velocity *= std::max(0.0f, 1 - d.drag * dt);
            p.position += p.velocity * dt;
            p.angle += p.spin * dt;
            if (d.ground == ParticleDef::Through || d.local || !has_ground()) continue;
            const float gy = ground_height(p.position.x, p.position.z);
            if (p.position.y >= gy) continue;
            p.position.y = gy;
            if (d.ground == ParticleDef::Die) {
                p.age = p.life;
            } else if (d.ground == ParticleDef::Bounce && std::fabs(p.velocity.y) * d.bounce > 0.5f) {
                p.velocity.y = -p.velocity.y * d.bounce;
            } else {
                p.velocity = glm::vec3(0.0f);
                p.resting = true;
            }
        }
        std::erase_if(em.particles, [](const Particle& p) { return p.age >= p.life; });
        em.owed += em.rate * dt;
        const int n = int(em.owed);
        em.owed -= float(n);
        const size_t before = em.particles.size();
        emit(e, em, d, n, nullptr, -1);
        for (size_t i = std::min(before, em.particles.size()); i < em.particles.size(); i++) em.particles[i].previous = em.particles[i].position;
    }
}

void World::collect_particles(float alpha, std::vector<render::Instance>& out, Memo& memo) {
    for (auto [e, em] : reg_.view<EmitterComponent>().each()) {
        if (em.particles.empty() || !active_in_hierarchy(e)) continue;
        const ParticleDef& d = particle_defs_[em.def - 1];
        if (!d.mesh) continue;
        const glm::mat4 frame = d.local ? world_at(e, alpha, memo) : glm::mat4(1.0f);
        for (const Particle& p : em.particles) {
            const float t = std::clamp(p.age / p.life, 0.0f, 1.0f);
            const float fade = std::clamp(curve(d.fade_over_life, t, 0.0f), 0.0f, 1.0f);
            if (fade >= 1.0f) continue;
            const float size = p.size * curve(d.size_over_life, t, 1.0f);
            const glm::vec3 at = glm::vec3(frame * glm::vec4(glm::mix(p.previous, p.position, alpha), 1));
            const float angle = p.previous_angle + (p.angle - p.previous_angle) * alpha;
            const glm::mat4 m = glm::scale(glm::rotate(glm::translate(glm::mat4(1.0f), at), angle, glm::vec3(0, 1, 0)), glm::vec3(size));
            render::Instance in;
            in.mesh = d.mesh;
            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 4; col++) in.model[size_t(row * 4 + col)] = m[col][row];
            const int over = step(d.tint_over_life, t);
            in.tint = uint8_t(over >= 0 ? over : p.tint);
            in.light = d.light;
            in.ground_fog = d.ground_fog;
            in.fade = fade;
            out.push_back(in);
        }
    }
}

}  // namespace alia::world
