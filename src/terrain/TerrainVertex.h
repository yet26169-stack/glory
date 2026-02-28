#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>

namespace glory {

struct TerrainVertex {
  glm::vec3 pos;     // world position
  glm::vec2 uv;      // texture coordinates
  glm::vec3 normal;  // surface normal
  glm::vec3 tangent; // tangent for normal mapping

  static VkVertexInputBindingDescription getBindingDescription() {
    VkVertexInputBindingDescription desc{};
    desc.binding = 0;
    desc.stride = sizeof(TerrainVertex);
    desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return desc;
  }

  static std::array<VkVertexInputAttributeDescription, 4>
  getAttributeDescriptions() {
    std::array<VkVertexInputAttributeDescription, 4> attrs{};

    // pos
    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = offsetof(TerrainVertex, pos);

    // uv
    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = offsetof(TerrainVertex, uv);

    // normal
    attrs[2].binding = 0;
    attrs[2].location = 2;
    attrs[2].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[2].offset = offsetof(TerrainVertex, normal);

    // tangent
    attrs[3].binding = 0;
    attrs[3].location = 3;
    attrs[3].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[3].offset = offsetof(TerrainVertex, tangent);

    return attrs;
  }
};

} // namespace glory
