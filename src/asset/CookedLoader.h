#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace glory {

struct MeshData; // forward-declared; defined in renderer headers

/// Binary cooked asset format header.
/// Magic: "GLRY" (0x47 0x4C 0x52 0x59)
struct CookedAssetHeader {
    uint32_t magic        = 0x59524C47u; // "GLRY" little-endian
    uint32_t version      = 1;
    uint32_t meshCount    = 0;
    uint32_t textureCount = 0;
};

/// Loads pre-cooked binary assets produced by the offline cook pipeline.
/// Avoids runtime GLB/OBJ parsing; enables basis_universal texture compression.
class CookedLoader {
public:
    bool loadMesh   (const std::string& path, MeshData& outMesh);
    bool loadTexture(const std::string& path, std::vector<uint8_t>& outPixels,
                     uint32_t& outW, uint32_t& outH);
};

} // namespace glory
