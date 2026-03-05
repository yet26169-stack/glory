#pragma once
#include <string>
#include <array>

namespace glory {

struct TowerTierConfig {
    float maxHP          = 3500.0f;
    float attackDamage   = 150.0f;
    float attackRange    = 15.0f;
    float attackCooldown = 0.833f;
    float armor          = 40.0f;
    float magicResist    = 40.0f;
    float projectileSpeed = 30.0f;
    float backdoorReduction = 0.66f;
    // Plates (outer only)
    int   plateCount     = 0;
    float goldPerPlate   = 160.0f;
    float armorPerPlate  = 0.40f;
    float plateFalloffTime = 840.0f;
};

struct InhibitorConfig {
    float maxHP       = 4000.0f;
    float armor       = 20.0f;
    float magicResist = 20.0f;
    float respawnTime = 300.0f;
};

struct NexusConfig {
    float maxHP       = 5500.0f;
    float armor       = 0.0f;
    float magicResist = 0.0f;
    float hpRegen     = 5.0f;
    float outOfCombatThreshold = 8.0f;
};

struct StructureConfig {
    TowerTierConfig towers[4]; // [Outer, Inner, Inhibitor, Nexus]
    float damageRampRate = 0.40f;
    float damageRampMax  = 1.20f;
    InhibitorConfig inhibitor;
    NexusConfig nexus;
};

namespace StructureConfigLoader {
    StructureConfig Load(const std::string &configDir);
}

} // namespace glory
