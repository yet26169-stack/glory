#include "ability/AbilityComponents.h"
#include "ability/AbilitySystem.h"
#include "ability/ProjectileSystem.h"
#include "combat/CombatComponents.h"
#include "combat/CombatSystem.h"
#include "combat/EconomySystem.h"
#include "combat/GpuCollisionSystem.h"
#include "combat/MinionWaveSystem.h"
#include "combat/NPCBehaviorSystem.h"
#include "combat/RespawnSystem.h"
#include "combat/StructureSystem.h"
#include "core/GameSystems.h"
#include "core/GameplaySystem.h"
#include "core/SystemScheduler.h"
#include "nav/DebugRenderer.h"
#include "physics/PhysicsSystem.h"
#include "renderer/ConeAbilityRenderer.h"
#include "renderer/DistortionRenderer.h"
#include "renderer/ExplosionRenderer.h"
#include "renderer/GroundDecalRenderer.h"
#include "renderer/DeferredDecalRenderer.h"
#include "renderer/SpriteEffectRenderer.h"
#include "scene/Components.h"
#include "scene/Scene.h"
#include "vfx/MeshEffectRenderer.h"
#include "vfx/TrailRenderer.h"
#include "vfx/VFXEventQueue.h"
#include "vfx/VFXRenderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <glm/glm.hpp>
#include <latch>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <unordered_map>

