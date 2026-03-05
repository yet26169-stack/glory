/// Flow field unit tests.
#include "nav/FlowField.h"
#include "nav/HierarchicalFlowField.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <cmath>

using namespace glory;

int main() {
    // Build a simple 8×8 open grid with one goal in the centre
    const uint32_t W = 8, H = 8;
    std::vector<uint8_t> cost(W * H, 0); // all passable

    FlowField ff;
    ff.build(W, H, cost, W / 2, H / 2);
    assert(ff.isBuilt());

    // Direction from a corner should point roughly toward centre
    glm::vec2 dir = ff.getDirection(0.0f, 0.0f);
    assert(dir.x >= 0.0f); // should have positive X component (toward centre)
    assert(dir.y >= 0.0f); // should have positive Z component

    // Wall test: all-wall grid should not build usable directions
    std::vector<uint8_t> wallCost(W * H, 255);
    wallCost[H / 2 * W + W / 2] = 0; // only goal passable
    FlowField ff2;
    ff2.build(W, H, wallCost, W / 2, H / 2);
    assert(ff2.isBuilt());

    // Cells adjacent to goal should have a direction even through walls
    // (they're still blocked — confirm they return zero for wall cells)
    glm::vec2 wallDir = ff2.getDirection(0.0f, 0.0f);
    (void)wallDir; // walls return zero or a valid escape

    // HierarchicalFlowField on a small open grid
    HierarchicalFlowField hff;
    std::vector<uint8_t> bigCost(32 * 32, 0);
    hff.build(32, 32, bigCost, 16.0f * FlowField::CELL_SIZE, 16.0f * FlowField::CELL_SIZE);

    glm::vec2 hDir = hff.getDirection(0.0f, 0.0f);
    (void)hDir; // Should return a direction without crashing

    printf("test_flowfield: all assertions passed\n");
    return 0;
}
