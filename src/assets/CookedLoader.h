#pragma once
#include "assets/AssetFormat.h"
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace glory {

class Device; // forward declare

struct CookedLodData {
    uint32_t indexOffset;  // offset into the mesh's index buffer
    uint32_t indexCount;
    float    error;
};

struct CookedMeshData {
    std::vector<CookedVertex>     vertices;
    std::vector<CookedSkinVertex> skinVertices;
    std::vector<uint32_t>         indices;
    MeshDescriptor                descriptor;
    bool                          isSkinned = false;
    std::vector<CookedLodData>    lods;
};

struct CookedAssetData {
    AssetHeader                    header;
    std::vector<CookedMeshData>    meshes;
    std::vector<MaterialDescriptor> materials;
    // Skeleton and animation data stored as raw bytes for now
    std::vector<uint8_t>           skeletonBlob;
    std::vector<uint8_t>           animationBlob;
};

// RAII wrapper for memory-mapped file regions
class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& o) noexcept;
    MappedFile& operator=(MappedFile&& o) noexcept;

    bool open(const std::string& path);
    const uint8_t* data() const { return m_data; }
    size_t size() const { return m_size; }
    explicit operator bool() const { return m_data != nullptr; }

private:
    void close();
    uint8_t* m_data = nullptr;
    size_t   m_size = 0;
    int      m_fd   = -1;
};

class CookedLoader {
public:
    // Load a .glory binary asset file (uses mmap when available)
    static std::optional<CookedAssetData> load(const std::string& path);

    // Memory-map a .glory file — returns raw mapped region for zero-copy upload
    static MappedFile mmap(const std::string& path);

    // Validate file header without loading full data
    static bool validate(const std::string& path);
};

} // namespace glory
