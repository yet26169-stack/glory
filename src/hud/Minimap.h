#pragma once

// ── League of Legends-style Minimap ──────────────────────────────────────────
// Renders a 2D top-down minimap overlay using ImGui draw lists.
// Supports: entity icons, camera frustum, left-click camera pan,
//           left-drag continuous pan, right-click move command.

#include "map/MapTypes.h"
#include "terrain/IsometricCamera.h"
#include "input/InputManager.h"

#include <entt.hpp>
#include <glm/glm.hpp>
#include <imgui.h>

namespace glory {

// Data passed from Renderer each frame for the minimap to read/write.
struct MinimapUpdateContext {
    const entt::registry& registry;
    entt::entity playerEntity;
    const IsometricCamera& camera;
    const MapData& mapData;
    float windowWidth;
    float windowHeight;
    float aspect;
    InputManager& input;

    // Output actions requested by the minimap this frame.
    struct Actions {
        bool moveCameraTo  = false;
        glm::vec3 cameraTarget{0.0f};
        bool movePlayerTo  = false;
        glm::vec3 moveTarget{0.0f};
        bool detachCamera  = false;
    } actions;
};

class Minimap {
public:
    struct Config {
        float size          = 220.0f;
        float margin        = 10.0f;
        float borderWidth   = 2.0f;
        float cornerRadius  = 6.0f;
        glm::vec2 worldMin  = {0.0f, 0.0f};
        glm::vec2 worldMax  = {200.0f, 200.0f};

        // Colors
        ImU32 bgColor         = IM_COL32(20, 25, 30, 200);
        ImU32 borderColor     = IM_COL32(100, 90, 70, 255);
        ImU32 borderColorInner= IM_COL32(40, 38, 35, 255);
        ImU32 allyColor       = IM_COL32(50, 140, 255, 255);
        ImU32 enemyColor      = IM_COL32(230, 50, 50, 255);
        ImU32 neutralColor    = IM_COL32(200, 200, 100, 255);
        ImU32 playerColor     = IM_COL32(80, 255, 120, 255);
        ImU32 frustumColor    = IM_COL32(255, 255, 255, 160);
        ImU32 towerAllyColor  = IM_COL32(80, 180, 255, 255);
        ImU32 towerEnemyColor = IM_COL32(255, 80, 80, 255);
        ImU32 laneColor       = IM_COL32(55, 55, 45, 120);
        ImU32 baseAllyColor   = IM_COL32(30, 60, 120, 100);
        ImU32 baseEnemyColor  = IM_COL32(120, 30, 30, 100);
    };

    Minimap() = default;

    void update(MinimapUpdateContext& ctx);

    bool containsScreenPos(float screenX, float screenY) const;
    bool isInteracting() const { return m_consumedInput; }

private:
    Config m_config;

    // Screen-space bounds (recomputed each frame)
    glm::vec2 m_origin{0.0f};
    glm::vec2 m_size{0.0f};

    // Interaction state
    bool m_isDraggingCamera  = false;
    bool m_consumedInput     = false;
    bool m_prevLeftDown      = false;   // for click edge detection
    bool m_prevRightDown     = false;

    // Coordinate conversions
    glm::vec2 worldToMinimap(const glm::vec3& worldPos) const;
    glm::vec3 minimapToWorld(const glm::vec2& screenPos) const;

    // Drawing
    void drawBackground(ImDrawList* dl, const MapData& map) const;
    void drawLanes(ImDrawList* dl, const MapData& map) const;
    void drawStructures(ImDrawList* dl, const MapData& map) const;
    void drawEntities(ImDrawList* dl, const entt::registry& reg,
                      entt::entity player, float gameTime) const;
    void drawCameraFrustum(ImDrawList* dl, const IsometricCamera& cam,
                           float aspect) const;
    void drawBorder(ImDrawList* dl) const;

    // Input
    void handleInput(MinimapUpdateContext& ctx);
};

} // namespace glory
