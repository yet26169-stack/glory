#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <array>

namespace glory {

// ── Constants ──────────────────────────────────────────────────────────────
static constexpr uint32_t MAX_PARTICLES_PER_EMITTER = 2048;
static constexpr uint32_t MAX_CONCURRENT_EMITTERS   = 32;
static constexpr uint32_t INVALID_VFX_HANDLE        = 0;

// ── GPU Particle layout (must EXACTLY match GLSL struct in shaders) ────────
// Keep at 64 bytes (4 × vec4) for clean SSBO alignment.
struct alignas(16) GpuParticle {
    glm::vec4 posLife;   // xyz = world pos, w = remaining lifetime (s)
    glm::vec4 velAge;    // xyz = velocity (m/s), w = age (s)
    glm::vec4 color;     // rgba (alpha fades in compute shader over lifetime)
    glm::vec4 params;    // x=size, y=rotation, z=atlasFrame, w=active(1)/dead(0)
};
static_assert(sizeof(GpuParticle) == 64, "GpuParticle must be 64 bytes");

// ── Color gradient key ─────────────────────────────────────────────────────
struct ColorKey {
    float     time;    // normalised lifetime fraction [0..1]
    glm::vec4 color;   // rgba
};

// ── Scalar curve key ────────────────────────────────────────────────────────
struct FloatKey {
    float time;   // normalised lifetime fraction [0..1]
    float value;
};

// ── Emitter definition (loaded from JSON or hard-coded) ───────────────────
struct EmitterDef {
    std::string  id;

    // Texture
    std::string  textureAtlas;    // path relative to ASSET_DIR, "" = use default white
    uint32_t     atlasFrameCount = 1;

    // Particle budget
    uint32_t     maxParticles    = 256;

    // Emission
    float        emitRate        = 40.0f;   // continuous rate (particles/s)
    float        burstCount      = 0.0f;    // instantaneous burst on spawn
    bool         looping         = false;
    float        duration        = 1.5f;    // total emitter lifetime (non-looping)

    // Per-particle randomisation
    float        lifetimeMin     = 0.8f;
    float        lifetimeMax     = 1.6f;
    float        initialSpeedMin = 2.0f;
    float        initialSpeedMax = 6.0f;
    float        sizeMin         = 0.2f;
    float        sizeMax         = 0.6f;
    float        spreadAngle     = 45.0f;   // cone half-angle in degrees (0 = straight up)

    // Physics
    float        gravity         = 4.0f;    // m/s² downward

    // Visual curves (evaluated at spawn; full curve support in AbilitySystem)
    std::vector<ColorKey> colorOverLifetime;  // empty = white→transparent
    std::vector<FloatKey> sizeOverLifetime;   // empty = constant sizeMin
};

// ── VFX Event (game logic → render thread via SPSC queue) ─────────────────
enum class VFXEventType : uint8_t {
    Spawn,          // create a new particle effect
    Destroy,        // force-stop an effect early
    Move,           // teleport an already-active effect to a new world position
};

struct VFXEvent {
    VFXEventType type        = VFXEventType::Spawn;
    uint32_t     handle      = 0;           // output (Spawn) / input (Destroy/Move)
    char         effectID[48]{};            // e.g. "vfx_fireball_cast"
    glm::vec3    position    {0.f};
    glm::vec3    direction   {0.f, 1.f, 0.f};
    float        scale       = 1.0f;
    float        lifetime    = -1.0f;       // <0 = use EmitterDef.duration
};

} // namespace glory
