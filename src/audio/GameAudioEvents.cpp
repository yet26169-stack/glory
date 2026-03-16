#include "audio/GameAudioEvents.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>

using json = nlohmann::json;

namespace glory {

GameAudioEvents::GameAudioEvents(AudioResourceManager& resources)
    : m_resources(resources) {}

void GameAudioEvents::loadConfig(const std::string& configPath) {
    if (!std::filesystem::exists(configPath)) {
        spdlog::warn("GameAudioEvents: config '{}' not found – all audio events disabled",
                     configPath);
        return;
    }

    std::ifstream file(configPath);
    if (!file.is_open()) {
        spdlog::warn("GameAudioEvents: could not open '{}'", configPath);
        return;
    }

    json root;
    try {
        file >> root;
    } catch (const json::parse_error& e) {
        spdlog::warn("GameAudioEvents: JSON parse error in '{}': {}", configPath, e.what());
        return;
    }

    if (!root.contains("events") || !root["events"].is_object()) {
        spdlog::warn("GameAudioEvents: config missing 'events' object");
        return;
    }

    for (auto& [key, value] : root["events"].items()) {
        std::string soundPath = value.get<std::string>();
        SoundId id = m_resources.loadSound(soundPath, SoundGroup::SFX);
        m_eventSounds[key] = id;
        spdlog::debug("GameAudioEvents: mapped '{}' -> '{}'  (id={})", key, soundPath, id);
    }

    spdlog::info("GameAudioEvents: loaded {} event mappings from '{}'",
                 m_eventSounds.size(), configPath);
}

// ── helpers ────────────────────────────────────────────────────────────────

void GameAudioEvents::playEvent3D(const std::string& key,
                                   const glm::vec3& position,
                                   float volume) {
    auto it = m_eventSounds.find(key);
    if (it == m_eventSounds.end()) return;
    m_resources.play3D(it->second, position, volume);
}

void GameAudioEvents::playEvent2D(const std::string& key, float volume) {
    auto it = m_eventSounds.find(key);
    if (it == m_eventSounds.end()) return;
    m_resources.play2D(it->second, volume);
}

// ── ability events ─────────────────────────────────────────────────────────

void GameAudioEvents::onAbilityCast(const std::string& abilityId,
                                     const glm::vec3& position) {
    std::string key = "ability_cast_" + abilityId;
    if (m_eventSounds.count(key) == 0) key = "ability_cast_default";
    playEvent3D(key, position);
}

void GameAudioEvents::onAbilityHit(const std::string& abilityId,
                                    const glm::vec3& position) {
    std::string key = "ability_hit_" + abilityId;
    if (m_eventSounds.count(key) == 0) key = "ability_hit_default";
    playEvent3D(key, position);
}

void GameAudioEvents::onProjectileFire(const glm::vec3& position) {
    playEvent3D("projectile_fire", position);
}

void GameAudioEvents::onProjectileImpact(const glm::vec3& position) {
    playEvent3D("projectile_impact", position);
}

// ── combat events ──────────────────────────────────────────────────────────

void GameAudioEvents::onAutoAttack(const glm::vec3& position, bool ranged) {
    playEvent3D(ranged ? "auto_attack_ranged" : "auto_attack_melee", position);
}

void GameAudioEvents::onDeath(const glm::vec3& position) {
    playEvent3D("death", position);
}

void GameAudioEvents::onLevelUp(const glm::vec3& position) {
    playEvent3D("level_up", position);
}

void GameAudioEvents::onGoldGain(const glm::vec3& position) {
    playEvent3D("gold_gain", position);
}

// ── structure events ───────────────────────────────────────────────────────

void GameAudioEvents::onTowerAttack(const glm::vec3& position) {
    playEvent3D("tower_attack", position);
}

void GameAudioEvents::onTowerDestroy(const glm::vec3& position) {
    playEvent3D("tower_destroy", position);
}

void GameAudioEvents::onInhibitorDestroy(const glm::vec3& position) {
    playEvent3D("inhibitor_destroy", position);
}

// ── UI events ──────────────────────────────────────────────────────────────

void GameAudioEvents::onButtonClick() {
    playEvent2D("button_click");
}

void GameAudioEvents::onPing(const glm::vec3& position) {
    playEvent3D("ping", position);
}

void GameAudioEvents::onChatMessage() {
    playEvent2D("chat_message");
}

// ── ambient ────────────────────────────────────────────────────────────────

void GameAudioEvents::startAmbientLoop() {
    auto it = m_eventSounds.find("ambient_loop");
    if (it == m_eventSounds.end()) return;
    m_ambientHandle = m_resources.playMusic(it->second, 0.4f);
}

void GameAudioEvents::stopAmbientLoop() {
    m_ambientHandle.stop();
    m_ambientHandle = {};
}

} // namespace glory
