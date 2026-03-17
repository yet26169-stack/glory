#include "combat/HeroRegistry.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace glory {

void HeroRegistry::loadFromDirectory(const std::string& dirPath) {
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
        spdlog::warn("HeroRegistry: directory not found: {}", dirPath);
        return;
    }

    for (const auto& entry : fs::directory_iterator(dirPath)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            loadFromFile(entry.path().string());
        }
    }

    spdlog::info("HeroRegistry: loaded {} hero definitions", m_heroes.size());
}

void HeroRegistry::loadFromFile(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        spdlog::warn("HeroRegistry: could not open {}", filePath);
        return;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(file);

        HeroDefinition def;
        def.heroId              = j.value("heroId", "");
        def.name                = j.value("name", "Unknown");
        def.modelPath           = j.value("modelPath", "models/scientist/scientist.glb");
        def.baseHP              = j.value("baseHP", 600.0f);
        def.baseMP              = j.value("baseMP", 500.0f);
        def.baseMoveSpeed       = j.value("baseMoveSpeed", 8.0f);
        def.baseAttackDamage    = j.value("baseAttackDamage", 60.0f);
        def.baseAttackRange     = j.value("baseAttackRange", 3.0f);
        def.baseAttackSpeed     = j.value("baseAttackSpeed", 1.0f);
        def.baseArmor           = j.value("baseArmor", 40.0f);
        def.baseMagicResist     = j.value("baseMagicResist", 32.0f);
        def.baseAbilityPower    = j.value("baseAbilityPower", 0.0f);
        def.hpPerLevel          = j.value("hpPerLevel", 90.0f);
        def.mpPerLevel          = j.value("mpPerLevel", 40.0f);
        def.damagePerLevel      = j.value("damagePerLevel", 3.5f);
        def.armorPerLevel       = j.value("armorPerLevel", 4.0f);
        def.magicResistPerLevel = j.value("magicResistPerLevel", 1.5f);
        def.abilityPowerPerLevel = j.value("abilityPowerPerLevel", 0.0f);
        def.isRanged            = j.value("isRanged", false);
        def.projectileSpeed     = j.value("projectileSpeed", 0.0f);
        def.projectileVfx       = j.value("projectileVfx", "");

        if (j.contains("abilities")) {
            const auto& ab = j["abilities"];
            def.abilityIds[0] = ab.value("Q", "");
            def.abilityIds[1] = ab.value("W", "");
            def.abilityIds[2] = ab.value("E", "");
            def.abilityIds[3] = ab.value("R", "");
        }

        def.summonerAbilityId = j.value("summoner", "");

        if (j.contains("portraitColor") && j["portraitColor"].is_array()
            && j["portraitColor"].size() >= 4) {
            def.portraitColor = glm::vec4(
                j["portraitColor"][0].get<float>(),
                j["portraitColor"][1].get<float>(),
                j["portraitColor"][2].get<float>(),
                j["portraitColor"][3].get<float>());
        }

        if (def.heroId.empty()) {
            spdlog::warn("HeroRegistry: skipping hero with empty heroId in {}", filePath);
            return;
        }

        m_index[def.heroId] = m_heroes.size();
        m_heroes.push_back(std::move(def));
        spdlog::info("HeroRegistry: loaded hero '{}'", m_heroes.back().heroId);

    } catch (const std::exception& e) {
        spdlog::warn("HeroRegistry: failed to parse {}: {}", filePath, e.what());
    }
}

const HeroDefinition* HeroRegistry::find(const std::string& heroId) const {
    auto it = m_index.find(heroId);
    if (it != m_index.end()) {
        return &m_heroes[it->second];
    }
    return nullptr;
}

} // namespace glory
