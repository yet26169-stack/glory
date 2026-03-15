#pragma once

#include "renderer/Frustum.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <cstdint>

namespace glory {

class Device;
class Descriptors;

struct FrameContext {
    uint32_t    frameIndex  = 0;   // current frame-in-flight (0 or 1)
    uint32_t    imageIndex  = 0;   // swapchain image index
    VkCommandBuffer cmd     = VK_NULL_HANDLE;
    VkExtent2D  extent      = {};
    float       aspect      = 1.0f;
    float       dt          = 0.0f;
    float       gameTime    = 0.0f;

    glm::mat4   view        = glm::mat4(1.0f);
    glm::mat4   proj        = glm::mat4(1.0f);
    glm::mat4   viewProj    = glm::mat4(1.0f);
    glm::vec3   cameraPos   = glm::vec3(0.0f);
    float       nearPlane   = 0.1f;
    float       farPlane    = 300.0f;
    Frustum     frustum;
};

} // namespace glory
