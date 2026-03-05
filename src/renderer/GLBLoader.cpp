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

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <meshoptimizer.h>

#include <cstring>
#include <stdexcept>
#include <unordered_map>
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
      int uvComponentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
      auto uvIt = prim.attributes.find("TEXCOORD_0");
      if (uvIt != prim.attributes.end()) {
        uvRaw = accessorData(gltfModel, uvIt->second);
        uvStride = getStride(uvIt->second, sizeof(float) * 2);
        uvComponentType = gltfModel.accessors[uvIt->second].componentType;
      }

      // Helper: decode one UV pair to float regardless of source packing
      auto decodeUV = [&](size_t i) -> glm::vec2 {
        if (!uvRaw) return glm::vec2(0.0f);
        const uint8_t* p = uvRaw + i * uvStride;
        if (uvComponentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
          glm::vec2 uv; std::memcpy(&uv, p, sizeof(glm::vec2)); return uv;
        } else if (uvComponentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
          const uint16_t* s = reinterpret_cast<const uint16_t*>(p);
          return glm::vec2(s[0] / 65535.0f, s[1] / 65535.0f);
        } else if (uvComponentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
          return glm::vec2(p[0] / 255.0f, p[1] / 255.0f);
        }
        return glm::vec2(0.0f);
      };

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
        v.texCoord = decodeUV(i);
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

      // ── Compute smooth normals if the GLB had none ───────────────
      if (!normRaw && indices.size() >= 3) {
        // Zero all normals
        for (auto &v : vertices)
          v.normal = glm::vec3(0.0f);
        // Accumulate face normals
        for (size_t fi = 0; fi + 2 < indices.size(); fi += 3) {
          const auto &p0 = vertices[indices[fi + 0]].position;
          const auto &p1 = vertices[indices[fi + 1]].position;
          const auto &p2 = vertices[indices[fi + 2]].position;
          glm::vec3 faceN = glm::cross(p1 - p0, p2 - p0);
          // Weight by area (unnormalized cross product length)
          vertices[indices[fi + 0]].normal += faceN;
          vertices[indices[fi + 1]].normal += faceN;
          vertices[indices[fi + 2]].normal += faceN;
        }
        // Normalize
        for (auto &v : vertices) {
          float len = glm::length(v.normal);
          v.normal = (len > 1e-8f) ? (v.normal / len) : glm::vec3(0.0f, 1.0f, 0.0f);
        }
        spdlog::info("Computed smooth normals for GLB mesh '{}'", mesh.name);
      }

      spdlog::info("Loaded GLB mesh '{}': {} vertices, {} indices", mesh.name,
                   vertices.size(), indices.size());
      model.m_meshes.emplace_back(device, allocator, vertices, indices);
      model.m_meshMaterialIndices.push_back(prim.material);
    }
  }

  if (model.m_meshes.empty())
    throw std::runtime_error("GLB file contains no renderable meshes: " +
                             filepath);

  return model;
}

// ── Model::loadGLBTextures ──────────────────────────────────────────────────
std::vector<Model::GLBTexture> Model::loadGLBTextures(const Device &device,
                                            const std::string &filepath) {
  tinygltf::TinyGLTF loader;
  tinygltf::Model gltfModel;
  std::string err, warn;

  bool ok = loader.LoadBinaryFromFile(&gltfModel, &err, &warn, filepath);
  if (!ok) {
    spdlog::error("loadGLBTextures: failed to load {}: {}", filepath, err);
    return {};
  }

  std::vector<GLBTexture> textures;

  // Iterate materials and pull the base-color texture from each
  for (int i = 0; i < static_cast<int>(gltfModel.materials.size()); ++i) {
    const auto &mat = gltfModel.materials[i];
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

    GLBTexture glbTex;
    glbTex.materialIndex = i;
    glbTex.texture = Texture::createFromPixels(device, pixels, w, h);
    textures.push_back(std::move(glbTex));
    spdlog::info("Loaded GLB texture '{}' for material {} ({}x{}, {} components)", img.name, i, w,
                 h, img.component);
  }

  return textures;
}

// ── Helper: get byte stride for an accessor ─────────────────────────────────
static size_t getAccessorStride(const tinygltf::Model &gltfModel, int accIdx,
                                size_t defaultSize) {
  const auto &bv =
      gltfModel.bufferViews[gltfModel.accessors[accIdx].bufferView];
  return bv.byteStride > 0 ? bv.byteStride : defaultSize;
}

