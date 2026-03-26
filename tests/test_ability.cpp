// ── Ability System Unit Tests ────────────────────────────────────────────────
// Standalone executable — uses assert() for validation.

#include "ability/AbilityComponents.h"
#include "ability/AbilitySystem.h"
#include "vfx/VFXEventQueue.h"
#include "vfx/TrailRenderer.h"
#include "audio/GameAudioEvents.h"
#include "renderer/ExplosionRenderer.h"
#include "renderer/DistortionRenderer.h"
#include "vfx/MeshEffectRenderer.h"
#include "renderer/GroundDecalRenderer.h"
#include "renderer/SpriteEffectRenderer.h"
#include "renderer/ConeAbilityRenderer.h"

#include <entt.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

using namespace glory;

// ── Dummies for Linker ───────────────────────────────────────────────────────
namespace glory {
uint32_t TrailRenderer::spawn(const std::string&, glm::vec3) { return 0; }
void GameAudioEvents::onAbilityHit(const std::string&, const glm::vec3&) {}
void GameAudioEvents::onAbilityCast(const std::string&, const glm::vec3&) {}
void ExplosionRenderer::addExplosion(glm::vec3) {}
uint32_t DistortionRenderer::spawn(const std::string&, glm::vec3) { return 0; }
void MeshEffectRenderer::spawn(const std::string&, glm::vec3, glm::vec3, float) {}
uint32_t GroundDecalRenderer::spawn(const std::string&, glm::vec3, float, float) { return 0; }
void GroundDecalRenderer::setColor(uint32_t, glm::vec4) {}
void GroundDecalRenderer::destroy(uint32_t) {}
void SpriteEffectRenderer::spawn(uint32_t, const glm::vec3&, float, const glm::vec4&) {}
}

static bool nearEqual(float a, float b, float eps = 0.01f) {
  return std::abs(a - b) < eps;
}

// ── DoT Ticking test ────────────────────────────────────────────────────────

void test_dot_ticking() {
    VFXEventQueue vfxQueue;
    AbilitySystem abilitySys(vfxQueue);

    entt::registry reg;
    auto target = reg.create();
    
    auto& stats = reg.emplace<StatsComponent>(target);
    stats.base.maxHP = 1000.0f;
    stats.base.currentHP = 1000.0f;
    stats.base.magicResist = 0.0f; // No resistance for simple test

    auto& status = reg.emplace<StatusEffectsComponent>(target);
    
    EffectDef dotDef;
    dotDef.type = EffectType::DOT;
    dotDef.damageType = DamageType::TRUE_DMG;
    dotDef.duration = 3.0f;
    dotDef.tickRate = 1.0f;
    dotDef.value = 100.0f; // 100 raw damage per tick

    ActiveStatusEffect ase;
    ase.def = &dotDef;
    ase.remainingDuration = 3.0f;
    ase.totalValue = 100.0f;
    ase.tickAccumulator = 0.0f;
    
    status.activeEffects.push_back(ase);

    // Initial state
    assert(nearEqual(stats.base.currentHP, 1000.0f));

    // Tick 0.5s -> no damage yet
    abilitySys.update(reg, 0.5f);
    assert(nearEqual(stats.base.currentHP, 1000.0f));
    assert(status.activeEffects.size() == 1);

    // Tick another 0.6s (total 1.1s) -> 1 tick applied
    abilitySys.update(reg, 0.6f);
    assert(nearEqual(stats.base.currentHP, 900.0f));
    assert(status.activeEffects.size() == 1);

    // Tick 1.0s (total 2.1s) -> 2nd tick applied
    abilitySys.update(reg, 1.0f);
    assert(nearEqual(stats.base.currentHP, 800.0f));

    // Tick 1.0s (total 3.1s) -> 3rd tick applied and effect expired
    abilitySys.update(reg, 1.0f);
    assert(nearEqual(stats.base.currentHP, 700.0f));
    assert(status.activeEffects.empty());

    printf("  PASS: DoT ticks and health decreases correctly\n");
}

// ── Death during DoT test ────────────────────────────────────────────────────

void test_death_during_dot() {
    VFXEventQueue vfxQueue;
    AbilitySystem abilitySys(vfxQueue);

    entt::registry reg;
    auto target = reg.create();
    
    auto& stats = reg.emplace<StatsComponent>(target);
    stats.base.maxHP = 1000.0f;
    stats.base.currentHP = 50.0f; // Low HP
    stats.base.magicResist = 0.0f;

    auto& status = reg.emplace<StatusEffectsComponent>(target);
    
    EffectDef dotDef;
    dotDef.type = EffectType::DOT;
    dotDef.damageType = DamageType::TRUE_DMG;
    dotDef.duration = 10.0f;
    dotDef.tickRate = 1.0f;
    dotDef.value = 100.0f;

    ActiveStatusEffect ase;
    ase.def = &dotDef;
    ase.remainingDuration = 10.0f;
    ase.totalValue = 100.0f;
    
    status.activeEffects.push_back(ase);

    // One tick should kill the target
    abilitySys.update(reg, 1.1f);
    assert(stats.base.currentHP <= 0.0f);
    assert(status.activeEffects.empty()); // Should be cleared on death

    printf("  PASS: DoT cleared correctly on entity death\n");
}

// ── Main ────────────────────────────────────────────────────────────────────

int main() {
    printf("=== Ability System: Tick Effect Tests ===\n");

    test_dot_ticking();
    test_death_during_dot();

    printf("\nAll tests passed!\n");
    return 0;
}
