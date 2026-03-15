#include "nav/NavMeshBuilder.h"

#include <spdlog/spdlog.h>
#include <fstream>

namespace glory {

NavMeshData NavMeshBuilder::build(const std::vector<float>& vertices,
                                  const std::vector<int>& triangles,
                                  const NavMeshConfig& config) {
    const auto vertCount = vertices.size() / 3;
    const auto triCount  = triangles.size() / 3;

    spdlog::info("[NavMeshBuilder] build() called — {} vertices, {} triangles", vertCount, triCount);
    spdlog::info("[NavMeshBuilder]   cellSize={:.2f}  cellHeight={:.2f}  agentHeight={:.1f}  agentRadius={:.1f}",
                 config.cellSize, config.cellHeight, config.agentHeight, config.agentRadius);
    spdlog::info("[NavMeshBuilder]   maxClimb={:.1f}  maxSlope={:.0f}  regionMin={:.0f}  regionMerge={:.0f}",
                 config.agentMaxClimb, config.agentMaxSlope, config.regionMinSize, config.regionMergeSize);
    spdlog::warn("[NavMeshBuilder] Recast/Detour not linked — returning empty NavMeshData");

    // TODO: Replace with actual Recast/Detour pipeline:
    //   1. rcCreateHeightfield / rcRasterizeTriangles
    //   2. rcFilterWalkable* passes
    //   3. rcBuildCompactHeightfield / rcBuildDistanceField / rcBuildRegions
    //   4. rcBuildContours / rcBuildPolyMesh / rcBuildPolyMeshDetail
    //   5. dtCreateNavMeshData / dtNavMesh::init

    NavMeshData result;
    result.valid = false;
    return result;
}

NavMeshData NavMeshBuilder::buildFromGLB(const std::string& glbPath,
                                          const NavMeshConfig& config) {
    spdlog::info("[NavMeshBuilder] buildFromGLB() — path=\"{}\"", glbPath);
    spdlog::warn("[NavMeshBuilder] GLB triangle extraction not yet implemented");

    // TODO: Use GLBLoader to extract vertex/index data from the map mesh,
    //       then delegate to build().
    return build({}, {}, config);
}

bool NavMeshBuilder::save(const NavMeshData& data, const std::string& path) {
    if (!data.valid || data.serializedData.empty()) {
        spdlog::warn("[NavMeshBuilder] save() — nothing to write (data not valid)");
        return false;
    }

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        spdlog::error("[NavMeshBuilder] save() — failed to open \"{}\"", path);
        return false;
    }

    const uint64_t size = data.serializedData.size();
    out.write(reinterpret_cast<const char*>(&size), sizeof(size));
    out.write(reinterpret_cast<const char*>(data.serializedData.data()),
              static_cast<std::streamsize>(size));

    spdlog::info("[NavMeshBuilder] save() — wrote {} bytes to \"{}\"", size, path);
    return out.good();
}

NavMeshData NavMeshBuilder::load(const std::string& path) {
    NavMeshData result;

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        spdlog::error("[NavMeshBuilder] load() — failed to open \"{}\"", path);
        return result;
    }

    uint64_t size = 0;
    in.read(reinterpret_cast<char*>(&size), sizeof(size));
    if (!in.good() || size == 0) {
        spdlog::error("[NavMeshBuilder] load() — invalid header in \"{}\"", path);
        return result;
    }

    result.serializedData.resize(size);
    in.read(reinterpret_cast<char*>(result.serializedData.data()),
            static_cast<std::streamsize>(size));

    if (in.good()) {
        result.valid = true;
        spdlog::info("[NavMeshBuilder] load() — read {} bytes from \"{}\"", size, path);
    } else {
        spdlog::error("[NavMeshBuilder] load() — read error in \"{}\"", path);
    }
    return result;
}

} // namespace glory
