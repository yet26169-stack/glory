#pragma once

// Data-Driven VFX Definition — enhanced definition struct loaded from JSON.
// Extends the existing EmitterDef concept with emit shapes, multi-force modules,
// render modes, and size curves. Backward-compatible: toEmitterDef() converts to
// the legacy struct for ParticleSystem consumption.

#include "vfx/VFXTypes.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace glory {

struct VFXDefinition {
    std::string name;

    // ── Particle budget and emission ─────────────────────────────────────
    uint32_t maxParticles  = 256;
    float    lifeMin       = 0.8f;
    float    lifeMax       = 1.6f;
    float    emitRate      = 40.0f;
    float    burstCount    = 0.0f;
    bool     looping       = false;
    float    duration      = 1.5f;

    // ── Emission shape ───────────────────────────────────────────────────
    enum class EmitShape : uint8_t { Point, Sphere, Cone, Box };
    EmitShape emitShape    = EmitShape::Cone;
    float     spreadAngle  = 45.0f;   // half-angle for Cone (degrees)
    float     emitRadius   = 0.0f;    // radius for Sphere
    glm::vec3 emitExtents  {0.0f};    // half-extents for Box

    // ── Per-particle randomisation ───────────────────────────────────────
    float initialSpeedMin  = 2.0f;
    float initialSpeedMax  = 6.0f;
    float sizeMin          = 0.2f;
    float sizeMax          = 0.6f;
    float sizeEnd          = -1.0f;   // <0 = use sizeMin (no change)
    float rotationSpeedMin = 0.0f;
    float rotationSpeedMax = 0.0f;

    // ── Physics ──────────────────────────────────────────────────────────
    float     gravity      = 4.0f;
    float     drag         = 0.0f;
    float     alphaCurve   = 1.0f;
    float     softFadeDistance = 0.5f;  // depth-fade for soft particles (0 = hard)
    glm::vec3 windDirection{0.0f};
    float     windStrength = 0.0f;

    // ── Force modules (multiple simultaneous forces) ─────────────────────
    struct ForceModule {
        enum class Type : uint8_t { Gravity, Radial, Vortex, Turbulence };
        Type      type      = Type::Gravity;
        float     strength  = 1.0f;
        glm::vec3 direction {0.0f, -1.0f, 0.0f};
        glm::vec3 center    {0.0f};    // attractor position for Radial/Vortex
    };
    std::vector<ForceModule> forces;

    // ── Rendering ────────────────────────────────────────────────────────
    enum class RenderMode : uint8_t { Billboard, Mesh, Trail };
    RenderMode renderMode  = RenderMode::Billboard;

    enum class BlendMode : uint8_t { Alpha, Additive };
    BlendMode  blendMode   = BlendMode::Alpha;

    std::string texturePath;                  // atlas texture path
    uint32_t    atlasFrameCount = 1;
    uint32_t    atlasRows       = 1;
    uint32_t    atlasCols       = 1;
    float       atlasFrameRate  = 10.0f;  // frames per second (legacy, see flipbookFPS)
    bool        atlasLoopFrames = true;

    // Flipbook animation
    float       flipbookFPS        = 0.0f;   // 0 = lifetime-based; >0 = fixed FPS
    bool        flipbookRandomStart = false;

    // ── Visual curves ────────────────────────────────────────────────────
    std::vector<std::pair<float, glm::vec4>> colorOverLife;   // {time, rgba}
    std::vector<std::pair<float, float>>     sizeOverLife;    // {time, scale}

    // ── Sub-emitters (per-particle child spawns) ─────────────────────────
    std::vector<SubEmitterDef> subEmitters;

    // ── Conversion to legacy EmitterDef ──────────────────────────────────
    EmitterDef toEmitterDef() const {
        EmitterDef def;
        def.id              = name;
        def.textureAtlas    = texturePath;
        def.atlasFrameCount = atlasFrameCount;
        def.atlasRows       = atlasRows;
        def.atlasCols       = atlasCols;
        def.atlasFrameRate  = atlasFrameRate;
        def.atlasLoopFrames = atlasLoopFrames;
        def.flipbookFPS        = flipbookFPS;
        def.flipbookRandomStart = flipbookRandomStart;
        def.maxParticles    = maxParticles;
        def.emitRate        = emitRate;
        def.burstCount      = burstCount;
        def.looping         = looping;
        def.duration        = duration;
        def.lifetimeMin     = lifeMin;
        def.lifetimeMax     = lifeMax;
        def.initialSpeedMin = initialSpeedMin;
        def.initialSpeedMax = initialSpeedMax;
        def.sizeMin         = sizeMin;
        def.sizeMax         = sizeMax;
        def.sizeEnd         = sizeEnd;
        def.rotationSpeedMin= rotationSpeedMin;
        def.rotationSpeedMax= rotationSpeedMax;
        def.spreadAngle     = spreadAngle;
        def.gravity         = gravity;
        def.drag            = drag;
        def.alphaCurve      = alphaCurve;
        def.softFadeDistance = softFadeDistance;
        def.windDirection   = windDirection;
        def.windStrength    = windStrength;

        // Map first force module to legacy single-force fields
        if (!forces.empty()) {
            const auto& f = forces[0];
            def.forceType     = static_cast<uint32_t>(f.type);
            def.forceStrength = f.strength;
            def.attractorPos  = f.center;
        }

        // Build force bitmask for GPU (stored in forceParams.z)
        uint32_t forceMask = 0;
        for (const auto& f : forces)
            forceMask |= (1u << static_cast<uint32_t>(f.type));
        // Encode bitmask as float for GPU (will be cast back to uint in shader)
        def.forceParams_bitmask = forceMask;

        // Convert color curve
        for (const auto& [t, c] : colorOverLife) {
            def.colorOverLifetime.push_back({t, c});
        }

        // Convert size curve
        for (const auto& [t, s] : sizeOverLife) {
            def.sizeOverLifetime.push_back({t, s});
        }

        if (blendMode == BlendMode::Additive)
            def.blendMode = EmitterDef::BlendMode::Additive;
        else
            def.blendMode = EmitterDef::BlendMode::Alpha;

        def.subEmitters = subEmitters;

        return def;
    }
};

} // namespace glory
