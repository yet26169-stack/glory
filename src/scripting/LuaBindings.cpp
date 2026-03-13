#include "LuaBindings.h"
#include "ScriptEngine.h"
#include <spdlog/spdlog.h>

namespace glory {

// ── Individual binding stubs ────────────────────────────────────────────────

static void registerEntityBindings([[maybe_unused]] ScriptEngine& engine) {
    // TODO: expose entity position, rotation, scale, spawn/destroy
    spdlog::info("LuaBindings: registered Entity bindings (stub)");
}

static void registerWorldBindings([[maybe_unused]] ScriptEngine& engine) {
    // TODO: expose spatial queries, game-time, entity-by-component lookups
    spdlog::info("LuaBindings: registered World bindings (stub)");
}

static void registerAbilityBindings([[maybe_unused]] ScriptEngine& engine) {
    // TODO: expose AbilityDefinition fields, cooldowns, damage/heal/buff API
    spdlog::info("LuaBindings: registered Ability bindings (stub)");
}

static void registerVFXBindings([[maybe_unused]] ScriptEngine& engine) {
    // TODO: expose particle emitters, trail spawning, composite VFX sequences
    spdlog::info("LuaBindings: registered VFX bindings (stub)");
}

static void registerCombatBindings([[maybe_unused]] ScriptEngine& engine) {
    // TODO: expose StatsComponent, damage application, status checks
    spdlog::info("LuaBindings: registered Combat/Stats bindings (stub)");
}

// ── Public entry point ──────────────────────────────────────────────────────

void registerAllBindings(ScriptEngine& engine) {
    spdlog::info("LuaBindings: registering all Lua bindings (stub)…");

    registerEntityBindings(engine);
    registerWorldBindings(engine);
    registerAbilityBindings(engine);
    registerVFXBindings(engine);
    registerCombatBindings(engine);

    spdlog::info("LuaBindings: all binding categories registered");
}

} // namespace glory
