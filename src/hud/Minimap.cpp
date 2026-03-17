#include "hud/Minimap.h"
#include "scene/Components.h"
#include "combat/CombatComponents.h"
#include "fog/FogOfWarGameplay.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace glory {

// ── Coordinate conversions ──────────────────────────────────────────────────

glm::vec2 Minimap::worldToMinimap(const glm::vec3& worldPos) const {
    float nx = (worldPos.x - m_config.worldMin.x) /
               (m_config.worldMax.x - m_config.worldMin.x);
    float nz = (worldPos.z - m_config.worldMin.y) /
               (m_config.worldMax.y - m_config.worldMin.y);
    return {m_origin.x + nx * m_size.x,
            m_origin.y + nz * m_size.y};
}

glm::vec3 Minimap::minimapToWorld(const glm::vec2& screenPos) const {
    float nx = (screenPos.x - m_origin.x) / m_size.x;
    float nz = (screenPos.y - m_origin.y) / m_size.y;
    float wx = m_config.worldMin.x + nx * (m_config.worldMax.x - m_config.worldMin.x);
    float wz = m_config.worldMin.y + nz * (m_config.worldMax.y - m_config.worldMin.y);
    // Clamp to map bounds
    wx = std::clamp(wx, m_config.worldMin.x, m_config.worldMax.x);
    wz = std::clamp(wz, m_config.worldMin.y, m_config.worldMax.y);
    return {wx, 0.0f, wz};
}

bool Minimap::containsScreenPos(float screenX, float screenY) const {
    return screenX >= m_origin.x && screenX <= m_origin.x + m_size.x &&
           screenY >= m_origin.y && screenY <= m_origin.y + m_size.y;
}

// ── Main update ─────────────────────────────────────────────────────────────

void Minimap::update(MinimapUpdateContext& ctx) {
    m_consumedInput = false;

    // Compute minimap position: bottom-left of window
    m_size = {m_config.size, m_config.size};
    m_origin = {m_config.margin,
                ctx.windowHeight - m_config.size - m_config.margin};

    // Set world bounds from map data
    m_config.worldMin = {ctx.mapData.mapBoundsMin.x, ctx.mapData.mapBoundsMin.z};
    m_config.worldMax = {ctx.mapData.mapBoundsMax.x, ctx.mapData.mapBoundsMax.z};

    // Process input first (may consume clicks)
    handleInput(ctx);

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // Clip all drawing to minimap bounds
    dl->PushClipRect(ImVec2(m_origin.x, m_origin.y),
                     ImVec2(m_origin.x + m_size.x, m_origin.y + m_size.y),
                     true);

    drawBackground(dl, ctx.mapData);
    drawLanes(dl, ctx.mapData);
    drawStructures(dl, ctx.mapData);
    drawEntities(dl, ctx.registry, ctx.playerEntity, 0.0f);
    drawCameraFrustum(dl, ctx.camera, ctx.aspect);

    dl->PopClipRect();

    // Border drawn outside clip rect so it's fully visible
    drawBorder(dl);
}

// ── Drawing helpers ─────────────────────────────────────────────────────────

void Minimap::drawBackground(ImDrawList* dl, const MapData& map) const {
    ImVec2 tl(m_origin.x, m_origin.y);
    ImVec2 br(m_origin.x + m_size.x, m_origin.y + m_size.y);
    dl->AddRectFilled(tl, br, m_config.bgColor, m_config.cornerRadius);

    // Team base areas
    auto drawBase = [&](const Base& base, ImU32 color) {
        glm::vec2 center = worldToMinimap(base.spawnPlatformCenter);
        float r = (base.spawnPlatformRadius /
                   (m_config.worldMax.x - m_config.worldMin.x)) * m_size.x;
        r = std::max(r, 8.0f);
        dl->AddCircleFilled(ImVec2(center.x, center.y), r, color);
    };
    if (map.teams[0].base.spawnPlatformRadius > 0.0f)
        drawBase(map.teams[0].base, m_config.baseAllyColor);
    if (map.teams[1].base.spawnPlatformRadius > 0.0f)
        drawBase(map.teams[1].base, m_config.baseEnemyColor);
}

