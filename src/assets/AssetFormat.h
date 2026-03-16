#pragma once
#include <cstdint>
#include <cstring>

namespace glory {

// Magic: "GLRY" in little-endian
static constexpr uint32_t GLORY_ASSET_MAGIC   = 0x59524C47;
static constexpr uint32_t GLORY_ASSET_VERSION = 1;

enum class AssetFlags : uint32_t {
    None         = 0,
    Compressed   = 1 << 0,  // zstd compressed
    HasAnimation = 1 << 1,
    HasSkeleton  = 1 << 2,
    HasNormalMap  = 1 << 3,
};

inline AssetFlags operator|(AssetFlags a, AssetFlags b) {
    return static_cast<AssetFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool operator&(AssetFlags a, AssetFlags b) {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

struct AssetHeader {
    uint32_t   magic          = GLORY_ASSET_MAGIC;
    uint32_t   version        = GLORY_ASSET_VERSION;
    AssetFlags flags          = AssetFlags::None;
    uint32_t   meshCount      = 0;
    uint32_t   materialCount  = 0;
    uint32_t   animationCount = 0;
    uint32_t   textureRefCount = 0;
    uint32_t   _pad0          = 0;

    // Section offsets (from file start)
    uint64_t vertexDataOffset   = 0;
    uint64_t vertexDataSize     = 0;
    uint64_t indexDataOffset    = 0;
    uint64_t indexDataSize      = 0;
    uint64_t meshDescOffset     = 0;   // array of MeshDescriptor
    uint64_t meshDescSize       = 0;
    uint64_t skeletonDataOffset = 0;
    uint64_t skeletonDataSize   = 0;
    uint64_t animDataOffset     = 0;
    uint64_t animDataSize       = 0;
    uint64_t materialDataOffset = 0;
    uint64_t materialDataSize   = 0;
    uint64_t lodDescOffset      = 0;   // array of LodLevel
    uint64_t lodDescSize        = 0;
};

// Vertex layout matching GPU format exactly — no conversion at load time
struct CookedVertex {
    float position[3];
    float color[3];
    float normal[3];
    float texCoord[2];
};

struct CookedSkinVertex {
    float    position[3];
    float    color[3];
    float    normal[3];
    float    texCoord[2];
    uint32_t boneIds[4];
    float    boneWeights[4];
};

// LOD level — shares vertex buffer with base mesh, different indices
struct LodLevel {
    uint32_t indexOffset;  // offset in global index array
    uint32_t indexCount;
    float    error;        // meshopt simplification error metric
    uint32_t _pad0 = 0;
};

struct MeshDescriptor {
    uint32_t vertexOffset;   // offset in vertex array
    uint32_t vertexCount;
    uint32_t indexOffset;    // offset in index array
    uint32_t indexCount;
    uint32_t materialIndex;
    uint32_t isSkinned;      // 0 = static, 1 = skinned
    uint32_t lodCount;       // number of LOD levels (0 = base only)
    uint32_t _pad0 = 0;
    float    aabbMin[3];
    float    aabbMax[3];
};

struct MaterialDescriptor {
    float baseColor[4];
    float metallic;
    float roughness;
    float emissive;
    float shininess;
    char  diffuseTexturePath[256];  // relative path or empty
    char  normalTexturePath[256];   // relative path or empty
};

} // namespace glory