namespace glory {

// ═══ GameplaySystem.cpp ═══

// ── Initialisation ──────────────────────────────────────────────────────────

void GameplaySystem::init(Scene& scene, AbilitySystem* abilities, CombatSystem* combat,
                          GpuCollisionSystem* gpuCollision, DebugRenderer* debugRenderer) {
    m_scene         = &scene;
    m_abilitySystem = abilities;
    m_combatSystem  = combat;
    m_gpuCollision  = gpuCollision;
    m_debugRenderer = debugRenderer;
}

// ── Per-tick entry point ────────────────────────────────────────────────────

void GameplaySystem::update(float dt, const GameplayInput& input, GameplayOutput& output) {
    // Block gameplay input when the player is dead
    bool playerDead = false;
    if (m_playerEntity != entt::null && m_scene) {
        auto* rc = m_scene->getRegistry().try_get<RespawnComponent>(m_playerEntity);
        playerDead = rc && rc->state != LifeState::ALIVE;
    }

    if (!playerDead) {
        processRightClick(dt, input, output);
        processTargetingMode(input);
        processAbilityKeys(input);
        processCombatKeys(input);
        processSpawning(dt, input);
        processSelection(input, output);
    }

    // These still run (minions keep moving, animations keep playing)
    updateMinionMovement(dt);
    updatePlayerMovement(dt, output);
    updateAnimations(dt);
}

// ── Screen / world coordinate conversion ────────────────────────────────────

glm::vec3 GameplaySystem::screenToWorld(float mx, float my, const GameplayInput& input) const {
    glm::vec4 rayClip{
        (mx / input.screenW) * 2.0f - 1.0f,
        (my / input.screenH) * 2.0f - 1.0f,
        -1.0f, 1.0f
    };
    glm::vec4 rayEye = glm::inverse(input.proj) * rayClip;
    rayEye = glm::vec4(rayEye.x, rayEye.y, -1.0f, 0.0f);
    glm::vec3 rayWorld = glm::normalize(glm::vec3(glm::inverse(input.view) * rayEye));

    glm::vec3 origin = glm::vec3(glm::inverse(input.view)[3]);
    if (std::abs(rayWorld.y) < 1e-5f) return origin;
    float t = -origin.y / rayWorld.y;
    return origin + t * rayWorld;
}

glm::vec2 GameplaySystem::worldToScreen(const glm::vec3& worldPos, const GameplayInput& input) const {
    glm::mat4 vp = input.proj * input.view;
    glm::vec4 clipPos = vp * glm::vec4(worldPos, 1.0f);
    if (clipPos.w < 0.1f) return glm::vec2(-1000.0f);
    glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
    return glm::vec2(
        (ndc.x * 0.5f + 0.5f) * input.screenW,
        (ndc.y * 0.5f + 0.5f) * input.screenH
    );
}

// ── Right-click: move or target ─────────────────────────────────────────────

void GameplaySystem::processRightClick(float /*dt*/, const GameplayInput& input, GameplayOutput& output) {
    if (!input.rightClicked || input.minimapHovered || m_playerEntity == entt::null)
        return;

    glm::vec3 worldPos = screenToWorld(input.lastClickPos.x, input.lastClickPos.y, input);

    auto& reg    = m_scene->getRegistry();
    if (!reg.all_of<CharacterComponent, CombatComponent>(m_playerEntity)) return;
    auto& c      = reg.get<CharacterComponent>(m_playerEntity);
    auto& combat = reg.get<CombatComponent>(m_playerEntity);

    entt::entity target = input.hoveredEntity;
    if (target != entt::null && m_combatSystem) {
        // Target enemy for auto-attack (will chase if out of range)
        combat.targetEntity = target;

        // Animation canceling: interrupt windup if a new target is clicked
        if (combat.state == CombatState::ATTACK_WINDUP || combat.state == CombatState::ATTACK_WINDDOWN) {
            combat.state = CombatState::IDLE;
        }
    } else {
        // No enemy under cursor — move to position and clear target
        combat.targetEntity = entt::null;
        c.targetPosition = worldPos;
        c.hasTarget = true;
        output.clickAnim = ClickAnim{ worldPos, 0.0f, 0.25f };

        // Animation canceling: interrupt windup if move command issued
        if (combat.state == CombatState::ATTACK_WINDUP || combat.state == CombatState::ATTACK_WINDDOWN) {
            combat.state = CombatState::IDLE;
        }
    }
}

// ── Targeting mode: visual indicators while aiming ──────────────────────────

void GameplaySystem::processTargetingMode(const GameplayInput& input) {
    if (!m_inTargetingMode) return;
    if (!m_abilitySystem || m_playerEntity == entt::null) {
        m_inTargetingMode = false;
        return;
    }

    auto& reg = m_scene->getRegistry();
    if (!reg.all_of<AbilityBookComponent>(m_playerEntity)) {
        m_inTargetingMode = false;
        return;
    }

    const auto& book = reg.get<AbilityBookComponent>(m_playerEntity);
    const auto& inst = book.abilities[static_cast<size_t>(m_targetingSlot)];
    if (!inst.def || inst.level == 0) {
        m_inTargetingMode = false;
        return;
    }

    // Cancel on Esc or right-click
    if (input.escPressed || input.rightClicked) {
        if (m_groundDecals && m_targetingDecalHandle)
            m_groundDecals->destroy(m_targetingDecalHandle);
        if (m_groundDecals && m_rangeDecalHandle)
            m_groundDecals->destroy(m_rangeDecalHandle);
        m_targetingDecalHandle = 0;
        m_rangeDecalHandle = 0;
        m_inTargetingMode = false;
        return;
    }

    glm::vec3 worldPos = screenToWorld(input.mousePos.x, input.mousePos.y, input);
    glm::vec3 playerPos{0.f};
    if (reg.all_of<TransformComponent>(m_playerEntity))
        playerPos = reg.get<TransformComponent>(m_playerEntity).position;

    // Update indicator position each frame
    if (m_groundDecals) {
        const auto& def = *inst.def;

        // Range ring centered on player
        if (m_rangeDecalHandle != 0)
            m_groundDecals->destroy(m_rangeDecalHandle);
        m_rangeDecalHandle = m_groundDecals->spawn("ability_range",
            playerPos, def.castRange, 0.0f);

        // Targeting indicator — recreate each tick for position updates
        if (m_targetingDecalHandle != 0)
            m_groundDecals->destroy(m_targetingDecalHandle);

        if (def.targeting == TargetingType::SKILLSHOT) {
            glm::vec3 dir = worldPos - playerPos;
            dir.y = 0.0f;
            float len = glm::length(dir);
            if (len > 0.001f) dir /= len;
            float halfRange = std::min(len, def.castRange) * 0.5f;
            glm::vec3 center = playerPos + dir * halfRange;
            float rot = std::atan2(dir.x, dir.z);
            m_targetingDecalHandle = m_groundDecals->spawn("ability_skillshot",
                center, halfRange, rot);
        } else if (def.targeting == TargetingType::POINT) {
            float radius = def.areaRadius > 0.0f ? def.areaRadius : 3.0f;
            m_targetingDecalHandle = m_groundDecals->spawn("ability_aoe",
                worldPos, radius, 0.0f);
        } else {
            m_targetingDecalHandle = 0;
        }
    }

    // Confirm on left-click
    if (input.leftClicked) {
        if (m_groundDecals && m_targetingDecalHandle)
            m_groundDecals->destroy(m_targetingDecalHandle);
        if (m_groundDecals && m_rangeDecalHandle)
            m_groundDecals->destroy(m_rangeDecalHandle);
        m_targetingDecalHandle = 0;
        m_rangeDecalHandle = 0;
        m_inTargetingMode = false;

        TargetInfo target;
        target.targetPosition = worldPos;
        glm::vec3 toMouse = worldPos - playerPos;
        toMouse.y = 0.0f;
        target.direction = glm::length(toMouse) > 0.001f
                           ? glm::normalize(toMouse) : glm::vec3(0, 0, 1);
        target.type = inst.def->targeting == TargetingType::SKILLSHOT
                      ? TargetingType::SKILLSHOT
                      : TargetingType::POINT;
        if (inst.def->targeting == TargetingType::TARGETED && input.hoveredEntity != entt::null)
            target.targetEntity = static_cast<EntityID>(
                entt::to_integral(input.hoveredEntity));

        m_abilitySystem->enqueueRequest(m_playerEntity, m_targetingSlot, target);
    }
}

// ── Ability keys: Q / W / E / R / D ─────────────────────────────────────────

void GameplaySystem::processAbilityKeys(const GameplayInput& input) {
    if (!m_abilitySystem || m_playerEntity == entt::null)
        return;

    auto& reg = m_scene->getRegistry();
    if (!reg.all_of<AbilityBookComponent>(m_playerEntity))
        return;

    auto& book = reg.get<AbilityBookComponent>(m_playerEntity);

    // Ctrl+Q/W/E/R → level up ability
    if (input.ctrlHeld) {
        auto tryLevelUp = [&](bool pressed, AbilitySlot slot) {
            if (!pressed) return;
            auto& inst = book.abilities[static_cast<size_t>(slot)];
            if (!inst.def) return;

            // Max 5 for basic, 3 for R
            int maxLevel = (slot == AbilitySlot::R) ? 3 : 5;
            if (inst.level >= maxLevel) return;

            // R requires hero levels 6/11/16
            if (slot == AbilitySlot::R) {
                auto* eco = reg.try_get<EconomyComponent>(m_playerEntity);
                int heroLevel = eco ? eco->level : 1;
                int reqLevel = (inst.level == 0) ? 6 : (inst.level == 1) ? 11 : 16;
                if (heroLevel < reqLevel) return;
            }

            // Check skill points: 1 point per hero level, spent = sum of ability levels
            auto* eco = reg.try_get<EconomyComponent>(m_playerEntity);
            int heroLevel = eco ? eco->level : 1;
            int spentPoints = 0;
            for (const auto& a : book.abilities) spentPoints += a.level;
            if (spentPoints >= heroLevel) return;

            m_abilitySystem->setAbilityLevel(reg, m_playerEntity, slot, inst.level + 1);
            spdlog::info("Ability {} leveled up to {}", static_cast<int>(slot), inst.level + 1);
        };
        tryLevelUp(input.qPressed, AbilitySlot::Q);
        tryLevelUp(input.wPressed, AbilitySlot::W);
        tryLevelUp(input.ePressed, AbilitySlot::E);
        tryLevelUp(input.rPressed, AbilitySlot::R);
        return; // don't also cast
    }

    // If already in targeting mode, pressing same key cancels, different key switches
    auto enterTargeting = [&](bool pressed, AbilitySlot slot) {
        if (!pressed) return;
        const auto& inst = book.abilities[static_cast<size_t>(slot)];
        if (!inst.def || inst.level == 0) return;
        if (!inst.isReady()) return;

        // SELF abilities fire immediately (no targeting needed)
        if (inst.def->targeting == TargetingType::SELF ||
            inst.def->targeting == TargetingType::NONE) {
            glm::vec3 worldPos = screenToWorld(input.mousePos.x, input.mousePos.y, input);
            TargetInfo target;
            target.type = inst.def->targeting;
            target.targetPosition = worldPos;
            if (reg.all_of<TransformComponent>(m_playerEntity)) {
                const auto& pt = reg.get<TransformComponent>(m_playerEntity);
                glm::vec3 toMouse = worldPos - pt.position;
                toMouse.y = 0.0f;
                target.direction = glm::length(toMouse) > 0.001f
                                   ? glm::normalize(toMouse) : glm::vec3(0, 0, 1);
            }
            m_abilitySystem->enqueueRequest(m_playerEntity, slot, target);
            return;
        }

        // Toggle same slot off
        if (m_inTargetingMode && m_targetingSlot == slot) {
            if (m_groundDecals && m_targetingDecalHandle)
                m_groundDecals->destroy(m_targetingDecalHandle);
            if (m_groundDecals && m_rangeDecalHandle)
                m_groundDecals->destroy(m_rangeDecalHandle);
            m_targetingDecalHandle = 0;
            m_rangeDecalHandle = 0;
            m_inTargetingMode = false;
            return;
        }

        // Clean up previous targeting decals if switching slot
        if (m_inTargetingMode && m_groundDecals) {
            if (m_targetingDecalHandle) m_groundDecals->destroy(m_targetingDecalHandle);
            if (m_rangeDecalHandle) m_groundDecals->destroy(m_rangeDecalHandle);
            m_targetingDecalHandle = 0;
            m_rangeDecalHandle = 0;
        }

        m_inTargetingMode = true;
        m_targetingSlot = slot;
    };

    enterTargeting(input.qPressed, AbilitySlot::Q);
    enterTargeting(input.wPressed, AbilitySlot::W);
    enterTargeting(input.ePressed, AbilitySlot::E);
    enterTargeting(input.rPressed, AbilitySlot::R);

    // D — summoner fires immediately
    if (input.dPressed) {
        glm::vec3 worldPos = screenToWorld(input.mousePos.x, input.mousePos.y, input);
        TargetInfo groundTarget;
        groundTarget.type           = TargetingType::POINT;
        groundTarget.targetPosition = worldPos;
        if (reg.all_of<TransformComponent>(m_playerEntity)) {
            const auto& pt = reg.get<TransformComponent>(m_playerEntity);
            glm::vec3 toMouse = worldPos - pt.position;
            toMouse.y = 0.0f;
            groundTarget.direction = glm::length(toMouse) > 0.001f
                                     ? glm::normalize(toMouse) : glm::vec3(0, 0, 1);
        }
        m_abilitySystem->enqueueRequest(m_playerEntity, AbilitySlot::SUMMONER, groundTarget);
    }
}

// ── Combat keys: A (auto-attack), S (shield) ────────────────────────────────

void GameplaySystem::processCombatKeys(const GameplayInput& input) {
    if (!m_combatSystem || m_playerEntity == entt::null)
        return;

    auto& reg = m_scene->getRegistry();
    if (!reg.all_of<CombatComponent>(m_playerEntity))
        return;

    auto& combat = reg.get<CombatComponent>(m_playerEntity);

    // A — auto-attack nearest enemy
    if (input.aPressed && combat.state == CombatState::IDLE
        && combat.attackCooldown <= 0.0f) {
        entt::entity target = m_gpuCollision->findNearestEnemy(
            reg, m_playerEntity, combat.attackRange);
        if (target != entt::null) {
            combat.targetEntity = target; // Start chasing/attacking
        }
    }

    // S — shield (duration 3.5s)
    if (input.sPressed && combat.state == CombatState::IDLE
        && combat.shieldCooldown <= 0.0f) {
        m_combatSystem->requestShield(m_playerEntity, reg);
    }
}

// ── Spawning (X key) ────────────────────────────────────────────────────────

void GameplaySystem::processSpawning(float dt, const GameplayInput& input) {
    m_spawnTimer -= dt;
    if (!input.xKeyDown || m_spawnTimer > 0.0f)
        return;

    glm::vec3 worldPos = screenToWorld(input.mousePos.x, input.mousePos.y, input);

    auto minion = m_scene->createEntity("MeleeMinion");
    auto& reg = m_scene->getRegistry();
    auto& t = reg.get<TransformComponent>(minion);
    t.position = worldPos;
    t.scale    = glm::vec3(0.05f); // Match player scale
    t.rotation = glm::vec3(0.0f);

    reg.emplace<SelectableComponent>(minion, SelectableComponent{ false, 1.0f });
    reg.emplace<UnitComponent>(minion, UnitComponent{ UnitComponent::State::IDLE, worldPos, 5.0f });
    reg.emplace<CharacterComponent>(minion, CharacterComponent{ worldPos, 5.0f });
    reg.emplace<GPUSkinnedMeshComponent>(minion, GPUSkinnedMeshComponent{ m_spawnConfig.meshIndex });

    // Combat components for minions
    reg.emplace<TeamComponent>(minion, TeamComponent{ Team::ENEMY });
    auto& minionStats = reg.emplace<StatsComponent>(minion);
    minionStats.base.maxHP = 300.0f;
    minionStats.base.currentHP = 300.0f;
    minionStats.base.armor = 10.0f;
    minionStats.base.attackDamage = 12.0f;
    auto& minionCombat = reg.emplace<CombatComponent>(minion);
    minionCombat.attackRange = 2.0f;
    minionCombat.attackSpeed = 0.8f;
    minionCombat.attackDamage = 12.0f;
    reg.emplace<MinionComponent>(minion, MinionComponent{ MinionType::MELEE, 20, 60 });
    reg.emplace<EconomyComponent>(minion);

    // Setup simple Material
    reg.emplace<MaterialComponent>(minion,
        MaterialComponent{ m_spawnConfig.texIndex, m_spawnConfig.flatNormIndex, 0.0f, 0.0f, 0.5f, 0.2f });
    // Setup Animation (Melee minion)
    SkeletonComponent skelComp;
    skelComp.skeleton = m_spawnConfig.skeleton; // vertex arrays not needed for GPU skinning

    AnimationComponent animComp;
    animComp.player.setSkeleton(&skelComp.skeleton);
    animComp.clips = m_spawnConfig.clips;
    if (!animComp.clips.empty()) {
        animComp.activeClipIndex = 0;
        animComp.player.setClip(&animComp.clips[0]);
    }

    reg.emplace<SkeletonComponent>(minion, std::move(skelComp));
    reg.emplace<AnimationComponent>(minion, std::move(animComp));

    // Re-point raw pointers to registry-owned copies
    auto& regSkel = reg.get<SkeletonComponent>(minion);
    auto& regAnim = reg.get<AnimationComponent>(minion);
    regAnim.player.setSkeleton(&regSkel.skeleton);
    if (!regAnim.clips.empty())
        regAnim.player.setClip(&regAnim.clips[regAnim.activeClipIndex]);

    m_spawnTimer = 0.5f; // Debounce
    spdlog::info("Spawned minion at ({}, {}, {})", worldPos.x, worldPos.y, worldPos.z);
}

// ── Selection (marquee + click-to-select/move) ──────────────────────────────

void GameplaySystem::processSelection(const GameplayInput& input, GameplayOutput& /*output*/) {
    auto& reg = m_scene->getRegistry();

    if (input.leftMouseDown) {
        if (!m_selection.isDragging) {
            m_selection.isDragging = true;
            m_selection.dragStart = input.mousePos;
        }
        m_selection.dragEnd = input.mousePos;

        // Draw Marquee Box using DebugRenderer on the ground plane
        glm::vec3 tl = screenToWorld(m_selection.dragStart.x, m_selection.dragStart.y, input);
        glm::vec3 tr = screenToWorld(m_selection.dragEnd.x, m_selection.dragStart.y, input);
        glm::vec3 br = screenToWorld(m_selection.dragEnd.x, m_selection.dragEnd.y, input);
        glm::vec3 bl = screenToWorld(m_selection.dragStart.x, m_selection.dragEnd.y, input);

        // Offset slightly above ground to prevent z-fighting
        tl.y = tr.y = br.y = bl.y = 0.05f;

        glm::vec4 color(0.2f, 1.0f, 0.4f, 1.0f);
        m_debugRenderer->drawLine(tl, tr, color);
        m_debugRenderer->drawLine(tr, br, color);
        m_debugRenderer->drawLine(br, bl, color);
        m_debugRenderer->drawLine(bl, tl, color);

    } else {
        if (m_selection.isDragging) {
            m_selection.isDragging = false;
            glm::vec2 start = m_selection.dragStart;
            glm::vec2 end   = m_selection.dragEnd;

            float dist = glm::distance(start, end);
            bool isClick = dist < 5.0f;

            auto selView = reg.view<TransformComponent, SelectableComponent>();
            glm::vec2 bmin = glm::min(start, end);
            glm::vec2 bmax = glm::max(start, end);

            if (isClick) {
                // Determine if we clicked on a unit
                bool clickedOnUnit = false;
                for (auto e : selView) {
                    auto& tf = selView.get<TransformComponent>(e);
                    if (glm::distance(worldToScreen(tf.position, input), end) < 20.0f) {
                        clickedOnUnit = true;
                        break;
                    }
                }

                if (clickedOnUnit) {
                    // Select clicked unit(s)
                    for (auto e : selView) {
                        auto& tf = selView.get<TransformComponent>(e);
                        auto& s  = selView.get<SelectableComponent>(e);
                        if (glm::distance(worldToScreen(tf.position, input), end) < 20.0f) {
                            s.isSelected = true;
                        } else {
                            if (!input.shiftHeld) s.isSelected = false;
                        }
                    }
                } else {
                    // Clicked on ground -> move selected units
                    glm::vec3 targetWorld = screenToWorld(end.x, end.y, input);
                    auto unitView = reg.view<SelectableComponent, CharacterComponent, UnitComponent>();

                    int numSelected = 0;
                    for (auto e : unitView) {
                        if (unitView.get<SelectableComponent>(e).isSelected) numSelected++;
                    }

                    int unitIndex = 0;
                    for (auto e : unitView) {
                        auto& s = unitView.get<SelectableComponent>(e);
                        if (s.isSelected) {
                            auto& c = unitView.get<CharacterComponent>(e);
                            auto& u = unitView.get<UnitComponent>(e);

                            // Calculate simple circular formation offset
                            glm::vec3 offset(0.0f);
                            if (numSelected > 1) {
                                float radius = 1.0f + (numSelected * 0.2f);
                                float angle = (6.2831853f / numSelected) * unitIndex;
                                offset = glm::vec3(std::cos(angle) * radius, 0.0f, std::sin(angle) * radius);
                            }

                            c.targetPosition = targetWorld + offset;
                            c.hasTarget = true;
                            u.state = UnitComponent::State::MOVING;
                            u.targetPosition = targetWorld + offset;
                            unitIndex++;
                        }
                    }
                }
            } else {
                // Marquee selection
                for (auto e : selView) {
                    auto& tf = selView.get<TransformComponent>(e);
                    auto& s  = selView.get<SelectableComponent>(e);
                    glm::vec2 screenPos = worldToScreen(tf.position, input);
                    if (screenPos.x >= bmin.x && screenPos.x <= bmax.x &&
                        screenPos.y >= bmin.y && screenPos.y <= bmax.y) {
                        s.isSelected = true;
                    } else {
                        if (!input.shiftHeld) s.isSelected = false;
                    }
                }
            }
        }
    }
}

// ── Minion movement ─────────────────────────────────────────────────────────

void GameplaySystem::updateMinionMovement(float dt) {
    auto& reg = m_scene->getRegistry();
    auto minionView = reg.view<UnitComponent, CharacterComponent, TransformComponent>();
    for (auto e : minionView) {
        if (e == m_playerEntity) continue;

        // Stunned entities cannot move
        if (reg.all_of<CombatComponent>(e)) {
            auto& combat = reg.get<CombatComponent>(e);
            if (combat.state == CombatState::STUNNED) {
                auto& c2 = minionView.get<CharacterComponent>(e);
                c2.hasTarget = false;
                continue;
            }
        }

        auto& c = minionView.get<CharacterComponent>(e);
        auto& t = minionView.get<TransformComponent>(e);
        auto& u = minionView.get<UnitComponent>(e);

        const float accelRate = 30.0f;

        if (c.hasTarget) {
            glm::vec3 dir = c.targetPosition - t.position;
            dir.y = 0.0f;
            float dist = glm::length(dir);
            if (dist < 0.05f) {
                t.position.x = c.targetPosition.x;
                t.position.z = c.targetPosition.z;
                c.hasTarget = false;
                u.state = UnitComponent::State::IDLE;
            } else {
                dir /= dist;
                // League-style: snap rotation instantly to target direction
                c.currentYaw = std::atan2(dir.x, dir.z);
                t.rotation.y = c.currentYaw;

                // Fast acceleration toward full speed
                c.currentSpeed += (c.moveSpeed - c.currentSpeed) * std::min(accelRate * dt, 1.0f);

                float step = c.currentSpeed * dt;
                if (step >= dist) {
                    t.position.x = c.targetPosition.x;
                    t.position.z = c.targetPosition.z;
                    c.hasTarget = false;
                    u.state = UnitComponent::State::IDLE;
                } else {
                    t.position += dir * step;
                    u.state = UnitComponent::State::MOVING;
                }
            }
        } else {
            c.currentSpeed *= std::max(1.0f - 30.0f * dt, 0.0f);
        }
    }
}

// ── Player character movement + chase / attack ──────────────────────────────

void GameplaySystem::updatePlayerMovement(float dt, GameplayOutput& output) {
    auto& reg = m_scene->getRegistry();
    if (m_playerEntity == entt::null ||
        !reg.valid(m_playerEntity) ||
        !reg.all_of<CharacterComponent, TransformComponent, CombatComponent>(m_playerEntity))
        return;

    auto& c      = reg.get<CharacterComponent>(m_playerEntity);
    auto& t      = reg.get<TransformComponent>(m_playerEntity);
    auto& combat = reg.get<CombatComponent>(m_playerEntity);

    // Chasing / Attack Logic
    if (combat.targetEntity != entt::null && reg.valid(combat.targetEntity)) {
        if (reg.all_of<TransformComponent>(combat.targetEntity)) {
            auto& targetT = reg.get<TransformComponent>(combat.targetEntity);
            float dist = glm::distance(t.position, targetT.position);

            if (dist > combat.attackRange) {
                // OUT OF RANGE: Chase the target
                if (combat.state == CombatState::IDLE || combat.state == CombatState::ATTACK_WINDDOWN) {
                    c.targetPosition = targetT.position;
                    c.hasTarget = true;
                }
            } else {
                // IN RANGE: Start attack cycle if possible
                if (combat.state == CombatState::IDLE && combat.attackCooldown <= 0.0f) {
                    c.hasTarget = false; // Stop moving to attack
                    m_combatSystem->requestAutoAttack(m_playerEntity, combat.targetEntity, reg);
                } else if (combat.state == CombatState::ATTACK_WINDUP || combat.state == CombatState::ATTACK_WINDDOWN) {
                    c.hasTarget = false; // Stay still during attack
                }
            }
        }
    }

    // Stunned player cannot move
    bool playerStunned = (combat.state == CombatState::STUNNED);
    if (playerStunned) {
        c.hasTarget = false;
    }

    if (!playerStunned) {
    const float accelRate = 30.0f;
    if (c.hasTarget) {
        glm::vec3 dir = c.targetPosition - t.position;
        dir.y = 0.0f;
        float dist = glm::length(dir);
        if (dist < 0.05f) {
            t.position.x = c.targetPosition.x;
            t.position.z = c.targetPosition.z;
            c.hasTarget = false;
        } else {
            dir /= dist;
            // League-style: snap rotation instantly to target direction
            c.currentYaw = std::atan2(dir.x, dir.z);
            t.rotation.y = c.currentYaw;

            // Fast acceleration toward full speed
            c.currentSpeed += (c.moveSpeed - c.currentSpeed) * std::min(accelRate * dt, 1.0f);

            // Clamp step to remaining distance to prevent overshoot
            float step = c.currentSpeed * dt;
            if (step >= dist) {
                t.position.x = c.targetPosition.x;
                t.position.z = c.targetPosition.z;
                c.hasTarget = false;
            } else {
                t.position += dir * step;
            }
        }
    } else {
        // Decelerate to stop
        c.currentSpeed *= std::max(1.0f - 30.0f * dt, 0.0f);
    }
    } // end if (!playerStunned)
    output.cameraFollowTarget = t.position;
}

// ── Animation clip selection & speed scaling ────────────────────────────────

void GameplaySystem::updateAnimations(float dt) {
    auto& reg = m_scene->getRegistry();
    auto animView = reg
        .view<SkeletonComponent, AnimationComponent, GPUSkinnedMeshComponent, TransformComponent>();
    for (auto&& [e, skel, anim, ssm, t] : animView.each()) {
        // Switch clip based on movement/combat state
        // (0=idle, 1=walk, 2=attack)
        if (reg.all_of<CharacterComponent>(e)) {
            auto& c = reg.get<CharacterComponent>(e);
            int targetClip = 0; // default idle

            if (reg.all_of<CombatComponent>(e)) {
                auto& combat = reg.get<CombatComponent>(e);
                if (combat.state == CombatState::ATTACK_WINDUP ||
                    combat.state == CombatState::ATTACK_FIRE ||
                    combat.state == CombatState::ATTACK_WINDDOWN) {
                    targetClip = 2; // attack

                    // Face the target while attacking
                    if (reg.valid(combat.targetEntity) &&
                        reg.all_of<TransformComponent>(combat.targetEntity)) {
                        auto& targetTrans = reg.get<TransformComponent>(combat.targetEntity);
                        glm::vec3 dir = targetTrans.position - t.position;
                        dir.y = 0.0f;
                        if (glm::length(dir) > 0.001f) {
                            c.currentYaw = std::atan2(dir.x, dir.z);
                            t.rotation.y = c.currentYaw;
                        }
                    }
                }
                else if (combat.state == CombatState::SHIELDING || combat.state == CombatState::STUNNED) {
                    targetClip = 0; // idle/stunned
                }
                else if (c.hasTarget) {
                    targetClip = 1; // walk
                }
            } else if (c.hasTarget) {
                targetClip = 1; // walk fallback
            }

            if (anim.activeClipIndex != targetClip && targetClip < (int)anim.clips.size()) {
                anim.activeClipIndex = targetClip;
                // Shorter blend into attack (0.05s) for a snappy responsive feel;
                // longer blend out of attack (0.12s) for a smooth recovery.
                float blendTime = (targetClip == 2) ? 0.05f : 0.12f;
                anim.player.crossfadeTo(&anim.clips[targetClip], blendTime);
            }

            // Scale animation speed
            if (anim.activeClipIndex == 2 && reg.all_of<CombatComponent>(e)) {
                // Attack: scale so the clip spans exactly one full attack cycle.
                auto& combat = reg.get<CombatComponent>(e);
                const auto& attackClip = anim.clips[anim.activeClipIndex];
                float clipDuration = (attackClip.duration > 0.0f) ? attackClip.duration : 1.0f;
                if (combat.attackSpeed > 0.0f)
                    anim.player.setTimeScale(clipDuration * combat.attackSpeed);
            } else if (anim.activeClipIndex == 1 && anim.activeClipIndex < (int)anim.clips.size()) {
                // Walk: scale to match movement speed
                const auto& walkClip = anim.clips[anim.activeClipIndex];
                float rawTimeScale = 1.0f;
                if (walkClip.strideLength > 0.0f && walkClip.duration > 0.0f) {
                    float animNaturalSpeed = walkClip.strideLength / walkClip.duration;
                    rawTimeScale = (animNaturalSpeed > 0.0f) ? (c.currentSpeed / animNaturalSpeed) : 1.0f;
                } else if (c.moveSpeed > 0.0f) {
                    // No strideLength data — estimate: at full moveSpeed the walk
                    // cycle should take ~1.2s regardless of clip duration to match
                    // the stride visually and prevent sliding.
                    constexpr float kDesiredCycleSec = 1.2f;
                    float clipDur = (walkClip.duration > 0.0f) ? walkClip.duration : 1.0f;
                    float baseScale = clipDur / kDesiredCycleSec;
                    rawTimeScale = (c.currentSpeed / c.moveSpeed) * baseScale;
                }
                // Smooth toward the raw target; lag constant ~15 Hz keeps the
                // walk cycle visually continuous across rapid speed changes.
                const float kSmooth = 15.0f;
                anim.smoothedTimeScale += (rawTimeScale - anim.smoothedTimeScale)
                                         * std::min(kSmooth * dt, 1.0f);
                anim.player.setTimeScale(anim.smoothedTimeScale);
            } else {
                // Idle: reset smoothed scale so next walk starts clean.
                anim.smoothedTimeScale = 1.0f;
                anim.player.setTimeScale(1.0f);
            }
        }
        anim.player.refreshSkeleton(&skel.skeleton);
        // NOTE: anim.player.update(dt) is NOT called here — it is already
        // called once per tick by AnimationUpdateSystem in the SimulationLoop.
        // Calling it again would double the animation playback speed.
    }
}

// ═══ GameSystems.cpp ═══

// ═══════════════════════════════════════════════════════════════════════════════
// VFXFlushSystem
// ═══════════════════════════════════════════════════════════════════════════════
void VFXFlushSystem::execute(entt::registry& /*registry*/, float dt) {
    if (m_vfx) {
        if (m_q1) m_vfx->processQueue(*m_q1);
        if (m_q2) m_vfx->processQueue(*m_q2);
        m_vfx->update(dt);
    }
    if (m_trail)      m_trail->update(dt);
    if (m_decals)     m_decals->update(dt);
    if (m_deferredDecals) m_deferredDecals->update(dt);
    if (m_mesh)       m_mesh->update(dt);
    if (m_distortion) m_distortion->update(dt);
}

// ═══════════════════════════════════════════════════════════════════════════════
// AbilityUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void AbilityUpdateSystem::execute(entt::registry& registry, float dt) {
    if (!m_abilities) return;

