#pragma once
#include "map/MapTypes.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace glory {

struct MonsterMobDef {
    std::string name;
    float hp = 100.0f;
    float ad = 10.0f;
    float armor = 0.0f;
    float mr = 0.0f;
    float range = 2.0f;
    float cooldown = 1.0f;
    bool isBig = false;
};

struct MonsterBuffDef {
    std::string id;
    float duration = 0.0f; // 0 = permanent
};

struct CampDef {
    CampType type;
    std::vector<MonsterMobDef> mobs;
    MonsterBuffDef buff;
    float xpReward = 100.0f;
    float goldReward = 50.0f;
    bool isEpic = false;
    std::string passiveBehavior; // "flee" for scuttler
    int maxKills = 0; // 0 = unlimited
};

struct JungleConfig {
    std::unordered_map<CampType, CampDef> camps;
    float hpScalePerMinute = 0.03f;
    float adScalePerMinute = 0.02f;
};

namespace JungleConfigLoader {
    JungleConfig Load(const std::string &configDir);
}

} // namespace glory
