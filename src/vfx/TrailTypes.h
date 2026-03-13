#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <cstdint>

namespace glory {

static constexpr uint32_t MAX_TRAIL_POINTS    = 64;
static constexpr uint32_t MAX_ACTIVE_TRAILS   = 32;
static constexpr uint32_t INVALID_TRAIL_HANDLE = 0;

// GPU-side trail point (matches GLSL)
struct alignas(16) GpuTrailPoint {
    glm::vec4 posWidth;    // xyz = world position, w = half-width at this point
    glm::vec4 colorAge;    // rgb = color, a = normalized age (0=newest, 1=oldest)
};
static_assert(sizeof(GpuTrailPoint) == 32, "GpuTrailPoint must be 32 bytes");

// CPU-side trail definition
struct TrailDef {
    std::string id;
    float maxLength       = 3.0f;    // seconds of trail history (max)
    float emitInterval    = 0.016f;  // seconds between new points (~60 Hz)
    float widthStart      = 0.5f;
    float widthEnd        = 0.05f;
    float fadeSpeed       = 2.0f;    // how fast points age
    glm::vec4 colorStart  = {1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 colorEnd    = {1.0f, 1.0f, 1.0f, 0.0f};
    std::string texturePath = "";    // optional texture
    bool additive         = true;
};

} // namespace glory
