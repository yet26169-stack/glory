// Standalone asset cook tool: converts GLB files to binary .glory format
// Usage: asset_cook <input.glb> [-o output.glory]
//
// The .glory format stores pre-processed, meshoptimizer-optimized vertex/index
// data with LOD meshes that can be memory-mapped directly into GPU buffers.

#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include <tiny_gltf.h>
#include <meshoptimizer.h>

#include "assets/AssetFormat.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <cmath>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

// ── Helpers ─────────────────────────────────────────────────────────────────

static const uint8_t* accessorData(const tinygltf::Model& model, int accessorIdx) {
    const auto& acc = model.accessors[accessorIdx];
    const auto& bv  = model.bufferViews[acc.bufferView];
    return model.buffers[bv.buffer].data.data() + bv.byteOffset + acc.byteOffset;
}

static size_t accessorCount(const tinygltf::Model& model, int accessorIdx) {
    return model.accessors[accessorIdx].count;
}

static size_t accessorStride(const tinygltf::Model& model, int accessorIdx) {
    const auto& acc = model.accessors[accessorIdx];
    const auto& bv  = model.bufferViews[acc.bufferView];
    if (bv.byteStride > 0) return bv.byteStride;
    switch (acc.type) {
        case TINYGLTF_TYPE_SCALAR: return sizeof(float);
        case TINYGLTF_TYPE_VEC2:   return sizeof(float) * 2;
        case TINYGLTF_TYPE_VEC3:   return sizeof(float) * 3;
        case TINYGLTF_TYPE_VEC4:   return sizeof(float) * 4;
        default:                   return sizeof(float);
    }
}

static void printUsage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s <input.glb> [-o output.glory]\n"
        "       %s <input_dir> <output_dir>  (batch mode)\n", prog, prog);
}

// ── Per-mesh cooking state ──────────────────────────────────────────────────

struct RawMesh {
    std::vector<glory::CookedVertex> vertices;
    std::vector<uint32_t>            indices;
    uint32_t                         materialIndex = 0;
    float aabbMin[3] = { std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max() };
    float aabbMax[3] = { std::numeric_limits<float>::lowest(),
                         std::numeric_limits<float>::lowest(),
                         std::numeric_limits<float>::lowest() };

    // LOD levels (base mesh is LOD 0, stored in indices above)
    struct LodData {
        std::vector<uint32_t> indices;
        float                 error = 0.0f;
    };
    std::vector<LodData> lods;  // LOD 1, 2, 3 (50%, 25%, 12.5%)
};

// ── Meshoptimizer passes ────────────────────────────────────────────────────

static void optimizeMesh(RawMesh& mesh) {
    if (mesh.indices.empty() || mesh.vertices.empty()) return;

    size_t indexCount  = mesh.indices.size();
    size_t vertexCount = mesh.vertices.size();

    // 1. Vertex cache optimization (GPU pre/post-transform cache)
    meshopt_optimizeVertexCache(mesh.indices.data(), mesh.indices.data(),
                                indexCount, vertexCount);

    // 2. Overdraw optimization (reorder triangles to reduce overdraw)
    meshopt_optimizeOverdraw(mesh.indices.data(), mesh.indices.data(),
                             indexCount,
                             &mesh.vertices[0].position[0],
                             vertexCount, sizeof(glory::CookedVertex),
                             1.05f);  // threshold

    // 3. Vertex fetch optimization (reorder vertices for sequential access)
    std::vector<uint32_t> remap(vertexCount);
    size_t uniqueVertices = meshopt_optimizeVertexFetchRemap(
        remap.data(), mesh.indices.data(), indexCount, vertexCount);

    meshopt_remapIndexBuffer(mesh.indices.data(), mesh.indices.data(),
                             indexCount, remap.data());

    std::vector<glory::CookedVertex> newVerts(uniqueVertices);
    meshopt_remapVertexBuffer(newVerts.data(), mesh.vertices.data(),
                              vertexCount, sizeof(glory::CookedVertex),
                              remap.data());
    mesh.vertices = std::move(newVerts);
}

