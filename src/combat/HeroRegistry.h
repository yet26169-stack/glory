#pragma once

#include "combat/HeroDefinition.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace glory {

class HeroRegistry {
public:
    void loadFromDirectory(const std::string& dirPath);
    void loadFromFile(const std::string& filePath);

    const HeroDefinition* find(const std::string& heroId) const;

    const std::vector<HeroDefinition>& all() const { return m_heroes; }
    size_t count() const { return m_heroes.size(); }

private:
    std::vector<HeroDefinition> m_heroes;
    std::unordered_map<std::string, size_t> m_index;
};

} // namespace glory
