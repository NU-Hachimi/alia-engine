#include "world/rig.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>

namespace alia::world {

glm::quat euler_degrees(float rx, float ry, float rz) {
    return glm::quat_cast(glm::eulerAngleYXZ(glm::radians(ry), glm::radians(rx), glm::radians(rz)));
}

void key_position(const asset::RigAnimData& anim, float phase, int* i, int* j, float* t) {
    const int keys = anim.keys;
    if (keys <= 1) {
        *i = *j = 0;
        *t = 0;
        return;
    }
    if (anim.loop) {
        float f = std::fmod(std::max(phase, 0.0f), 256.0f) * float(keys) / 256.0f;
        *i = std::min(int(f), keys - 1);
        *j = (*i + 1) % keys;
        *t = f - float(*i);
        return;
    }
    float f = std::clamp(phase, 0.0f, 255.0f) * float(keys - 1) / 255.0f;
    *i = std::min(int(f), keys - 2);
    *j = *i + 1;
    *t = f - float(*i);
}

bool pose_local(const asset::RigData& rig, int anim_id, float phase, std::vector<LocalPose>& out) {
    if (anim_id < 0 || anim_id >= int(rig.anims.size())) return false;
    const asset::RigAnimData& anim = rig.anims[size_t(anim_id)];
    int i, j;
    float t;
    key_position(anim, phase, &i, &j, &t);
    out.resize(rig.bones.size());
    for (size_t b = 0; b < rig.bones.size(); b++) {
        const asset::RigBoneData& bone = rig.bones[b];
        glm::vec3 offset(bone.offset[0], bone.offset[1], bone.offset[2]);
        if (bone.parent < 0 && !anim.root_y.empty())
            offset.y += anim.root_y[size_t(i)] + (anim.root_y[size_t(j)] - anim.root_y[size_t(i)]) * t;
        glm::quat rotation(1, 0, 0, 0);
        const std::vector<float>* track = anim.tracks.size() > b ? &anim.tracks[b] : nullptr;
        if (track && !track->empty()) {
            const float* a = &(*track)[size_t(i) * 3];
            const float* c = &(*track)[size_t(j) * 3];
            rotation = euler_degrees(a[0] + (c[0] - a[0]) * t, a[1] + (c[1] - a[1]) * t, a[2] + (c[2] - a[2]) * t);
        }
        out[b] = {offset, rotation};
    }
    return true;
}

bool pose_rig_space(const asset::RigData& rig, int anim, float phase, std::vector<glm::mat4>& out) {
    std::vector<LocalPose> local;
    if (!pose_local(rig, anim, phase, local)) return false;
    out.resize(local.size());
    for (size_t b = 0; b < local.size(); b++) {
        glm::mat4 m = glm::translate(glm::mat4(1.0f), local[b].position) * glm::mat4_cast(local[b].rotation);
        const int parent = rig.bones[b].parent;
        out[b] = parent < 0 ? m : out[size_t(parent)] * m;
    }
    return true;
}

}  // namespace alia::world
