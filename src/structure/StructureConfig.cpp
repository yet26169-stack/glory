#include "StructureConfig.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <spdlog/spdlog.h>

namespace glory {
namespace StructureConfigLoader {

StructureConfig Load(const std::string &configDir) {
    StructureConfig config;
    std::string path = configDir + "/structure_config.json";
    std::ifstream file(path);

    if (!file.is_open()) {
        spdlog::warn("Could not open structure config at {}, using defaults.", path);
        return config;
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const std::exception& e) {
        spdlog::error("Error parsing structure config: {}", e.what());
        return config;
    }

    if (j.contains("towers")) {
        auto tj = j["towers"];
        
        auto loadTower = [&](const std::string& key, int index) {
            if (tj.contains(key)) {
                auto tj_tier = tj[key];
                if (tj_tier.contains("maxHP")) config.towers[index].maxHP = tj_tier["maxHP"];
                if (tj_tier.contains("attackDamage")) config.towers[index].attackDamage = tj_tier["attackDamage"];
                if (tj_tier.contains("attackRange")) config.towers[index].attackRange = tj_tier["attackRange"];
                if (tj_tier.contains("attackCooldown")) config.towers[index].attackCooldown = tj_tier["attackCooldown"];
                if (tj_tier.contains("armorBase")) config.towers[index].armor = tj_tier["armorBase"];
                if (tj_tier.contains("magicResistBase")) config.towers[index].magicResist = tj_tier["magicResistBase"];
                if (tj_tier.contains("projectileSpeed")) config.towers[index].projectileSpeed = tj_tier["projectileSpeed"];
                if (tj_tier.contains("backdoorReduction")) config.towers[index].backdoorReduction = tj_tier["backdoorReduction"];

                if (tj_tier.contains("plates")) {
                    auto pj = tj_tier["plates"];
                    if (pj.contains("count")) config.towers[index].plateCount = pj["count"];
                    if (pj.contains("goldPerPlate")) config.towers[index].goldPerPlate = pj["goldPerPlate"];
                    if (pj.contains("armorPerPlate")) config.towers[index].armorPerPlate = pj["armorPerPlate"];
                    if (pj.contains("falloffTime")) config.towers[index].plateFalloffTime = pj["falloffTime"];
                }
            }
        };

        loadTower("outer", 0);
        loadTower("inner", 1);
        loadTower("inhibitor_tower", 2);
        loadTower("nexus_tower", 3);

        if (tj.contains("damageRamp")) {
            auto dj = tj["damageRamp"];
            if (dj.contains("rate")) config.damageRampRate = dj["rate"];
            if (dj.contains("max")) config.damageRampMax = dj["max"];
        }
    }

    if (j.contains("inhibitors")) {
        auto ij = j["inhibitors"];
        if (ij.contains("maxHP")) config.inhibitor.maxHP = ij["maxHP"];
        if (ij.contains("armorBase")) config.inhibitor.armor = ij["armorBase"];
        if (ij.contains("magicResistBase")) config.inhibitor.magicResist = ij["magicResistBase"];
        if (ij.contains("respawnTime")) config.inhibitor.respawnTime = ij["respawnTime"];
    }

    if (j.contains("nexus")) {
        auto nj = j["nexus"];
        if (nj.contains("maxHP")) config.nexus.maxHP = nj["maxHP"];
        if (nj.contains("armorBase")) config.nexus.armor = nj["armorBase"];
        if (nj.contains("magicResistBase")) config.nexus.magicResist = nj["magicResistBase"];
        if (nj.contains("hpRegen")) config.nexus.hpRegen = nj["hpRegen"];
        if (nj.contains("outOfCombatThreshold")) config.nexus.outOfCombatThreshold = nj["outOfCombatThreshold"];
    }

    return config;
}

} // namespace StructureConfigLoader
} // namespace glory