    m_abilities->update(registry, dt, m_trail, m_decals);

    if (m_q && m_trail && m_decals && m_deferredDecals && m_mesh &&
        m_explosions && m_cone && m_sprites && m_distortion)
    {
        m_abilities->getSequencer().update(dt,
            *m_q, *m_trail, *m_decals, *m_deferredDecals,
            *m_mesh, *m_explosions, *m_cone, *m_sprites, *m_distortion);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// ProjectileUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void ProjectileUpdateSystem::execute(entt::registry& registry, float dt) {
    if (!m_proj || !m_abilities || !m_q) return;

    m_proj->update(registry, dt, *m_q, *m_abilities, m_trail, m_gpuCollision);

    if (m_explosions) {
        for (const auto& pos : m_proj->getLandedPositions()) {
            m_explosions->addExplosion(pos);
        }
        m_proj->clearLandedPositions();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// EffectsUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void EffectsUpdateSystem::execute(entt::registry& /*registry*/, float dt) {
    if (m_explosions) m_explosions->update(dt);
    if (m_sprites)    m_sprites->update(dt);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ConeEffectSystem
// ═══════════════════════════════════════════════════════════════════════════════
void ConeEffectSystem::execute(entt::registry& /*registry*/, float dt) {
    if (!m_state || m_state->timer <= 0.0f || !m_cone) return;

    m_state->timer -= dt;
    float coneElapsed = m_state->duration - m_state->timer;
    m_cone->update(dt, m_state->apex, m_state->direction,
                   m_state->halfAngle, m_state->range, coneElapsed);
}

// ═══════════════════════════════════════════════════════════════════════════════
// CombatUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void CombatUpdateSystem::execute(entt::registry& registry, float dt) {
    if (m_combat) m_combat->update(registry, dt);
}

// ═══════════════════════════════════════════════════════════════════════════════
// PhysicsUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void PhysicsUpdateSystem::execute(entt::registry& registry, float dt) {
    PhysicsSystem::integrate(registry, dt);
    PhysicsSystem::resolveCollisionsAndWake(registry);
    PhysicsSystem::updateSleep(registry, dt);
}

// ═══════════════════════════════════════════════════════════════════════════════
// AnimationUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void AnimationUpdateSystem::execute(entt::registry& registry, float dt) {
    auto view = registry.view<AnimationComponent>();
    for (auto [entity, anim] : view.each()) {
        anim.player.update(dt);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// EconomyUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void EconomyUpdateSystem::execute(entt::registry& registry, float dt) {
    if (m_econ && m_gameTime)
        m_econ->update(registry, *m_gameTime, dt);
}

// ═══════════════════════════════════════════════════════════════════════════════
// StructureUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void StructureUpdateSystem::execute(entt::registry& registry, float dt) {
    if (m_structures) m_structures->update(registry, dt);
}

