// ── Ability System Unit Tests ────────────────────────────────────────────────
// Standalone executable — uses assert() for validation.
// Tests: JSON loading, state machine, cooldowns, damage formula, status
// effects.

#include "ability/AbilityComponents.h"
#include "ability/AbilityDef.h"
#include "ability/AbilitySystem.h"
#include "ability/CooldownSystem.h"
#include "ability/EffectSystem.h"
#include "ability/StatusEffectSystem.h"

#include <entt.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

using namespace glory;

#ifndef ABILITY_DATA_DIR
#define ABILITY_DATA_DIR "assets/abilities/"
#endif

static bool nearEqual(float a, float b, float eps = 0.01f) {
  return std::abs(a - b) < eps;
}

static AbilityDefinition g_fireballDef;

// ── JSON loading test ───────────────────────────────────────────────────────

void test_load_ability_json() {
  std::string path = std::string(ABILITY_DATA_DIR) + "fire_mage_fireball.json";
  g_fireballDef = AbilityDefinition::LoadFromFile(path);
  assert(g_fireballDef.id == "fire_mage_fireball");
  assert(g_fireballDef.displayName == "Fireball");
  assert(g_fireballDef.slot == AbilitySlot::Q);
  assert(g_fireballDef.targeting == TargetingType::SKILLSHOT);
  assert(g_fireballDef.resourceType == ResourceType::MANA);
  assert(nearEqual(g_fireballDef.costPerLevel[0], 60.0f));
  assert(nearEqual(g_fireballDef.cooldownPerLevel[0], 8.0f));
  assert(nearEqual(g_fireballDef.castTime, 0.25f));
  assert(nearEqual(g_fireballDef.projectile.speed, 1200.0f));
  assert(g_fireballDef.onHitEffects.size() == 1);
  assert(g_fireballDef.onHitEffects[0].type == EffectType::DAMAGE);
  assert(g_fireballDef.onHitEffects[0].damageType == DamageType::MAGICAL);
  assert(nearEqual(g_fireballDef.onHitEffects[0].scaling.apRatio, 0.75f));
  std::printf("  PASS: JSON ability loads correctly\n");
}

// ── Scaling formula test ────────────────────────────────────────────────────

