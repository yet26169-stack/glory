#pragma once

#include <string>
#include <cstdint>

namespace glory {

/// Defines the Lua hook points that an ability script may implement.
/// Each constant names a Lua function that the ScriptEngine will attempt to
/// call at the corresponding phase of ability execution.
struct AbilityScriptHooks {
    /// Path to the .lua file that contains the hook implementations.
    std::string scriptFile;

    /// ScriptId returned by ScriptEngine::loadScript() (0 = not loaded).
    uint32_t scriptId{0};

    // ── Hook-point names ────────────────────────────────────────────────
    /// Called when the ability is first cast (channel/wind-up start).
    static constexpr const char* ON_CAST      = "onCast";
    /// Called when the ability hits a target (projectile impact, AoE tick, etc.).
    static constexpr const char* ON_HIT       = "onHit";
    /// Called every tick while the ability is active (DoT, channelled, zone).
    static constexpr const char* ON_TICK      = "onTick";
    /// Called when the ability's duration ends naturally.
    static constexpr const char* ON_EXPIRE    = "onExpire";
    /// Called when the ability is interrupted (stun, silence, cancel).
    static constexpr const char* ON_INTERRUPT = "onInterrupt";
};

} // namespace glory
