#pragma once
/// DissolveEffect: Per-entity noise-based dissolve for death/spawn animations.
/// progress 0.0 = fully visible, 1.0 = fully dissolved (invisible).
/// Set reverse=true for spawn reveal (progress goes 1→0).

#include <glm/glm.hpp>

namespace glory {

struct DissolveEffect {
    float     progress  = 0.0f;                      // 0 = visible, 1 = gone
    float     speed     = 1.0f;                       // units per second
    bool      reverse   = false;                      // true = reveal (spawn), false = dissolve (death)
    float     edgeWidth = 0.05f;                      // glow border width
    glm::vec3 edgeColor = {3.0f, 1.5f, 0.2f};        // HDR edge glow (hot orange/gold)
};

} // namespace glory