void test_scaling_formula() {
  ScalingFormula sf;
  sf.basePerLevel = {80, 130, 180, 230, 280};
  sf.apRatio = 0.75f;

  // Level 1, 100 AP
  float dmg = sf.evaluate(1, 0.0f, 100.0f, 0.0f, 0.0f, 0.0f);
  assert(nearEqual(dmg, 80.0f + 75.0f)); // 155

  // Level 3, 200 AP
  dmg = sf.evaluate(3, 0.0f, 200.0f, 0.0f, 0.0f, 0.0f);
  assert(nearEqual(dmg, 180.0f + 150.0f)); // 330

  // Level clamped (0 -> index 0)
  dmg = sf.evaluate(0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
  assert(nearEqual(dmg, 80.0f));

  std::printf("  PASS: Scaling formula evaluates correctly\n");
}

// ── Damage formula test ─────────────────────────────────────────────────────

void test_damage_formula() {
  // No resistance -> full damage
  float dmg = EffectSystem::CalculateDamage(100.0f, DamageType::MAGICAL, 0.0f,
                                            0.0f, 0.0f);
  assert(nearEqual(dmg, 100.0f));

  // 100 MR -> 50% reduction (100 / (100 + 100) = 0.5)
  dmg = EffectSystem::CalculateDamage(100.0f, DamageType::MAGICAL, 100.0f, 0.0f,
                                      0.0f);
  assert(nearEqual(dmg, 50.0f));

  // 50 MR -> 66.7% (100 / 150 = 0.667)
  dmg = EffectSystem::CalculateDamage(100.0f, DamageType::MAGICAL, 50.0f, 0.0f,
                                      0.0f);
  assert(nearEqual(dmg, 66.67f, 0.1f));

  // True damage ignores resistance
  dmg = EffectSystem::CalculateDamage(100.0f, DamageType::TRUE_DMG, 100.0f,
                                      0.0f, 0.0f);
  assert(nearEqual(dmg, 100.0f));

  // Penetration: 40% percent pen on 100 MR -> effective 60 MR
  // -> 100 / 160 = 0.625
  dmg = EffectSystem::CalculateDamage(100.0f, DamageType::MAGICAL, 100.0f, 0.0f,
                                      0.4f);
  assert(nearEqual(dmg, 62.5f));

  // Flat pen: 20 flat pen on 50 MR -> effective 30 MR
  // -> 100 / 130 = 0.769
  dmg = EffectSystem::CalculateDamage(100.0f, DamageType::PHYSICAL, 50.0f,
                                      20.0f, 0.0f);
  assert(nearEqual(dmg, 76.92f, 0.1f));

  std::printf("  PASS: Damage formula calculates correctly\n");
}

// ── State machine test ──────────────────────────────────────────────────────

void test_state_machine_transitions() {
  entt::registry reg;
  AbilitySystem abilitySys;
  CooldownSystem cdSys;

  // Create a champion entity with ability book and stats
  auto champion = reg.create();
  auto &book = reg.emplace<AbilityBookComponent>(champion);
  auto &stats = reg.emplace<CombatStatsComponent>(champion);
  stats.currentResource = 500.0f; // Enough mana

  // Set up Q ability as fireball
  auto &qAbility = book.get(AbilitySlot::Q);
  qAbility.def = &g_fireballDef;
  qAbility.level = 1;

  // Initially READY
  assert(qAbility.currentPhase == AbilityPhase::READY);

  // Request cast
  AbilitySystem::requestCast(reg, champion, AbilitySlot::Q);

  // Process: should transition to CASTING (castTime = 0.25s)
  abilitySys.update(reg, 0.016f);
  assert(qAbility.currentPhase == AbilityPhase::CASTING);

  // Not enough time passed -- still CASTING
  abilitySys.update(reg, 0.1f);
  assert(qAbility.currentPhase == AbilityPhase::CASTING);

  // Finish cast time (total > 0.25s) -- transitions to EXECUTING
  abilitySys.update(reg, 0.2f);
  assert(qAbility.currentPhase == AbilityPhase::EXECUTING);

  // One more tick: EXECUTING dispatches effects then enters ON_COOLDOWN
  abilitySys.update(reg, 0.016f);
  assert(qAbility.currentPhase == AbilityPhase::ON_COOLDOWN);
  assert(qAbility.cooldownRemaining > 0.0f);
  assert(nearEqual(qAbility.cooldownRemaining, 8.0f, 0.1f));

  // Tick cooldown
  cdSys.update(reg, 4.0f);
  assert(nearEqual(qAbility.cooldownRemaining, 4.0f, 0.1f));
  assert(qAbility.currentPhase == AbilityPhase::ON_COOLDOWN);

  // Finish cooldown
  cdSys.update(reg, 5.0f);
  assert(qAbility.currentPhase == AbilityPhase::READY);
  assert(qAbility.cooldownRemaining <= 0.0f);

  std::printf("  PASS: State machine transitions correctly\n");
}

// ── Pre-cast validation tests ───────────────────────────────────────────────

void test_precast_validation() {
  entt::registry reg;
  AbilitySystem abilitySys;

  auto champion = reg.create();
  auto &book = reg.emplace<AbilityBookComponent>(champion);
  auto &stats = reg.emplace<CombatStatsComponent>(champion);
  stats.currentResource = 500.0f;

  auto &qAbility = book.get(AbilitySlot::Q);
  qAbility.def = &g_fireballDef;
  qAbility.level = 0; // Not learned!

  // Cast should fail -- not learned
  AbilitySystem::requestCast(reg, champion, AbilitySlot::Q);
  abilitySys.update(reg, 0.016f);
  assert(qAbility.currentPhase == AbilityPhase::READY);

  // Learn the ability, but no mana
  qAbility.level = 1;
  stats.currentResource = 0.0f;

  AbilitySystem::requestCast(reg, champion, AbilitySlot::Q);
  abilitySys.update(reg, 0.016f);
  assert(qAbility.currentPhase == AbilityPhase::READY);

  // Restore mana, but silence the caster
  stats.currentResource = 500.0f;
  auto &status = reg.emplace<StatusEffectsComponent>(champion);
  EffectDef silenceDef;
  silenceDef.type = EffectType::SILENCE;
  silenceDef.duration = 2.0f;
  status.activeEffects.push_back({&silenceDef, entt::null, 2.0f, 0.0f, 0.0f});

  AbilitySystem::requestCast(reg, champion, AbilitySlot::Q);
  abilitySys.update(reg, 0.016f);
  assert(qAbility.currentPhase == AbilityPhase::READY);

  std::printf("  PASS: Pre-cast validation blocks correctly\n");
}

// ── CDR integration test ────────────────────────────────────────────────────

void test_cdr_integration() {
  entt::registry reg;
  AbilitySystem abilitySys;

  auto champion = reg.create();
  auto &book = reg.emplace<AbilityBookComponent>(champion);
  auto &stats = reg.emplace<CombatStatsComponent>(champion);
  stats.currentResource = 500.0f;
  stats.cdr = 0.4f; // 40% CDR

  auto &qAbility = book.get(AbilitySlot::Q);
  qAbility.def = &g_fireballDef;
  qAbility.level = 1;

  // Cast and execute (3 frames: CASTING, EXECUTING, ON_COOLDOWN)
  AbilitySystem::requestCast(reg, champion, AbilitySlot::Q);
  abilitySys.update(reg, 0.016f); // -> CASTING
  abilitySys.update(reg, 0.3f);   // -> EXECUTING
  abilitySys.update(reg, 0.016f); // -> ON_COOLDOWN

  // Cooldown should be 8.0 * (1 - 0.4) = 4.8s
  assert(qAbility.currentPhase == AbilityPhase::ON_COOLDOWN);
  assert(nearEqual(qAbility.cooldownRemaining, 4.8f, 0.2f));

  std::printf("  PASS: CDR reduces cooldown correctly\n");
}

// ── Interrupt test ──────────────────────────────────────────────────────────

void test_interrupt_75_percent_cooldown() {
  entt::registry reg;
  AbilitySystem abilitySys;

  auto champion = reg.create();
  auto &book = reg.emplace<AbilityBookComponent>(champion);
  auto &stats = reg.emplace<CombatStatsComponent>(champion);
  stats.currentResource = 500.0f;

  auto &qAbility = book.get(AbilitySlot::Q);
  qAbility.def = &g_fireballDef;
  qAbility.level = 1;

  // Cast
  AbilitySystem::requestCast(reg, champion, AbilitySlot::Q);
  abilitySys.update(reg, 0.016f);
  assert(qAbility.currentPhase == AbilityPhase::CASTING);

  // Stun mid-cast
  auto &status = reg.emplace<StatusEffectsComponent>(champion);
  EffectDef stunDef;
  stunDef.type = EffectType::STUN;
  stunDef.duration = 1.5f;
  status.activeEffects.push_back({&stunDef, entt::null, 1.5f, 0.0f, 0.0f});

  // Next update: CASTING -> INTERRUPTED
  abilitySys.update(reg, 0.016f);
  assert(qAbility.currentPhase == AbilityPhase::INTERRUPTED);

  // Next update: INTERRUPTED -> ON_COOLDOWN
  abilitySys.update(reg, 0.016f);
  assert(qAbility.currentPhase == AbilityPhase::ON_COOLDOWN);
  // 75% of 8.0 = 6.0
  assert(nearEqual(qAbility.cooldownRemaining, 6.0f, 0.1f));

  std::printf("  PASS: Interrupt applies 75%% cooldown\n");
}

// ── Status effect ticking test ──────────────────────────────────────────────

void test_status_effect_ticking() {
  entt::registry reg;
  StatusEffectSystem statusSys;

  auto target = reg.create();
  auto &stats = reg.emplace<CombatStatsComponent>(target);
  stats.currentHP = 1000.0f;
  stats.maxHP = 1000.0f;

  auto &status = reg.emplace<StatusEffectsComponent>(target);
  EffectDef dotDef;
  dotDef.type = EffectType::DOT;
  dotDef.duration = 3.0f;
  dotDef.tickRate = 1.0f;
  // totalValue = 300 -> 100 per tick (3 ticks)

  status.activeEffects.push_back({&dotDef, entt::null, 3.0f, 0.0f, 300.0f});

  // Tick 1 second
  statusSys.update(reg, 1.0f);
  assert(nearEqual(stats.currentHP, 900.0f, 1.0f));
  assert(status.activeEffects.size() == 1);

  // Tick another second
  statusSys.update(reg, 1.0f);
  assert(nearEqual(stats.currentHP, 800.0f, 1.0f));

  // Tick final second + a bit (should expire)
  statusSys.update(reg, 1.1f);
  assert(nearEqual(stats.currentHP, 700.0f, 1.0f));
  assert(status.activeEffects.empty());

  std::printf("  PASS: Status effects tick and expire correctly\n");
}

// ── Resource deduction test ─────────────────────────────────────────────────

void test_resource_deduction() {
  entt::registry reg;
  AbilitySystem abilitySys;

  auto champion = reg.create();
  auto &book = reg.emplace<AbilityBookComponent>(champion);
  auto &stats = reg.emplace<CombatStatsComponent>(champion);
  stats.currentResource = 100.0f;

  auto &qAbility = book.get(AbilitySlot::Q);
  qAbility.def = &g_fireballDef;
  qAbility.level = 1;

  // Cost at level 1 = 60
  AbilitySystem::requestCast(reg, champion, AbilitySlot::Q);
  abilitySys.update(reg, 0.016f);
  assert(qAbility.currentPhase == AbilityPhase::CASTING);
  assert(nearEqual(stats.currentResource, 40.0f));

  std::printf("  PASS: Resource deducted on cast\n");
}

// ── Combat stats helpers test ───────────────────────────────────────────────

void test_combat_stats_helpers() {
  CombatStatsComponent stats;
  stats.maxHP = 1000.0f;
  stats.currentHP = 1000.0f;
  stats.shield = 0.0f;

  // Damage without shield
  stats.takeDamage(200.0f);
  assert(nearEqual(stats.currentHP, 800.0f));

  // Add shield
  stats.addShield(150.0f);
  stats.takeDamage(100.0f);
  assert(nearEqual(stats.shield, 50.0f));
  assert(nearEqual(stats.currentHP, 800.0f));

  // Damage depletes shield and rest goes to HP
  stats.takeDamage(100.0f);
  assert(nearEqual(stats.shield, 0.0f));
  assert(nearEqual(stats.currentHP, 750.0f));

  // Healing capped at max
  stats.heal(500.0f);
  assert(nearEqual(stats.currentHP, 1000.0f));

  // Death
  stats.takeDamage(1200.0f);
  assert(nearEqual(stats.currentHP, 0.0f));
  assert(!stats.isAlive());

  std::printf("  PASS: Combat stats helpers work correctly\n");
}

// ── Effect system integration test ──────────────────────────────────────────

void test_effect_system_apply() {
  entt::registry reg;
  EffectSystem effectSys;

  auto source = reg.create();
  auto &srcStats = reg.emplace<CombatStatsComponent>(source);
  srcStats.abilityPower = 100.0f;

  auto target = reg.create();
  auto &tgtStats = reg.emplace<CombatStatsComponent>(target);
  tgtStats.currentHP = 1000.0f;
  tgtStats.maxHP = 1000.0f;
  tgtStats.magicResist = 50.0f;

  // Create effect queue
  auto queueEntity = reg.create();
  auto &queue = reg.emplace<EffectQueueComponent>(queueEntity);

  // Queue fireball damage: base 80 + 0.75 * 100 AP = 155 raw
  // 50 MR -> multiplier = 100/150 = 0.667 -> final ~103.3
  queue.enqueue(source, target, &g_fireballDef.onHitEffects[0], 1);

  effectSys.apply(reg);
  float expectedDmg = 155.0f * (100.0f / 150.0f);
  assert(nearEqual(tgtStats.currentHP, 1000.0f - expectedDmg, 1.0f));

  std::printf("  PASS: EffectSystem applies damage correctly\n");
}

// ── Main ────────────────────────────────────────────────────────────────────

int main() {
  std::printf("=== Ability System Tests ===\n");

  test_load_ability_json();
  test_scaling_formula();
  test_damage_formula();
  test_state_machine_transitions();
  test_precast_validation();
  test_cdr_integration();
  test_interrupt_75_percent_cooldown();
  test_status_effect_ticking();
  test_resource_deduction();
  test_combat_stats_helpers();
  test_effect_system_apply();

  std::printf("\nAll %d tests passed!\n", 11);
  return 0;
}