// ── Helper: build parent-child map from glTF node hierarchy ─────────────────
static void buildParentMap(const tinygltf::Model &gltfModel,
                           std::vector<int> &parentOf) {
  parentOf.assign(gltfModel.nodes.size(), -1);
  for (int n = 0; n < static_cast<int>(gltfModel.nodes.size()); ++n) {
    for (int child : gltfModel.nodes[n].children) {
      if (child >= 0 && child < static_cast<int>(parentOf.size()))
        parentOf[child] = n;
    }
  }
}

// ── Model::loadSkinnedFromGLB ───────────────────────────────────────────────
SkinnedModelData Model::loadSkinnedFromGLB(const Device &device,
                                           VmaAllocator allocator,
                                           const std::string &filepath,
                                           float targetReduction) {
  tinygltf::TinyGLTF loader;
  tinygltf::Model gltfModel;
  std::string err, warn;

  bool ok = loader.LoadBinaryFromFile(&gltfModel, &err, &warn, filepath);
  if (!warn.empty())
    spdlog::warn("glTF warn (skinned): {}", warn);
  if (!ok)
    throw std::runtime_error("Failed to load skinned GLB: " + filepath + " " +
                             err);

  SkinnedModelData result;

  // ══════════════════════════════════════════════════════════════════════════
  // 1. Extract skeleton from skin[0]
  // ══════════════════════════════════════════════════════════════════════════
  if (gltfModel.skins.empty())
    throw std::runtime_error("GLB has no skin data: " + filepath);

  const auto &skin = gltfModel.skins[0];
  int jointCount = static_cast<int>(skin.joints.size());
  spdlog::info("Skinned GLB '{}': {} joints", filepath, jointCount);

  // Build node→parent map
  std::vector<int> nodeParent;
  buildParentMap(gltfModel, nodeParent);

  // Map glTF node index → joint index in our skeleton
  std::unordered_map<int, int> nodeToJoint;
  for (int j = 0; j < jointCount; ++j) {
    nodeToJoint[skin.joints[j]] = j;
  }

  // Read inverse bind matrices
  std::vector<glm::mat4> inverseBindMatrices(jointCount, glm::mat4(1.0f));
  if (skin.inverseBindMatrices >= 0) {
    const auto *ibmData = reinterpret_cast<const float *>(
        accessorData(gltfModel, skin.inverseBindMatrices));
    for (int j = 0; j < jointCount; ++j) {
      std::memcpy(&inverseBindMatrices[j], ibmData + j * 16,
                  sizeof(float) * 16);
    }
  }

  // ── Extract armature root transform ────────────────────────────────────
  // The armature node sits above the skeleton root in the scene graph but is
  // NOT listed in skin.joints.  Its TRS (typically a rotation from Blender
  // Z-up to glTF Y-up plus a unit-conversion scale) must be applied when
  // computing global joint transforms so that skinMatrix = armature * joint *
  // IBM produces correct results.
  {
    // Walk up from the first skin joint's node to find the first non-joint
    // ancestor — that is the armature node.
    int armatureNode = -1;
    if (!skin.joints.empty()) {
      int firstJointNode = skin.joints[0];
      int cursor = nodeParent[firstJointNode];
      while (cursor >= 0) {
        if (nodeToJoint.count(cursor) == 0) {
          armatureNode = cursor;
          break;
        }
        cursor = nodeParent[cursor];
      }
    }
    if (armatureNode >= 0) {
      const auto &aNode = gltfModel.nodes[armatureNode];
      // Build TRS matrix for armature node
      glm::vec3 aT(0.0f);
      glm::quat aR(1.0f, 0.0f, 0.0f, 0.0f);
      glm::vec3 aS(1.0f);
      if (aNode.translation.size() == 3) {
        aT = glm::vec3(static_cast<float>(aNode.translation[0]),
                        static_cast<float>(aNode.translation[1]),
                        static_cast<float>(aNode.translation[2]));
      }
      if (aNode.rotation.size() == 4) {
        aR = glm::quat(static_cast<float>(aNode.rotation[3]),
                        static_cast<float>(aNode.rotation[0]),
                        static_cast<float>(aNode.rotation[1]),
                        static_cast<float>(aNode.rotation[2]));
      }
      if (aNode.scale.size() == 3) {
        aS = glm::vec3(static_cast<float>(aNode.scale[0]),
                        static_cast<float>(aNode.scale[1]),
                        static_cast<float>(aNode.scale[2]));
      }
      glm::mat4 T = glm::translate(glm::mat4(1.0f), aT);
      glm::mat4 R = glm::toMat4(aR);
      glm::mat4 S = glm::scale(glm::mat4(1.0f), aS);
      result.skeleton.armatureTransform = T * R * S;
      spdlog::info("Armature '{}' transform extracted (scale {:.4f}, {:.4f}, {:.4f})",
                   aNode.name, aS.x, aS.y, aS.z);
    }
  }

  // Build joints with parent indices and rest-pose transforms
  result.skeleton.joints.resize(jointCount);
  result.skeleton.jointNodeIndices.resize(jointCount);

  for (int j = 0; j < jointCount; ++j) {
    int nodeIdx = skin.joints[j];
    const auto &node = gltfModel.nodes[nodeIdx];
    auto &joint = result.skeleton.joints[j];

    joint.name = node.name;
    joint.inverseBindMatrix = inverseBindMatrices[j];
    result.skeleton.jointNodeIndices[j] = nodeIdx;

    // Find parent joint: walk up glTF node tree until we hit another joint
    int parentNode = nodeParent[nodeIdx];
    joint.parentIndex = -1;
    while (parentNode >= 0) {
      auto it = nodeToJoint.find(parentNode);
      if (it != nodeToJoint.end()) {
        joint.parentIndex = it->second;
        break;
      }
      parentNode = nodeParent[parentNode];
    }

    // Rest pose from node TRS
    if (node.translation.size() == 3) {
      joint.localTranslation =
          glm::vec3(static_cast<float>(node.translation[0]),
                    static_cast<float>(node.translation[1]),
                    static_cast<float>(node.translation[2]));
    }
    if (node.rotation.size() == 4) {
      // glTF quaternion is (x,y,z,w), GLM constructor is (w,x,y,z)
      joint.localRotation = glm::quat(static_cast<float>(node.rotation[3]),
                                      static_cast<float>(node.rotation[0]),
                                      static_cast<float>(node.rotation[1]),
                                      static_cast<float>(node.rotation[2]));
    }
    if (node.scale.size() == 3) {
      joint.localScale = glm::vec3(static_cast<float>(node.scale[0]),
                                   static_cast<float>(node.scale[1]),
                                   static_cast<float>(node.scale[2]));
    }
  }

  // Topological sort: ensure parents come before children
  {
    std::vector<Joint> sorted(jointCount);
    std::vector<int> sortedNodeIndices(jointCount);
    std::vector<int> oldToNew(jointCount, -1);
    std::vector<bool> placed(jointCount, false);
    int outIdx = 0;

    // Simple iterative topological sort
    while (outIdx < jointCount) {
      bool progress = false;
      for (int j = 0; j < jointCount; ++j) {
        if (placed[j])
          continue;
        int parent = result.skeleton.joints[j].parentIndex;
        if (parent < 0 || placed[parent]) {
          oldToNew[j] = outIdx;
          sorted[outIdx] = result.skeleton.joints[j];
          sortedNodeIndices[outIdx] = result.skeleton.jointNodeIndices[j];
          placed[j] = true;
          ++outIdx;
          progress = true;
        }
      }
      if (!progress) {
        spdlog::warn("Skeleton has cycles, breaking topological sort");
        // Place remaining joints
        for (int j = 0; j < jointCount; ++j) {
          if (!placed[j]) {
            oldToNew[j] = outIdx;
            sorted[outIdx] = result.skeleton.joints[j];
            sortedNodeIndices[outIdx] = result.skeleton.jointNodeIndices[j];
            ++outIdx;
          }
        }
        break;
      }
    }

    // Remap parent indices
    for (int j = 0; j < jointCount; ++j) {
      int oldParent = sorted[j].parentIndex;
      sorted[j].parentIndex = (oldParent >= 0) ? oldToNew[oldParent] : -1;
    }

    result.skeleton.joints = std::move(sorted);
    result.skeleton.jointNodeIndices = std::move(sortedNodeIndices);

    // Update nodeToJoint mapping after sort
    nodeToJoint.clear();
    for (int j = 0; j < jointCount; ++j) {
      nodeToJoint[result.skeleton.jointNodeIndices[j]] = j;
    }
  }

  // ══════════════════════════════════════════════════════════════════════════
  // 2. Extract meshes with JOINTS_0 / WEIGHTS_0
  // ══════════════════════════════════════════════════════════════════════════
  for (const auto &mesh : gltfModel.meshes) {
    for (const auto &prim : mesh.primitives) {
      auto posIt = prim.attributes.find("POSITION");
      if (posIt == prim.attributes.end())
        continue;

      const size_t vertCount = accessorCount(gltfModel, posIt->second);
      size_t posStride =
          getAccessorStride(gltfModel, posIt->second, sizeof(float) * 3);
      const uint8_t *posBytes = accessorData(gltfModel, posIt->second);

      // Normals
      const uint8_t *normRaw = nullptr;
      size_t normStride = sizeof(float) * 3;
      auto normIt = prim.attributes.find("NORMAL");
      if (normIt != prim.attributes.end()) {
        normRaw = accessorData(gltfModel, normIt->second);
        normStride =
            getAccessorStride(gltfModel, normIt->second, sizeof(float) * 3);
      }

      // UVs
      const uint8_t *uvRaw = nullptr;
      size_t uvStride = sizeof(float) * 2;
      int uvComponentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
      auto uvIt = prim.attributes.find("TEXCOORD_0");
      if (uvIt != prim.attributes.end()) {
        uvRaw = accessorData(gltfModel, uvIt->second);
        uvStride =
            getAccessorStride(gltfModel, uvIt->second, sizeof(float) * 2);
        uvComponentType = gltfModel.accessors[uvIt->second].componentType;
      }

      // Helper: decode one UV pair to float regardless of source packing
      auto decodeUV = [&](size_t i) -> glm::vec2 {
        if (!uvRaw) return glm::vec2(0.0f);
        const uint8_t* p = uvRaw + i * uvStride;
        if (uvComponentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
          glm::vec2 uv; std::memcpy(&uv, p, sizeof(glm::vec2)); return uv;
        } else if (uvComponentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
          const uint16_t* s = reinterpret_cast<const uint16_t*>(p);
          return glm::vec2(s[0] / 65535.0f, s[1] / 65535.0f);
        } else if (uvComponentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
          return glm::vec2(p[0] / 255.0f, p[1] / 255.0f);
        }
        return glm::vec2(0.0f);
      };

      // Build vertices
      std::vector<Vertex> vertices(vertCount);
      for (size_t i = 0; i < vertCount; ++i) {
        auto &v = vertices[i];
        std::memcpy(&v.position, posBytes + i * posStride, sizeof(float) * 3);
        v.color = glm::vec3(1.0f);
        if (normRaw)
          std::memcpy(&v.normal, normRaw + i * normStride, sizeof(float) * 3);
        else
          v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        v.texCoord = decodeUV(i);
      }

      // ── JOINTS_0 and WEIGHTS_0 ──────────────────────────────────────
      std::vector<SkinVertex> skinVerts(vertCount);
      auto jointsIt = prim.attributes.find("JOINTS_0");
      auto weightsIt = prim.attributes.find("WEIGHTS_0");

      if (jointsIt != prim.attributes.end() &&
          weightsIt != prim.attributes.end()) {
        const auto &jointsAcc = gltfModel.accessors[jointsIt->second];
        const uint8_t *jointsRaw = accessorData(gltfModel, jointsIt->second);
        const auto &jointsBV = gltfModel.bufferViews[jointsAcc.bufferView];

        const auto &weightsAcc = gltfModel.accessors[weightsIt->second];
        const uint8_t *weightsRaw = accessorData(gltfModel, weightsIt->second);
        const auto &weightsBV = gltfModel.bufferViews[weightsAcc.bufferView];

        for (size_t i = 0; i < vertCount; ++i) {
          auto &sv = skinVerts[i];

          // Joints: may be UNSIGNED_BYTE or UNSIGNED_SHORT
          if (jointsAcc.componentType ==
              TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
            size_t stride = jointsBV.byteStride > 0 ? jointsBV.byteStride : 4;
            const uint8_t *ptr = jointsRaw + i * stride;
            sv.joints = glm::ivec4(ptr[0], ptr[1], ptr[2], ptr[3]);
          } else if (jointsAcc.componentType ==
                     TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
            size_t stride = jointsBV.byteStride > 0 ? jointsBV.byteStride : 8;
            const uint16_t *ptr =
                reinterpret_cast<const uint16_t *>(jointsRaw + i * stride);
            sv.joints = glm::ivec4(ptr[0], ptr[1], ptr[2], ptr[3]);
          }

          // Weights: FLOAT
          size_t wStride = weightsBV.byteStride > 0 ? weightsBV.byteStride : 16;
          std::memcpy(&sv.weights, weightsRaw + i * wStride, sizeof(float) * 4);

          // Normalize weights
          float sum = sv.weights.x + sv.weights.y + sv.weights.z + sv.weights.w;
          if (sum > 0.0f && std::abs(sum - 1.0f) > 1e-4f) {
            sv.weights /= sum;
          }
        }
      }

      // ── Indices ─────────────────────────────────────────────────────
      std::vector<uint32_t> indices;
      if (prim.indices >= 0) {
        const auto &idxAcc = gltfModel.accessors[prim.indices];
        const auto *idxRaw = accessorData(gltfModel, prim.indices);
        indices.resize(idxAcc.count);
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
          continue;
        }
      } else {
        indices.resize(vertCount);
        for (size_t i = 0; i < vertCount; ++i)
          indices[i] = static_cast<uint32_t>(i);
      }

      spdlog::info("Skinned mesh '{}': {} verts, {} indices, {} skin verts",
                   mesh.name, vertices.size(), indices.size(),
                   skinVerts.size());

      // ── Mesh simplification via meshoptimizer ─────────────────────────
      if (targetReduction > 0.0f && !indices.empty()) {
        size_t targetIndexCount = static_cast<size_t>(
            indices.size() * (1.0f - std::clamp(targetReduction, 0.0f, 0.99f)));
        // Ensure minimum triangle count (at least 300 triangles)
        targetIndexCount = std::max(targetIndexCount, static_cast<size_t>(900));

        // meshoptimizer simplify expects position data as float[3]
        std::vector<unsigned int> simplifiedIndices(indices.size());
        float targetError = 0.02f; // Allow some error for aggressive reduction
        float resultError = 0.0f;

        size_t newIndexCount = meshopt_simplify(
            simplifiedIndices.data(), indices.data(), indices.size(),
            reinterpret_cast<const float *>(&vertices[0].position),
            vertices.size(), sizeof(Vertex), targetIndexCount, targetError,
            0, &resultError);

        if (newIndexCount > 0 && newIndexCount < indices.size()) {
          simplifiedIndices.resize(newIndexCount);

          // Remap: find which vertices are actually used
          std::vector<unsigned int> remap(vertices.size(), ~0u);
          uint32_t newVertCount = 0;
          for (size_t i = 0; i < newIndexCount; ++i) {
            unsigned int vi = simplifiedIndices[i];
            if (remap[vi] == ~0u) {
              remap[vi] = newVertCount++;
            }
          }

          // Build compacted arrays
          std::vector<Vertex> newVerts(newVertCount);
          std::vector<SkinVertex> newSkin(newVertCount);
          for (size_t i = 0; i < vertices.size(); ++i) {
            if (remap[i] != ~0u) {
              newVerts[remap[i]] = vertices[i];
              newSkin[remap[i]] = skinVerts[i];
            }
          }
          std::vector<uint32_t> newIndices(newIndexCount);
          for (size_t i = 0; i < newIndexCount; ++i) {
            newIndices[i] = remap[simplifiedIndices[i]];
          }

          spdlog::info("  Simplified: {} -> {} verts, {} -> {} indices "
                       "(reduction: {:.1f}%, error: {:.4f})",
                       vertices.size(), newVertCount, indices.size(),
                       newIndexCount,
                       (1.0f - float(newIndexCount) / indices.size()) * 100.0f,
                       resultError);

          vertices = std::move(newVerts);
          skinVerts = std::move(newSkin);
          indices = std::move(newIndices);
        }
      }

      result.bindPoseVertices.push_back(vertices);
      result.skinVertices.push_back(std::move(skinVerts));
      result.indices.push_back(indices);
      result.model.m_meshes.emplace_back(device, allocator, vertices, indices);
      result.model.m_meshMaterialIndices.push_back(prim.material);
    }
  }

  if (result.model.m_meshes.empty())
    throw std::runtime_error("Skinned GLB has no renderable meshes: " +
                             filepath);

  // ══════════════════════════════════════════════════════════════════════════
  // 3. Extract animations
  // ══════════════════════════════════════════════════════════════════════════
  for (const auto &anim : gltfModel.animations) {
    AnimationClip clip;
    clip.name = anim.name;
    clip.duration = 0.0f;
    clip.looping = true;

    for (const auto &channel : anim.channels) {
      if (channel.target_node < 0)
        continue;

      // Map target node to our joint index
      auto it = nodeToJoint.find(channel.target_node);
      if (it == nodeToJoint.end())
        continue;

      const auto &sampler = anim.samplers[channel.sampler];

      AnimationChannel ac;
      ac.targetJointIndex = it->second;

      // Timestamps
      size_t keyCount = accessorCount(gltfModel, sampler.input);
      const float *timeData = reinterpret_cast<const float *>(
          accessorData(gltfModel, sampler.input));
      ac.timestamps.resize(keyCount);
      std::memcpy(ac.timestamps.data(), timeData, keyCount * sizeof(float));

      if (keyCount > 0 && ac.timestamps.back() > clip.duration)
        clip.duration = ac.timestamps.back();

      // Keyframe values
      const uint8_t *valData = accessorData(gltfModel, sampler.output);

      if (channel.target_path == "translation") {
        ac.path = AnimationPath::Translation;
        ac.translationKeys.resize(keyCount);
        for (size_t k = 0; k < keyCount; ++k) {
          std::memcpy(&ac.translationKeys[k], valData + k * sizeof(float) * 3,
                      sizeof(float) * 3);
        }
      } else if (channel.target_path == "rotation") {
        ac.path = AnimationPath::Rotation;
        ac.rotationKeys.resize(keyCount);
        for (size_t k = 0; k < keyCount; ++k) {
          // glTF quaternion: (x,y,z,w) → GLM: (w,x,y,z)
          float xyzw[4];
          std::memcpy(xyzw, valData + k * sizeof(float) * 4, sizeof(float) * 4);
          ac.rotationKeys[k] = glm::quat(xyzw[3], xyzw[0], xyzw[1], xyzw[2]);
        }
      } else if (channel.target_path == "scale") {
        ac.path = AnimationPath::Scale;
        ac.scaleKeys.resize(keyCount);
        for (size_t k = 0; k < keyCount; ++k) {
          std::memcpy(&ac.scaleKeys[k], valData + k * sizeof(float) * 3,
                      sizeof(float) * 3);
        }
      } else {
        continue;
      }

      clip.channels.push_back(std::move(ac));
    }

    // Store the rest pose this clip was authored against.  When this animation
    // is retargeted onto a different skeleton (e.g. idle model's skeleton), the
    // player uses these rest transforms instead of the host skeleton's.
    clip.restPose.resize(jointCount);
    for (int j = 0; j < jointCount; ++j) {
      clip.restPose[j].translation = result.skeleton.joints[j].localTranslation;
      clip.restPose[j].rotation    = result.skeleton.joints[j].localRotation;
      clip.restPose[j].scale       = result.skeleton.joints[j].localScale;
    }

    spdlog::info("Animation '{}': duration={:.3f}s, {} channels", clip.name,
                 clip.duration, clip.channels.size());
    result.animations.push_back(std::move(clip));
  }

  spdlog::info("Skinned GLB loaded: {} meshes, {} joints, {} animations",
               result.model.getMeshCount(), jointCount,
               result.animations.size());

  return result;
}

