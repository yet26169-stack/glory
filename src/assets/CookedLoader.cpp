#include "assets/CookedLoader.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <cstring>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#define GLORY_HAS_MMAP 1
#else
#define GLORY_HAS_MMAP 0
#endif

namespace glory {

// ── MappedFile ──────────────────────────────────────────────────────────────

MappedFile::~MappedFile() { close(); }

MappedFile::MappedFile(MappedFile&& o) noexcept
    : m_data(o.m_data), m_size(o.m_size), m_fd(o.m_fd) {
    o.m_data = nullptr; o.m_size = 0; o.m_fd = -1;
}

MappedFile& MappedFile::operator=(MappedFile&& o) noexcept {
    if (this != &o) {
        close();
        m_data = o.m_data; m_size = o.m_size; m_fd = o.m_fd;
        o.m_data = nullptr; o.m_size = 0; o.m_fd = -1;
    }
    return *this;
}

bool MappedFile::open(const std::string& path) {
    close();
#if GLORY_HAS_MMAP
    m_fd = ::open(path.c_str(), O_RDONLY);
    if (m_fd < 0) return false;

    struct stat st{};
    if (fstat(m_fd, &st) != 0) { ::close(m_fd); m_fd = -1; return false; }
    m_size = static_cast<size_t>(st.st_size);
    if (m_size == 0) { ::close(m_fd); m_fd = -1; return false; }

    void* ptr = ::mmap(nullptr, m_size, PROT_READ, MAP_PRIVATE, m_fd, 0);
    if (ptr == MAP_FAILED) { ::close(m_fd); m_fd = -1; return false; }
    m_data = static_cast<uint8_t*>(ptr);
    return true;
#else
    // Fallback: read entire file into memory
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;
    m_size = static_cast<size_t>(file.tellg());
    if (m_size == 0) return false;
    file.seekg(0);
    m_data = new uint8_t[m_size];
    file.read(reinterpret_cast<char*>(m_data), static_cast<std::streamsize>(m_size));
    return file.good();
#endif
}

void MappedFile::close() {
    if (!m_data) return;
#if GLORY_HAS_MMAP
    ::munmap(m_data, m_size);
    if (m_fd >= 0) ::close(m_fd);
    m_fd = -1;
#else
    delete[] m_data;
#endif
    m_data = nullptr;
    m_size = 0;
}

// ── CookedLoader ────────────────────────────────────────────────────────────

static bool validateHeader(const AssetHeader& header) {
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
    file.read(reinterpret_cast<char*>(&header), sizeof(AssetHeader));
    return file.good() && validateHeader(header);
}

MappedFile CookedLoader::mmap(const std::string& path) {
    MappedFile mf;
    if (!mf.open(path)) {
        spdlog::error("CookedLoader: failed to mmap '{}'", path);
        return {};
    }
    if (mf.size() < sizeof(AssetHeader)) {
        spdlog::error("CookedLoader: file too small for header");
        return {};
    }
    const auto& header = *reinterpret_cast<const AssetHeader*>(mf.data());
    if (!validateHeader(header)) return {};
    return mf;
}

std::optional<CookedAssetData> CookedLoader::load(const std::string& path) {
    // Use mmap for zero-copy reads
    MappedFile mf;
    if (!mf.open(path)) {
        spdlog::error("CookedLoader: cannot open '{}'", path);
        return std::nullopt;
    }
    if (mf.size() < sizeof(AssetHeader)) {
        spdlog::error("CookedLoader: file too small");
        return std::nullopt;
    }

    CookedAssetData asset{};
    std::memcpy(&asset.header, mf.data(), sizeof(AssetHeader));
    if (!validateHeader(asset.header)) return std::nullopt;

    const auto& h   = asset.header;
    const auto* base = mf.data();

    // ── Mesh descriptors ────────────────────────────────────────────────────
    if (h.meshDescSize > 0) {
        size_t count = h.meshDescSize / sizeof(MeshDescriptor);
        const auto* descs = reinterpret_cast<const MeshDescriptor*>(base + h.meshDescOffset);

        // LOD descriptors (flat array, indexed by cumulative lodCount)
        const LodLevel* lodDescs = nullptr;
        if (h.lodDescSize > 0) {
            lodDescs = reinterpret_cast<const LodLevel*>(base + h.lodDescOffset);
        }

        const auto* vertexBlob = base + h.vertexDataOffset;
        const auto* indexBlob  = reinterpret_cast<const uint32_t*>(base + h.indexDataOffset);

        asset.meshes.resize(count);
        size_t lodCursor = 0;

        for (size_t i = 0; i < count; ++i) {
            auto& mesh = asset.meshes[i];
            mesh.descriptor = descs[i];
            mesh.isSkinned  = (descs[i].isSkinned != 0);

            // Indices
            if (descs[i].indexCount > 0) {
                mesh.indices.assign(
                    indexBlob + descs[i].indexOffset,
                    indexBlob + descs[i].indexOffset + descs[i].indexCount);
            }

            // Vertices
            if (mesh.isSkinned) {
                const auto* src = reinterpret_cast<const CookedSkinVertex*>(
                    vertexBlob + descs[i].vertexOffset * sizeof(CookedSkinVertex));
                mesh.skinVertices.assign(src, src + descs[i].vertexCount);
            } else {
                const auto* src = reinterpret_cast<const CookedVertex*>(
                    vertexBlob + descs[i].vertexOffset * sizeof(CookedVertex));
                mesh.vertices.assign(src, src + descs[i].vertexCount);
            }

            // LODs
            if (descs[i].lodCount > 0 && lodDescs) {
                mesh.lods.resize(descs[i].lodCount);
                for (uint32_t l = 0; l < descs[i].lodCount; ++l) {
                    const auto& ll = lodDescs[lodCursor + l];
                    mesh.lods[l] = { ll.indexOffset, ll.indexCount, ll.error };
                }
                lodCursor += descs[i].lodCount;
            }
        }
    }

    // ── Material descriptors ────────────────────────────────────────────────
    if (h.materialDataSize > 0) {
        size_t matCount = h.materialDataSize / sizeof(MaterialDescriptor);
        const auto* src = reinterpret_cast<const MaterialDescriptor*>(base + h.materialDataOffset);
        asset.materials.assign(src, src + matCount);
    }

    // ── Skeleton blob ───────────────────────────────────────────────────────
    if (h.skeletonDataSize > 0) {
        asset.skeletonBlob.assign(
            base + h.skeletonDataOffset,
            base + h.skeletonDataOffset + h.skeletonDataSize);
    }

    // ── Animation blob ──────────────────────────────────────────────────────
    if (h.animDataSize > 0) {
        asset.animationBlob.assign(
            base + h.animDataOffset,
            base + h.animDataOffset + h.animDataSize);
    }

    spdlog::info("CookedLoader: loaded '{}' — {} meshes, {} materials (mmap)",
                 path, asset.meshes.size(), asset.materials.size());
    return asset;
}

} // namespace glory
