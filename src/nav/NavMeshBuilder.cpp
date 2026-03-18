#include "nav/NavMeshBuilder.h"
#include "renderer/Model.h"

#include <spdlog/spdlog.h>
#include <fstream>
#include <algorithm>

#include <Recast.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>

namespace glory {

class GloryBuildContext : public rcContext {
public:
    GloryBuildContext() : rcContext(false) {}
protected:
    void doLog(const rcLogCategory category, const char* msg, const int) override {
        switch (category) {
            case RC_LOG_PROGRESS: spdlog::debug("[Recast] {}", msg); break;
            case RC_LOG_WARNING:  spdlog::warn("[Recast] {}", msg); break;
            case RC_LOG_ERROR:    spdlog::error("[Recast] {}", msg); break;
        }
    }
};

NavMeshData NavMeshBuilder::build(const std::vector<float>& vertices,
                                  const std::vector<int>& triangles,
                                  const NavMeshConfig& config) {
    NavMeshData result;
    result.valid = false;

    if (vertices.empty() || triangles.empty()) {
        spdlog::error("[NavMeshBuilder] Empty input geometry");
        return result;
    }

    GloryBuildContext ctx;

    const float* verts = vertices.data();
    const int nverts = static_cast<int>(vertices.size() / 3);
    const int* tris = triangles.data();
    const int ntris = static_cast<int>(triangles.size() / 3);

    // 1. Initialize build configuration
    rcConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.cs = config.cellSize;
    cfg.ch = config.cellHeight;
    cfg.walkableSlopeAngle = config.agentMaxSlope;
    cfg.walkableHeight = static_cast<int>(std::ceil(config.agentHeight / cfg.ch));
    cfg.walkableClimb = static_cast<int>(std::floor(config.agentMaxClimb / cfg.ch));
    cfg.walkableRadius = static_cast<int>(std::ceil(config.agentRadius / cfg.cs));
    cfg.maxEdgeLen = static_cast<int>(config.edgeMaxLen / config.cellSize);
    cfg.maxSimplificationError = config.edgeMaxError;
    cfg.minRegionArea = static_cast<int>(rcSqr(config.regionMinSize));
    cfg.mergeRegionArea = static_cast<int>(rcSqr(config.regionMergeSize));
    cfg.maxVertsPerPoly = config.vertsPerPoly;
    cfg.detailSampleDist = config.detailSampleDist < 0.9f ? 0 : config.cellSize * config.detailSampleDist;
    cfg.detailSampleMaxError = config.cellHeight * config.detailSampleMaxError;

    // Set AABB
    float bmin[3], bmax[3];
    rcCalcBounds(verts, nverts, bmin, bmax);
    rcVcopy(cfg.bmin, bmin);
    rcVcopy(cfg.bmax, bmax);
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

    spdlog::info("[NavMeshBuilder] Building: {}x{} grid, {} verts, {} tris", cfg.width, cfg.height, nverts, ntris);

    // 2. Rasterize input polygon soup
    std::unique_ptr<rcHeightfield, decltype(&rcFreeHeightField)> hf(rcAllocHeightfield(), rcFreeHeightField);
    if (!hf || !rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) {
        spdlog::error("[NavMeshBuilder] Could not create heightfield");
        return result;
    }

    std::vector<unsigned char> triAreas(ntris, RC_WALKABLE_AREA);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts, nverts, tris, ntris, triAreas.data());
    if (!rcRasterizeTriangles(&ctx, verts, nverts, tris, triAreas.data(), ntris, *hf, cfg.walkableClimb)) {
        spdlog::error("[NavMeshBuilder] Could not rasterize triangles");
        return result;
    }

