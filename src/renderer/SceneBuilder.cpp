#include "renderer/SceneBuilder.h"
#include "renderer/Renderer.h"
#include "renderer/Model.h"
#include "renderer/StaticSkinnedMesh.h"
#include "renderer/Texture.h"
#include "renderer/WaterRenderer.h"
#include "renderer/SSRPass.h"
#include "scene/Components.h"
#include "combat/CombatComponents.h"
#include "combat/HeroDefinition.h"
#include "ability/AbilityComponents.h"
#include "map/MapLoader.h"
#include "nav/NavMeshBuilder.h"
#include "core/Profiler.h"
#include "fog/FogOfWarGameplay.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <filesystem>
#include <future>
#include <string>
#include <unordered_map>

namespace glory {

void SceneBuilder::build(Renderer& r) {
    GLORY_ZONE_N("buildScene");

    // Batch all texture GPU uploads into a single command buffer submission.
    // This eliminates 50+ individual vkQueueWaitIdle stalls.
    TextureUploadBatch uploadBatch(*r.m_device);

    // ── Load map data for minimap & spawn system ─────────────────────────
    {
        GLORY_ZONE_N("buildScene:mapData");
        try {
            r.m_mapData = MapLoader::LoadFromFile(std::string(ASSET_DIR) + "maps/map_summonersrift.json");
            spdlog::info("Map '{}' loaded ({} towers per team)", r.m_mapData.mapName,
                         r.m_mapData.teams[0].towers.size());
        } catch (const std::exception& e) {
            spdlog::warn("Could not load map JSON: {} — using default bounds", e.what());
        }
    }

    // ── Navigation: init dynamic obstacles + flow fields ────────────────
    // (navmesh built later, after map meshes are loaded)
    {
        GLORY_ZONE_N("buildScene:navigation");
        r.m_dynamicObstacles.init();
    }

    // ── Spawn MOBA structures (towers, inhibitors, nexus) ────────────────
    {
        r.m_structureSystem->setVictoryCallback([&r](uint8_t winningTeam) {
            spdlog::info("[Renderer] Victory callback: team {} wins!", winningTeam);
            if (r.m_onVictory) r.m_onVictory(winningTeam);
        });
        auto result = r.m_structureSystem->spawnStructures(r.m_scene.getRegistry(), r.m_mapData);
        int totalTowers = 0, totalInhibitors = 0;
        for (int t = 0; t < 2; ++t) {
            totalInhibitors += static_cast<int>(result.inhibitors[t].size());
            for (auto& lane : result.laneTowers[t])
                for (auto e : lane) if (e != entt::null) totalTowers++;
            totalTowers += static_cast<int>(result.nexusTowers[t].size());
        }
        spdlog::info("[Renderer] Structures spawned: {} towers, {} inhibitors, 2 nexus",
                     totalTowers, totalInhibitors);
    }

    // ── Init minion wave system with map data + pathfinding ─────────────
    r.m_minionWaveSystem->init(r.m_mapData);
    r.m_minionWaveSystem->setPathfinding(&r.m_pathfinding);
    r.m_minionWaveSystem->setScene(&r.m_scene);

    // ── Init respawn system ──────────────────────────────────────────────
    r.m_respawnSystem->init(r.m_mapData, r.m_economySystem.get(), r.m_audioEvents.get());
    r.m_respawnSystem->onRespawn = [&r](entt::entity e, glm::vec3 fountain) {
        if (e == r.m_playerEntity) {
            r.m_isoCam.setFollowTarget(fountain);
            r.m_isoCam.setAttached(true);
        }
    };

    // Default textures — use batched upload path for thread safety
    uint32_t white = 0xFFFFFFFF;
    uint32_t defaultTex = r.m_scene.addTexture(
        Texture::createFromPixels(*r.m_device, &white, 1, 1,
                                  VK_FORMAT_R8G8B8A8_SRGB, &uploadBatch));

    // Checkerboard (8 tiles × 16px = 256×256)
    constexpr uint32_t kTiles = 8, kTileSize = 16;
    constexpr uint32_t kCheckerRes = kTiles * kTileSize * 2;
    std::vector<uint32_t> checkerPixels(kCheckerRes * kCheckerRes);
    for (uint32_t y = 0; y < kCheckerRes; ++y)
        for (uint32_t x = 0; x < kCheckerRes; ++x)
            checkerPixels[y * kCheckerRes + x] =
                ((x / kTileSize + y / kTileSize) & 1) ? 0xFF444444 : 0xFFCCCCCC;
    uint32_t checkerTex = r.m_scene.addTexture(
        Texture::createFromPixels(*r.m_device, checkerPixels.data(), kCheckerRes, kCheckerRes,
                                  VK_FORMAT_R8G8B8A8_SRGB, &uploadBatch));

    // Flat normal (tangent-space (0,0,1) → (128,128,255,255) ABGR)
    uint32_t flatPixel = 0xFFFF8080;
    uint32_t flatNorm = r.m_scene.addTexture(
        Texture::createFromPixels(*r.m_device, &flatPixel, 1, 1,
                                  VK_FORMAT_R8G8B8A8_UNORM, &uploadBatch));
    r.m_flatNormIndex      = flatNorm;

    // Bind to bindless descriptor array
    for (uint32_t i = 0; i < static_cast<uint32_t>(r.m_scene.getTextures().size()); ++i) {
        auto& tex = r.m_scene.getTexture(i);
        r.m_bindless->registerTexture(tex.getImageView(), tex.getSampler());
    }

    // ── Toon ramp texture (binding 5, 256×1 R8G8B8A8_UNORM) ─────────────
    // LoL-style gradient: dark cool shadow → midtone → bright warm highlight.
    // Zones: [0-39] shadow, [40-89] shadow→mid transition,
    //        [90-179] midtone, [180-219] mid→highlight transition, [220-255] highlight
    {
        constexpr uint32_t RAMP_W = 256;
        uint32_t pixels[RAMP_W];

        // Pack RGBA into a uint32_t for VK_FORMAT_R8G8B8A8_UNORM (little-endian)
        auto pack = [](float r, float g, float b) -> uint32_t {
            uint32_t R = static_cast<uint32_t>(r * 255.0f + 0.5f);
            uint32_t G = static_cast<uint32_t>(g * 255.0f + 0.5f);
            uint32_t B = static_cast<uint32_t>(b * 255.0f + 0.5f);
            return R | (G << 8) | (B << 16) | (0xFFu << 24);
        };

        // Anchor colours
        const float sR = 0.25f, sG = 0.22f, sB = 0.30f; // shadow
        const float mR = 0.75f, mG = 0.70f, mB = 0.68f; // midtone
        const float hR = 1.00f, hG = 0.98f, hB = 0.95f; // highlight

        for (uint32_t x = 0; x < RAMP_W; ++x) {
            if (x < 40) {
                pixels[x] = pack(sR, sG, sB);
            } else if (x < 90) {
                float t   = static_cast<float>(x - 40) / 50.0f;
                pixels[x] = pack(sR + t * (mR - sR), sG + t * (mG - sG), sB + t * (mB - sB));
            } else if (x < 180) {
                pixels[x] = pack(mR, mG, mB);
            } else if (x < 220) {
                float t   = static_cast<float>(x - 180) / 40.0f;
                pixels[x] = pack(mR + t * (hR - mR), mG + t * (hG - mG), mB + t * (hB - mB));
            } else {
                pixels[x] = pack(hR, hG, hB);
            }
        }

        r.m_toonRamp = Texture::createFromPixels(*r.m_device, pixels, RAMP_W, 1,
                                               VK_FORMAT_R8G8B8A8_UNORM,
                                               &uploadBatch);
        r.m_descriptors->writeToonRamp(r.m_toonRamp.getImageView(), r.m_toonRamp.getSampler());
        spdlog::info("Toon ramp texture created and bound to descriptor binding 5");
    }

    if (r.m_fogOfWar) {
        r.m_descriptors->writeFogOfWar(r.m_fogOfWar->getVisibilityView(), r.m_fogOfWar->getSampler());
        if (r.m_groundDecalRenderer) {
            // Decals sample FoW so ability indicators fade in unexplored areas
            r.m_groundDecalRenderer->setFogOfWar(
                r.m_fogOfWar->getVisibilityView(), r.m_fogOfWar->getSampler(),
                glm::vec2(0.0f, 0.0f), glm::vec2(200.0f, 200.0f));
        }
    }

    // ── Base ground plane (200×200, Y=0, below all lane tiles) ───────────
    {
        auto groundMesh = Model::createTerrain(*r.m_device, r.m_device->getAllocator(),
                                               200.0f, 1, 0.0f);
        uint32_t groundMeshIdx = r.m_scene.addMesh(std::move(groundMesh));

        auto groundEnt = r.m_scene.createEntity("ground_plane");
        auto& reg      = r.m_scene.getRegistry();
        reg.emplace<MeshComponent>(groundEnt, MeshComponent{ groundMeshIdx });
        // Matte green-brown terrain: no metallic, rough, slight warm tint
        reg.emplace<MaterialComponent>(groundEnt,
            MaterialComponent{ defaultTex, flatNorm, /*shininess*/0.0f, /*metallic*/0.0f, /*roughness*/0.9f, /*emissive*/0.0f });
        reg.emplace<MapComponent>(groundEnt);
        auto& gt   = reg.get<TransformComponent>(groundEnt);
        gt.position = glm::vec3(100.0f, -0.01f, 100.0f); // centred, just under lane tiles
        gt.scale    = glm::vec3(1.0f);
    }

    // ── Load map models from "map models/" ─────────────────────────────
    struct MapAsset {
        uint32_t mesh;
        uint32_t texture;                    // primary (sub-mesh 0) texture
        std::vector<uint32_t> subMeshTextures; // per-sub-mesh textures (empty = all use 'texture')
        bool isZUp = false;                  // model needs -90° X rotation
        glm::vec3 boundsMin{0.0f};           // local-space AABB min
        glm::vec3 boundsMax{0.0f};           // local-space AABB max
    };
    std::unordered_map<std::string, MapAsset> mapAssets;

    // Phase 1: Parse GLB files in parallel (CPU-heavy glTF parsing + GPU
    // buffer creation which is now thread-safe via pool/queue locks).
    struct ParsedMapModel {
        std::string filename;
        Model model;
        std::vector<int> subMatIndices;
        bool ok = false;
    };

    const std::vector<std::string> mapFiles = {
        "blue_team_tower_1.glb", "blue_team_tower_2.glb",
        "blue_team_tower_3.glb", "blue_team_inhib.glb",
        "blue_team_nexus.glb",   "red_team_tower_1.glb",
        "red_team_tower_2.glb", "red_team_tower_3.glb",
        "red_team_inhib.glb",    "red_team_nexus.glb",
        "arcane+tile+3d+model.glb", "jungle_tile.glb", "river_tile.glb"
    };

    std::vector<std::future<ParsedMapModel>> modelFutures;
    modelFutures.reserve(mapFiles.size());
    for (auto& filename : mapFiles) {
        modelFutures.push_back(std::async(std::launch::async,
            [&r, filename]() -> ParsedMapModel {
                ParsedMapModel pm;
                pm.filename = filename;
                std::string path = std::string(MODEL_DIR) + "map models/" + filename;
                try {
                    pm.model = Model::loadFromGLB(*r.m_device, r.m_device->getAllocator(), path);
                    uint32_t subCount = pm.model.getMeshCount();
                    pm.subMatIndices.reserve(subCount);
                    for (uint32_t si = 0; si < subCount; ++si)
                        pm.subMatIndices.push_back(pm.model.getMeshMaterialIndex(si));
                    pm.ok = true;
                } catch (const std::exception& e) {
                    spdlog::warn("Failed to load map model '{}': {}", filename, e.what());
                }
                return pm;
            }));
    }

    // Phase 2: Collect results and register in scene + load textures (serial).
    for (auto& fut : modelFutures) {
        auto pm = fut.get();
        if (!pm.ok) continue;

        uint32_t meshIdx = r.m_scene.addMesh(std::move(pm.model));

        std::string path = std::string(MODEL_DIR) + "map models/" + pm.filename;
        auto glbTextures = Model::loadGLBTextures(*r.m_device, path, &uploadBatch);
        std::unordered_map<int, uint32_t> matToTex;
        uint32_t primaryTex = defaultTex;
        for (auto& t : glbTextures) {
            uint32_t tid = r.m_scene.addTexture(std::move(t.texture));
            r.m_bindless->registerTexture(
                r.m_scene.getTexture(tid).getImageView(),
                r.m_scene.getTexture(tid).getSampler());
            matToTex[t.materialIndex] = tid;
            if (primaryTex == defaultTex) primaryTex = tid;
        }

        std::vector<uint32_t> subTextures;
        subTextures.reserve(pm.subMatIndices.size());
        for (int gltfMatIdx : pm.subMatIndices)
            subTextures.push_back(matToTex.count(gltfMatIdx) ? matToTex[gltfMatIdx] : primaryTex);

        mapAssets[pm.filename] = { meshIdx, primaryTex, subTextures };

        // Z-up heuristic: if model Y range >> Z range, it was exported Z-up
        try {
            auto bounds = Model::getGLBBounds(path);
            mapAssets[pm.filename].boundsMin = bounds.min;
            mapAssets[pm.filename].boundsMax = bounds.max;
            float yExtent = bounds.max.y - bounds.min.y;
            float zExtent = bounds.max.z - bounds.min.z;
            if (yExtent > zExtent * 2.0f) {
                mapAssets[pm.filename].isZUp = true;
                spdlog::info("  {} detected as Z-up, will apply -90° X rotation", pm.filename);
            }
        } catch (...) {}

        spdlog::info("Loaded map asset: {} (mesh={}, {} textures, {} submeshes)",
                     pm.filename, meshIdx, glbTextures.size(), pm.subMatIndices.size());
    }

    // ── Build navmesh from tile geometry (load from cache if available) ──
    {
        GLORY_ZONE_N("buildScene:navmesh");
        const std::string navCachePath = std::string(ASSET_DIR) + "nav/navmesh.bin";
        NavMeshData navData = NavMeshBuilder::load(navCachePath);

        if (!navData.valid) {
            // Build navmesh at runtime from walkable tile GLBs
            const std::vector<std::string> tileFiles = {
                "arcane+tile+3d+model.glb", "jungle_tile.glb", "river_tile.glb"
            };

            std::vector<float>  allVerts;
            std::vector<int>    allTris;

            for (const auto& tile : tileFiles) {
                std::string tilePath = std::string(MODEL_DIR) + "map models/" + tile;
                try {
                    auto raw = Model::getGLBRawMesh(tilePath);
                    uint32_t baseVertex = static_cast<uint32_t>(allVerts.size() / 3);
                    for (const auto& p : raw.positions) {
                        allVerts.push_back(p.x);
                        allVerts.push_back(p.y);
                        allVerts.push_back(p.z);
                    }
                    for (uint32_t idx : raw.indices) {
                        allTris.push_back(static_cast<int>(baseVertex + idx));
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("[NavMesh] Could not read tile '{}': {}", tile, e.what());
                }
            }

            if (!allVerts.empty() && !allTris.empty()) {
                NavMeshBuilder builder;
                navData = builder.build(allVerts, allTris);
                if (navData.valid) {
                    // Cache to disk for next run
                    namespace fs = std::filesystem;
                    fs::create_directories(fs::path(navCachePath).parent_path());
                    NavMeshBuilder::save(navData, navCachePath);
                }
            }
        }

        if (navData.valid) {
            r.m_pathfinding.init(navData);
        } else {
            spdlog::warn("[NavMesh] Navmesh build failed — champion A* pathfinding "
                         "unavailable, falling back to flow field steering");
        }
        r.m_pathfinding.initFlowFields(r.m_mapData);
        spdlog::info("[Renderer] Flow fields ready for {} lanes x 2 teams", 3);
    }

    // ── Give structures proper meshes from map models ───────────────────
    {
        auto& reg = r.m_scene.getRegistry();
        auto structView = reg.view<StructureComponent, TransformComponent>();
        for (auto e : structView) {
            if (reg.all_of<MeshComponent>(e)) continue;
            auto& sc = structView.get<StructureComponent>(e);
            auto& tc = structView.get<TransformComponent>(e);

            std::string modelFile;
            if (sc.teamIndex == 0) { // Blue
                switch (sc.type) {
                    case StructureType::TOWER_T1:    modelFile = "blue_team_tower_1.glb"; break;
                    case StructureType::TOWER_T2:    modelFile = "blue_team_tower_2.glb"; break;
                    case StructureType::TOWER_T3:    modelFile = "blue_team_tower_3.glb"; break;
                    case StructureType::TOWER_NEXUS: modelFile = "blue_team_tower_3.glb"; break;
                    case StructureType::INHIBITOR:   modelFile = "blue_team_inhib.glb";   break;
                    case StructureType::NEXUS:       modelFile = "blue_team_nexus.glb";   break;
                }
            } else { // Red
                switch (sc.type) {
                    case StructureType::TOWER_T1:    modelFile = "red_team_tower_1.glb";  break;
                    case StructureType::TOWER_T2:    modelFile = "red_team_tower_2.glb";  break;
                    case StructureType::TOWER_T3:    modelFile = "red_team_tower_3.glb";  break;
                    case StructureType::TOWER_NEXUS: modelFile = "red_team_tower_3.glb";  break;
                    case StructureType::INHIBITOR:   modelFile = "red_team_inhib.glb";    break;
                    case StructureType::NEXUS:       modelFile = "red_team_nexus.glb";    break;
                }
            }

            if (mapAssets.count(modelFile)) {
                auto& asset = mapAssets[modelFile];

                // Target visible height per structure type (world units)
                float targetHeight = 6.0f;
                switch (sc.type) {
                    case StructureType::TOWER_T1:
                    case StructureType::TOWER_T2:
                    case StructureType::TOWER_T3:    targetHeight = 8.0f; break;
                    case StructureType::TOWER_NEXUS: targetHeight = 8.0f; break;
                    case StructureType::INHIBITOR:   targetHeight = 5.0f; break;
                    case StructureType::NEXUS:       targetHeight = 7.0f; break;
                }

                // Uniform scale so the model reaches targetHeight
                float modelHeight = asset.boundsMax.y - asset.boundsMin.y;
                float scaleFactor = (modelHeight > 1e-4f)
                                      ? (targetHeight / modelHeight)
                                      : 1.0f;
                tc.scale = glm::vec3(scaleFactor);

                // Raise so the base of the scaled model sits at ground (y = 0)
                tc.position.y = -asset.boundsMin.y * scaleFactor;

                if (asset.isZUp)
                    tc.rotation.x = glm::radians(-90.0f);

                AABB localAABB;
                localAABB.min = asset.boundsMin;
                localAABB.max = asset.boundsMax;
                reg.emplace<MeshComponent>(e, MeshComponent{
                    asset.mesh, -1, localAABB });
                reg.emplace<MaterialComponent>(e, MaterialComponent{
                    asset.texture, flatNorm,
                    /*shininess=*/0.0f, /*metallic=*/0.0f,
                    /*roughness=*/0.6f, /*emissive=*/0.0f,
                    asset.subMeshTextures });

                // Subtle team tint for visibility/differentiation
                glm::vec4 teamTint = (sc.teamIndex == 0)
                    ? glm::vec4(0.7f, 0.8f, 1.0f, 1.0f)   // blue tinge
                    : glm::vec4(1.0f, 0.8f, 0.7f, 1.0f);   // red tinge
                reg.emplace<TintComponent>(e, TintComponent{ teamTint });

                spdlog::info("[Renderer] Structure GLB '{}' assigned: "
                             "mesh={}, scale={:.1f} (modelH={:.3f}), "
                             "pos=({:.1f},{:.1f},{:.1f})",
                             modelFile, asset.mesh, scaleFactor, modelHeight,
                             tc.position.x, tc.position.y, tc.position.z);
            } else {
                // Fallback to cube if model failed to load
                spdlog::warn("[Renderer] Structure fallback to cube — model '{}' not found "
                             "(team={}, type={})", modelFile, sc.teamIndex,
                             static_cast<int>(sc.type));
                uint32_t cubeMesh = r.m_scene.addMesh(Model::createCube(*r.m_device, r.m_device->getAllocator()));
                float s = 1.0f;
                switch (sc.type) {
                    case StructureType::TOWER_T1:
                    case StructureType::TOWER_T2:
                    case StructureType::TOWER_T3:
                    case StructureType::TOWER_NEXUS: s = 2.0f;  break;
                    case StructureType::INHIBITOR:   s = 2.5f;  break;
                    case StructureType::NEXUS:       s = 3.5f;  break;
                }
                tc.scale = glm::vec3(s, s * 2.0f, s);
                // Cube is centered at origin; raise so base sits at ground (y=0).
                tc.position.y = s;
                reg.emplace<MeshComponent>(e, MeshComponent{ cubeMesh });
                reg.emplace<MaterialComponent>(e,
                    MaterialComponent{ checkerTex, flatNorm, 0.0f, 0.0f, 0.8f, 0.0f });
                // Team-colored tint so cubes are visible against the fog
                glm::vec4 teamTint = (sc.teamIndex == 0)
                    ? glm::vec4(0.3f, 0.5f, 1.0f, 1.0f)   // blue team
                    : glm::vec4(1.0f, 0.3f, 0.3f, 1.0f);   // red team
                reg.emplace<TintComponent>(e, TintComponent{ teamTint });
            }
        }
        spdlog::info("[Renderer] Structure meshes assigned from map models");
    }

    // ── Lane tiles (GLB tile models placed along each lane path) ──────────
    {
        // Use actual GLB tile meshes instead of procedural flat quads.
        // All three tile GLBs are ~1x1 unit and get scaled to laneWidth.
        struct TileAsset {
            uint32_t mesh = 0;
            uint32_t texture = 0;
            std::vector<uint32_t> subMeshTextures;
            bool ok = false;
            bool isZUp = false;
        };

        TileAsset laneTile, riverTile, jungleTile;

        if (mapAssets.count("arcane+tile+3d+model.glb")) {
            auto& a              = mapAssets["arcane+tile+3d+model.glb"];
            laneTile.mesh            = a.mesh;
            laneTile.texture         = a.texture;
            laneTile.subMeshTextures = a.subMeshTextures;
            laneTile.isZUp           = a.isZUp;
            laneTile.ok              = true;
        }
        if (mapAssets.count("river_tile.glb")) {
            auto& a               = mapAssets["river_tile.glb"];
            riverTile.mesh            = a.mesh;
            riverTile.texture         = a.texture;
            riverTile.subMeshTextures = a.subMeshTextures;
            riverTile.isZUp           = a.isZUp;
            riverTile.ok              = true;
        }
        if (mapAssets.count("jungle_tile.glb")) {
            auto& a                = mapAssets["jungle_tile.glb"];
            jungleTile.mesh            = a.mesh;
            jungleTile.texture         = a.texture;
            jungleTile.subMeshTextures = a.subMeshTextures;
            jungleTile.isZUp           = a.isZUp;
            jungleTile.ok              = true;
        }

        // Fallback: procedural 1x1 quad if GLB failed to load
        if (!laneTile.ok) {
            auto tileQuad = Model::createTerrain(*r.m_device, r.m_device->getAllocator(),
                                                  1.0f, 1, 0.0f);
            laneTile.mesh    = r.m_scene.addMesh(std::move(tileQuad));
            laneTile.texture = checkerTex;
            laneTile.ok      = true;
        }

        if (laneTile.ok) {
            // GLB tiles are ~1x1 unit; scale to laneWidth for each lane.
            auto placeLaneTiles = [&](const std::vector<glm::vec3>& waypoints,
                                      float laneWidth, const std::string& laneName,
                                      float laneY)
            {
                int tileCount = 0;
                float stride = laneWidth;

                for (size_t i = 0; i + 1 < waypoints.size(); ++i) {
                    glm::vec3 a    = waypoints[i];
                    glm::vec3 b    = waypoints[i + 1];
                    glm::vec3 diff = b - a;
                    float segLen   = glm::length(diff);
                    if (segLen < 0.001f) continue;
                    glm::vec3 dir = diff / segLen;

                    float yaw = std::atan2(dir.x, dir.z);

                    float walked = 0.0f;
                    while (walked < segLen) {
                        glm::vec3 pos = a + dir * (walked + stride * 0.5f);
                        pos.y = laneY;

                        // Pick the right tile variant (mesh + textures)
                        const TileAsset* cur = &laneTile;
                        if (std::abs(pos.x + pos.z - 200.0f) < 25.0f) {
                            if (riverTile.ok) cur = &riverTile;
                        } else if (laneName != "MidLane") {
                            if (jungleTile.ok) cur = &jungleTile;
                        }

                        auto tile = r.m_scene.createEntity(
                            laneName + "_tile_" + std::to_string(tileCount));
                        r.m_scene.getRegistry().emplace<MeshComponent>(
                            tile, MeshComponent{ cur->mesh });
                        r.m_scene.getRegistry().emplace<MaterialComponent>(
                            tile, MaterialComponent{ cur->texture, flatNorm,
                                                     /*shininess*/0.0f, /*metallic*/0.0f,
                                                     /*roughness*/0.8f, /*emissive*/0.0f,
                                                     cur->subMeshTextures });
                        r.m_scene.getRegistry().emplace<MapComponent>(tile);

                        auto& tt    = r.m_scene.getRegistry().get<TransformComponent>(tile);
                        tt.position = pos;
                        tt.rotation = glm::vec3(
                            cur->isZUp ? glm::radians(-90.0f) : 0.0f, yaw, 0.0f);
                        tt.scale    = glm::vec3(laneWidth);

                        ++tileCount;
                        walked += stride;
                    }
                }
                spdlog::info("Placed {} tiles for {}", tileCount, laneName);
            };

            // Use actual lane waypoints from map data rather than hardcoded paths.
            // Stagger Y per lane so overlapping tiles don't Z-fight.  Mid lane on
            // top (widest, most visible), Top/Bot underneath at distinct heights.
            const auto& blueTeam = r.m_mapData.teams[0];
            if (!blueTeam.lanes[0].waypoints.empty()) {
                placeLaneTiles(blueTeam.lanes[0].waypoints,
                               blueTeam.lanes[0].width, "TopLane", 0.05f);
            } else {
                placeLaneTiles({
                    {22,0,22}, {22,0,60}, {22,0,100}, {22,0,140}, {22,0,178},
                    {60,0,178}, {100,0,178}, {140,0,178}, {178,0,178}
                }, 12.0f, "TopLane", 0.05f);
            }

            // Mid Lane
            if (!blueTeam.lanes[1].waypoints.empty()) {
                placeLaneTiles(blueTeam.lanes[1].waypoints,
                               blueTeam.lanes[1].width, "MidLane", 0.15f);
            } else {
                placeLaneTiles({
                    {22,0,22}, {40,0,40}, {60,0,60}, {80,0,80}, {100,0,100},
                    {120,0,120}, {140,0,140}, {160,0,160}, {178,0,178}
                }, 14.0f, "MidLane", 0.15f);
            }

            // Bot Lane
            if (!blueTeam.lanes[2].waypoints.empty()) {
                placeLaneTiles(blueTeam.lanes[2].waypoints,
                               blueTeam.lanes[2].width, "BotLane", 0.10f);
            } else {
                placeLaneTiles({
                    {22,0,22}, {60,0,22}, {100,0,22}, {140,0,22}, {178,0,22},
                    {178,0,60}, {178,0,100}, {178,0,140}, {178,0,178}
                }, 12.0f, "BotLane", 0.10f);
            }
        }
    }

    // ── Player character ─────────────────────────────────────────────────
    auto character = r.m_scene.createEntity("PlayerCharacter");
    bool skinnedLoaded = false;

    try {
        std::string charPath = std::string(MODEL_DIR) + "models/scientist/scientist.glb";
        auto skinnedData = Model::loadSkinnedFromGLB(
            *r.m_device, r.m_device->getAllocator(), charPath, 0.0f);

        // Load ALL textures and build per-sub-mesh mapping
        uint32_t charTex = defaultTex;
        std::vector<uint32_t> charSubMeshTextures;
        auto glbTextures = Model::loadGLBTextures(*r.m_device, charPath, &uploadBatch);
        if (!glbTextures.empty()) {
            std::unordered_map<int, uint32_t> matToTex;
            for (auto& t : glbTextures) {
                uint32_t tid = r.m_scene.addTexture(std::move(t.texture));
                r.m_bindless->registerTexture(
                    r.m_scene.getTexture(tid).getImageView(),
                    r.m_scene.getTexture(tid).getSampler());
                matToTex[t.materialIndex] = tid;
                if (charTex == defaultTex) charTex = tid;
            }
            spdlog::info("Character: {} texture(s) loaded, primary slot {}", glbTextures.size(), charTex);
        } else {
            spdlog::warn("Character texture not found in GLB — using default white texture (slot {})", charTex);
        }

        r.m_scene.addMesh(std::move(skinnedData.model));

        // Build StaticSkinnedMesh from first mesh in GLB
        if (!skinnedData.bindPoseVertices.empty() && !skinnedData.skinVertices.empty()) {
            std::vector<SkinnedVertex> sverts;
            sverts.reserve(skinnedData.bindPoseVertices[0].size());
            for (size_t vi = 0; vi < skinnedData.bindPoseVertices[0].size(); ++vi) {
                SkinnedVertex sv{};
                sv.position = skinnedData.bindPoseVertices[0][vi].position;
                sv.color    = skinnedData.bindPoseVertices[0][vi].color;
                sv.normal   = skinnedData.bindPoseVertices[0][vi].normal;
                sv.texCoord = skinnedData.bindPoseVertices[0][vi].texCoord;
                sv.joints   = skinnedData.skinVertices[0][vi].joints;
                sv.weights  = skinnedData.skinVertices[0][vi].weights;
                sverts.push_back(sv);
            }
            uint32_t ssIdx = r.m_scene.addStaticSkinnedMesh(
                StaticSkinnedMesh(*r.m_device, r.m_device->getAllocator(),
                                  sverts, skinnedData.indices[0]));

            // Setup animation
            SkeletonComponent skelComp;
            skelComp.skeleton         = std::move(skinnedData.skeleton);
            skelComp.skinVertices     = std::move(skinnedData.skinVertices);
            skelComp.bindPoseVertices = std::move(skinnedData.bindPoseVertices);

            AnimationComponent animComp;
            animComp.player.setSkeleton(&skelComp.skeleton);

            // Try to load idle and walk animations
            auto tryLoadAnim = [&](const std::string& path) {
                try {
                    auto d = Model::loadSkinnedFromGLB(*r.m_device, r.m_device->getAllocator(), path, 0.0f);
                    if (!d.animations.empty()) {
                        animComp.clips.push_back(std::move(d.animations[0]));
                        retargetClip(animComp.clips.back(), skelComp.skeleton);
                        return true;
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("Failed to load animation '{}': {}", path, e.what());
                }
                return false;
            };

            std::string base = std::string(MODEL_DIR) + "models/scientist/";
            bool idleOk = tryLoadAnim(base + "scientist_idle.glb");
            if (!idleOk && !skinnedData.animations.empty())
                animComp.clips.push_back(skinnedData.animations[0]); // embedded fallback
            tryLoadAnim(base + "scientist_walk.glb");
            // Attack animation (clip[2]): play once and hold — no looping
            if (tryLoadAnim(base + "scientist_auto_attack.glb"))
                animComp.clips.back().looping = false;

            if (!animComp.clips.empty()) {
                animComp.activeClipIndex = 0;
                animComp.player.setClip(&animComp.clips[0]);
            }

            r.m_scene.getRegistry().emplace<SkeletonComponent>(character, std::move(skelComp));
            r.m_scene.getRegistry().emplace<AnimationComponent>(character, std::move(animComp));

            // Re-point raw pointers to the registry-owned copies (the locals were moved-from)
            auto& regSkel = r.m_scene.getRegistry().get<SkeletonComponent>(character);
            auto& regAnim = r.m_scene.getRegistry().get<AnimationComponent>(character);
            regAnim.player.setSkeleton(&regSkel.skeleton);
            if (!regAnim.clips.empty())
                regAnim.player.setClip(&regAnim.clips[regAnim.activeClipIndex]);

            r.m_scene.getRegistry().emplace<GPUSkinnedMeshComponent>(character,
                GPUSkinnedMeshComponent{ ssIdx });
            r.m_scene.getRegistry().emplace<MaterialComponent>(character,
                MaterialComponent{ charTex, flatNorm, 0.0f, 0.0f, 0.5f, 0.0f });

            auto& ct = r.m_scene.getRegistry().get<TransformComponent>(character);
            ct.position  = glm::vec3(100.0f, 0.0f, 100.0f);
            ct.scale     = glm::vec3(0.05f);
            skinnedLoaded = true;
            spdlog::info("Character loaded with GPU skinning");
        }
    } catch (const std::exception& e) {
        spdlog::warn("Could not load skinned character: {} — using capsule fallback", e.what());
    }

    if (!skinnedLoaded) {
        // Fallback: simple capsule
        uint32_t capsuleMesh = r.m_scene.addMesh(
            Model::createCapsule(*r.m_device, r.m_device->getAllocator(), 0.5f, 1.0f));
        r.m_scene.getRegistry().emplace<MeshComponent>(character, MeshComponent{ capsuleMesh });
        r.m_scene.getRegistry().emplace<MaterialComponent>(character,
            MaterialComponent{ defaultTex, flatNorm, 0.0f, 0.0f, 0.5f, 0.0f });
        auto& ct = r.m_scene.getRegistry().get<TransformComponent>(character);
        ct.position = glm::vec3(100.0f, 0.0f, 100.0f);
        ct.scale    = glm::vec3(1.0f);
    }

    // Look up selected hero definition
    const HeroDefinition* heroDef = r.m_heroRegistry.find(r.m_selectedHeroId);
    if (!heroDef && r.m_heroRegistry.count() > 0)
        heroDef = &r.m_heroRegistry.all()[0];

    float moveSpeed = heroDef ? heroDef->baseMoveSpeed : 8.0f;
    r.m_scene.getRegistry().emplace<CharacterComponent>(character,
        CharacterComponent{ glm::vec3(100.0f, 0.0f, 100.0f), moveSpeed });
    r.m_scene.getRegistry().emplace<TeamComponent>(character, TeamComponent{ Team::PLAYER });
    auto& combat = r.m_scene.getRegistry().emplace<CombatComponent>(character);
    if (heroDef) {
        combat.isRanged = heroDef->isRanged;
        combat.projectileSpeed = heroDef->projectileSpeed;
        combat.projectileVfx = heroDef->projectileVfx;
        combat.attackRange = heroDef->baseAttackRange;
        combat.attackSpeed = heroDef->baseAttackSpeed;
        combat.attackDamage = heroDef->baseAttackDamage;
    } else {
        combat.isRanged = true;
        combat.projectileSpeed = 30.0f;
        combat.projectileVfx = "vfx_fireball_projectile";
        combat.attackRange = 15.0f;
        combat.attackSpeed = 1.2f;
    }

    r.m_scene.getRegistry().emplace<SelectableComponent>(character, SelectableComponent{ false, 2.5f });

    ResourceComponent res;
    if (heroDef) {
        res.maximum = heroDef->baseMP;
        res.current = heroDef->baseMP;
    }
    r.m_scene.getRegistry().emplace<ResourceComponent>(character, res);
    
    StatsComponent playerStats;
    if (heroDef) {
        playerStats.base.maxHP = heroDef->baseHP;
        playerStats.base.currentHP = heroDef->baseHP;
        playerStats.base.attackDamage = heroDef->baseAttackDamage;
        playerStats.base.armor = heroDef->baseArmor;
        playerStats.base.magicResist = heroDef->baseMagicResist;
        playerStats.base.abilityPower = heroDef->baseAbilityPower;
    } else {
        playerStats.base.maxHP = 600.0f;
        playerStats.base.currentHP = 600.0f;
    }
    r.m_scene.getRegistry().emplace<StatsComponent>(character, playerStats);

    r.m_scene.getRegistry().emplace<StatusEffectsComponent>(character);

    HeroComponent heroComp;
    heroComp.definition = heroDef;
    heroComp.level = 1;
    r.m_scene.getRegistry().emplace<HeroComponent>(character, heroComp);

    // Economy: starting gold + XP tracking
    r.m_scene.getRegistry().emplace<EconomyComponent>(character, EconomyComponent{500, 0, 1});

    // Respawn: hero can die and respawn
    r.m_scene.getRegistry().emplace<RespawnComponent>(character, RespawnComponent{
        LifeState::ALIVE, 0.f, 0.f, glm::vec3(0.f), true /*isHero*/
    });

    // FoW vision: hero sight radius
    r.m_scene.getRegistry().emplace<VisionComponent>(character, VisionComponent{
        FogOfWarGameplay::HERO_VISION
    });
    if (r.m_abilitySystem) {
        std::array<std::string, 4> abilityIds;
        if (heroDef) {
            abilityIds = heroDef->abilityIds;
        }
        bool hasAbilities = false;
        for (auto& id : abilityIds) {
            if (!id.empty()) { hasAbilities = true; break; }
        }
        if (!hasAbilities) {
            abilityIds = {"fire_mage_fireball", "ice_zone", "nature_shield", "storm_strike"};
        }
        r.m_abilitySystem->initEntity(r.m_scene.getRegistry(), character, abilityIds);
        r.m_abilitySystem->setAbilityLevel(r.m_scene.getRegistry(), character, AbilitySlot::Q, 1);
        r.m_abilitySystem->setAbilityLevel(r.m_scene.getRegistry(), character, AbilitySlot::W, 1);
        r.m_abilitySystem->setAbilityLevel(r.m_scene.getRegistry(), character, AbilitySlot::E, 1);
        r.m_abilitySystem->setAbilityLevel(r.m_scene.getRegistry(), character, AbilitySlot::R, 1);

        // D-key summoner ability
        std::string summonerId = heroDef ? heroDef->summonerAbilityId : "fire_mage_trick";
        if (!summonerId.empty()) {
            const auto* trickDef = r.m_abilitySystem->findDefinition(summonerId);
            if (trickDef) {
                auto& book = r.m_scene.getRegistry().get<AbilityBookComponent>(character);
                auto& inst = book.abilities[static_cast<size_t>(AbilitySlot::SUMMONER)];
                inst.def   = trickDef;
                inst.level = 1;
                inst.currentPhase = AbilityPhase::READY;
            }
        }
    }
    
    r.m_playerEntity = character;
    r.m_gameplaySystem.init(r.m_scene, r.m_abilitySystem.get(), r.m_combatSystem.get(),
                          &r.m_gpuCollision, &r.m_debugRenderer);
    r.m_gameplaySystem.setGroundDecals(r.m_groundDecalRenderer.get());
    r.m_gameplaySystem.setPlayerEntity(r.m_playerEntity);
    r.m_isoCam.setFollowTarget(glm::vec3(100.0f, 0.0f, 100.0f));

    // ── Load Minion assets for spawning ───────────────────────────────────
    try {
        std::string minionPath = std::string(MODEL_DIR) + "models/melee_minion/melee_minion_walking.glb";
        auto skinnedData = Model::loadSkinnedFromGLB(*r.m_device, r.m_device->getAllocator(), minionPath, 0.6f);

        uint32_t minionTex = defaultTex;
        auto glbTextures = Model::loadGLBTextures(*r.m_device, minionPath, &uploadBatch);
        if (!glbTextures.empty()) {
            for (auto& t : glbTextures) {
                uint32_t tid = r.m_scene.addTexture(std::move(t.texture));
                r.m_bindless->registerTexture(
                    r.m_scene.getTexture(tid).getImageView(),
                    r.m_scene.getTexture(tid).getSampler());
                if (minionTex == defaultTex) minionTex = tid;
            }
            spdlog::info("Minion: {} texture(s) loaded, primary slot {}", glbTextures.size(), minionTex);
        } else {
            spdlog::warn("Minion texture not found in GLB — using default white texture (slot {})", minionTex);
        }

        uint32_t minionMeshIdx = r.m_scene.addMesh(std::move(skinnedData.model));

        if (!skinnedData.bindPoseVertices.empty() && !skinnedData.skinVertices.empty()) {
            std::vector<SkinnedVertex> sverts;
            sverts.reserve(skinnedData.bindPoseVertices[0].size());
            for (size_t vi = 0; vi < skinnedData.bindPoseVertices[0].size(); ++vi) {
                SkinnedVertex sv{};
                sv.position = skinnedData.bindPoseVertices[0][vi].position;
                sv.color    = skinnedData.bindPoseVertices[0][vi].color;
                sv.normal   = skinnedData.bindPoseVertices[0][vi].normal;
                sv.texCoord = skinnedData.bindPoseVertices[0][vi].texCoord;
                sv.joints   = skinnedData.skinVertices[0][vi].joints;
                sv.weights  = skinnedData.skinVertices[0][vi].weights;
                sverts.push_back(sv);
            }
            uint32_t minionMeshIdx = r.m_scene.addStaticSkinnedMesh(
                StaticSkinnedMesh(*r.m_device, r.m_device->getAllocator(),
                                  sverts, skinnedData.indices[0]));

            Skeleton minionSkeleton = std::move(skinnedData.skeleton);
            std::vector<AnimationClip> minionClips;

            // Load idle animation from melee_minion_idle.glb (clip[0])
            // Falls back to empty clip if file not found
            std::string minionAnimBase = std::string(MODEL_DIR) + "models/melee_minion/";
            bool idleOk = false;
            try {
                auto idleData = Model::loadSkinnedFromGLB(
                    *r.m_device, r.m_device->getAllocator(),
                    minionAnimBase + "melee_minion_idle.glb", 0.0f);
                if (!idleData.animations.empty()) {
                    minionClips.push_back(std::move(idleData.animations[0]));
                    retargetClip(minionClips.back(), minionSkeleton);
                    idleOk = true;
                    spdlog::info("Minion idle animation loaded and retargeted");
                }
            } catch (const std::exception& ie) {
                spdlog::warn("Could not load minion idle animation: {}", ie.what());
            }
            if (!idleOk)
                minionClips.push_back(AnimationClip{}); // empty fallback

            // Walk animation is embedded in the base model (clip[1])
            if (!skinnedData.animations.empty()) {
                minionClips.push_back(std::move(skinnedData.animations[0]));
            } else {
                spdlog::warn("No walk animation found in minion base model.");
            }

            // Attack animation (clip[2])
            bool attackOk = false;
            try {
                auto attackData = Model::loadSkinnedFromGLB(
                    *r.m_device, r.m_device->getAllocator(),
                    minionAnimBase + "melee_minion_attack1.glb", 0.0f);
                if (!attackData.animations.empty()) {
                    minionClips.push_back(std::move(attackData.animations[0]));
                    retargetClip(minionClips.back(), minionSkeleton);
                    minionClips.back().looping = false; // Attack plays once, don't loop
                    attackOk = true;
                    spdlog::info("Minion attack animation loaded and retargeted");
                }
            } catch (const std::exception& ae) {
                spdlog::warn("Could not load minion attack animation: {}", ae.what());
            }
            if (!attackOk)
                minionClips.push_back(AnimationClip{}); // empty fallback

            spdlog::info("Minion model and {} animations loaded for spawning", minionClips.size());

            MinionSpawnConfig spawnCfg;
            spawnCfg.meshIndex        = minionMeshIdx;
            spawnCfg.texIndex         = minionTex;
            spawnCfg.flatNormIndex    = flatNorm;
            spawnCfg.skeleton         = std::move(minionSkeleton);
            spawnCfg.clips            = std::move(minionClips);
            r.m_gameplaySystem.setSpawnConfig(std::move(spawnCfg));

            // Copy spawn config to MinionWaveSystem for lane wave spawning
            {
                const auto& src = r.m_gameplaySystem.getSpawnConfig();
                WaveSpawnConfig wsc;
                wsc.meshIndex     = src.meshIndex;
                wsc.texIndex      = src.texIndex;
                wsc.flatNormIndex = src.flatNormIndex;
                wsc.skeleton      = src.skeleton;
                wsc.clips         = src.clips;
                wsc.ready         = true;
                r.m_minionWaveSystem->setSpawnConfig(std::move(wsc));
            }
        }
    } catch (const std::exception& e) {
        spdlog::warn("Could not load minion model: {}", e.what());
    }

    spdlog::info("Scene built: flat map + player character");

    // ── Pre-load Q ability model (q+ability.glb) for projectile rendering ────
    if (r.m_projectileSystem) {
        try {
            std::string qModelPath = std::string(MODEL_DIR) + "models/abiliities_models/q+ability.glb";
            auto model = Model::loadFromGLB(*r.m_device, r.m_device->getAllocator(), qModelPath);
            uint32_t qMeshIdx = r.m_scene.addMesh(std::move(model));

            // Try to load embedded texture(s); fall back to default
            uint32_t qTexIdx = defaultTex;
            auto glbTextures = Model::loadGLBTextures(*r.m_device, qModelPath, &uploadBatch);
            if (!glbTextures.empty()) {
                for (auto& t : glbTextures) {
                    uint32_t tid = r.m_scene.addTexture(std::move(t.texture));
                    r.m_bindless->registerTexture(
                        r.m_scene.getTexture(tid).getImageView(),
                        r.m_scene.getTexture(tid).getSampler());
                    if (qTexIdx == defaultTex) qTexIdx = tid;
                }
            }

            // Register with the projectile system (scale = 1.0 — model is ~1 unit long)
            r.m_projectileSystem->registerAbilityMesh("fire_mage_fireball",
                { qMeshIdx, qTexIdx, r.m_flatNormIndex, glm::vec3(3.5f) });

            // Reuse the same Q model for the D-key purple trick skillshot
            // but with a solid purple texture so it's visually distinct
            uint32_t purplePixel = 0xFF9900CC;  // ABGR: opaque purple (R=0xCC, G=0x00, B=0x99)
            auto purpleTex = Texture::createFromPixels(*r.m_device, &purplePixel, 1, 1,
                                                      VK_FORMAT_R8G8B8A8_SRGB, &uploadBatch);
            uint32_t purpleTexIdx = r.m_scene.addTexture(std::move(purpleTex));
            r.m_bindless->registerTexture(
                r.m_scene.getTexture(purpleTexIdx).getImageView(),
                r.m_scene.getTexture(purpleTexIdx).getSampler());
            r.m_projectileSystem->registerAbilityMesh("fire_mage_trick",
                { qMeshIdx, purpleTexIdx, r.m_flatNormIndex, glm::vec3(3.0f) });

            // Reuse Q model for R-key bomb lob with a bright red-orange texture
            uint32_t bombPixel = 0xFF0044FF;  // ABGR: opaque red-orange
            auto bombTex = Texture::createFromPixels(*r.m_device, &bombPixel, 1, 1,
                                                    VK_FORMAT_R8G8B8A8_SRGB, &uploadBatch);
            uint32_t bombTexIdx = r.m_scene.addTexture(std::move(bombTex));
            r.m_bindless->registerTexture(
                r.m_scene.getTexture(bombTexIdx).getImageView(),
                r.m_scene.getTexture(bombTexIdx).getSampler());
            r.m_projectileSystem->registerAbilityMesh("fire_mage_bomb",
                { qMeshIdx, bombTexIdx, r.m_flatNormIndex, glm::vec3(4.5f) });

            spdlog::info("Q ability model loaded (meshIdx={})", qMeshIdx);
        } catch (const std::exception& e) {
            spdlog::warn("Could not load Q ability model: {}", e.what());
        }
    }

    // ── Water renderer ────────────────────────────────────────────────────
    // Register its 3 procedural textures just after all scene textures.
    {
        r.m_waterRenderer = std::make_unique<WaterRenderer>();
        r.m_waterRenderer->init(*r.m_device, r.m_hdrFB->mainFormats(),
                              r.m_descriptors->getLayout(),
                              r.m_bindless->getLayout(),
                              *r.m_bindless);

        // Register SSR output in bindless array for water to sample
        if (r.m_ssrPass.isEnabled() && r.m_ssrPass.getReflectionView()) {
            int ssrIdx = static_cast<int>(r.m_bindless->registerTexture(
                r.m_ssrPass.getReflectionView(), r.m_ssrPass.getReflectionSampler()));
            r.m_waterRenderer->ssrBindlessIdx = ssrIdx;
            spdlog::info("[SSR] Registered in bindless slot {}", ssrIdx);
        }
    }

    // ── Flush all batched texture uploads in a single GPU submission ─────
    if (!uploadBatch.empty()) {
        uploadBatch.flush();
    }

    // ── Mega-buffer: suballocate all scene meshes ────────────────────────
    if (r.m_megaBuffer) {
        const auto& models = r.m_scene.getMeshes();
        r.m_meshHandles.resize(models.size());

        for (uint32_t mi = 0; mi < static_cast<uint32_t>(models.size()); ++mi) {
            auto& model = r.m_scene.getMesh(mi);
            uint32_t subCount = model.getMeshCount();
            r.m_meshHandles[mi].resize(subCount);

            for (uint32_t si = 0; si < subCount; ++si) {
                auto& mesh = model.getSubMesh(si);
                const auto& verts = mesh.getCPUVertices();
                const auto& idxs  = mesh.getCPUIndices();
                if (!verts.empty() && !idxs.empty()) {
                    r.m_meshHandles[mi][si] = r.m_megaBuffer->suballocate(
                        verts.data(), static_cast<uint32_t>(verts.size()),
                        idxs.data(),  static_cast<uint32_t>(idxs.size()));
                }
                mesh.releaseCPUData();
            }
        }

        // One-shot transfer staging → device-local
        r.m_megaBuffer->flush();
        spdlog::info("Mega-buffer: {} models suballocated",
                     models.size());
    }

    // ── LOD system: load config + register chains from cooked LOD data ───
    {
        r.m_lodSystem.loadConfig(std::string(ASSET_DIR) + "config/render_settings.json");
        r.m_lodSystem.setQuality(static_cast<int>(r.m_renderQuality));
        spdlog::info("[Renderer] LODSystem initialised (quality={})",
                     static_cast<int>(r.m_renderQuality));
    }
}

} // namespace glory
