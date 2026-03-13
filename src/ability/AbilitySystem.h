#pragma once

#include "ability/AbilityTypes.h"
#include "ability/AbilityComponents.h"
#include "vfx/VFXEventQueue.h"
#include "vfx/CompositeVFXSequencer.h"

#include <entt.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include <queue>

namespace glory {

class TrailRenderer;

// ── AbilitySystem ──────────────────────────────────────────────────────────
// Processes ability requests, drives the per-slot state machine, resolves
// targets, applies effects, and emits VFX events through the SPSC queue.
//
// Call order each frame (must match documented update order):
//   AbilitySystem::enqueueRequest(...)   — from InputSystem
//   AbilitySystem::update(registry, dt)  — processes state machines
//
class AbilitySystem {
public:
    explicit AbilitySystem(VFXEventQueue& vfxQueue);

    // ── Ability library ────────────────────────────────────────────────────

    // Load all *.json files in a directory as AbilityDefinitions.
    void loadDirectory(const std::string& dirPath);

    // Register a single definition (can also be built at runtime).
    void registerDefinition(AbilityDefinition def);

    // Return nullptr if not found.
    const AbilityDefinition* findDefinition(const std::string& id) const;

    // ── Entity setup ──────────────────────────────────────────────────────

    // Attach AbilityBookComponent to an entity and assign ability IDs per slot.
    void initEntity(entt::registry& registry, entt::entity entity,
                    const std::array<std::string, 4>& abilityIDs); // Q W E R

    // Learn / level-up a slot (1-5). Resets cooldown.
    void setAbilityLevel(entt::registry& registry, entt::entity entity,
                         AbilitySlot slot, int level);

    // ── Input interface ───────────────────────────────────────────────────

    void enqueueRequest(entt::entity caster, AbilitySlot slot, TargetInfo target);

    // ── Per-frame update ──────────────────────────────────────────────────

    void update(entt::registry& registry, float dt, TrailRenderer* trailRenderer = nullptr);

    // Public hit resolver called by ProjectileSystem when a projectile lands
    void resolveHit(entt::registry& reg, entt::entity caster,
                    const AbilityDefinition& def, const TargetInfo& target);

    // Public VFX emitter so ProjectileSystem can fire impact/destroy events
    uint32_t emitVFXPublic(const std::string& effectID,
                           const glm::vec3& position,
                           const glm::vec3& direction = {0,1,0},
                           float scale = 1.0f,
                           float lifetime = -1.0f,
                           uint32_t preHandle = 0);

    CompositeVFXSequencer& getSequencer() { return m_compositeSequencer; }

private:
    VFXEventQueue& m_vfxQueue;
    CompositeVFXSequencer m_compositeSequencer;

    std::unordered_map<std::string, AbilityDefinition> m_defs;
    std::queue<AbilityRequest> m_requests;

    // ── State machine ──────────────────────────────────────────────────────
    void processRequest(entt::registry& reg, const AbilityRequest& req);
    void tickAbility(entt::registry& reg, entt::entity entity,
                     AbilityInstance& inst, float dt, TrailRenderer* trailRenderer);

    // ── Validation ────────────────────────────────────────────────────────
    bool validateRequest(entt::registry& reg, const AbilityRequest& req,
                         AbilityInstance& inst) const;

    // ── Execution ─────────────────────────────────────────────────────────
    void executeAbility(entt::registry& reg, entt::entity caster,
                        AbilityInstance& inst, TrailRenderer* trailRenderer);
    void spawnProjectile(entt::registry& reg, entt::entity caster,
                         const AbilityDefinition& def,
                         const TargetInfo& target,
                         TrailRenderer* trailRenderer);
    void spawnLobProjectile(entt::registry& reg, entt::entity caster,
                            const AbilityDefinition& def,
                            const TargetInfo& target,
                            TrailRenderer* trailRenderer);
    void resolveInstantEffects(entt::registry& reg, entt::entity caster,
                               const AbilityDefinition& def,
                               const TargetInfo& target);
    void applyEffectToEntity(entt::registry& reg, entt::entity caster,
                             entt::entity target, const EffectDef& effect,
                             const AbilityDefinition& def, int level);

    // ── VFX helpers ───────────────────────────────────────────────────────
    uint32_t emitVFX(const std::string& effectID,
                     const glm::vec3& position,
                     const glm::vec3& direction = {0,1,0},
                     float scale = 1.0f,
                     float lifetime = -1.0f,
                     uint32_t preHandle = 0);

    // ── JSON loader ────────────────────────────────────────────────────────
    AbilityDefinition parseJSON(const nlohmann::json& j,
                                const std::string& fallbackID) const;
    EffectDef         parseEffect(const nlohmann::json& j) const;
    ScalingFormula    parseScaling(const nlohmann::json& j) const;
};

} // namespace glory
