#pragma once

namespace glory {

class ScriptEngine;

/// Register every Lua binding category with the given engine.
///
/// When sol2 is integrated each helper will expose the following:
///
/// **Entity bindings** – get/set position, rotation, scale; access to
///   entt::entity handle; destroy / spawn helpers.
///
/// **World bindings** – query entities by component, spatial queries
///   (nearest enemy, allies in radius), game-time access.
///
/// **Ability bindings** – read AbilityDefinition fields, cooldown helpers,
///   damage / heal / apply-buff / remove-buff calls, projectile spawning.
///
/// **VFX bindings** – emit particle bursts, spawn trails, trigger composite
///   sequences, screen-shake, sound cues.
///
/// **Combat / Stats bindings** – read StatsComponent (AD, AP, armour, MR),
///   apply damage with type, check alive / stunned / silenced.
///
void registerAllBindings(ScriptEngine& engine);

} // namespace glory
