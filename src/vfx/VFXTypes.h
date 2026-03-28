#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <array>

namespace glory {

// ── Constants ──────────────────────────────────────────────────────────────
static constexpr uint32_t MAX_PARTICLES_PER_EMITTER = 2048;
static constexpr uint32_t MAX_CONCURRENT_EMITTERS   = 512;
static constexpr uint32_t INVALID_VFX_HANDLE        = 0;

// ── GPU Particle layout (must EXACTLY match GLSL struct in shaders) ────────
// Keep at 64 bytes (4 × vec4) for clean SSBO alignment.
struct alignas(16) GpuParticle {
    glm::vec4 posLife;   // xyz = world pos, w = remaining lifetime (s)
    glm::vec4 velAge;    // xyz = velocity (m/s), w = age (s)
    glm::vec4 color;     // rgba
    glm::vec4 params;    // x=size, y=rotation, z=angularVel, w=packed (atlasFrame+active)
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

// ── Emitter Params (passed to GPU via UBO) ───────────────────────────────
struct alignas(16) GpuColorKey {
    glm::vec4 color;
    float     time;
    float     _pad[3];
};

struct alignas(16) EmitterParams {
    glm::vec4  wind_dt;        // xyz=windDir*strength, w=dt
    glm::vec4  phys;           // x=gravity, y=drag, z=alphaCurve, w=count
    glm::vec4  size;           // x=sizeMin, y=sizeMax, z=sizeEnd, w=softFadeDistance
    glm::vec4  forceParams;    // x=forceType (0-3), y=forceStrength, z=forceBitmask, w=reserved
    glm::vec4  attractorPos;   // xyz=attractor world position, w=reserved
    glm::vec4  atlasParams;   // x=atlasRows, y=atlasCols, z=atlasFrameRate, w=loop(0/1)
    uint32_t   colorKeyCount;
    float      _pad[3];
    GpuColorKey colorKeys[8];
};
static_assert(sizeof(EmitterParams) <= 512, "EmitterParams must be compact");

// ── Sub-emitter definition ─────────────────────────────────────────────────
// Describes a child effect that should be spawned when a particle triggers
// a specific event (death, collision, or a lifetime fraction).
struct SubEmitterDef {
    enum class Trigger : uint8_t {
        OnDeath,              // fires when a particle's lifetime reaches 0
        OnCollision,          // fires on ground/geometry collision (future)
        OnLifetimeFraction    // fires when particle age/totalLife crosses triggerTime
    };
    Trigger     trigger          = Trigger::OnDeath;
    std::string emitterRef;      // ID of the child EmitterDef to spawn
    float       triggerTime      = 1.0f;   // for OnLifetimeFraction: normalised [0..1]
    uint32_t    inheritVelocity  = 0;      // 0=none, 1=inherit parent velocity
    float       probability      = 1.0f;   // 0..1 chance to actually fire
};

// ── Emitter definition (loaded from JSON or hard-coded) ───────────────────
struct EmitterDef {
    std::string  id;

    // Texture
    std::string  textureAtlas;    // path relative to ASSET_DIR, "" = use default white
    uint32_t     atlasFrameCount = 1;
    uint32_t     atlasRows       = 1;
    uint32_t     atlasCols       = 1;
    float        atlasFrameRate  = 10.0f;  // frames per second (legacy, see flipbookFPS)
    bool         atlasLoopFrames = true;

    // Flipbook animation
    float        flipbookFPS        = 0.0f;   // 0 = spread all frames over particle lifetime; >0 = fixed FPS
    bool         flipbookRandomStart = false;  // randomize starting frame per particle

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
    float        sizeEnd         = -1.0f;   // if negative, use sizeMin (no change over life)
    float        rotationSpeedMin= 0.0f;    // deg/s
    float        rotationSpeedMax= 0.0f;
    float        spreadAngle     = 45.0f;   // cone half-angle in degrees (0 = straight up)

    // Physics
    float        gravity         = 4.0f;    // m/s² downward
    float        drag            = 0.0f;    // damping: vel *= (1.0 - drag * dt)
    float        alphaCurve      = 1.0f;    // 1.0 = linear, >1.0 = fast fade in, etc.
    float        softFadeDistance = 0.5f;   // depth-fade for soft particles (0 = hard, higher = softer)
    glm::vec3    windDirection   {0.0f};
    float        windStrength    = 0.0f;

    // Force types: 0=linear gravity, 1=point attractor, 2=vortex, 3=noise turbulence
    uint32_t     forceType       = 0;
    float        forceStrength   = 1.0f;
    glm::vec3    attractorPos    {0.0f};    // world position of attractor (type 1,2)
    uint32_t     forceParams_bitmask = 0;   // bitmask of active force types (for multi-force)

    // Visual curves
    std::vector<ColorKey> colorOverLifetime;  // Evaluated on GPU if not empty
    std::vector<FloatKey> sizeOverLifetime;   // Not strictly used if sizeEnd is set, but keeping for compatibility

    enum class BlendMode : uint8_t { Alpha, Additive };
    BlendMode blendMode = BlendMode::Alpha;

    // Sub-emitters: child effects triggered by particle events
    std::vector<SubEmitterDef> subEmitters;
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
