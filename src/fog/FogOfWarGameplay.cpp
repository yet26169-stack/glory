#include "fog/FogOfWarGameplay.h"
#include "combat/StructureSystem.h"

#include <spdlog/spdlog.h>
#include <algorithm>

namespace glory {

// ── Main tick ───────────────────────────────────────────────────────────────
void FogOfWarGameplay::update(entt::registry& reg, FogSystem& fogSystem,
                               float dt, Team localTeam) {
    tickWards(reg, dt);
    gatherVisionSources(reg, localTeam);
    fogSystem.update(m_visionEntities);
    updateEnemyVisibility(reg, fogSystem, dt, localTeam);
}

// ── Gather all allied vision sources ────────────────────────────────────────
void FogOfWarGameplay::gatherVisionSources(entt::registry& reg, Team localTeam) {
    m_visionEntities.clear();
    m_visionEntities.reserve(64);

    // Allied units with TransformComponent + TeamComponent
    auto unitView = reg.view<TransformComponent, TeamComponent>();
    for (auto [ent, tf, tc] : unitView.each()) {
        if (tc.team != localTeam) continue;

        float radius = MINION_VISION; // default for minions/misc

        // Check for explicit VisionComponent
        if (auto* vc = reg.try_get<VisionComponent>(ent)) {
            radius = vc->sightRadius;
        } else if (reg.all_of<CharacterComponent>(ent)) {
            // Heroes have larger vision
            radius = HERO_VISION;
        }

        // Structures use tower vision
        if (auto* sc = reg.try_get<StructureComponent>(ent)) {
            radius = TOWER_VISION;
        }

        m_visionEntities.push_back({ tf.position, radius });
    }

    // Wards (not necessarily on the same team via TeamComponent, use WardComponent.teamIndex)
    auto wardView = reg.view<TransformComponent, WardComponent>();
    for (auto [ent, tf, ward] : wardView.each()) {
        uint8_t teamIdx = static_cast<uint8_t>(localTeam);
        if (ward.teamIndex != teamIdx) continue;
        m_visionEntities.push_back({ tf.position, ward.sightRadius });
    }
}

// ── Update enemy visibility states ──────────────────────────────────────────
void FogOfWarGameplay::updateEnemyVisibility(entt::registry& reg,
                                              FogSystem& fogSystem,
                                              float dt, Team localTeam) {
    auto view = reg.view<TransformComponent, TeamComponent>();
    for (auto [ent, tf, tc] : view.each()) {
        // Allied and neutral units are always visible — no FowVisibilityComponent needed
        if (tc.team == localTeam || tc.team == Team::NEUTRAL) continue;

        // Ensure enemy has a FowVisibilityComponent
        auto& fv = reg.get_or_emplace<FowVisibilityComponent>(ent);

        // Query the fog grid for this entity's position
        bool inVision = fogSystem.isPositionVisible(tf.position.x, tf.position.z);
        fv.inVision = inVision;

        switch (fv.state) {
        case FowVisState::HIDDEN:
            if (inVision) {
                fv.state = FowVisState::FADING_IN;
                fv.fadeTimer = FADE_IN_TIME;
                fv.alpha = 0.0f;
            }
            break;

        case FowVisState::FADING_IN:
            fv.fadeTimer -= dt;
            fv.alpha = 1.0f - std::max(fv.fadeTimer / FADE_IN_TIME, 0.0f);
            if (fv.fadeTimer <= 0.0f) {
                fv.state = FowVisState::VISIBLE;
                fv.alpha = 1.0f;
            }
            if (!inVision) {
                // Lost vision during fade-in → start fading out
                fv.state = FowVisState::FADING_OUT;
                fv.fadeTimer = FADE_OUT_TIME;
                fv.lastKnownPos = tf.position;
            }
            break;

        case FowVisState::VISIBLE:
            fv.alpha = 1.0f;
            fv.lastKnownPos = tf.position; // continuously update last known
            if (!inVision) {
                fv.state = FowVisState::FADING_OUT;
                fv.fadeTimer = FADE_OUT_TIME;
            }
            break;

        case FowVisState::FADING_OUT:
            fv.fadeTimer -= dt;
            fv.alpha = std::max(fv.fadeTimer / FADE_OUT_TIME, 0.0f) * 0.5f;
            // Ghost: render at last known position with fading alpha
            if (fv.fadeTimer <= 0.0f) {
                fv.state = FowVisState::HIDDEN;
                fv.alpha = 0.0f;
            }
            if (inVision) {
                // Re-entered vision while fading out
                fv.state = FowVisState::FADING_IN;
                fv.fadeTimer = FADE_IN_TIME * (1.0f - fv.alpha);
            }
            break;
        }
    }
}

// ── Ward lifecycle ──────────────────────────────────────────────────────────
void FogOfWarGameplay::tickWards(entt::registry& reg, float dt) {
    std::vector<entt::entity> expired;
    auto view = reg.view<WardComponent>();
    for (auto [ent, ward] : view.each()) {
        ward.timeLeft -= dt;
        if (ward.timeLeft <= 0.0f) {
            expired.push_back(ent);
        }
    }
    for (auto e : expired) {
        if (reg.valid(e)) {
            spdlog::info("Ward expired, destroying entity");
            reg.destroy(e);
        }
    }
}

// ── Place ward ──────────────────────────────────────────────────────────────
entt::entity FogOfWarGameplay::placeWard(entt::registry& reg,
                                          const glm::vec3& position,
                                          Team team) {
    auto ward = reg.create();
    reg.emplace<TransformComponent>(ward, TransformComponent{
        .position = position,
        .scale = glm::vec3(0.3f)
    });
    reg.emplace<TeamComponent>(ward, TeamComponent{ team });
    reg.emplace<WardComponent>(ward, WardComponent{
        .sightRadius = WARD_SIGHT,
        .duration    = WARD_DURATION,
        .timeLeft    = WARD_DURATION,
        .teamIndex   = static_cast<uint8_t>(team)
    });

    spdlog::info("Ward placed at ({:.1f}, {:.1f}) for team {}",
                 position.x, position.z, static_cast<int>(team));
    return ward;
}

// ── Static helpers for render filtering ─────────────────────────────────────
bool FogOfWarGameplay::shouldRender(const entt::registry& reg, entt::entity e,
                                     Team localTeam) {
    auto* tc = reg.try_get<TeamComponent>(e);
    if (!tc) return true;                         // no team = always visible
    if (tc->team == localTeam) return true;        // allies always visible
    if (tc->team == Team::NEUTRAL) return true;    // neutrals always visible

    auto* fv = reg.try_get<FowVisibilityComponent>(e);
    if (!fv) return true;   // no FoW component yet = visible (first frame)

    return fv->state != FowVisState::HIDDEN;
}

float FogOfWarGameplay::getRenderAlpha(const entt::registry& reg, entt::entity e) {
    auto* fv = reg.try_get<FowVisibilityComponent>(e);
    if (!fv) return 1.0f;
    return fv->alpha;
}

bool FogOfWarGameplay::isVisibleOnMinimap(const entt::registry& reg,
                                           entt::entity e, Team localTeam) {
    auto* tc = reg.try_get<TeamComponent>(e);
    if (!tc) return true;
    if (tc->team == localTeam || tc->team == Team::NEUTRAL) return true;

    auto* fv = reg.try_get<FowVisibilityComponent>(e);
    if (!fv) return true;

    // Show on minimap if visible or fading out (ghost)
    return fv->state == FowVisState::VISIBLE ||
           fv->state == FowVisState::FADING_IN;
}

} // namespace glory
