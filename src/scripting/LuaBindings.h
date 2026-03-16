#pragma once

#include <entt.hpp>
#include "vfx/VFXEventQueue.h"

namespace glory {

class ScriptEngine;
class AbilitySystem;

/// Register all Lua binding categories with the given engine.
/// Must be called after ScriptEngine::init() and before loading scripts.
///
/// Binding categories:
///   entity — get/set position, rotation; destroy/spawn
///   world  — spatial queries (findEntitiesInRadius, nearestEnemy), getGameTime
///   ability — cooldowns, levels, dealDamage, heal, applyBuff, spawnProjectile
///   vfx    — emit particle effects, screenShake
///   stats  — getAD, getAP, getHealth, isAlive
///
void registerAllBindings(ScriptEngine& engine,
                         entt::registry& registry,
                         AbilitySystem* abilitySystem,
                         VFXEventQueue* vfxQueue,
                         float* gameTime);

} // namespace glory
