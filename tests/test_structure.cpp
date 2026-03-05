#include "structure/StructureSystem.h"
#include "structure/StructureComponents.h"
#include "scene/Components.h"
#include <iostream>

using namespace glory;

#define TEST(name) std::cout << "RUNNING: " << name << "..." << std::endl
#define PASS() std::cout << "  -> PASS" << std::endl
#define ASSERT_TRUE(cond) if (!(cond)) { std::cerr << "  -> FAIL: " << #cond << " at line " << __LINE__ << std::endl; exit(1); }

void test_basic_structure() {
    TEST("Basic Structure System Init");
    entt::registry reg;
    StructureSystem sys;
    StructureConfig config;
    MapData mapData;
    mapData.teams[0].towers.push_back({glm::vec3(0), std::nullopt, LaneType::Mid, TowerTier::Outer, 15.0f, 3500.0f, 150.0f});
    
    sys.init(config, mapData, reg);
    ASSERT_TRUE(reg.view<TowerTag>().size() == 1);
    PASS();
}

int main() {
    std::cout << "=== Structure System Tests ===" << std::endl;
    test_basic_structure();
    return 0;
}