void Minimap::drawLanes(ImDrawList* dl, const MapData& map) const {
    for (int ti = 0; ti < 2; ++ti) {
        for (const auto& lane : map.teams[ti].lanes) {
            if (lane.waypoints.size() < 2) continue;
            std::vector<ImVec2> pts;
            pts.reserve(lane.waypoints.size());
            for (const auto& wp : lane.waypoints) {
                glm::vec2 mp = worldToMinimap(wp);
                pts.emplace_back(mp.x, mp.y);
            }
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()),
                            m_config.laneColor, ImDrawFlags_None, 2.5f);
        }
    }
}

void Minimap::drawStructures(ImDrawList* dl, const MapData& map) const {
    auto drawDiamond = [&](const glm::vec2& pos, float halfSize, ImU32 color) {
        ImVec2 top(pos.x, pos.y - halfSize);
        ImVec2 right(pos.x + halfSize, pos.y);
        ImVec2 bottom(pos.x, pos.y + halfSize);
        ImVec2 left(pos.x - halfSize, pos.y);
        dl->AddQuadFilled(top, right, bottom, left, color);
    };

    for (int ti = 0; ti < 2; ++ti) {
        ImU32 towerCol = (ti == 0) ? m_config.towerAllyColor : m_config.towerEnemyColor;
        ImU32 structCol = towerCol;

        // Towers
        for (const auto& tower : map.teams[ti].towers) {
            glm::vec2 pos = worldToMinimap(tower.position);
            drawDiamond(pos, 4.0f, towerCol);
        }

        // Inhibitors
        for (const auto& inhib : map.teams[ti].inhibitors) {
            glm::vec2 pos = worldToMinimap(inhib.position);
            drawDiamond(pos, 3.5f, structCol);
            dl->AddCircle(ImVec2(pos.x, pos.y), 4.5f, structCol, 0, 1.0f);
        }

        // Nexus
        glm::vec2 nexusPos = worldToMinimap(map.teams[ti].base.nexusPosition);
        drawDiamond(nexusPos, 5.5f, structCol);
        dl->AddCircle(ImVec2(nexusPos.x, nexusPos.y), 7.0f, structCol, 0, 1.5f);
    }

    // Neutral camps
    for (const auto& camp : map.neutralCamps) {
        glm::vec2 pos = worldToMinimap(camp.position);
        dl->AddCircleFilled(ImVec2(pos.x, pos.y), 2.5f, m_config.neutralColor);
    }
}

void Minimap::drawEntities(ImDrawList* dl, const entt::registry& reg,
                           entt::entity player, float /*gameTime*/) const {
    auto view = reg.view<const TransformComponent, const TeamComponent>();
    for (auto [entity, transform, team] : view.each()) {
        // FoW: skip enemies not visible to the player's team
        if (team.team == Team::ENEMY &&
            !FogOfWarGameplay::isVisibleOnMinimap(reg, entity, Team::PLAYER))
            continue;

        glm::vec2 pos = worldToMinimap(transform.position);
        ImVec2 imPos(pos.x, pos.y);

        // Determine color and size based on entity type
        ImU32 color;
        float radius;

        if (entity == player) {
            // Player: larger green dot with white border and subtle pulse
            color = m_config.playerColor;
            radius = 5.0f;
            dl->AddCircleFilled(imPos, radius, color);
            dl->AddCircle(imPos, radius + 1.5f, IM_COL32(255, 255, 255, 200), 0, 1.5f);
            continue;
        }

        bool isTower = reg.all_of<MeshComponent>(entity) &&
                       !reg.all_of<CharacterComponent>(entity) &&
                       !reg.all_of<SelectableComponent>(entity);

        if (team.team == Team::PLAYER) {
            color = m_config.allyColor;
        } else if (team.team == Team::ENEMY) {
            color = m_config.enemyColor;
        } else {
            color = m_config.neutralColor;
        }

        // Minions are small, characters are medium
        if (reg.all_of<CharacterComponent>(entity)) {
            radius = 3.5f;
        } else if (reg.all_of<UnitComponent>(entity)) {
            radius = 2.5f;
        } else {
            radius = 2.0f;
        }

        dl->AddCircleFilled(imPos, radius, color);
    }
}

