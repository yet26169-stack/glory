#include "JungleConfig.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <spdlog/spdlog.h>

namespace glory {
namespace JungleConfigLoader {

JungleConfig Load(const std::string &configDir) {
    JungleConfig config;
    std::string path = configDir + "/monster_config.json";
    std::ifstream file(path);

    if (!file.is_open()) {
        spdlog::warn("Could not open monster config at {}, using defaults.", path);
        return config;
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const std::exception& e) {
        spdlog::error("Error parsing monster config: {}", e.what());
        return config;
    }

    auto parseCampType = [](const std::string& name) -> CampType {
        if (name == "RedBuff") return CampType::RedBuff;
        if (name == "BlueBuff") return CampType::BlueBuff;
        if (name == "Wolves") return CampType::Wolves;
        if (name == "Raptors") return CampType::Raptors;
        if (name == "Gromp") return CampType::Gromp;
        if (name == "Krugs") return CampType::Krugs;
        if (name == "Scuttler") return CampType::Scuttler;
        if (name == "Dragon") return CampType::Dragon;
        if (name == "Baron") return CampType::Baron;
        if (name == "Herald") return CampType::Herald;
        return CampType::Wolves; // Default fallback
    };

    if (j.contains("camps")) {
        for (auto& el : j["camps"].items()) {
            CampDef def;
            def.type = parseCampType(el.key());
            auto& val = el.value();
            
            if (val.contains("xpReward")) def.xpReward = val["xpReward"];
            if (val.contains("goldReward")) def.goldReward = val["goldReward"];
            if (val.contains("isEpic")) def.isEpic = val["isEpic"];
            if (val.contains("passiveBehavior")) def.passiveBehavior = val["passiveBehavior"];
            if (val.contains("maxKills")) def.maxKills = val["maxKills"];
            
            if (val.contains("buff")) {
                def.buff.id = val["buff"].value("id", "");
                def.buff.duration = val["buff"].value("duration", 0.0f);
            }
            
            if (val.contains("mobs") && val["mobs"].is_array()) {
                for (auto& mj : val["mobs"]) {
                    MonsterMobDef mob;
                    if (mj.contains("name")) mob.name = mj["name"];
                    if (mj.contains("hp")) mob.hp = mj["hp"];
                    if (mj.contains("ad")) mob.ad = mj["ad"];
                    if (mj.contains("armor")) mob.armor = mj["armor"];
                    if (mj.contains("mr")) mob.mr = mj["mr"];
                    if (mj.contains("range")) mob.range = mj["range"];
                    if (mj.contains("cooldown")) mob.cooldown = mj["cooldown"];
                    if (mj.contains("big")) mob.isBig = mj["big"];
                    def.mobs.push_back(mob);
                }
            }
            config.camps[def.type] = def;
        }
    }

    if (j.contains("scaling")) {
        if (j["scaling"].contains("hpPerMinute")) config.hpScalePerMinute = j["scaling"]["hpPerMinute"];
        if (j["scaling"].contains("adPerMinute")) config.adScalePerMinute = j["scaling"]["adPerMinute"];
    }

    return config;
}

} // namespace JungleConfigLoader
} // namespace glory
