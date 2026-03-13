#pragma once

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace glory {

struct MeshEffectDef {
    std::string id;
    std::string meshPath;       // glTF / OBJ file
    std::string vertShader = "mesh_effect.vert.spv";
    std::string fragShader = "mesh_effect_energy.frag.spv";
    float duration     = 0.5f;
    float scaleStart   = 0.1f;
    float scaleEnd     = 1.0f;
    float alphaStart   = 1.0f;
    float alphaEnd     = 0.0f;
    float rotationSpeed = 0.0f; // rad/s
    glm::vec4 colorStart = {1,1,1,1};
    glm::vec4 colorEnd   = {1,1,1,0};
    bool additive = true;
};

struct MeshEffectInstance {
    uint32_t handle;
    const MeshEffectDef* def;
    glm::vec3 position;
    glm::vec3 direction;
    float scaleBase;
    float elapsed = 0.0f;
    float currentRotation = 0.0f;
};

} // namespace glory