// ── Model::getGLBBounds ─────────────────────────────────────────────────────
Model::AABB Model::getGLBBounds(const std::string &filepath) {
  tinygltf::TinyGLTF loader;
  tinygltf::Model gltfModel;
  std::string err, warn;

  bool ok = loader.LoadBinaryFromFile(&gltfModel, &err, &warn, filepath);
  if (!ok)
    throw std::runtime_error("getGLBBounds: failed to load " + filepath + " " +
                             err);

  AABB bounds;

  for (const auto &mesh : gltfModel.meshes) {
    for (const auto &prim : mesh.primitives) {
      auto posIt = prim.attributes.find("POSITION");
      if (posIt == prim.attributes.end())
        continue;

      const auto &acc = gltfModel.accessors[posIt->second];

      // Use accessor min/max if available (most exporters provide these)
      if (acc.minValues.size() >= 3 && acc.maxValues.size() >= 3) {
        bounds.min.x =
            std::min(bounds.min.x, static_cast<float>(acc.minValues[0]));
        bounds.min.y =
            std::min(bounds.min.y, static_cast<float>(acc.minValues[1]));
        bounds.min.z =
            std::min(bounds.min.z, static_cast<float>(acc.minValues[2]));
        bounds.max.x =
            std::max(bounds.max.x, static_cast<float>(acc.maxValues[0]));
        bounds.max.y =
            std::max(bounds.max.y, static_cast<float>(acc.maxValues[1]));
        bounds.max.z =
            std::max(bounds.max.z, static_cast<float>(acc.maxValues[2]));
      } else {
        // Fallback: scan vertex data
        const auto *data = accessorData(gltfModel, posIt->second);
        const auto &bv = gltfModel.bufferViews[acc.bufferView];
        size_t stride = bv.byteStride > 0 ? bv.byteStride : sizeof(float) * 3;

        for (size_t i = 0; i < acc.count; ++i) {
          glm::vec3 pos;
          std::memcpy(&pos, data + i * stride, sizeof(float) * 3);
          bounds.min = glm::min(bounds.min, pos);
          bounds.max = glm::max(bounds.max, pos);
        }
      }
    }
  }

  spdlog::info(
      "GLB bounds: min({:.1f},{:.1f},{:.1f}) max({:.1f},{:.1f},{:.1f})",
      bounds.min.x, bounds.min.y, bounds.min.z, bounds.max.x, bounds.max.y,
      bounds.max.z);
  return bounds;
}

