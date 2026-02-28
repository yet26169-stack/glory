// GLBLoader.cpp – glTF/GLB loading via tinygltf
// Adds Model::loadFromGLB() and Model::loadGLBTextures()
//
// tinygltf needs its implementation defined exactly once, but stb_image is
// already compiled in Texture.cpp, so we suppress the duplicate.

// Tell tinygltf we'll provide json ourselves (ours is at nlohmann/json.hpp)
#define TINYGLTF_NO_INCLUDE_JSON
// Don't write images
#define TINYGLTF_NO_STB_IMAGE_WRITE
// Implementation goes in this .cpp only
#define TINYGLTF_IMPLEMENTATION

// Provide our json header
#include <nlohmann/json.hpp>

// stb_image.h is at extern/stb/ - tinygltf will include it for us
// STB_IMAGE_IMPLEMENTATION is already defined in Texture.cpp so we don't define
// it here

#include <tiny_gltf.h>

#include "renderer/Buffer.h"
#include "renderer/Device.h"
#include "renderer/Model.h"
#include "renderer/Texture.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <stdexcept>
#include <vector>

namespace glory {

// ── Helper: read an accessor's data as a typed span ───────────────────────
static const uint8_t *accessorData(const tinygltf::Model &gltfModel,
                                   int accessorIdx) {
  const auto &acc = gltfModel.accessors[accessorIdx];
  const auto &bv = gltfModel.bufferViews[acc.bufferView];
  const auto &buffer = gltfModel.buffers[bv.buffer];
  return buffer.data.data() + bv.byteOffset + acc.byteOffset;
}

static size_t accessorCount(const tinygltf::Model &gltfModel, int idx) {
  return gltfModel.accessors[idx].count;
}

// ── Model::loadFromGLB ──────────────────────────────────────────────────────
Model Model::loadFromGLB(const Device &device, VmaAllocator allocator,
                         const std::string &filepath) {
  tinygltf::TinyGLTF loader;
  tinygltf::Model gltfModel;
  std::string err, warn;

  bool ok = loader.LoadBinaryFromFile(&gltfModel, &err, &warn, filepath);
  if (!warn.empty())
    spdlog::warn("glTF warn: {}", warn);
  if (!ok)
    throw std::runtime_error("Failed to load GLB: " + filepath + " " + err);

  Model model;

  for (const auto &mesh : gltfModel.meshes) {
    for (const auto &prim : mesh.primitives) {
      // ── Positions (required) ────────────────────────────────────
      auto posIt = prim.attributes.find("POSITION");
      if (posIt == prim.attributes.end()) {
        spdlog::warn("glTF mesh '{}': primitive has no POSITION, skipping",
                     mesh.name);
        continue;
      }

      const size_t vertCount = accessorCount(gltfModel, posIt->second);
      const auto *posData = reinterpret_cast<const float *>(
          accessorData(gltfModel, posIt->second));

      // Byte strides – fall back to tightly packed if bufferView has no stride
      auto getStride = [&](int accIdx, size_t defaultSize) -> size_t {
        const auto &bv =
            gltfModel.bufferViews[gltfModel.accessors[accIdx].bufferView];
        return bv.byteStride > 0 ? bv.byteStride : defaultSize;
      };

      size_t posStride = getStride(posIt->second, sizeof(float) * 3);

      // ── Normals (optional) ──────────────────────────────────────
      const uint8_t *normRaw = nullptr;
      size_t normStride = sizeof(float) * 3;
      auto normIt = prim.attributes.find("NORMAL");
      if (normIt != prim.attributes.end()) {
        normRaw = accessorData(gltfModel, normIt->second);
        normStride = getStride(normIt->second, sizeof(float) * 3);
      }

      // ── UVs (optional, set 0) ───────────────────────────────────
      const uint8_t *uvRaw = nullptr;
      size_t uvStride = sizeof(float) * 2;
      auto uvIt = prim.attributes.find("TEXCOORD_0");
      if (uvIt != prim.attributes.end()) {
        uvRaw = accessorData(gltfModel, uvIt->second);
        uvStride = getStride(uvIt->second, sizeof(float) * 2);
      }

      // ── Build vertex array ──────────────────────────────────────
      std::vector<Vertex> vertices(vertCount);
      const uint8_t *posBytes = reinterpret_cast<const uint8_t *>(posData);

      for (size_t i = 0; i < vertCount; ++i) {
        auto &v = vertices[i];

        // Position (3 floats)
        std::memcpy(&v.position, posBytes + i * posStride, sizeof(float) * 3);

        // Color: white (glTF vertex colors are rare; we rely on textures)
        v.color = glm::vec3(1.0f);

        // Normal
        if (normRaw) {
          std::memcpy(&v.normal, normRaw + i * normStride, sizeof(float) * 3);
        } else {
          v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        }

        // UV
        if (uvRaw) {
          std::memcpy(&v.texCoord, uvRaw + i * uvStride, sizeof(float) * 2);
        } else {
          v.texCoord = glm::vec2(0.0f);
        }
      }

      // ── Indices ─────────────────────────────────────────────────
      std::vector<uint32_t> indices;
      if (prim.indices >= 0) {
        const auto &idxAcc = gltfModel.accessors[prim.indices];
        const auto *idxRaw = accessorData(gltfModel, prim.indices);
        indices.resize(idxAcc.count);

        // glTF indices may be uint8, uint16 or uint32
        switch (idxAcc.componentType) {
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
          for (size_t i = 0; i < idxAcc.count; ++i)
            indices[i] = reinterpret_cast<const uint16_t *>(idxRaw)[i];
          break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
          std::memcpy(indices.data(), idxRaw, idxAcc.count * sizeof(uint32_t));
          break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
          for (size_t i = 0; i < idxAcc.count; ++i)
            indices[i] = idxRaw[i];
          break;
        default:
          spdlog::error("glTF: unsupported index component type {}",
                        idxAcc.componentType);
          continue;
        }
      } else {
        // No index buffer – generate sequential indices
        indices.resize(vertCount);
        for (size_t i = 0; i < vertCount; ++i)
          indices[i] = static_cast<uint32_t>(i);
      }

      spdlog::info("Loaded GLB mesh '{}': {} vertices, {} indices", mesh.name,
                   vertices.size(), indices.size());
      model.m_meshes.emplace_back(device, allocator, vertices, indices);
    }
  }

  if (model.m_meshes.empty())
    throw std::runtime_error("GLB file contains no renderable meshes: " +
                             filepath);

  return model;
}

// ── Model::loadGLBTextures ──────────────────────────────────────────────────
std::vector<Texture> Model::loadGLBTextures(const Device &device,
                                            const std::string &filepath) {
  tinygltf::TinyGLTF loader;
  tinygltf::Model gltfModel;
  std::string err, warn;

  bool ok = loader.LoadBinaryFromFile(&gltfModel, &err, &warn, filepath);
  if (!ok) {
    spdlog::error("loadGLBTextures: failed to load {}: {}", filepath, err);
    return {};
  }

  std::vector<Texture> textures;

  // Iterate materials and pull the base-color texture from each
  for (const auto &mat : gltfModel.materials) {
    int texIdx = mat.pbrMetallicRoughness.baseColorTexture.index;
    if (texIdx < 0)
      continue;

    const auto &tex = gltfModel.textures[texIdx];
    if (tex.source < 0 ||
        tex.source >= static_cast<int>(gltfModel.images.size()))
      continue;

    const auto &img = gltfModel.images[tex.source];
    if (img.image.empty()) {
      spdlog::warn("glTF texture '{}' has no pixel data, skipping", img.name);
      continue;
    }

    // tinygltf decodes images to RGBA via stb_image internally
    // img.width, img.height, img.component, img.image (std::vector<uint8_t>)
    uint32_t w = static_cast<uint32_t>(img.width);
    uint32_t h = static_cast<uint32_t>(img.height);

    // If the image has 3 components (RGB), we need to expand to RGBA
    std::vector<uint8_t> rgbaData;
    const uint32_t *pixels = nullptr;

    if (img.component == 4) {
      pixels = reinterpret_cast<const uint32_t *>(img.image.data());
    } else if (img.component == 3) {
      rgbaData.resize(w * h * 4);
      for (uint32_t i = 0; i < w * h; ++i) {
        rgbaData[i * 4 + 0] = img.image[i * 3 + 0];
        rgbaData[i * 4 + 1] = img.image[i * 3 + 1];
        rgbaData[i * 4 + 2] = img.image[i * 3 + 2];
        rgbaData[i * 4 + 3] = 255;
      }
      pixels = reinterpret_cast<const uint32_t *>(rgbaData.data());
    } else {
      spdlog::warn("glTF image '{}' has {} components, skipping", img.name,
                   img.component);
      continue;
    }

    textures.push_back(Texture::createFromPixels(device, pixels, w, h));
    spdlog::info("Loaded GLB texture '{}' ({}x{}, {} components)", img.name, w,
                 h, img.component);
  }

  return textures;
}

} // namespace glory
