#pragma once

#include <string>
#include <array>

#include <glm/glm.hpp>

namespace glory {

struct HeroDefinition {
    std::string heroId;
    std::string name;
    std::string modelPath;

    // Base stats at level 1
    float baseHP            = 600.0f;
    float baseMP            = 500.0f;
    float baseMoveSpeed     = 8.0f;
    float baseAttackDamage  = 60.0f;
    float baseAttackRange   = 3.0f;
    float baseAttackSpeed   = 1.0f;
    float baseArmor         = 40.0f;
    float baseMagicResist   = 32.0f;
    float baseAbilityPower  = 0.0f;

    // Per-level scaling
    float hpPerLevel            = 90.0f;
    float mpPerLevel            = 40.0f;
    float damagePerLevel        = 3.5f;
    float armorPerLevel         = 4.0f;
    float magicResistPerLevel   = 1.5f;
    float abilityPowerPerLevel  = 0.0f;

    // Attack properties
    bool  isRanged          = false;
    float projectileSpeed   = 0.0f;
    std::string projectileVfx;

    // Abilities: Q, W, E, R
    std::array<std::string, 4> abilityIds;
    std::string summonerAbilityId;

    // UI placeholder color
    glm::vec4 portraitColor{0.5f, 0.5f, 0.5f, 1.0f};
};

struct HeroComponent {
    const HeroDefinition* definition = nullptr;
    int level = 1;

    float getMaxHP() const {
        if (!definition) return 600.0f;
        return definition->baseHP + definition->hpPerLevel * (level - 1);
    }
    float getMaxMP() const {
        if (!definition) return 500.0f;
        return definition->baseMP + definition->mpPerLevel * (level - 1);
    }
    float getAttackDamage() const {
        if (!definition) return 60.0f;
        return definition->baseAttackDamage + definition->damagePerLevel * (level - 1);
    }
    float getArmor() const {
        if (!definition) return 40.0f;
        return definition->baseArmor + definition->armorPerLevel * (level - 1);
    }
    float getMoveSpeed() const {
        if (!definition) return 8.0f;
        return definition->baseMoveSpeed;
    }
    float getMagicResist() const {
        if (!definition) return 32.0f;
        return definition->baseMagicResist + definition->magicResistPerLevel * (level - 1);
    }
    float getAbilityPower() const {
        if (!definition) return 0.0f;
        return definition->baseAbilityPower + definition->abilityPowerPerLevel * (level - 1);
    }
};

} // namespace glory
