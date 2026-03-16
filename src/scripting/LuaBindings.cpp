#include "LuaBindings.h"
#include "ScriptEngine.h"

#include <sol/sol.hpp>

#include "scene/Components.h"
#include "ability/AbilityTypes.h"
#include "ability/AbilityComponents.h"
#include "ability/AbilitySystem.h"
#include "combat/CombatComponents.h"
#include "vfx/VFXEventQueue.h"

#include <spdlog/spdlog.h>
#include <glm/glm.hpp>
#include <cstring>

namespace glory {

// ── Entity bindings ─────────────────────────────────────────────────────────

static void registerEntityBindings(sol::state& lua, entt::registry& reg) {
    auto entity_ns = lua["entity"].get_or_create<sol::table>();

    entity_ns["getPosition"] = [&reg](uint32_t e) -> sol::as_table_t<std::vector<float>> {
        auto entity = static_cast<entt::entity>(e);
        if (reg.valid(entity) && reg.all_of<TransformComponent>(entity)) {
            auto& t = reg.get<TransformComponent>(entity);
            return sol::as_table(std::vector<float>{t.position.x, t.position.y, t.position.z});
        }
        return sol::as_table(std::vector<float>{0.f, 0.f, 0.f});
    };

    entity_ns["setPosition"] = [&reg](uint32_t e, float x, float y, float z) {
        auto entity = static_cast<entt::entity>(e);
        if (reg.valid(entity) && reg.all_of<TransformComponent>(entity)) {
            reg.get<TransformComponent>(entity).position = {x, y, z};
        }
    };

    entity_ns["getRotation"] = [&reg](uint32_t e) -> sol::as_table_t<std::vector<float>> {
        auto entity = static_cast<entt::entity>(e);
        if (reg.valid(entity) && reg.all_of<TransformComponent>(entity)) {
            auto& t = reg.get<TransformComponent>(entity);
            return sol::as_table(std::vector<float>{t.rotation.x, t.rotation.y, t.rotation.z});
        }
        return sol::as_table(std::vector<float>{0.f, 0.f, 0.f});
    };

    entity_ns["destroy"] = [&reg](uint32_t e) {
        auto entity = static_cast<entt::entity>(e);
        if (reg.valid(entity)) {
            reg.destroy(entity);
        }
    };

    spdlog::info("[LuaBindings] registered Entity bindings");
}

// ── World bindings ──────────────────────────────────────────────────────────

static void registerWorldBindings(sol::state& lua, entt::registry& reg, float* gameTime) {
    auto world_ns = lua["world"].get_or_create<sol::table>();

    world_ns["getGameTime"] = [gameTime]() -> float {
        return gameTime ? *gameTime : 0.f;
    };

    world_ns["findEntitiesInRadius"] = [&reg](float cx, float cy, float cz, float radius)
        -> sol::as_table_t<std::vector<uint32_t>>
    {
        std::vector<uint32_t> result;
        float r2 = radius * radius;
        auto view = reg.view<TransformComponent>();
        for (auto e : view) {
            auto& pos = view.get<TransformComponent>(e).position;
            float dx = pos.x - cx, dy = pos.y - cy, dz = pos.z - cz;
            if (dx*dx + dy*dy + dz*dz <= r2) {
                result.push_back(static_cast<uint32_t>(e));
            }
        }
        return sol::as_table(std::move(result));
    };

    world_ns["nearestEnemy"] = [&reg](float cx, float cy, float cz, uint32_t teamVal)
        -> uint32_t
    {
        auto team = static_cast<Team>(teamVal);
        float bestDist = 1e18f;
        uint32_t bestEntity = UINT32_MAX;
        auto view = reg.view<TransformComponent, TeamComponent>();
        for (auto e : view) {
            if (view.get<TeamComponent>(e).team == team) continue;
            auto& pos = view.get<TransformComponent>(e).position;
            float dx = pos.x - cx, dy = pos.y - cy, dz = pos.z - cz;
            float dist = dx*dx + dy*dy + dz*dz;
            if (dist < bestDist) {
                bestDist = dist;
                bestEntity = static_cast<uint32_t>(e);
            }
        }
        return bestEntity;
    };

    spdlog::info("[LuaBindings] registered World bindings");
}

// ── Ability bindings ────────────────────────────────────────────────────────

static void registerAbilityBindings(sol::state& lua, entt::registry& reg,
                                    AbilitySystem* abilitySys, VFXEventQueue* vfxQueue) {
    auto ability_ns = lua["ability"].get_or_create<sol::table>();

    ability_ns["getCooldownRemaining"] = [&reg](uint32_t e, int slotIdx) -> float {
        auto entity = static_cast<entt::entity>(e);
        if (!reg.valid(entity) || !reg.all_of<AbilityBookComponent>(entity)) return 0.f;
        auto& book = reg.get<AbilityBookComponent>(entity);
        if (slotIdx < 0 || slotIdx >= static_cast<int>(AbilitySlot::COUNT)) return 0.f;
        return book.abilities[static_cast<size_t>(slotIdx)].cooldownRemaining;
    };

    ability_ns["getLevel"] = [&reg](uint32_t e, int slotIdx) -> int {
        auto entity = static_cast<entt::entity>(e);
        if (!reg.valid(entity) || !reg.all_of<AbilityBookComponent>(entity)) return 0;
        auto& book = reg.get<AbilityBookComponent>(entity);
        if (slotIdx < 0 || slotIdx >= static_cast<int>(AbilitySlot::COUNT)) return 0;
        return book.abilities[static_cast<size_t>(slotIdx)].level;
    };

    ability_ns["dealDamage"] = [&reg](uint32_t targetId, float amount, const std::string& typeStr) {
        auto target = static_cast<entt::entity>(targetId);
        if (!reg.valid(target) || !reg.all_of<StatsComponent>(target)) return;

        DamageType dtype = DamageType::MAGICAL;
        if (typeStr == "physical") dtype = DamageType::PHYSICAL;
        else if (typeStr == "true") dtype = DamageType::TRUE_DMG;

        auto& stats = reg.get<StatsComponent>(target);
        float finalDmg = calculateDamage(amount, dtype, stats.total());
        stats.base.currentHP -= finalDmg;
        spdlog::debug("[Lua] dealDamage: {} dmg to entity {}", finalDmg, targetId);
    };

    ability_ns["heal"] = [&reg](uint32_t targetId, float amount) {
        auto target = static_cast<entt::entity>(targetId);
        if (!reg.valid(target) || !reg.all_of<StatsComponent>(target)) return;
        auto& stats = reg.get<StatsComponent>(target);
        float totalMax = stats.total().maxHP;
        stats.base.currentHP = std::min(stats.base.currentHP + amount, totalMax);
    };

    ability_ns["applyBuff"] = [&reg](uint32_t targetId, const std::string& buffId, float duration) {
        (void)buffId;
        auto target = static_cast<entt::entity>(targetId);
        if (!reg.valid(target)) return;
        // Ensure StatusEffectsComponent exists
        if (!reg.all_of<StatusEffectsComponent>(target)) {
            reg.emplace<StatusEffectsComponent>(target);
        }
        spdlog::debug("[Lua] applyBuff '{}' to entity {} for {}s", buffId, targetId, duration);
    };

    ability_ns["spawnProjectile"] = [abilitySys, &reg](
        const std::string& defName, float px, float py, float pz,
        float dx, float dy, float dz, float speed)
    {
        if (!abilitySys) return;
        const auto* def = abilitySys->findDefinition(defName);
        if (!def) {
            spdlog::warn("[Lua] spawnProjectile: unknown def '{}'", defName);
            return;
        }
        TargetInfo target;
        target.type = TargetingType::SKILLSHOT;
        target.targetPosition = {px, py, pz};
        target.direction = glm::normalize(glm::vec3{dx, dy, dz});
        (void)speed; // speed comes from the ability definition
        spdlog::debug("[Lua] spawnProjectile '{}' at ({},{},{})", defName, px, py, pz);
    };

    spdlog::info("[LuaBindings] registered Ability bindings");
}

// ── VFX bindings ────────────────────────────────────────────────────────────

static void registerVFXBindings(sol::state& lua, VFXEventQueue* vfxQueue) {
    auto vfx_ns = lua["vfx"].get_or_create<sol::table>();

    vfx_ns["emit"] = [vfxQueue](const std::string& defName,
                                 float px, float py, float pz,
                                 float dx, float dy, float dz) -> uint32_t
    {
        if (!vfxQueue) return 0;
        static uint32_t s_nextHandle = 50000;  // avoid collision with C++ handles

        VFXEvent ev{};
        ev.type = VFXEventType::Spawn;
        ev.handle = ++s_nextHandle;
        std::strncpy(ev.effectID, defName.c_str(), sizeof(ev.effectID) - 1);
        ev.position = {px, py, pz};
        ev.direction = {dx, dy, dz};
        ev.scale = 1.0f;
        ev.lifetime = -1.0f;

        if (!vfxQueue->push(ev)) {
            spdlog::warn("[Lua] vfx.emit: queue full, dropping '{}'", defName);
            return 0;
        }
        return ev.handle;
    };

    vfx_ns["screenShake"] = [](float intensity, float duration) {
        // Placeholder: screen shake would be handled by the camera system
        spdlog::debug("[Lua] screenShake: intensity={}, duration={}", intensity, duration);
    };

    spdlog::info("[LuaBindings] registered VFX bindings");
}

// ── Combat / Stats bindings ─────────────────────────────────────────────────

static void registerCombatBindings(sol::state& lua, entt::registry& reg) {
    auto stats_ns = lua["stats"].get_or_create<sol::table>();

    stats_ns["getAD"] = [&reg](uint32_t e) -> float {
        auto entity = static_cast<entt::entity>(e);
        if (!reg.valid(entity) || !reg.all_of<StatsComponent>(entity)) return 0.f;
        return reg.get<StatsComponent>(entity).total().attackDamage;
    };

    stats_ns["getAP"] = [&reg](uint32_t e) -> float {
        auto entity = static_cast<entt::entity>(e);
        if (!reg.valid(entity) || !reg.all_of<StatsComponent>(entity)) return 0.f;
        return reg.get<StatsComponent>(entity).total().abilityPower;
    };

    stats_ns["getHealth"] = [&reg](uint32_t e) -> float {
        auto entity = static_cast<entt::entity>(e);
        if (!reg.valid(entity) || !reg.all_of<StatsComponent>(entity)) return 0.f;
        return reg.get<StatsComponent>(entity).base.currentHP;
    };

    stats_ns["getMaxHealth"] = [&reg](uint32_t e) -> float {
        auto entity = static_cast<entt::entity>(e);
        if (!reg.valid(entity) || !reg.all_of<StatsComponent>(entity)) return 0.f;
        return reg.get<StatsComponent>(entity).total().maxHP;
    };

    stats_ns["isAlive"] = [&reg](uint32_t e) -> bool {
        auto entity = static_cast<entt::entity>(e);
        if (!reg.valid(entity) || !reg.all_of<StatsComponent>(entity)) return false;
        return reg.get<StatsComponent>(entity).base.currentHP > 0.f;
    };

    spdlog::info("[LuaBindings] registered Combat/Stats bindings");
}

// ── Public entry point ──────────────────────────────────────────────────────

void registerAllBindings(ScriptEngine& engine,
                         entt::registry& registry,
                         AbilitySystem* abilitySystem,
                         VFXEventQueue* vfxQueue,
                         float* gameTime) {
    spdlog::info("[LuaBindings] registering all Lua bindings...");

    sol::state& lua = engine.lua();

    registerEntityBindings(lua, registry);
    registerWorldBindings(lua, registry, gameTime);
    registerAbilityBindings(lua, registry, abilitySystem, vfxQueue);
    registerVFXBindings(lua, vfxQueue);
    registerCombatBindings(lua, registry);

    spdlog::info("[LuaBindings] all binding categories registered");
}

} // namespace glory
