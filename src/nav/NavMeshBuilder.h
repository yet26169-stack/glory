#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <cstdint>
#include <memory>

namespace glory {

// Configuration for NavMesh generation (Recast parameters)
struct NavMeshConfig {
    float cellSize       = 0.3f;    // XZ cell size (smaller = more detail)
    float cellHeight     = 0.2f;    // Y cell height
    float agentHeight    = 2.0f;    // character height
    float agentRadius    = 0.5f;    // character radius
    float agentMaxClimb  = 0.9f;    // max step height
    float agentMaxSlope  = 45.0f;   // max walkable slope (degrees)
    float regionMinSize  = 8.0f;    // minimum region area
    float regionMergeSize = 20.0f;  // merge regions smaller than this
    float edgeMaxLen     = 12.0f;   // max edge length
    float edgeMaxError   = 1.3f;    // max edge deviation
    int   vertsPerPoly   = 6;       // max vertices per polygon
    float detailSampleDist  = 6.0f;
    float detailSampleMaxError = 1.0f;
};

// Opaque handle to the built navmesh data
struct NavMeshData {
    std::vector<uint8_t> serializedData;  // Recast/Detour serialized navmesh
    bool valid = false;
};

class NavMeshBuilder {
public:
    // Build navmesh from triangle soup (extracted from map GLB)
    NavMeshData build(const std::vector<float>& vertices,     // flat xyz
                      const std::vector<int>& triangles,       // flat index triplets
                      const NavMeshConfig& config = {});

    // Build from AABB bounds + walkable triangles from a GLB file
    NavMeshData buildFromGLB(const std::string& glbPath,
                              const NavMeshConfig& config = {});

    // Serialize navmesh to binary file for fast reload
    static bool save(const NavMeshData& data, const std::string& path);

    // Load previously serialized navmesh
    static NavMeshData load(const std::string& path);
};

} // namespace glory