// ── Model::getGLBRawMesh ────────────────────────────────────────────────────
Model::RawMeshData Model::getGLBRawMesh(const std::string &filepath) {
  tinygltf::TinyGLTF loader;
  tinygltf::Model gltfModel;
  std::string err, warn;

  if (!loader.LoadBinaryFromFile(&gltfModel, &err, &warn, filepath))
    throw std::runtime_error("getGLBRawMesh: failed to load " + filepath +
                             " " + err);

  RawMeshData result;

  for (const auto &mesh : gltfModel.meshes) {
    for (const auto &prim : mesh.primitives) {
      auto posIt = prim.attributes.find("POSITION");
      if (posIt == prim.attributes.end()) continue;

      const auto &posAcc = gltfModel.accessors[posIt->second];
      const auto &posBV  = gltfModel.bufferViews[posAcc.bufferView];
      const auto &posBuf = gltfModel.buffers[posBV.buffer];
      const uint8_t *posBase = posBuf.data.data() + posBV.byteOffset + posAcc.byteOffset;
      size_t posStride = posBV.byteStride > 0 ? posBV.byteStride : sizeof(float) * 3;

      uint32_t baseVert = static_cast<uint32_t>(result.positions.size());
      for (size_t i = 0; i < posAcc.count; ++i) {
        glm::vec3 p;
        std::memcpy(&p, posBase + i * posStride, sizeof(float) * 3);
        result.positions.push_back(p);
      }

      if (prim.indices >= 0) {
        const auto &idxAcc = gltfModel.accessors[prim.indices];
        const auto &idxBV  = gltfModel.bufferViews[idxAcc.bufferView];
        const auto &idxBuf = gltfModel.buffers[idxBV.buffer];
        const uint8_t *idxBase = idxBuf.data.data() + idxBV.byteOffset + idxAcc.byteOffset;
        for (size_t i = 0; i < idxAcc.count; ++i) {
          uint32_t idx = 0;
          switch (idxAcc.componentType) {
          case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            idx = reinterpret_cast<const uint16_t*>(idxBase)[i]; break;
          case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
            idx = reinterpret_cast<const uint32_t*>(idxBase)[i]; break;
          case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            idx = idxBase[i]; break;
          }
          result.indices.push_back(baseVert + idx);
        }
      }
    }
  }

  spdlog::info("getGLBRawMesh: {} positions, {} indices", result.positions.size(),
               result.indices.size());
  return result;
}

} // namespace glory