void Minimap::drawCameraFrustum(ImDrawList* dl, const IsometricCamera& cam,
                                float aspect) const {
    glm::mat4 view = cam.getViewMatrix();
    glm::mat4 proj = cam.getProjectionMatrix(aspect);

    // Unproject a screen NDC point to the Y=0 ground plane
    auto unprojectToGround = [&](float ndcX, float ndcY) -> glm::vec2 {
        glm::vec4 rayClip(ndcX, ndcY, -1.0f, 1.0f);
        glm::vec4 rayEye = glm::inverse(proj) * rayClip;
        rayEye = glm::vec4(rayEye.x, rayEye.y, -1.0f, 0.0f);
        glm::vec3 rayDir = glm::normalize(
            glm::vec3(glm::inverse(view) * rayEye));
        glm::vec3 origin = cam.getPosition();
        if (std::abs(rayDir.y) < 1e-5f) return worldToMinimap(origin);
        float t = -origin.y / rayDir.y;
        glm::vec3 hit = origin + t * rayDir;
        return worldToMinimap(hit);
    };

    // Four NDC corners (Vulkan Y convention handled by projection matrix)
    glm::vec2 c0 = unprojectToGround(-1.0f, -1.0f);
    glm::vec2 c1 = unprojectToGround( 1.0f, -1.0f);
    glm::vec2 c2 = unprojectToGround( 1.0f,  1.0f);
    glm::vec2 c3 = unprojectToGround(-1.0f,  1.0f);

    ImVec2 corners[4] = {
        {c0.x, c0.y}, {c1.x, c1.y}, {c2.x, c2.y}, {c3.x, c3.y}
    };
    dl->AddPolyline(corners, 4, m_config.frustumColor, ImDrawFlags_Closed, 1.5f);
}

void Minimap::drawBorder(ImDrawList* dl) const {
    ImVec2 tl(m_origin.x, m_origin.y);
    ImVec2 br(m_origin.x + m_size.x, m_origin.y + m_size.y);

    // Inner dark border
    dl->AddRect(tl, br, m_config.borderColorInner, m_config.cornerRadius,
                ImDrawFlags_None, m_config.borderWidth + 1.0f);
    // Outer gold border
    ImVec2 otl(tl.x - 1.0f, tl.y - 1.0f);
    ImVec2 obr(br.x + 1.0f, br.y + 1.0f);
    dl->AddRect(otl, obr, m_config.borderColor, m_config.cornerRadius + 1.0f,
                ImDrawFlags_None, m_config.borderWidth);
}

// ── Input handling ──────────────────────────────────────────────────────────

void Minimap::handleInput(MinimapUpdateContext& ctx) {
    glm::vec2 mousePos = ctx.input.getMousePos();
    bool mouseOnMinimap = containsScreenPos(mousePos.x, mousePos.y);

    // Edge-detect clicks from raw button state (avoids consuming wasRightClicked etc.)
    bool leftDown   = ctx.input.isLeftMouseDown();
    bool rightDown  = ctx.input.isRightMouseDown();
    bool leftClick  = leftDown && !m_prevLeftDown;
    bool rightClick = rightDown && !m_prevRightDown;
    m_prevLeftDown  = leftDown;
    m_prevRightDown = rightDown;

    // Left-click on minimap: move camera to that position
    if (leftClick && mouseOnMinimap) {
        glm::vec3 worldTarget = minimapToWorld(mousePos);
        ctx.actions.moveCameraTo = true;
        ctx.actions.cameraTarget = worldTarget;
        ctx.actions.detachCamera = true;
        m_isDraggingCamera = true;
        m_consumedInput = true;
    }

    // Left-drag: continuous camera panning (even if mouse leaves minimap)
    if (m_isDraggingCamera && leftDown) {
        glm::vec2 clamped(
            std::clamp(mousePos.x, m_origin.x, m_origin.x + m_size.x),
            std::clamp(mousePos.y, m_origin.y, m_origin.y + m_size.y));
        glm::vec3 worldTarget = minimapToWorld(clamped);
        ctx.actions.moveCameraTo = true;
        ctx.actions.cameraTarget = worldTarget;
        m_consumedInput = true;
    }

    // Release left button: stop dragging
    if (m_isDraggingCamera && !leftDown) {
        m_isDraggingCamera = false;
    }

    // Right-click on minimap: issue move command
    if (rightClick && mouseOnMinimap) {
        glm::vec3 worldTarget = minimapToWorld(mousePos);
        ctx.actions.movePlayerTo = true;
        ctx.actions.moveTarget = worldTarget;
        m_consumedInput = true;
    }
}

} // namespace glory