    // 3. Filter walkables
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *hf);

    // 4. Partition heightfield to regions
    std::unique_ptr<rcCompactHeightfield, decltype(&rcFreeCompactHeightfield)> chf(rcAllocCompactHeightfield(), rcFreeCompactHeightfield);
    if (!chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf, *chf)) {
        spdlog::error("[NavMeshBuilder] Could not build compact heightfield");
        return result;
    }

    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf)) {
        spdlog::error("[NavMeshBuilder] Could not erode walkable area");
        return result;
    }

    if (!rcBuildDistanceField(&ctx, *chf)) {
        spdlog::error("[NavMeshBuilder] Could not build distance field");
        return result;
    }

    if (!rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea)) {
        spdlog::error("[NavMeshBuilder] Could not build regions");
        return result;
    }

    // 5. Build contours
    std::unique_ptr<rcContourSet, decltype(&rcFreeContourSet)> cset(rcAllocContourSet(), rcFreeContourSet);
    if (!cset || !rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset)) {
        spdlog::error("[NavMeshBuilder] Could not build contours");
        return result;
    }

    if (cset->nconts == 0) {
        spdlog::error("[NavMeshBuilder] No contours generated");
        return result;
    }

    // 6. Build polygon mesh
    std::unique_ptr<rcPolyMesh, decltype(&rcFreePolyMesh)> pmesh(rcAllocPolyMesh(), rcFreePolyMesh);
    if (!pmesh || !rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh)) {
        spdlog::error("[NavMeshBuilder] Could not build poly mesh");
        return result;
    }

    if (pmesh->npolys == 0) {
        spdlog::error("[NavMeshBuilder] No polygons generated");
        return result;
    }

    // 7. Build detail mesh
    std::unique_ptr<rcPolyMeshDetail, decltype(&rcFreePolyMeshDetail)> dmesh(rcAllocPolyMeshDetail(), rcFreePolyMeshDetail);
    if (!dmesh || !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh)) {
        spdlog::error("[NavMeshBuilder] Could not build poly mesh detail");
        return result;
    }

    // 8. Create Detour navmesh data
    spdlog::info("[NavMeshBuilder] PolyMesh: {} verts, {} polys", pmesh->nverts, pmesh->npolys);
    dtNavMeshCreateParams params;
    std::memset(&params, 0, sizeof(params));
    params.verts = pmesh->verts;
    params.vertCount = pmesh->nverts;
    params.polys = pmesh->polys;
    params.polyAreas = pmesh->areas;
    // Detour requires flags to match the query filter
    for (int i = 0; i < pmesh->npolys; ++i) pmesh->flags[i] = 1;
    params.polyFlags = pmesh->flags;
    params.polyCount = pmesh->npolys;
    params.nvp = pmesh->nvp;
    params.detailMeshes = dmesh->meshes;
    params.detailVerts = dmesh->verts;
    params.detailVertsCount = dmesh->nverts;
    params.detailTris = dmesh->tris;
    params.detailTriCount = dmesh->ntris;
    params.walkableHeight = config.agentHeight;
    params.walkableRadius = config.agentRadius;
    params.walkableClimb = config.agentMaxClimb;
    rcVcopy(params.bmin, pmesh->bmin);
    rcVcopy(params.bmax, pmesh->bmax);
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;

    unsigned char* navData = nullptr;
    int navDataSize = 0;
    if (!dtCreateNavMeshData(&params, &navData, &navDataSize)) {
        spdlog::error("[NavMeshBuilder] Could not create Detour navmesh data");
        return result;
    }

    result.serializedData.assign(navData, navData + navDataSize);
    dtFree(navData);
    result.valid = true;

    spdlog::info("[NavMeshBuilder] Successfully built navmesh ({} polys, {} bytes)", pmesh->npolys, navDataSize);
    return result;
}

NavMeshData NavMeshBuilder::buildFromGLB(const std::string& glbPath,
                                          const NavMeshConfig& config) {
    spdlog::info("[NavMeshBuilder] buildFromGLB() — path=\"{}\"", glbPath);
    
    Model::RawMeshData raw = Model::getGLBRawMesh(glbPath);
    if (raw.positions.empty()) {
        spdlog::error("[NavMeshBuilder] No geometry found in GLB: {}", glbPath);
        return {};
    }

    std::vector<float> vertices;
    vertices.reserve(raw.positions.size() * 3);
    for (const auto& p : raw.positions) {
        vertices.push_back(p.x);
        vertices.push_back(p.y);
        vertices.push_back(p.z);
    }

    std::vector<int> triangles;
    triangles.reserve(raw.indices.size());
    for (uint32_t idx : raw.indices) {
        triangles.push_back(static_cast<int>(idx));
    }

    return build(vertices, triangles, config);
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
