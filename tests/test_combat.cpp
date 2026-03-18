#include "combat/CombatSystem.h"
#include "combat/EconomySystem.h"
#include "ability/AbilityComponents.h"
#include "scene/Components.h"
#include "vfx/VFXEventQueue.h"

#include <entt.hpp>
#include <cassert>
#include <cstdio>

using namespace glory;

void test_auto_attack_damage() {
    VFXEventQueue vfx;
    CombatSystem combat(vfx);
    entt::registry reg;

    auto attacker = reg.create();
    auto target = reg.create();

    auto& a_comp = reg.emplace<CombatComponent>(attacker);
    a_comp.attackDamage = 100.0f;
    reg.emplace<TransformComponent>(attacker, TransformComponent{{0,0,0}});
    reg.emplace<TeamComponent>(attacker, TeamComponent{Team::PLAYER});

    auto& t_stats = reg.emplace<StatsComponent>(target);
    t_stats.base = Stats{}; // Default is now 0 armor/mr
    t_stats.base.currentHP = 500.0f;
    t_stats.base.maxHP = 500.0f;
    t_stats.bonus = {}; 

    reg.emplace<TransformComponent>(target, TransformComponent{{1,0,0}});
    reg.emplace<TeamComponent>(target, TeamComponent{Team::ENEMY});
    reg.emplace<CombatComponent>(target);

    // Initial HP
    assert(reg.get<StatsComponent>(target).base.currentHP == 500.0f);

    // Trigger windup
    combat.requestAutoAttack(attacker, target, reg);
    a_comp.attackDamage = 100.0f;
    assert(a_comp.state == CombatState::ATTACK_WINDUP);
    
    // Set a very short timer so it fires
    a_comp.stateTimer = 0.05f;

    // Update 1: WINDUP -> FIRE
    combat.update(reg, 0.06f);
    assert(a_comp.state == CombatState::ATTACK_FIRE);

    // Update 2: process FIRE (apply damage) -> WINDDOWN
    combat.update(reg, 0.01f);
    
    // Check if damage was applied.
    float currentHP = reg.get<StatsComponent>(target).base.currentHP;
    assert(currentHP == 400.0f);
    assert(a_comp.state == CombatState::ATTACK_WINDDOWN);

    printf("  PASS: test_auto_attack_damage\n");
}

void test_death_event() {
    VFXEventQueue vfx;
    CombatSystem combat(vfx);
    EconomySystem econ;
    combat.setEconomySystem(&econ);
    entt::registry reg;

    auto attacker = reg.create();
    auto target = reg.create();

    auto& a_comp_death = reg.emplace<CombatComponent>(attacker, CombatComponent{.attackDamage = 100.0f});
    reg.emplace<TransformComponent>(attacker, TransformComponent{{0,0,0}});
    reg.emplace<TeamComponent>(attacker, TeamComponent{Team::PLAYER});

    auto& ts = reg.emplace<StatsComponent>(target);
    ts.base = Stats{};
    ts.base.maxHP = 100.0f;
    ts.base.currentHP = 50.0f;
    ts.bonus = {};
    reg.emplace<TransformComponent>(target, TransformComponent{{1,0,0}});
    reg.emplace<TeamComponent>(target, TeamComponent{Team::ENEMY});
    reg.emplace<CombatComponent>(target);

    combat.requestAutoAttack(attacker, target, reg);
    a_comp_death.stateTimer = 0.01f;

    // Update 1: WINDUP -> FIRE
    combat.update(reg, 0.02f);
    // Update 2: FIRE -> WINDDOWN (applies damage)
    combat.update(reg, 0.01f);

    assert(reg.get<StatsComponent>(target).base.currentHP <= 0.0f);
    // In a real scenario, EconomySystem would award gold/xp here.
    // We check if currentHP is 0.
    
    printf("  PASS: test_death_event\n");
}

int main() {
    printf("=== Combat System Tests ===\n");
    test_auto_attack_damage();
    test_death_event();
    return 0;
}
