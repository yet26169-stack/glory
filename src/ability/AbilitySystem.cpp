#include "ability/AbilitySystem.h"
#include "scene/Components.h"  // TransformComponent

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstring>

namespace glory {

// ── Constructor ──────────────────────────────────────────────────────────────
AbilitySystem::AbilitySystem(VFXEventQueue& vfxQueue)
    : m_vfxQueue(vfxQueue) {}

// ── loadDirectory ─────────────────────────────────────────────────────────────
void AbilitySystem::loadDirectory(const std::string& dirPath) {
    namespace fs = std::filesystem;
    if (!fs::exists(dirPath)) {
        spdlog::warn("AbilitySystem: directory '{}' not found", dirPath);
        return;
    }
    for (const auto& entry : fs::directory_iterator(dirPath)) {
        if (entry.path().extension() != ".json") continue;
        std::ifstream f(entry.path());
        if (!f.is_open()) continue;
        try {
            nlohmann::json j;
            f >> j;
            auto def = parseJSON(j, entry.path().stem().string());
            const std::string id = def.id;
            m_defs[id] = std::move(def);
            spdlog::debug("AbilitySystem: loaded '{}'", id);
        } catch (const std::exception& e) {
            spdlog::warn("AbilitySystem: failed to parse '{}': {}",
                         entry.path().string(), e.what());
        }
    }
}

// ── registerDefinition ────────────────────────────────────────────────────────
void AbilitySystem::registerDefinition(AbilityDefinition def) {
    const std::string id = def.id;
    m_defs[id] = std::move(def);
}

// ── findDefinition ────────────────────────────────────────────────────────────
const AbilityDefinition* AbilitySystem::findDefinition(const std::string& id) const {
    auto it = m_defs.find(id);
    return it != m_defs.end() ? &it->second : nullptr;
}

// ── initEntity ────────────────────────────────────────────────────────────────
void AbilitySystem::initEntity(entt::registry& registry, entt::entity entity,
                                const std::array<std::string, 4>& abilityIDs) {
    auto& book = registry.emplace_or_replace<AbilityBookComponent>(entity);

    const std::array<AbilitySlot, 4> slots = {
        AbilitySlot::Q, AbilitySlot::W, AbilitySlot::E, AbilitySlot::R
    };

    for (size_t i = 0; i < 4; ++i) {
        const auto* def = findDefinition(abilityIDs[i]);
        if (!def) {
            spdlog::warn("AbilitySystem: definition '{}' not found", abilityIDs[i]);
            continue;
        }
        auto& inst = book.abilities[static_cast<size_t>(slots[i])];
        inst.def   = def;
        inst.level = 1;  // starts at level 1
        inst.currentPhase = AbilityPhase::READY;
    }

    if (!registry.all_of<StatusEffectsComponent>(entity))
        registry.emplace<StatusEffectsComponent>(entity);
    if (!registry.all_of<ResourceComponent>(entity))
        registry.emplace<ResourceComponent>(entity);
    if (!registry.all_of<StatsComponent>(entity))
        registry.emplace<StatsComponent>(entity);
}

// ── setAbilityLevel ───────────────────────────────────────────────────────────
void AbilitySystem::setAbilityLevel(entt::registry& registry, entt::entity entity,
                                     AbilitySlot slot, int level) {
    if (!registry.all_of<AbilityBookComponent>(entity)) return;
    auto& inst  = registry.get<AbilityBookComponent>(entity)
                          .abilities[static_cast<size_t>(slot)];
    inst.level  = std::clamp(level, 0, 5);
}

// ── enqueueRequest ────────────────────────────────────────────────────────────
void AbilitySystem::enqueueRequest(entt::entity caster, AbilitySlot slot,
                                    TargetInfo target) {
    m_requests.push({caster, slot, target});
}

// ── update ────────────────────────────────────────────────────────────────────
void AbilitySystem::update(entt::registry& registry, float dt) {
    // 1. Process incoming requests
    while (!m_requests.empty()) {
        processRequest(registry, m_requests.front());
        m_requests.pop();
    }

    // 2. Tick all ability state machines
    auto view = registry.view<AbilityBookComponent>();
    for (auto entity : view) {
        auto& book = view.get<AbilityBookComponent>(entity);
        for (auto& inst : book.abilities) {
            if (inst.def) tickAbility(registry, entity, inst, dt);
        }
    }

    // 3. Tick status effects (DoTs, buffs expiry)
    auto statusView = registry.view<StatusEffectsComponent>();
    for (auto entity : statusView) {
        auto& statusComp = statusView.get<StatusEffectsComponent>(entity);
        auto  it         = statusComp.activeEffects.begin();
        while (it != statusComp.activeEffects.end()) {
            it->remainingDuration -= dt;

            // Tick DoTs/HoTs
            if (it->def && it->def->tickRate > 0.0f) {
                it->tickAccumulator += dt;
                while (it->tickAccumulator >= it->def->tickRate) {
                    it->tickAccumulator -= it->def->tickRate;
                    // TODO: call applyTickEffect when HealthComponent is available
                }
            }

            if (it->remainingDuration <= 0.0f) {
                it = statusComp.activeEffects.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// ── processRequest ────────────────────────────────────────────────────────────
void AbilitySystem::processRequest(entt::registry& reg, const AbilityRequest& req) {
    if (!reg.valid(req.casterEntity)) return;
    if (!reg.all_of<AbilityBookComponent>(req.casterEntity)) return;

    auto& inst = reg.get<AbilityBookComponent>(req.casterEntity)
                    .abilities[static_cast<size_t>(req.slot)];

    if (!validateRequest(reg, req, inst)) return;

    // Deduct resource cost
    if (reg.all_of<ResourceComponent>(req.casterEntity) && inst.def) {
        const int idx = std::max(0, std::min(inst.level - 1, 4));
        auto& res = reg.get<ResourceComponent>(req.casterEntity);
        const float cost = inst.def->costPerLevel[static_cast<size_t>(idx)];
        if (!res.spend(cost)) return; // double-check insufficient resource
    }

    // Transition READY → CASTING
    inst.currentPhase  = AbilityPhase::CASTING;
    inst.currentTarget = req.target;
    inst.phaseTimer    = inst.def->castTime;

    // Emit cast VFX
    if (!inst.def->castVFX.empty()) {
        glm::vec3 pos{0.f};
        if (reg.all_of<TransformComponent>(req.casterEntity))
            pos = reg.get<TransformComponent>(req.casterEntity).position;
        emitVFX(inst.def->castVFX, pos, req.target.direction);
    }
}

// ── validateRequest ───────────────────────────────────────────────────────────
bool AbilitySystem::validateRequest(entt::registry& reg, const AbilityRequest& req,
                                     AbilityInstance& inst) const {
    if (!inst.def || inst.level == 0)       return false;
    if (inst.currentPhase != AbilityPhase::READY) return false;
    if (inst.cooldownRemaining > 0.0f)      return false;

    // CC checks
    if (reg.all_of<StatusEffectsComponent>(req.casterEntity)) {
        const auto& s = reg.get<StatusEffectsComponent>(req.casterEntity);
        if (!s.canCast()) return false;
    }

    // Resource check
    if (reg.all_of<ResourceComponent>(req.casterEntity)) {
        const auto& res = reg.get<ResourceComponent>(req.casterEntity);
        const int idx   = std::max(0, std::min(inst.level - 1, 4));
        const float cost = inst.def->costPerLevel[static_cast<size_t>(idx)];
        if (res.current < cost) return false;
    }

    return true;
}

// ── tickAbility ───────────────────────────────────────────────────────────────
void AbilitySystem::tickAbility(entt::registry& reg, entt::entity entity,
                                  AbilityInstance& inst, float dt) {
    switch (inst.currentPhase) {
        case AbilityPhase::READY: break;

        case AbilityPhase::CASTING:
            inst.phaseTimer -= dt;
            if (inst.phaseTimer <= 0.0f) {
                if (inst.def->channelDuration > 0.0f) {
                    // → CHANNELING
                    inst.currentPhase = AbilityPhase::CHANNELING;
                    inst.phaseTimer   = inst.def->channelDuration;
                } else {
                    // → EXECUTING (instant)
                    inst.currentPhase = AbilityPhase::EXECUTING;
                    executeAbility(reg, entity, inst);
                }
            }
            break;

        case AbilityPhase::CHANNELING:
            inst.phaseTimer -= dt;
            if (inst.phaseTimer <= 0.0f) {
                inst.currentPhase = AbilityPhase::EXECUTING;
                executeAbility(reg, entity, inst);
            }
            break;

        case AbilityPhase::EXECUTING:
            // Effects have been dispatched; start cooldown
            {
                const int   idx = std::max(0, std::min(inst.level - 1, 4));
                const float cdr = reg.all_of<StatsComponent>(entity)
                                  ? reg.get<StatsComponent>(entity).total().cooldownReduction
                                  : 0.0f;
                const float cd  = inst.def->cooldownPerLevel[static_cast<size_t>(idx)]
                                  * (1.0f - cdr);
                inst.cooldownRemaining = cd;
                inst.currentPhase      = AbilityPhase::ON_COOLDOWN;
            }
            break;

        case AbilityPhase::ON_COOLDOWN:
            inst.cooldownRemaining -= dt;
            if (inst.cooldownRemaining <= 0.0f) {
                inst.cooldownRemaining = 0.0f;
                inst.currentPhase      = AbilityPhase::READY;
            }
            break;

        case AbilityPhase::INTERRUPTED:
            {
                const int   idx = std::max(0, std::min(inst.level - 1, 4));
                const float cdr = reg.all_of<StatsComponent>(entity)
                                  ? reg.get<StatsComponent>(entity).total().cooldownReduction
                                  : 0.0f;
                // Interrupted → 75% cooldown penalty
                const float cd  = inst.def->cooldownPerLevel[static_cast<size_t>(idx)]
                                  * 0.75f * (1.0f - cdr);
                inst.cooldownRemaining = cd;
                inst.currentPhase      = AbilityPhase::ON_COOLDOWN;
            }
            break;
    }
}

// ── executeAbility ────────────────────────────────────────────────────────────
void AbilitySystem::executeAbility(entt::registry& reg, entt::entity caster,
                                    AbilityInstance& inst) {
    const AbilityDefinition& def    = *inst.def;
    const TargetInfo&        target = inst.currentTarget;

    glm::vec3 casterPos{0.f};
    if (reg.all_of<TransformComponent>(caster))
        casterPos = reg.get<TransformComponent>(caster).position;

    if (def.targeting == TargetingType::SKILLSHOT && !def.projectileVFX.empty()) {
        spawnProjectile(reg, caster, def, target);
    } else if (def.targeting == TargetingType::POINT && def.projectile.isLob) {
        spawnLobProjectile(reg, caster, def, target);
    } else {
        resolveInstantEffects(reg, caster, def, target);
    }

    // Impact VFX: for SKILLSHOT the projectile handles it on actual hit.
    // For instant-hit abilities emit it now at the target position.
    if (!def.impactVFX.empty() && def.targeting != TargetingType::SKILLSHOT) {
        emitVFX(def.impactVFX, target.targetPosition, target.direction);
    }
}

// ── spawnProjectile ────────────────────────────────────────────────────────────
void AbilitySystem::spawnProjectile(entt::registry& reg, entt::entity caster,
                                     const AbilityDefinition& def,
                                     const TargetInfo& target) {
    glm::vec3 origin{0.f};
    if (reg.all_of<TransformComponent>(caster))
        origin = reg.get<TransformComponent>(caster).position;

    entt::entity proj = reg.create();
    reg.emplace<TransformComponent>(proj,
        TransformComponent{origin, glm::vec3(0.f), glm::vec3(1.f)});

    glm::vec3 dir = glm::length(target.direction) > 0.001f
                    ? glm::normalize(target.direction)
                    : glm::vec3(0, 0, 1);

    auto& pc      = reg.emplace<ProjectileComponent>(proj);
    pc.sourceDef   = &def;
    pc.casterEntity = static_cast<EntityID>(entt::to_integral(caster));
    pc.velocity    = dir * def.projectile.speed;
    pc.acceleration= def.projectile.acceleration;
    pc.maxSpeed    = def.projectile.maxSpeed;
    pc.maxRange    = def.projectile.maxRange;
    pc.piercing    = def.projectile.piercing;
    pc.maxTargets  = def.projectile.maxTargets;

    if (!def.projectileVFX.empty()) {
        // Use (entity index + 1) as a stable, pre-assigned VFX handle so we can
        // move/destroy the trail each frame from ProjectileSystem.
        const uint32_t vfxHandle = static_cast<uint32_t>(entt::to_integral(proj)) + 1u;
        pc.vfxHandle = vfxHandle;
        // Spawn VFX at character center height so trail doesn't clip into the ground
        const glm::vec3 vfxOrigin = origin + glm::vec3(0.f, 0.5f, 0.f);
        emitVFX(def.projectileVFX, vfxOrigin, dir, 1.0f, -1.0f, vfxHandle);
    }
}

// ── spawnLobProjectile ─────────────────────────────────────────────────────────
void AbilitySystem::spawnLobProjectile(entt::registry& reg, entt::entity caster,
                                        const AbilityDefinition& def,
                                        const TargetInfo& target) {
    glm::vec3 origin{0.f};
    if (reg.all_of<TransformComponent>(caster))
        origin = reg.get<TransformComponent>(caster).position;

    const glm::vec3 spawnPos = origin + glm::vec3(0.f, 1.0f, 0.f);
    const glm::vec3 landPos  = target.targetPosition;
    const glm::vec3 midPoint = (spawnPos + landPos) * 0.5f
                              + glm::vec3(0.f, def.projectile.lobApexHeight, 0.f);

    entt::entity proj = reg.create();
    reg.emplace<TransformComponent>(proj,
        TransformComponent{spawnPos, glm::vec3(0.f), glm::vec3(0.6f)});

    auto& pc          = reg.emplace<ProjectileComponent>(proj);
    pc.sourceDef      = &def;
    pc.casterEntity   = static_cast<EntityID>(entt::to_integral(caster));
    pc.maxRange       = def.projectile.maxRange;
    pc.piercing       = def.projectile.piercing;
    pc.maxTargets     = def.projectile.maxTargets;
    pc.isLob          = true;
    pc.lobOrigin      = spawnPos;
    pc.lobApex        = midPoint;
    pc.lobTarget      = landPos;
    pc.lobFlightTime  = def.projectile.lobFlightTime;
    pc.lobElapsed     = 0.0f;

    if (!def.projectileVFX.empty()) {
        const uint32_t vfxHandle = static_cast<uint32_t>(entt::to_integral(proj)) + 1u;
        pc.vfxHandle = vfxHandle;
        emitVFX(def.projectileVFX, spawnPos,
                glm::normalize(landPos - spawnPos + glm::vec3(0.f, 0.01f, 0.f)),
                1.0f, -1.0f, vfxHandle);
    }
}

// ── resolveInstantEffects ─────────────────────────────────────────────────────
void AbilitySystem::resolveInstantEffects(entt::registry& reg, entt::entity caster,
                                           const AbilityDefinition& def,
                                           const TargetInfo& target) {
    // Apply onSelfEffects to caster
    for (const auto& eff : def.onSelfEffects) {
        applyEffectToEntity(reg, caster, caster, eff, def,
                            reg.all_of<AbilityBookComponent>(caster)
                            ? reg.get<AbilityBookComponent>(caster)
                                   .abilities[static_cast<size_t>(def.slot)].level
                            : 1);
    }

    // For TARGETED: apply to target entity
    if (def.targeting == TargetingType::TARGETED &&
        target.targetEntity != NULL_ENTITY &&
        reg.valid(static_cast<entt::entity>(target.targetEntity))) {
        for (const auto& eff : def.onHitEffects) {
            applyEffectToEntity(reg, caster,
                                static_cast<entt::entity>(target.targetEntity),
                                eff, def, 1);
        }
    }
    // POINT / SELF AoE: collect entities in range (simplified: direct hit only)
    // A full spatial query would be done by the TargetingSystem.
}

// ── applyEffectToEntity ───────────────────────────────────────────────────────
void AbilitySystem::applyEffectToEntity(entt::registry& reg, entt::entity caster,
                                         entt::entity target,
                                         const EffectDef& effect,
                                         const AbilityDefinition& def,
                                         int level) {
    if (!reg.valid(target)) return;

    switch (effect.type) {
        case EffectType::DAMAGE: {
            Stats casterStats;
            if (reg.all_of<StatsComponent>(caster))
                casterStats = reg.get<StatsComponent>(caster).total();
            Stats targetStats;
            if (reg.all_of<StatsComponent>(target))
                targetStats = reg.get<StatsComponent>(target).total();

            const float raw  = effect.scaling.evaluate(level,
                                   casterStats.attackDamage, casterStats.abilityPower,
                                   casterStats.currentHP, casterStats.armor,
                                   casterStats.magicResist);
            const float dmg  = calculateDamage(raw, effect.damageType, targetStats,
                                               casterStats.flatArmorPen,
                                               casterStats.percentArmorPen);
            if (reg.all_of<StatsComponent>(target)) {
                auto& ts = reg.get<StatsComponent>(target);
                ts.base.currentHP = std::max(0.0f, ts.base.currentHP - dmg);
            }
            break;
        }
        case EffectType::STUN:
        case EffectType::ROOT:
        case EffectType::SILENCE:
        case EffectType::SLOW:
        case EffectType::DOT:
        case EffectType::HOT: {
            if (effect.duration > 0.0f) {
                auto& sc  = reg.get_or_emplace<StatusEffectsComponent>(target);
                ActiveStatusEffect ase;
                ase.def               = &effect;
                ase.sourceEntity      = static_cast<EntityID>(entt::to_integral(caster));
                ase.remainingDuration = effect.duration;
                ase.totalValue        = effect.value;
                if (!effect.applyVFX.empty()) {
                    glm::vec3 pos{0.f};
                    if (reg.all_of<TransformComponent>(target))
                        pos = reg.get<TransformComponent>(target).position;
                    emitVFX(effect.applyVFX, pos);
                }
                sc.activeEffects.push_back(ase);
            }
            break;
        }
        default:
            break;
    }
}

// ── emitVFX ───────────────────────────────────────────────────────────────────
uint32_t AbilitySystem::emitVFX(const std::string& effectID,
                                 const glm::vec3& position,
                                 const glm::vec3& direction,
                                 float scale,
                                 float lifetime,
                                 uint32_t preHandle) {
    VFXEvent ev{};
    ev.type     = VFXEventType::Spawn;
    ev.handle   = preHandle;   // 0 → VFXRenderer auto-assigns; non-zero → pinned handle
    ev.position = position;
    ev.direction= direction;
    ev.scale    = scale;
    ev.lifetime = lifetime;
    std::strncpy(ev.effectID, effectID.c_str(), sizeof(ev.effectID) - 1);

    if (!m_vfxQueue.push(ev)) {
        spdlog::warn("AbilitySystem: VFX queue full, dropping '{}'", effectID);
    }
    return preHandle;
}

// ── emitVFXPublic (public façade for external callers) ────────────────────────
uint32_t AbilitySystem::emitVFXPublic(const std::string& effectID,
                                       const glm::vec3& position,
                                       const glm::vec3& direction,
                                       float scale,
                                       float lifetime,
                                       uint32_t preHandle) {
    return emitVFX(effectID, position, direction, scale, lifetime, preHandle);
}

// ── resolveHit (public wrapper used by ProjectileSystem) ─────────────────────
void AbilitySystem::resolveHit(entt::registry& reg, entt::entity caster,
                                const AbilityDefinition& def,
                                const TargetInfo& target) {
    resolveInstantEffects(reg, caster, def, target);
}

// ── JSON parsing ──────────────────────────────────────────────────────────────
AbilityDefinition AbilitySystem::parseJSON(const nlohmann::json& j,
                                            const std::string& fallbackID) const {
    AbilityDefinition def;
    def.id          = j.value("id", fallbackID);
    def.displayName = j.value("displayName", def.id);

    // Slot
    const std::string slotStr = j.value("slot", "Q");
    if      (slotStr == "W") def.slot = AbilitySlot::W;
    else if (slotStr == "E") def.slot = AbilitySlot::E;
    else if (slotStr == "R") def.slot = AbilitySlot::R;
    else if (slotStr == "PASSIVE")  def.slot = AbilitySlot::PASSIVE;
    else if (slotStr == "SUMMONER" || slotStr == "D") def.slot = AbilitySlot::SUMMONER;
    else                             def.slot = AbilitySlot::Q;

    // Targeting
    const std::string targetStr = j.value("targeting", "SKILLSHOT");
    if      (targetStr == "POINT")    def.targeting = TargetingType::POINT;
    else if (targetStr == "TARGETED") def.targeting = TargetingType::TARGETED;
    else if (targetStr == "SELF")     def.targeting = TargetingType::SELF;
    else if (targetStr == "VECTOR")   def.targeting = TargetingType::VECTOR;
    else if (targetStr == "NONE")     def.targeting = TargetingType::NONE;
    else                              def.targeting = TargetingType::SKILLSHOT;

    // Resource
    const std::string resStr = j.value("resourceType", "MANA");
    if      (resStr == "ENERGY") def.resourceType = ResourceType::ENERGY;
    else if (resStr == "HEALTH") def.resourceType = ResourceType::HEALTH;
    else if (resStr == "NONE")   def.resourceType = ResourceType::NONE;
    else                          def.resourceType = ResourceType::MANA;

    // Arrays
    if (j.contains("costPerLevel")) {
        auto& arr = j["costPerLevel"];
        for (size_t i = 0; i < std::min(arr.size(), size_t(5)); ++i)
            def.costPerLevel[i] = arr[i].get<float>();
    }
    if (j.contains("cooldownPerLevel")) {
        auto& arr = j["cooldownPerLevel"];
        for (size_t i = 0; i < std::min(arr.size(), size_t(5)); ++i)
            def.cooldownPerLevel[i] = arr[i].get<float>();
    }

    def.castTime        = j.value("castTime",        0.0f);
    def.channelDuration = j.value("channelDuration", 0.0f);
    def.canMoveWhileCasting = j.value("canMoveWhileCasting", false);
    def.canBeInterrupted    = j.value("canBeInterrupted",    true);
    def.castRange       = j.value("castRange",       1100.0f);

    if (j.contains("projectile")) {
        const auto& p    = j["projectile"];
        def.projectile.speed          = p.value("speed",          1200.0f);
        def.projectile.acceleration   = p.value("acceleration",   0.0f);
        def.projectile.maxSpeed       = p.value("maxSpeed",       9999.0f);
        def.projectile.width          = p.value("width",          60.0f);
        def.projectile.maxRange       = p.value("maxRange",       1100.0f);
        def.projectile.piercing       = p.value("piercing",       false);
        def.projectile.maxTargets     = p.value("maxTargets",     1);
        def.projectile.returnsToSource= p.value("returnsToSource",false);
        def.projectile.destroyOnWall  = p.value("destroyOnWall",  true);
        def.projectile.isLob          = p.value("isLob",          false);
        def.projectile.lobFlightTime  = p.value("lobFlightTime",  1.0f);
        def.projectile.lobApexHeight  = p.value("lobApexHeight",  8.0f);
    }

    if (j.contains("onHitEffects"))
        for (const auto& e : j["onHitEffects"])
            def.onHitEffects.push_back(parseEffect(e));

    if (j.contains("onSelfEffects"))
        for (const auto& e : j["onSelfEffects"])
            def.onSelfEffects.push_back(parseEffect(e));

    def.castVFX       = j.value("castVFX",       "");
    def.projectileVFX = j.value("projectileVFX", "");
    def.impactVFX     = j.value("impactVFX",     "");
    def.castSFX       = j.value("castSFX",       "");
    def.impactSFX     = j.value("impactSFX",     "");
    def.castAnimation = j.value("castAnimation", "");

    if (j.contains("tags"))
        for (const auto& t : j["tags"])
            def.tags.insert(t.get<std::string>());

    return def;
}

EffectDef AbilitySystem::parseEffect(const nlohmann::json& j) const {
    EffectDef e;

    const std::string typeStr = j.value("type", "DAMAGE");
    if      (typeStr == "HEAL")        e.type = EffectType::HEAL;
    else if (typeStr == "STUN")        e.type = EffectType::STUN;
    else if (typeStr == "SLOW")        e.type = EffectType::SLOW;
    else if (typeStr == "ROOT")        e.type = EffectType::ROOT;
    else if (typeStr == "SILENCE")     e.type = EffectType::SILENCE;
    else if (typeStr == "KNOCKBACK")   e.type = EffectType::KNOCKBACK;
    else if (typeStr == "SUPPRESS")    e.type = EffectType::SUPPRESS;
    else if (typeStr == "DOT")         e.type = EffectType::DOT;
    else if (typeStr == "HOT")         e.type = EffectType::HOT;
    else if (typeStr == "BUFF_STAT")   e.type = EffectType::BUFF_STAT;
    else if (typeStr == "DEBUFF_STAT") e.type = EffectType::DEBUFF_STAT;
    else                               e.type = EffectType::DAMAGE;

    const std::string dmgStr = j.value("damageType", "MAGICAL");
    if      (dmgStr == "PHYSICAL") e.damageType = DamageType::PHYSICAL;
    else if (dmgStr == "TRUE")     e.damageType = DamageType::TRUE_DMG;
    else                           e.damageType = DamageType::MAGICAL;

    if (j.contains("scaling")) e.scaling = parseScaling(j["scaling"]);

    e.duration  = j.value("duration",  0.0f);
    e.tickRate  = j.value("tickRate",  0.5f);
    e.value     = j.value("value",     0.0f);
    e.applyVFX  = j.value("applyVFX",  "");
    e.appliesGrievousWounds = j.value("appliesGrievousWounds", false);

    return e;
}

ScalingFormula AbilitySystem::parseScaling(const nlohmann::json& j) const {
    ScalingFormula s;
    if (j.contains("basePerLevel")) {
        auto& arr = j["basePerLevel"];
        for (size_t i = 0; i < std::min(arr.size(), size_t(5)); ++i)
            s.basePerLevel[i] = arr[i].get<float>();
    }
    s.adRatio = j.value("adRatio", 0.0f);
    s.apRatio = j.value("apRatio", 0.0f);
    s.hpRatio = j.value("hpRatio", 0.0f);
    return s;
}

} // namespace glory
