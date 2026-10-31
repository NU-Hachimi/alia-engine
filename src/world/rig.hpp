// L5 animation: rigid rigs (docs/api.md §4). A rig is bones with parent-relative offsets,
// keyframed Euler angles per bone, part slots and attach points (asset::RigData). Poses follow
// Megalith's rig.py exactly (its golden data checks this code): a phase 0..256 picks two keys and
// blends their angles component by component; R = Ry * Rx * Rz in degrees; the root bone's offset
// gets the animation's root_y.
#pragma once

#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "asset/pack.hpp"

namespace alia::world {

struct LocalPose {
    glm::vec3 position;  // relative to the parent bone (the rig's origin for the root)
    glm::quat rotation;
};

glm::quat euler_degrees(float rx, float ry, float rz);  // Ry(ry) * Rx(rx) * Rz(rz)

// The two keys and the blend for a phase (rig.py key_position): looping animations spread their
// keys over 0..256 and wrap; others over 0..255 and hold the last key.
void key_position(const asset::RigAnimData& anim, float phase, int* i, int* j, float* t);

// Every bone's local transform for `anim` at `phase`; false for an animation that doesn't exist.
bool pose_local(const asset::RigData& rig, int anim, float phase, std::vector<LocalPose>& out);

// The same pose in rig space (each bone composed with its parents).
bool pose_rig_space(const asset::RigData& rig, int anim, float phase, std::vector<glm::mat4>& out);

}  // namespace alia::world