// ═══════════════════════════════════════════════════════════════════════════════
// MinionWaveUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void MinionWaveUpdateSystem::execute(entt::registry& registry, float dt) {
    if (m_waves && m_gameTime)
        m_waves->update(registry, dt, *m_gameTime);
}

// ═══════════════════════════════════════════════════════════════════════════════
// RespawnUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void RespawnUpdateSystem::execute(entt::registry& registry, float dt) {
    if (m_respawn) m_respawn->update(registry, dt);
}

// ═══════════════════════════════════════════════════════════════════════════════
// NPCBehaviorUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void NPCBehaviorUpdateSystem::execute(entt::registry& registry, float dt) {
    if (m_npc && m_abilities)
        m_npc->update(registry, dt, *m_abilities);
}

// ═══ SystemScheduler.cpp ═══

void SystemScheduler::build() {
    const uint32_t N = static_cast<uint32_t>(m_systems.size());
    if (N == 0) { m_levels.clear(); m_dirty = false; return; }

    // Map type_index → system index for dependency lookup
    std::unordered_map<std::type_index, uint32_t> typeToIdx;
    for (uint32_t i = 0; i < N; ++i) {
        // Use the concrete type behind the ISystem pointer
        typeToIdx[std::type_index(typeid(*m_systems[i]))] = i;
    }

    // Build adjacency list + in-degree from declared dependencies
    std::vector<std::vector<uint32_t>> successors(N);
    std::vector<uint32_t> inDegree(N, 0);

    for (uint32_t i = 0; i < N; ++i) {
        for (const auto& dep : m_systems[i]->dependsOn()) {
            auto it = typeToIdx.find(dep);
            if (it == typeToIdx.end()) {
                spdlog::warn("SystemScheduler: {} depends on unregistered type, ignoring",
                             m_systems[i]->name());
                continue;
            }
            uint32_t depIdx = it->second;
            successors[depIdx].push_back(i);
            ++inDegree[i];
        }
    }

    // Kahn's algorithm: compute levels (BFS layers)
    m_levels.clear();
    std::vector<uint32_t> currentLevel;
    for (uint32_t i = 0; i < N; ++i) {
        if (inDegree[i] == 0) currentLevel.push_back(i);
    }

    uint32_t processed = 0;
    while (!currentLevel.empty()) {
        m_levels.push_back(currentLevel);
        processed += static_cast<uint32_t>(currentLevel.size());

        std::vector<uint32_t> nextLevel;
        for (uint32_t idx : currentLevel) {
            for (uint32_t succ : successors[idx]) {
                if (--inDegree[succ] == 0) {
                    nextLevel.push_back(succ);
                }
            }
        }
        currentLevel = std::move(nextLevel);
    }

    if (processed != N) {
        spdlog::error("SystemScheduler: dependency cycle detected! {} of {} systems scheduled.",
                      processed, N);
    }

    m_lastTimingsMs.resize(N, 0.0f);
    m_dirty = false;

    // Log the schedule
    for (size_t lvl = 0; lvl < m_levels.size(); ++lvl) {
        std::string names;
        for (uint32_t idx : m_levels[lvl]) {
            if (!names.empty()) names += ", ";
            names += m_systems[idx]->name();
        }
        spdlog::info("SystemScheduler level {}: [{}]", lvl, names);
    }
}

