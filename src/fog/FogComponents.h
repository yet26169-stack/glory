#pragma once
/// FoW gameplay components: vision radius, visibility state, wards.

#include <glm/glm.hpp>
#include <cstdint>

namespace glory {

// ── Vision source radius per entity ─────────────────────────────────────────
struct VisionComponent {
    float sightRadius = 8.0f;   // world units
};

// ── Fog-of-war visibility state (attached to enemy entities) ────────────────
// Tracks whether an entity is visible to the local player's team,
// including fade-in/out and ghost rendering.
enum class FowVisState : uint8_t {
    HIDDEN,     // fully hidden — don't render
    FADING_IN,  // recently entered vision — alpha increasing
    VISIBLE,    // fully visible
    FADING_OUT, // recently left vision — ghost fading
};

struct FowVisibilityComponent {
    FowVisState state        = FowVisState::HIDDEN;
    float       fadeTimer    = 0.0f;    // time remaining in current fade
    float       alpha        = 0.0f;    // render alpha (0=hidden, 1=visible)
    glm::vec3   lastKnownPos {0.f};     // where we last saw them (for ghost)
    bool        inVision     = false;    // raw: is the entity in our team's vision this frame?
};

// ── Ward: placeable vision source ───────────────────────────────────────────
struct WardComponent {
    float   sightRadius = 8.0f;     // vision radius in world units
    float   duration    = 180.0f;   // lifetime in seconds
    float   timeLeft    = 180.0f;   // remaining lifetime
    uint8_t teamIndex   = 0;        // 0=PLAYER, 1=ENEMY
};

} // namespace glory
