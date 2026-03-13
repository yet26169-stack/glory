#pragma once

#include "audio/AudioResourceManager.h"

#include <glm/glm.hpp>
#include <string>
#include <unordered_map>

namespace glory {

// Maps gameplay events to sound effects.
// Event-to-sound mappings are loaded from a JSON config file.
class GameAudioEvents {
public:
    explicit GameAudioEvents(AudioResourceManager& resources);

    void loadConfig(const std::string& configPath);

    // Ability events
    void onAbilityCast(const std::string& abilityId, const glm::vec3& position);
    void onAbilityHit(const std::string& abilityId, const glm::vec3& position);
    void onProjectileFire(const glm::vec3& position);
    void onProjectileImpact(const glm::vec3& position);

    // Combat events
    void onAutoAttack(const glm::vec3& position, bool ranged);
    void onDeath(const glm::vec3& position);
    void onLevelUp(const glm::vec3& position);
    void onGoldGain(const glm::vec3& position);

    // Structure events
    void onTowerAttack(const glm::vec3& position);
    void onTowerDestroy(const glm::vec3& position);
    void onInhibitorDestroy(const glm::vec3& position);

    // UI events
    void onButtonClick();
    void onPing(const glm::vec3& position);
    void onChatMessage();

    // Ambient
    void startAmbientLoop();
    void stopAmbientLoop();

private:
    AudioResourceManager& m_resources;

    // event key -> SoundId (loaded from JSON)
    std::unordered_map<std::string, SoundId> m_eventSounds;

    void playEvent3D(const std::string& key, const glm::vec3& position,
                     float volume = 1.0f);
    void playEvent2D(const std::string& key, float volume = 1.0f);
};

} // namespace glory
