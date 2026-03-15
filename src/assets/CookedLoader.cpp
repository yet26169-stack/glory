#include "assets/CookedLoader.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <cstring>

namespace glory {

static bool readHeader(std::ifstream& file, AssetHeader& header) {
    file.read(reinterpret_cast<char*>(&header), sizeof(AssetHeader));
    if (!file) return false;
    if (header.magic != GLORY_ASSET_MAGIC) {
        spdlog::error("CookedLoader: bad magic 0x{:08X} (expected 0x{:08X})",
                       header.magic, GLORY_ASSET_MAGIC);
        return false;
    }
    if (header.version != GLORY_ASSET_VERSION) {
        spdlog::error("CookedLoader: unsupported version {} (expected {})",
                       header.version, GLORY_ASSET_VERSION);
        return false;
    }
    return true;
}

bool CookedLoader::validate(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    AssetHeader header{};
    return readHeader(file, header);
}

std::optional<CookedAssetData> CookedLoader::load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        spdlog::error("CookedLoader: cannot open '{}'", path);
        return std::nullopt;
    }

    CookedAssetData asset{};
    if (!readHeader(file, asset.header)) return std::nullopt;

    const auto& h = asset.header;

    // ── Mesh descriptors ────────────────────────────────────────────────────
    if (h.meshDescSize > 0) {
        size_t count = h.meshDescSize / sizeof(MeshDescriptor);
        std::vector<MeshDescriptor> descs(count);
        file.seekg(static_cast<std::streamoff>(h.meshDescOffset));
        file.read(reinterpret_cast<char*>(descs.data()),
                  static_cast<std::streamsize>(h.meshDescSize));
        if (!file) {
            spdlog::error("CookedLoader: failed reading mesh descriptors");
            return std::nullopt;
        }

        // Read all vertex data into a single buffer
        std::vector<uint8_t> vertexBlob(h.vertexDataSize);
        if (h.vertexDataSize > 0) {
            file.seekg(static_cast<std::streamoff>(h.vertexDataOffset));
            file.read(reinterpret_cast<char*>(vertexBlob.data()),
                      static_cast<std::streamsize>(h.vertexDataSize));
            if (!file) {
                spdlog::error("CookedLoader: failed reading vertex data");
                return std::nullopt;
            }
        }

        // Read all index data into a single buffer
        std::vector<uint32_t> indexBlob(h.indexDataSize / sizeof(uint32_t));
        if (h.indexDataSize > 0) {
            file.seekg(static_cast<std::streamoff>(h.indexDataOffset));
            file.read(reinterpret_cast<char*>(indexBlob.data()),
                      static_cast<std::streamsize>(h.indexDataSize));
            if (!file) {
                spdlog::error("CookedLoader: failed reading index data");
                return std::nullopt;
            }
        }

        // Split into per-mesh data
        asset.meshes.resize(count);
        for (size_t i = 0; i < count; ++i) {
            auto& mesh = asset.meshes[i];
            mesh.descriptor = descs[i];
            mesh.isSkinned  = (descs[i].isSkinned != 0);

            // Indices
            if (descs[i].indexCount > 0) {
                mesh.indices.assign(
                    indexBlob.begin() + descs[i].indexOffset,
                    indexBlob.begin() + descs[i].indexOffset + descs[i].indexCount);
            }

            // Vertices
            if (mesh.isSkinned) {
                const auto* src = reinterpret_cast<const CookedSkinVertex*>(
                    vertexBlob.data() + descs[i].vertexOffset * sizeof(CookedSkinVertex));
                mesh.skinVertices.assign(src, src + descs[i].vertexCount);
            } else {
                const auto* src = reinterpret_cast<const CookedVertex*>(
                    vertexBlob.data() + descs[i].vertexOffset * sizeof(CookedVertex));
                mesh.vertices.assign(src, src + descs[i].vertexCount);
            }
        }
    }

    // ── Material descriptors ────────────────────────────────────────────────
    if (h.materialDataSize > 0) {
        size_t matCount = h.materialDataSize / sizeof(MaterialDescriptor);
        asset.materials.resize(matCount);
        file.seekg(static_cast<std::streamoff>(h.materialDataOffset));
        file.read(reinterpret_cast<char*>(asset.materials.data()),
                  static_cast<std::streamsize>(h.materialDataSize));
        if (!file) {
            spdlog::error("CookedLoader: failed reading material descriptors");
            return std::nullopt;
        }
    }

    // ── Skeleton blob ───────────────────────────────────────────────────────
    if (h.skeletonDataSize > 0) {
        asset.skeletonBlob.resize(h.skeletonDataSize);
        file.seekg(static_cast<std::streamoff>(h.skeletonDataOffset));
        file.read(reinterpret_cast<char*>(asset.skeletonBlob.data()),
                  static_cast<std::streamsize>(h.skeletonDataSize));
    }

    // ── Animation blob ──────────────────────────────────────────────────────
    if (h.animDataSize > 0) {
        asset.animationBlob.resize(h.animDataSize);
        file.seekg(static_cast<std::streamoff>(h.animDataOffset));
        file.read(reinterpret_cast<char*>(asset.animationBlob.data()),
                  static_cast<std::streamsize>(h.animDataSize));
    }

    spdlog::info("CookedLoader: loaded '{}' — {} meshes, {} materials",
                 path, asset.meshes.size(), asset.materials.size());
    return asset;
}

} // namespace glory
