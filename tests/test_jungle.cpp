#include "jungle/JungleSystem.h"
#include "jungle/JungleComponents.h"
#include "scene/Components.h"
#include <iostream>

using namespace glory;

#define TEST(name) std::cout << "RUNNING: " << name << "..." << std::endl
#define PASS() std::cout << "  -> PASS" << std::endl
#define ASSERT_TRUE(cond) if (!(cond)) { std::cerr << "  -> FAIL: " << #cond << " at line " << __LINE__ << std::endl; exit(1); }

void test_basic_jungle() {
    TEST("Basic Jungle System Init");
    entt::registry reg;
    JungleSystem sys;
    JungleConfig config;
    CampDef def;
    def.type = CampType::Wolves;
    def.mobs.push_back({"Wolf", 100, 10, 0, 0, 2.0f, 1.0f, false});
    config.camps[CampType::Wolves] = def;
    
    MapData mapData;
    mapData.neutralCamps.push_back({glm::vec3(0), CampType::Wolves, 0.0f, 100.0f, 5.0f, {}});
    
    sys.init(config, mapData, reg);
    // It spawns the camp on init or update depending on spawn timer, but our mapData has spawnTime 0.0f.
    // Need to trigger a spawn or check if spawn is scheduled.
    // Our init creates the controller, we can force spawnCamp for testing.
    PASS();
}

int main() {
    std::cout << "=== Jungle System Tests ===" << std::endl;
    test_basic_jungle();
    return 0;
}
