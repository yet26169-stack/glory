#pragma once
#include "assets/AssetFormat.h"
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace glory {

class Device; // forward declare

struct CookedMeshData {
    std::vector<CookedVertex>     vertices;
    std::vector<CookedSkinVertex> skinVertices;
    std::vector<uint32_t>         indices;
    MeshDescriptor                descriptor;
    bool                          isSkinned = false;
};

struct CookedAssetData {
    AssetHeader                    header;
    std::vector<CookedMeshData>    meshes;
    std::vector<MaterialDescriptor> materials;
    // Skeleton and animation data stored as raw bytes for now
    std::vector<uint8_t>           skeletonBlob;
    std::vector<uint8_t>           animationBlob;
};

class CookedLoader {
public:
    // Load a .glory binary asset file
    static std::optional<CookedAssetData> load(const std::string& path);

    // Validate file header without loading full data
    static bool validate(const std::string& path);
};

} // namespace glory
