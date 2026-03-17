#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <optional>
#include <vector>

#include "scene/Components.h"
#include "ability/AbilityTypes.h"
#include "animation/AnimationClip.h"
#include "animation/Skeleton.h"
#include "renderer/Buffer.h"   // Vertex, SkinVertex

namespace glory {

class AbilitySystem;
class CombatSystem;
class GpuCollisionSystem;
class Scene;
class DebugRenderer;
class GroundDecalRenderer;

struct ClickAnim {
    glm::vec3 position{};
    float     lifetime = 0.0f;
    float     maxLife  = 0.25f;
};

// Input state snapshot for one simulation tick
struct GameplayInput {
    // Mouse
    glm::vec2 mousePos{};
    glm::vec2 lastClickPos{};
    bool rightClicked   = false;
    bool leftMouseDown  = false;
    bool shiftHeld      = false;
    bool minimapHovered = false;

    // Ability/combat keys
    bool qPressed = false, wPressed = false, ePressed = false, rPressed = false;
    bool aPressed = false, sPressed = false, dPressed = false;
    bool xKeyDown = false;
    bool leftClicked = false;  // single-frame left mouse click event
    bool ctrlHeld    = false;  // Ctrl key held (for ability level-up)
    bool escPressed  = false;  // cancel targeting mode
    bool fKeyPressed = false;  // ward placement

    // Camera matrices for screen↔world conversion
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    float screenW = 0, screenH = 0;

    // Pre-computed by renderer
    entt::entity hoveredEntity = entt::null;
};

struct GameplayOutput {
    std::optional<ClickAnim> clickAnim;
    std::optional<glm::vec3> cameraFollowTarget;
};

struct MinionSpawnConfig {
    uint32_t meshIndex    = 0;
    uint32_t texIndex     = 0;
    uint32_t flatNormIndex = 0;
    Skeleton skeleton;
    std::vector<std::vector<SkinVertex>> skinVertices;
    std::vector<std::vector<Vertex>>     bindPoseVertices;
    std::vector<AnimationClip>           clips;
};

class GameplaySystem {
public:
    void init(Scene& scene, AbilitySystem* abilities, CombatSystem* combat,
              GpuCollisionSystem* gpuCollision, DebugRenderer* debugRenderer);

    void setGroundDecals(GroundDecalRenderer* decals) { m_groundDecals = decals; }
    void setPlayerEntity(entt::entity e) { m_playerEntity = e; }
    entt::entity getPlayerEntity() const { return m_playerEntity; }

    const MinionSpawnConfig& getSpawnConfig() const { return m_spawnConfig; }
    void setSpawnConfig(MinionSpawnConfig config) { m_spawnConfig = std::move(config); }

    void update(float dt, const GameplayInput& input, GameplayOutput& output);

private:
    glm::vec3 screenToWorld(float sx, float sy, const GameplayInput& input) const;
    glm::vec2 worldToScreen(const glm::vec3& worldPos, const GameplayInput& input) const;

    void processRightClick(float dt, const GameplayInput& input, GameplayOutput& output);
    void processAbilityKeys(const GameplayInput& input);
    void processTargetingMode(const GameplayInput& input);
    void processCombatKeys(const GameplayInput& input);
    void processSpawning(float dt, const GameplayInput& input);
    void processSelection(const GameplayInput& input, GameplayOutput& output);
    void updateMinionMovement(float dt);
    void updatePlayerMovement(float dt, GameplayOutput& output);
    void updateAnimations(float dt);

    Scene*            m_scene         = nullptr;
    AbilitySystem*    m_abilitySystem = nullptr;
    CombatSystem*     m_combatSystem  = nullptr;
    GpuCollisionSystem* m_gpuCollision = nullptr;
    DebugRenderer*    m_debugRenderer = nullptr;
    entt::entity      m_playerEntity  = entt::null;

    SelectionState    m_selection;
    float             m_spawnTimer = 0.0f;
    MinionSpawnConfig m_spawnConfig;

    // ── Targeting mode ──────────────────────────────────────────────────
    GroundDecalRenderer* m_groundDecals = nullptr;
    bool        m_inTargetingMode = false;
    AbilitySlot m_targetingSlot   = AbilitySlot::Q;
    uint32_t    m_targetingDecalHandle = 0;  // ground decal for indicator
    uint32_t    m_rangeDecalHandle     = 0;  // range ring indicator
};

} // namespace glory
