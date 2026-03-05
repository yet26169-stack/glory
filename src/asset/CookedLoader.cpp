#include "asset/CookedLoader.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <cstring>

namespace glory {

bool CookedLoader::loadMesh(const std::string& path, MeshData& /*outMesh*/) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        spdlog::error("CookedLoader::loadMesh: cannot open '{}'", path);
        return false;
    }
    CookedAssetHeader hdr;
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (hdr.magic != 0x59524C47u) {
        spdlog::error("CookedLoader::loadMesh: bad magic in '{}'", path);
        return false;
    }
    // TODO: deserialise mesh blobs (vertex + index data) from the cooked file.
    spdlog::warn("CookedLoader::loadMesh: stub — mesh data not yet populated ('{}')", path);
    return true;
}

bool CookedLoader::loadTexture(const std::string& path,
                                std::vector<uint8_t>& outPixels,
                                uint32_t& outW, uint32_t& outH) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        spdlog::error("CookedLoader::loadTexture: cannot open '{}'", path);
        return false;
    }
    CookedAssetHeader hdr;
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (hdr.magic != 0x59524C47u) {
        spdlog::error("CookedLoader::loadTexture: bad magic in '{}'", path);
        return false;
    }
    // TODO: read texture dimensions + pixel data (basis_universal decode if compressed).
    outW = 0; outH = 0; outPixels.clear();
    spdlog::warn("CookedLoader::loadTexture: stub — texture data not yet populated ('{}')", path);
    return true;
}

} // namespace glory
