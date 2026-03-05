#include "minion/MinionSystem.h"
#include "scene/Components.h"

#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>

namespace glory {

namespace {
inline float dist2(const glm::vec3 &a, const glm::vec3 &b) {
  glm::vec3 d = a - b;
  return glm::dot(d, d);
}
} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Init
// ═════════════════════════════════════════════════════════════════════════════

void MinionSystem::init(const MinionConfig &config, const MapData &mapData) {
  m_config = config;
  m_mapData = mapData;
  m_nextWaveTime = config.firstWaveTime;
  m_waveCounter = 0;
  m_livingCount = 0;
  std::memset(m_inhibitorDown, 0, sizeof(m_inhibitorDown));
  spdlog::info("MinionSystem initialized: first wave at {:.0f}s, interval {:.0f}s",
               m_config.firstWaveTime, m_config.waveInterval);
}

// ═════════════════════════════════════════════════════════════════════════════
// Main update
// ═════════════════════════════════════════════════════════════════════════════

void MinionSystem::update(entt::registry &reg, float dt, float gameTime,
                          const HeightQueryFn &heightQuery) {
  m_currentGameTime = gameTime;

  // 1. Spawn waves
  while (gameTime >= m_nextWaveTime) {
    spawnWave(reg, gameTime, heightQuery);
    m_nextWaveTime += m_config.waveInterval;
  }

  // 2. Rebuild spatial hash
  buildSpatialHash(reg);

  // 3. Stat scaling (cheap, checks internally if tick boundary crossed)
  updateStatScaling(reg, gameTime);

  // 4. AI: aggro evaluation (staggered — not every frame)
  updateAggro(reg, dt);

  // 5. State machine transitions
  updateStates(reg, dt);

  // 6. Movement (path following + separation + chasing)
  updateMovement(reg, dt, heightQuery);

  // 7. Combat (auto-attacks, projectile spawning)
  updateCombat(reg, dt);

  // 8. Projectile updates
  updateProjectiles(reg, dt);

  // 9. Death processing
  updateDeath(reg, dt, gameTime);

  // Clear champion aggro events
  m_champAggroEvents.clear();
}

// ═════════════════════════════════════════════════════════════════════════════
// Spawning
// ═════════════════════════════════════════════════════════════════════════════

void MinionSystem::spawnWave(entt::registry &reg, float gameTime,
                             const HeightQueryFn &heightQuery) {
  m_waveCounter++;
  bool cannon = isCannonWave(m_waveCounter, gameTime);

  for (int teamIdx = 0; teamIdx < 2; ++teamIdx) {
    TeamID team = static_cast<TeamID>(teamIdx);
    const auto &base = m_mapData.teams[teamIdx].base;
    glm::vec3 spawnPos = base.nexusPosition;
    // Snap spawn Y to terrain surface so minions aren't underground
    if (heightQuery) {
      spawnPos.y = heightQuery(spawnPos.x, spawnPos.z);
    }

    for (int laneIdx = 0; laneIdx < 3; ++laneIdx) {
      LaneType lane = static_cast<LaneType>(laneIdx);

      // Offset per lane to avoid overlap
      glm::vec3 offset(0.0f);
      offset.x = static_cast<float>(laneIdx - 1) * 2.0f;

      // Melee minions
      for (int i = 0; i < m_config.standardMelee; ++i) {
        glm::vec3 pos = spawnPos + offset;
        pos.x += static_cast<float>(i) * 1.5f;
        spawnMinion(reg, MinionType::Melee, team, lane, pos, m_waveCounter,
                    gameTime);
      }

      // Caster minions
      for (int i = 0; i < m_config.standardCaster; ++i) {
        glm::vec3 pos = spawnPos + offset;
        pos.z += 3.0f; // spawn behind melee
        pos.x += static_cast<float>(i) * 1.5f;
        spawnMinion(reg, MinionType::Caster, team, lane, pos, m_waveCounter,
                    gameTime);
      }

      // Cannon / Super minion — disabled for now (melee + caster only)
      (void)cannon;
    }
  }

  spdlog::info("Wave {} spawned (cannon={})", m_waveCounter, cannon);
}

entt::entity MinionSystem::spawnMinion(entt::registry &reg, MinionType type,
                                        TeamID team, LaneType lane,
                                        const glm::vec3 &pos,
                                        uint32_t waveIdx, float gameTime) {
  auto e = reg.create();
  int ti = static_cast<int>(type);
  const auto &stats = m_config.stats[ti];

  // Tag
  reg.emplace<MinionTag>(e);

  // Transform
  auto &transform = reg.emplace<TransformComponent>(e);
  transform.position = pos;
  transform.scale = glm::vec3(1.0f); // will be adjusted by renderer

  // Identity
  reg.emplace<MinionIdentityComponent>(e, MinionIdentityComponent{type, team, lane, waveIdx});

  // Health (scaled to current game time)
  float scaledHP = computeScaledStat(stats.hp, stats.hpPerTick, gameTime);
  reg.emplace<MinionHealthComponent>(e, MinionHealthComponent{scaledHP, scaledHP, false, entt::null});

  // Combat
  MinionCombatComponent combat;
  combat.attackDamage = computeScaledStat(stats.attackDamage, stats.adPerTick, gameTime);
  combat.armor = computeScaledStat(stats.armor, stats.armorPerTick, gameTime);
  combat.magicResist = stats.magicResist;
  combat.attackRange = stats.attackRange;
  combat.attackCooldown = stats.attackCooldown;
  combat.timeSinceLastAttack = stats.attackCooldown; // ready to attack immediately
  combat.attackStyle = stats.attackStyle;
  combat.projectileSpeed = stats.projectileSpeed;
  reg.emplace<MinionCombatComponent>(e, combat);

  // Movement
  MinionMovementComponent mov;
  mov.moveSpeed = stats.moveSpeed;
  mov.currentWaypointIndex = 0;
  reg.emplace<MinionMovementComponent>(e, mov);

  // Aggro
  MinionAggroComponent aggro;
  aggro.aggroRange = m_config.aggroRange;
  aggro.leashRange = m_config.leashRange;
  reg.emplace<MinionAggroComponent>(e, aggro);

  // State
  reg.emplace<MinionStateComponent>(e, MinionStateComponent{MinionState::Spawning, 0.0f});

  m_livingCount++;
  return e;
}

// ═════════════════════════════════════════════════════════════════════════════
// Spatial Hash
// ═════════════════════════════════════════════════════════════════════════════

void MinionSystem::buildSpatialHash(entt::registry &reg) {
  m_spatialBlue.clear();
  m_spatialRed.clear();

  auto view = reg.view<MinionTag, MinionIdentityComponent, TransformComponent,
                       MinionHealthComponent>();
  for (auto e : view) {
    auto &hp = view.get<MinionHealthComponent>(e);
    if (hp.isDead) continue;
    auto &id = view.get<MinionIdentityComponent>(e);
    auto &t = view.get<TransformComponent>(e);
    if (id.team == TeamID::Blue)
      m_spatialBlue.insert(e, t.position);
    else
      m_spatialRed.insert(e, t.position);
  }

  // Also insert champions into the spatial hash for targeting
  auto charView = reg.view<TransformComponent, CharacterComponent>();
  for (auto e : charView) {
    auto &t = charView.get<TransformComponent>(e);
    // For now treat all characters as Blue team (player)
    m_spatialBlue.insert(e, t.position);
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// Stat Scaling
// ═════════════════════════════════════════════════════════════════════════════

void MinionSystem::updateStatScaling(entt::registry &reg, float gameTime) {
  // Scaling is applied at spawn time via computeScaledStat().
  // For minions that survive across multiple tick boundaries, we could
  // periodically recalculate.  For now, stats are fixed at spawn.
  // This keeps the per-frame cost at zero.
}

// ═════════════════════════════════════════════════════════════════════════════
// Aggro System
// ═════════════════════════════════════════════════════════════════════════════

void MinionSystem::updateAggro(entt::registry &reg, float dt) {
  auto view = reg.view<MinionTag, MinionIdentityComponent, TransformComponent,
                       MinionAggroComponent, MinionHealthComponent,
                       MinionStateComponent>();

  for (auto e : view) {
    auto &hp = view.get<MinionHealthComponent>(e);
    if (hp.isDead) continue;

    auto &aggro = view.get<MinionAggroComponent>(e);
    auto &state = view.get<MinionStateComponent>(e);
    auto &id = view.get<MinionIdentityComponent>(e);
    auto &pos = view.get<TransformComponent>(e).position;

    // Decrement timers
    aggro.timeSinceLastTargetEval += dt;
    aggro.championAggroCooldown = std::max(0.0f, aggro.championAggroCooldown - dt);
    aggro.championAggroTimer = std::max(0.0f, aggro.championAggroTimer - dt);

    // Only evaluate aggro at the configured interval (staggered)
    if (aggro.timeSinceLastTargetEval < m_config.aggroCheckInterval) continue;
    aggro.timeSinceLastTargetEval = 0.0f;

    // Check if forced champion aggro is active
    if (aggro.championAggroTimer > 0.0f && aggro.forcedChampionTarget != entt::null) {
      if (reg.valid(aggro.forcedChampionTarget)) {
        auto *targetHP = reg.try_get<MinionHealthComponent>(aggro.forcedChampionTarget);
        if (!targetHP || !targetHP->isDead) {
          aggro.currentTarget = aggro.forcedChampionTarget;
          continue; // keep forced target
        }
      }
      aggro.championAggroTimer = 0.0f;
      aggro.forcedChampionTarget = entt::null;
    }

    // Process champion aggro draw events
    if (aggro.championAggroCooldown <= 0.0f) {
      for (const auto &evt : m_champAggroEvents) {
        // Check if the victim is on our team and within our aggro range
        float dx = evt.victimPos.x - pos.x;
        float dz = evt.victimPos.z - pos.z;
        if (dx * dx + dz * dz <= aggro.aggroRange * aggro.aggroRange) {
          // Check teams: we (id.team) help our team's champion
          // For now: if victim champion is on same team, target the attacker
          aggro.forcedChampionTarget = evt.attacker;
          aggro.championAggroTimer = m_config.championAggroDuration;
          aggro.championAggroCooldown = m_config.championAggroCooldown;
          aggro.currentTarget = evt.attacker;
          break;
        }
      }
    }

    // Check if current target is still valid
    if (aggro.currentTarget != entt::null) {
      bool valid = reg.valid(aggro.currentTarget);
      if (valid) {
        auto *targetHP = reg.try_get<MinionHealthComponent>(aggro.currentTarget);
        if (targetHP && targetHP->isDead) valid = false;
        if (valid) {
          auto *targetT = reg.try_get<TransformComponent>(aggro.currentTarget);
          if (targetT) {
            float d2 = dist2(pos, targetT->position);
            if (d2 > aggro.leashRange * aggro.leashRange) valid = false;
          } else {
            valid = false;
          }
        }
      }
      if (!valid) {
        aggro.currentTarget = entt::null;
      } else if (aggro.timeSinceLastTargetEval < m_config.targetReEvalInterval) {
        continue; // keep current target if not time for full re-eval
      }
    }

    // Find best target using priority list
    // Query enemy minions in aggro range
    const SpatialHash &enemyHash =
        (id.team == TeamID::Blue) ? m_spatialRed : m_spatialBlue;

    enemyHash.query(pos, aggro.aggroRange, m_queryResults);

    entt::entity bestTarget = entt::null;
    float bestDist2 = std::numeric_limits<float>::max();

    // Priority 6: closest enemy minion (most common case)
    for (auto candidate : m_queryResults) {
      if (!reg.valid(candidate)) continue;

      auto *candidateHP = reg.try_get<MinionHealthComponent>(candidate);
      if (candidateHP && candidateHP->isDead) continue;

      auto *candidateT = reg.try_get<TransformComponent>(candidate);
      if (!candidateT) continue;

      float d2 = dist2(pos, candidateT->position);
      if (d2 < bestDist2) {
        bestDist2 = d2;
        bestTarget = candidate;
      }
    }

    aggro.currentTarget = bestTarget;
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// State Machine
// ═════════════════════════════════════════════════════════════════════════════

void MinionSystem::updateStates(entt::registry &reg, float dt) {
  auto view = reg.view<MinionTag, MinionStateComponent, MinionAggroComponent,
                       MinionCombatComponent, MinionHealthComponent,
                       TransformComponent>();

  for (auto e : view) {
    auto &hp = view.get<MinionHealthComponent>(e);
    if (hp.isDead) continue;

    auto &state = view.get<MinionStateComponent>(e);
    auto &aggro = view.get<MinionAggroComponent>(e);
    auto &combat = view.get<MinionCombatComponent>(e);
    auto &pos = view.get<TransformComponent>(e).position;

    state.stateTimer += dt;

    switch (state.state) {
    case MinionState::Spawning:
      // Brief spawn delay (0.5s)
      if (state.stateTimer >= 0.5f) {
        state.state = MinionState::Pathing;
        state.stateTimer = 0.0f;
      }
      break;

    case MinionState::Pathing:
      if (aggro.currentTarget != entt::null) {
        state.state = MinionState::Engaging;
        state.stateTimer = 0.0f;
      }
      break;

    case MinionState::Engaging: {
      if (aggro.currentTarget == entt::null) {
        state.state = MinionState::Returning;
        state.stateTimer = 0.0f;
        break;
      }
      auto *targetT = reg.try_get<TransformComponent>(aggro.currentTarget);
      if (targetT) {
        float dist = glm::distance(pos, targetT->position);
        if (dist > aggro.leashRange) {
          aggro.currentTarget = entt::null;
          state.state = MinionState::Returning;
          state.stateTimer = 0.0f;
        } else if (dist <= combat.attackRange) {
          state.state = MinionState::Attacking;
          state.stateTimer = 0.0f;
        }
      }
      break;
    }

    case MinionState::Attacking: {
      if (aggro.currentTarget == entt::null) {
        state.state = MinionState::Returning;
        state.stateTimer = 0.0f;
        break;
      }
      auto *targetT = reg.try_get<TransformComponent>(aggro.currentTarget);
      if (targetT) {
        float dist = glm::distance(pos, targetT->position);
        if (dist > combat.attackRange) {
          state.state = MinionState::Chasing;
          state.stateTimer = 0.0f;
        }
      }
      break;
    }

    case MinionState::Chasing: {
      if (aggro.currentTarget == entt::null) {
        state.state = MinionState::Returning;
        state.stateTimer = 0.0f;
        break;
      }
      auto *targetT = reg.try_get<TransformComponent>(aggro.currentTarget);
      if (targetT) {
        float dist = glm::distance(pos, targetT->position);
        if (dist <= combat.attackRange) {
          state.state = MinionState::Attacking;
          state.stateTimer = 0.0f;
        } else if (dist > aggro.leashRange) {
          aggro.currentTarget = entt::null;
          state.state = MinionState::Returning;
          state.stateTimer = 0.0f;
        }
      }
      break;
    }

    case MinionState::Returning:
      // Return to lane for 2 seconds then resume pathing
      if (state.stateTimer >= 2.0f || aggro.currentTarget != entt::null) {
        state.state = (aggro.currentTarget != entt::null)
                          ? MinionState::Engaging
                          : MinionState::Pathing;
        state.stateTimer = 0.0f;
      }
      break;

    case MinionState::Dying:
      if (state.stateTimer >= 0.5f) {
        state.state = MinionState::Dead;
        state.stateTimer = 0.0f;
      }
      break;

    case MinionState::Dead:
      // Handled in updateDeath()
      break;
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// Movement
// ═════════════════════════════════════════════════════════════════════════════

void MinionSystem::updateMovement(entt::registry &reg, float dt,
                                   const HeightQueryFn &heightQuery) {
  auto view = reg.view<MinionTag, MinionIdentityComponent, TransformComponent,
                       MinionMovementComponent, MinionAggroComponent,
                       MinionStateComponent, MinionHealthComponent>();

  for (auto e : view) {
    auto &hp = view.get<MinionHealthComponent>(e);
    if (hp.isDead) continue;

    auto &state = view.get<MinionStateComponent>(e);
    if (state.state == MinionState::Spawning || state.state == MinionState::Dying ||
        state.state == MinionState::Dead || state.state == MinionState::Attacking)
      continue;

    auto &id = view.get<MinionIdentityComponent>(e);
    auto &t = view.get<TransformComponent>(e);
    auto &mov = view.get<MinionMovementComponent>(e);
    auto &aggro = view.get<MinionAggroComponent>(e);

    glm::vec3 targetPos;
    bool hasTarget = false;

    if (state.state == MinionState::Engaging || state.state == MinionState::Chasing) {
      // Move toward aggro target
      if (aggro.currentTarget != entt::null && reg.valid(aggro.currentTarget)) {
        auto *targetT = reg.try_get<TransformComponent>(aggro.currentTarget);
        if (targetT) {
          targetPos = targetT->position;
          hasTarget = true;
        }
      }
    }

    if (!hasTarget) {
      // Follow lane waypoints
      const auto &waypoints = getLaneWaypoints(id.team, id.lane);
      if (waypoints.empty()) continue;

      if (mov.currentWaypointIndex >= static_cast<uint32_t>(waypoints.size())) {
        mov.currentWaypointIndex = static_cast<uint32_t>(waypoints.size()) - 1;
      }
      targetPos = waypoints[mov.currentWaypointIndex];
      hasTarget = true;

      // Check if reached waypoint
      float dist = glm::distance(glm::vec2(t.position.x, t.position.z),
                                  glm::vec2(targetPos.x, targetPos.z));
      if (dist < 3.0f && mov.currentWaypointIndex < static_cast<uint32_t>(waypoints.size()) - 1) {
        mov.currentWaypointIndex++;
        targetPos = waypoints[mov.currentWaypointIndex];
      }
    }

    if (hasTarget) {
      glm::vec3 dir = targetPos - t.position;
      dir.y = 0.0f; // move on XZ plane
      float dist = glm::length(dir);
      if (dist > 0.1f) {
        dir /= dist;
        float step = mov.moveSpeed * dt;
        if (step > dist) step = dist;
        t.position.x += dir.x * step;
        t.position.z += dir.z * step;

        // Face movement direction
        t.rotation.y = std::atan2(dir.x, dir.z);
      }
    }

    // Separation steering — avoid overlapping other nearby minions
    {
      const SpatialHash &ownHash =
          (id.team == TeamID::Blue) ? m_spatialBlue : m_spatialRed;
      ownHash.query(t.position, 2.0f, m_queryResults);

      glm::vec3 separation(0.0f);
      int count = 0;
      for (auto other : m_queryResults) {
        if (other == e) continue;
        auto *otherT = reg.try_get<TransformComponent>(other);
        if (!otherT) continue;
        glm::vec3 diff = t.position - otherT->position;
        diff.y = 0.0f;
        float d2 = glm::dot(diff, diff);
        if (d2 > 0.01f && d2 < 2.0f * 2.0f) {
          separation += diff / d2; // weight inversely by distance²
          count++;
        }
      }
      if (count > 0) {
        separation /= static_cast<float>(count);
        float sepLen = glm::length(separation);
        if (sepLen > 0.01f) {
          separation /= sepLen;
          t.position.x += separation.x * mov.moveSpeed * 0.3f * dt;
          t.position.z += separation.z * mov.moveSpeed * 0.3f * dt;
        }
      }
    }

    // Snap to terrain
    if (heightQuery) {
      t.position.y = heightQuery(t.position.x, t.position.z);
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// Combat
// ═════════════════════════════════════════════════════════════════════════════

void MinionSystem::updateCombat(entt::registry &reg, float dt) {
  auto view = reg.view<MinionTag, MinionStateComponent, MinionCombatComponent,
                       MinionAggroComponent, MinionIdentityComponent,
                       TransformComponent, MinionHealthComponent>();

  for (auto e : view) {
    auto &hp = view.get<MinionHealthComponent>(e);
    if (hp.isDead) continue;

    auto &state = view.get<MinionStateComponent>(e);
    if (state.state != MinionState::Attacking) continue;

    auto &combat = view.get<MinionCombatComponent>(e);
    auto &aggro = view.get<MinionAggroComponent>(e);
    auto &pos = view.get<TransformComponent>(e).position;

    combat.timeSinceLastAttack += dt;

    if (combat.timeSinceLastAttack < combat.attackCooldown) continue;
    if (aggro.currentTarget == entt::null || !reg.valid(aggro.currentTarget))
      continue;

    combat.timeSinceLastAttack = 0.0f;

    if (combat.attackStyle == AttackStyle::Melee) {
      // Instant melee damage
      auto *targetHP = reg.try_get<MinionHealthComponent>(aggro.currentTarget);
      if (targetHP && !targetHP->isDead) {
        float targetArmor = 0.0f;
        auto *targetCombat = reg.try_get<MinionCombatComponent>(aggro.currentTarget);
        if (targetCombat) targetArmor = targetCombat->armor;
        float dmg = computeDamage(combat.attackDamage, targetArmor);
        targetHP->currentHP -= dmg;
        targetHP->lastAttacker = e;
        if (targetHP->currentHP <= 0.0f) {
          targetHP->currentHP = 0.0f;
          targetHP->isDead = true;
        }
      }
    } else {
      // Ranged: spawn projectile entity
      auto proj = reg.create();
      auto &projT = reg.emplace<TransformComponent>(proj);
      projT.position = pos;
      projT.scale = glm::vec3(0.3f);

      MinionProjectileComponent pc;
      pc.target = aggro.currentTarget;
      pc.owner = e;
      pc.speed = combat.projectileSpeed;
      pc.damage = combat.attackDamage;
      reg.emplace<MinionProjectileComponent>(proj, pc);
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// Projectiles
// ═════════════════════════════════════════════════════════════════════════════

void MinionSystem::updateProjectiles(entt::registry &reg, float dt) {
  auto view = reg.view<MinionProjectileComponent, TransformComponent>();

  std::vector<entt::entity> toDestroy;

  for (auto e : view) {
    auto &proj = view.get<MinionProjectileComponent>(e);
    auto &t = view.get<TransformComponent>(e);

    proj.age += dt;
    if (proj.age > proj.maxLifetime) {
      toDestroy.push_back(e);
      continue;
    }

    // Check if target is still valid
    if (proj.target == entt::null || !reg.valid(proj.target)) {
      toDestroy.push_back(e);
      continue;
    }

    auto *targetT = reg.try_get<TransformComponent>(proj.target);
    if (!targetT) {
      toDestroy.push_back(e);
      continue;
    }

    // Home toward target
    glm::vec3 dir = targetT->position - t.position;
    float dist = glm::length(dir);
    float step = proj.speed * dt;

    if (dist <= step || dist < 1.0f) {
      // Hit! Apply damage
      auto *targetHP = reg.try_get<MinionHealthComponent>(proj.target);
      if (targetHP && !targetHP->isDead) {
        float targetArmor = 0.0f;
        auto *targetCombat = reg.try_get<MinionCombatComponent>(proj.target);
        if (targetCombat) targetArmor = targetCombat->armor;
        float dmg = computeDamage(proj.damage, targetArmor);
        targetHP->currentHP -= dmg;
        targetHP->lastAttacker = proj.owner;
        if (targetHP->currentHP <= 0.0f) {
          targetHP->currentHP = 0.0f;
          targetHP->isDead = true;
        }
      }
      toDestroy.push_back(e);
    } else {
      dir /= dist;
      t.position += dir * step;
    }
  }

  for (auto e : toDestroy) {
    if (reg.valid(e)) reg.destroy(e);
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// Death
// ═════════════════════════════════════════════════════════════════════════════

void MinionSystem::updateDeath(entt::registry &reg, float dt, float gameTime) {
  auto view = reg.view<MinionTag, MinionHealthComponent, MinionStateComponent,
                       MinionIdentityComponent, TransformComponent>();

  std::vector<entt::entity> toRemove;

  for (auto e : view) {
    auto &hp = view.get<MinionHealthComponent>(e);
    auto &state = view.get<MinionStateComponent>(e);

    if (!hp.isDead) continue;

    if (state.state != MinionState::Dying && state.state != MinionState::Dead) {
      // Just died — transition to Dying
      state.state = MinionState::Dying;
      state.stateTimer = 0.0f;

      // Emit death event
      auto &id = view.get<MinionIdentityComponent>(e);
      auto &pos = view.get<TransformComponent>(e).position;

      MinionDeathEvent evt;
      evt.minion = e;
      evt.type = id.type;
      evt.team = id.team;
      evt.lane = id.lane;
      evt.position = pos;
      evt.killer = hp.lastAttacker;
      evt.goldValue = computeGold(id.type, gameTime);
      evt.xpValue = m_config.rewards[static_cast<int>(id.type)].xp;
      m_deathEvents.push_back(evt);

      m_livingCount--;
    }

    if (state.state == MinionState::Dead) {
      toRemove.push_back(e);
    }
  }

  for (auto e : toRemove) {
    if (reg.valid(e)) reg.destroy(e);
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// Public API
// ═════════════════════════════════════════════════════════════════════════════

void MinionSystem::notifyChampionAttack(entt::entity attacker,
                                         entt::entity victim,
                                         const glm::vec3 &victimPos) {
  m_champAggroEvents.push_back({attacker, victim, victimPos, m_currentGameTime});
}

void MinionSystem::setInhibitorDown(TeamID team, LaneType lane, bool down) {
  m_inhibitorDown[static_cast<int>(team)][static_cast<int>(lane)] = down;
}

std::vector<MinionDeathEvent> MinionSystem::consumeDeathEvents() {
  auto events = std::move(m_deathEvents);
  m_deathEvents.clear();
  return events;
}

// ═════════════════════════════════════════════════════════════════════════════
// Helpers
// ═════════════════════════════════════════════════════════════════════════════

float MinionSystem::computeScaledStat(float base, float perTick,
                                       float gameTime) const {
  int ticks = static_cast<int>(gameTime / m_config.scalingIntervalSeconds);
  return base + perTick * static_cast<float>(ticks);
}

float MinionSystem::computeDamage(float ad, float targetArmor) const {
  float multiplier = 100.0f / (100.0f + std::max(0.0f, targetArmor));
  return ad * multiplier;
}

float MinionSystem::computeGold(MinionType type, float gameTime) const {
  const auto &r = m_config.rewards[static_cast<int>(type)];
  float minutes = gameTime / 60.0f;
  return r.goldBase + r.goldPerMinute * minutes;
}

bool MinionSystem::isCannonWave(uint32_t waveIdx, float gameTime) const {
  if (waveIdx == 0) return false;

  // Find the applicable cannon rule (latest minGameTime that's <= gameTime)
  int everyN = 3; // default
  for (const auto &rule : m_config.cannonRules) {
    if (gameTime >= rule.minGameTime) {
      everyN = rule.everyNthWave;
    }
  }
  return (waveIdx % everyN) == 0;
}

const std::vector<glm::vec3> &
MinionSystem::getLaneWaypoints(TeamID team, LaneType lane) const {
  int ti = static_cast<int>(team);
  int li = static_cast<int>(lane);
  return m_mapData.teams[ti].lanes[li].waypoints;
}

} // namespace glory