void SystemScheduler::tick(entt::registry& registry, float dt, ThreadPool& pool) {
    if (m_dirty) build();

    for (const auto& level : m_levels) {
        if (level.size() == 1) {
            // Single system — run directly, no threading overhead
            auto t0 = std::chrono::high_resolution_clock::now();
            m_systems[level[0]]->execute(registry, dt);
            auto t1 = std::chrono::high_resolution_clock::now();
            m_lastTimingsMs[level[0]] = std::chrono::duration<float, std::milli>(t1 - t0).count();
        } else {
            // Multiple systems at this level — run in parallel
            std::latch done(static_cast<std::ptrdiff_t>(level.size()));
            for (uint32_t idx : level) {
                pool.submit([this, idx, &registry, dt, &done]() {
                    auto t0 = std::chrono::high_resolution_clock::now();
                    m_systems[idx]->execute(registry, dt);
                    auto t1 = std::chrono::high_resolution_clock::now();
                    m_lastTimingsMs[idx] = std::chrono::duration<float, std::milli>(t1 - t0).count();
                    done.count_down();
                });
            }
            done.wait();
        }
    }
}

void SystemScheduler::tickSequential(entt::registry& registry, float dt) {
    if (m_dirty) build();

    for (const auto& level : m_levels) {
        for (uint32_t idx : level) {
            auto t0 = std::chrono::high_resolution_clock::now();
            m_systems[idx]->execute(registry, dt);
            auto t1 = std::chrono::high_resolution_clock::now();
            m_lastTimingsMs[idx] = std::chrono::duration<float, std::milli>(t1 - t0).count();
        }
    }
}

} // namespace glory