static void generateLODs(RawMesh& mesh) {
    if (mesh.indices.size() < 12) return;  // too few triangles

    size_t vertexCount = mesh.vertices.size();
    float scale = meshopt_simplifyScale(&mesh.vertices[0].position[0],
                                         vertexCount, sizeof(glory::CookedVertex));

    const float ratios[] = { 0.50f, 0.25f, 0.125f };

    for (float ratio : ratios) {
        size_t targetIndexCount = static_cast<size_t>(
            static_cast<float>(mesh.indices.size()) * ratio);
        // Round down to multiple of 3
        targetIndexCount = (targetIndexCount / 3) * 3;
        if (targetIndexCount < 3) targetIndexCount = 3;

        float resultError = 0.0f;
        std::vector<uint32_t> lodIndices(mesh.indices.size());
        size_t lodIndexCount = meshopt_simplify(
            lodIndices.data(), mesh.indices.data(), mesh.indices.size(),
            &mesh.vertices[0].position[0], vertexCount,
            sizeof(glory::CookedVertex),
            targetIndexCount, 0.02f, 0, &resultError);

        if (lodIndexCount == 0) break;  // simplification failed

        lodIndices.resize(lodIndexCount);

        // Optimize the LOD indices for GPU cache
        meshopt_optimizeVertexCache(lodIndices.data(), lodIndices.data(),
                                     lodIndexCount, vertexCount);

        mesh.lods.push_back({std::move(lodIndices), resultError * scale});
    }
}

// ── Single-file cook ────────────────────────────────────────────────────────

static int cookFile(const std::string& inputPath, const std::string& outputPath) {
    // ── Load GLB ────────────────────────────────────────────────────────────
    tinygltf::Model gltfModel;
    tinygltf::TinyGLTF loader;
    std::string warn, err;

    std::fprintf(stdout, "Loading %s ...\n", inputPath.c_str());

    bool ok = loader.LoadBinaryFromFile(&gltfModel, &err, &warn, inputPath);
    if (!warn.empty()) std::fprintf(stderr, "Warning: %s\n", warn.c_str());
    if (!ok) {
        std::fprintf(stderr, "Error loading GLB: %s\n", err.c_str());
        return 1;
    }

    // ── Extract meshes ──────────────────────────────────────────────────────
    std::vector<RawMesh> rawMeshes;

    for (const auto& mesh : gltfModel.meshes) {
        for (const auto& prim : mesh.primitives) {
            if (prim.mode != TINYGLTF_MODE_TRIANGLES && prim.mode != -1)
                continue;

            RawMesh raw{};
            raw.materialIndex = (prim.material >= 0)
                ? static_cast<uint32_t>(prim.material) : 0;

            // ── Positions (required) ────────────────────────────────────────
            auto posIt = prim.attributes.find("POSITION");
            if (posIt == prim.attributes.end()) {
                std::fprintf(stderr, "  Skipping primitive without POSITION\n");
                continue;
            }
            size_t vertCount = accessorCount(gltfModel, posIt->second);
            const uint8_t* posBytes  = accessorData(gltfModel, posIt->second);
            size_t posStride = accessorStride(gltfModel, posIt->second);

            raw.vertices.resize(vertCount);
            for (size_t vi = 0; vi < vertCount; ++vi) {
                auto& v = raw.vertices[vi];
                std::memcpy(v.position, posBytes + vi * posStride, sizeof(float) * 3);
                v.color[0] = 1.0f; v.color[1] = 1.0f; v.color[2] = 1.0f;
                for (int a = 0; a < 3; ++a) {
                    raw.aabbMin[a] = std::fmin(raw.aabbMin[a], v.position[a]);
                    raw.aabbMax[a] = std::fmax(raw.aabbMax[a], v.position[a]);
                }
            }

            // ── Normals (optional) ──────────────────────────────────────────
            auto normIt = prim.attributes.find("NORMAL");
            if (normIt != prim.attributes.end()) {
                const uint8_t* normBytes = accessorData(gltfModel, normIt->second);
                size_t normStride = accessorStride(gltfModel, normIt->second);
                for (size_t vi = 0; vi < vertCount; ++vi) {
                    std::memcpy(raw.vertices[vi].normal,
                                normBytes + vi * normStride, sizeof(float) * 3);
                }
            }

            // ── Tex coords (optional) ───────────────────────────────────────
            auto uvIt = prim.attributes.find("TEXCOORD_0");
            if (uvIt != prim.attributes.end()) {
                const uint8_t* uvBytes = accessorData(gltfModel, uvIt->second);
                size_t uvStride = accessorStride(gltfModel, uvIt->second);
                for (size_t vi = 0; vi < vertCount; ++vi) {
                    std::memcpy(raw.vertices[vi].texCoord,
                                uvBytes + vi * uvStride, sizeof(float) * 2);
                }
            }

            // ── Vertex colors (optional) ────────────────────────────────────
            auto colIt = prim.attributes.find("COLOR_0");
            if (colIt != prim.attributes.end()) {
                const auto& colAcc = gltfModel.accessors[colIt->second];
                const uint8_t* colBytes = accessorData(gltfModel, colIt->second);
                size_t colStride = accessorStride(gltfModel, colIt->second);
                int components = (colAcc.type == TINYGLTF_TYPE_VEC4) ? 4 : 3;
                for (size_t vi = 0; vi < vertCount; ++vi) {
                    const float* c = reinterpret_cast<const float*>(
                        colBytes + vi * colStride);
                    raw.vertices[vi].color[0] = c[0];
                    raw.vertices[vi].color[1] = c[1];
                    raw.vertices[vi].color[2] = (components >= 3) ? c[2] : 1.0f;
                }
            }

            // ── Indices ─────────────────────────────────────────────────────
            if (prim.indices >= 0) {
                const auto& idxAcc = gltfModel.accessors[prim.indices];
                const uint8_t* idxBytes = accessorData(gltfModel, prim.indices);
                size_t idxCount = idxAcc.count;
                raw.indices.resize(idxCount);

                switch (idxAcc.componentType) {
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                        for (size_t ii = 0; ii < idxCount; ++ii)
                            raw.indices[ii] = reinterpret_cast<const uint16_t*>(idxBytes)[ii];
                        break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                        std::memcpy(raw.indices.data(), idxBytes,
                                    idxCount * sizeof(uint32_t));
                        break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                        for (size_t ii = 0; ii < idxCount; ++ii)
                            raw.indices[ii] = idxBytes[ii];
                        break;
                    default:
                        std::fprintf(stderr, "  Unsupported index type: %d\n",
                                     idxAcc.componentType);
                        continue;
                }
            } else {
                raw.indices.resize(vertCount);
                for (uint32_t ii = 0; ii < static_cast<uint32_t>(vertCount); ++ii)
                    raw.indices[ii] = ii;
            }

            // ── Meshoptimizer passes ────────────────────────────────────────
            optimizeMesh(raw);
            generateLODs(raw);

            std::fprintf(stdout, "  Mesh %zu: %zu verts, %zu tris, %zu LODs\n",
                         rawMeshes.size(), raw.vertices.size(),
                         raw.indices.size() / 3, raw.lods.size());

            rawMeshes.push_back(std::move(raw));
        }
    }

    if (rawMeshes.empty()) {
        std::fprintf(stderr, "Error: no meshes found in %s\n", inputPath.c_str());
        return 1;
    }

    // ── Extract materials ───────────────────────────────────────────────────
    std::vector<glory::MaterialDescriptor> materials;
    for (const auto& mat : gltfModel.materials) {
        glory::MaterialDescriptor md{};
        const auto& pbr = mat.pbrMetallicRoughness;
        for (int c = 0; c < 4; ++c)
            md.baseColor[c] = static_cast<float>(pbr.baseColorFactor[c]);
        md.metallic  = static_cast<float>(pbr.metallicFactor);
        md.roughness = static_cast<float>(pbr.roughnessFactor);
        md.emissive  = static_cast<float>(
            mat.emissiveFactor.empty() ? 0.0
                : (mat.emissiveFactor[0] + mat.emissiveFactor[1] + mat.emissiveFactor[2]) / 3.0);
        md.shininess = 1.0f - md.roughness;

        std::memset(md.diffuseTexturePath, 0, sizeof(md.diffuseTexturePath));
        std::memset(md.normalTexturePath, 0, sizeof(md.normalTexturePath));

        if (pbr.baseColorTexture.index >= 0) {
            int texIdx = pbr.baseColorTexture.index;
            if (texIdx < static_cast<int>(gltfModel.textures.size())) {
                int imgIdx = gltfModel.textures[texIdx].source;
                if (imgIdx >= 0 && imgIdx < static_cast<int>(gltfModel.images.size())) {
                    const auto& uri = gltfModel.images[imgIdx].uri;
                    if (!uri.empty()) {
                        std::strncpy(md.diffuseTexturePath, uri.c_str(),
                                     sizeof(md.diffuseTexturePath) - 1);
                    }
                }
            }
        }

        if (mat.normalTexture.index >= 0) {
            int texIdx = mat.normalTexture.index;
            if (texIdx < static_cast<int>(gltfModel.textures.size())) {
                int imgIdx = gltfModel.textures[texIdx].source;
                if (imgIdx >= 0 && imgIdx < static_cast<int>(gltfModel.images.size())) {
                    const auto& uri = gltfModel.images[imgIdx].uri;
                    if (!uri.empty()) {
                        std::strncpy(md.normalTexturePath, uri.c_str(),
                                     sizeof(md.normalTexturePath) - 1);
                    }
                }
            }
        }

        materials.push_back(md);
    }

    if (materials.empty()) {
        glory::MaterialDescriptor def{};
        def.baseColor[0] = 1.0f; def.baseColor[1] = 1.0f;
        def.baseColor[2] = 1.0f; def.baseColor[3] = 1.0f;
        def.metallic = 0.0f; def.roughness = 0.5f;
        def.emissive = 0.0f; def.shininess = 0.5f;
        std::memset(def.diffuseTexturePath, 0, sizeof(def.diffuseTexturePath));
        std::memset(def.normalTexturePath, 0, sizeof(def.normalTexturePath));
        materials.push_back(def);
    }

    // ── Build packed buffers ────────────────────────────────────────────────
    std::vector<glory::CookedVertex>   allVertices;
    std::vector<uint32_t>              allIndices;
    std::vector<glory::MeshDescriptor> meshDescs;
    std::vector<glory::LodLevel>       allLods;

    for (const auto& raw : rawMeshes) {
        glory::MeshDescriptor desc{};
        desc.vertexOffset  = static_cast<uint32_t>(allVertices.size());
        desc.vertexCount   = static_cast<uint32_t>(raw.vertices.size());
        desc.indexOffset   = static_cast<uint32_t>(allIndices.size());
        desc.indexCount    = static_cast<uint32_t>(raw.indices.size());
        desc.materialIndex = raw.materialIndex;
        desc.isSkinned     = 0;
        desc.lodCount      = static_cast<uint32_t>(raw.lods.size());
        std::memcpy(desc.aabbMin, raw.aabbMin, sizeof(float) * 3);
        std::memcpy(desc.aabbMax, raw.aabbMax, sizeof(float) * 3);

        allVertices.insert(allVertices.end(),
                           raw.vertices.begin(), raw.vertices.end());
        allIndices.insert(allIndices.end(),
                          raw.indices.begin(), raw.indices.end());

        // Append LOD index data and descriptors
        for (const auto& lod : raw.lods) {
            glory::LodLevel ll{};
            ll.indexOffset = static_cast<uint32_t>(allIndices.size());
            ll.indexCount  = static_cast<uint32_t>(lod.indices.size());
            ll.error       = lod.error;
            allLods.push_back(ll);

            allIndices.insert(allIndices.end(),
                              lod.indices.begin(), lod.indices.end());
        }

        meshDescs.push_back(desc);
    }

    // ── Compute layout & write file ─────────────────────────────────────────
    glory::AssetHeader header{};
    header.meshCount     = static_cast<uint32_t>(meshDescs.size());
    header.materialCount = static_cast<uint32_t>(materials.size());

    uint64_t cursor = sizeof(glory::AssetHeader);

    header.vertexDataOffset = cursor;
    header.vertexDataSize   = allVertices.size() * sizeof(glory::CookedVertex);
    cursor += header.vertexDataSize;

    header.indexDataOffset = cursor;
    header.indexDataSize   = allIndices.size() * sizeof(uint32_t);
    cursor += header.indexDataSize;

    header.meshDescOffset = cursor;
    header.meshDescSize   = meshDescs.size() * sizeof(glory::MeshDescriptor);
    cursor += header.meshDescSize;

    header.lodDescOffset = cursor;
    header.lodDescSize   = allLods.size() * sizeof(glory::LodLevel);
    cursor += header.lodDescSize;

    header.materialDataOffset = cursor;
    header.materialDataSize   = materials.size() * sizeof(glory::MaterialDescriptor);
    cursor += header.materialDataSize;

    // Skeleton / animation not yet supported in cook
    header.skeletonDataOffset = 0;
    header.skeletonDataSize   = 0;
    header.animDataOffset     = 0;
    header.animDataSize       = 0;

    std::ofstream out(outputPath, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "Error: cannot open output %s\n", outputPath.c_str());
        return 1;
    }

    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(allVertices.data()),
              static_cast<std::streamsize>(header.vertexDataSize));
    out.write(reinterpret_cast<const char*>(allIndices.data()),
              static_cast<std::streamsize>(header.indexDataSize));
    out.write(reinterpret_cast<const char*>(meshDescs.data()),
              static_cast<std::streamsize>(header.meshDescSize));
    if (!allLods.empty()) {
        out.write(reinterpret_cast<const char*>(allLods.data()),
                  static_cast<std::streamsize>(header.lodDescSize));
    }
    out.write(reinterpret_cast<const char*>(materials.data()),
              static_cast<std::streamsize>(header.materialDataSize));
    out.close();

    // ── Stats ───────────────────────────────────────────────────────────────
    std::ifstream inFile(inputPath, std::ios::binary | std::ios::ate);
    auto inputSize = inFile.tellg();
    inFile.close();

    std::ifstream outCheck(outputPath, std::ios::binary | std::ios::ate);
    auto outputSize = outCheck.tellg();
    outCheck.close();

    size_t totalLods = 0;
    for (const auto& m : rawMeshes) totalLods += m.lods.size();

    std::printf("\n=== Asset Cook Complete ===\n");
    std::printf("  Input:      %s\n", inputPath.c_str());
    std::printf("  Output:     %s\n", outputPath.c_str());
    std::printf("  Meshes:     %zu\n", meshDescs.size());
    std::printf("  LOD levels: %zu\n", totalLods);
    std::printf("  Materials:  %zu\n", materials.size());
    std::printf("  Vertices:   %zu (optimized)\n", allVertices.size());
    std::printf("  Indices:    %zu (base + LOD)\n", allIndices.size());
    std::printf("  Input size: %.2f KB\n", static_cast<double>(inputSize) / 1024.0);
    std::printf("  Output size:%.2f KB\n", static_cast<double>(outputSize) / 1024.0);
    if (inputSize > 0) {
        std::printf("  Ratio:      %.1f%%\n",
                    100.0 * static_cast<double>(outputSize) / static_cast<double>(inputSize));
    }

    return 0;
}

// ── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    namespace fs = std::filesystem;

    std::string arg1 = argv[1];

    // Batch mode: asset_cook <input_dir> <output_dir>
    if (fs::is_directory(arg1)) {
        if (argc < 3) {
            std::fprintf(stderr, "Error: batch mode requires output directory\n");
            printUsage(argv[0]);
            return 1;
        }
        std::string outDir = argv[2];
        fs::create_directories(outDir);

        int cooked = 0, failed = 0;
        for (const auto& entry : fs::recursive_directory_iterator(arg1)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".glb") continue;

            std::string outPath = (fs::path(outDir) /
                entry.path().stem()).string() + ".glory";
            std::printf("\n── Cooking: %s ──\n", entry.path().c_str());
            if (cookFile(entry.path().string(), outPath) == 0) ++cooked;
            else ++failed;
        }

        std::printf("\n=== Batch Complete: %d cooked, %d failed ===\n", cooked, failed);
        return (failed > 0) ? 1 : 0;
    }

    // Single-file mode: asset_cook <input.glb> [-o output.glory]
    std::string inputPath = arg1;
    std::string outputPath;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            outputPath = argv[++i];
        }
    }

    if (outputPath.empty()) {
        outputPath = inputPath;
        auto dot = outputPath.rfind('.');
        if (dot != std::string::npos) outputPath = outputPath.substr(0, dot);
        outputPath += ".glory";
    }

    return cookFile(inputPath, outputPath);
}
