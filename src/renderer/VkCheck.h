#pragma once

#include <vulkan/vulkan.h>
#include <spdlog/spdlog.h>

#include <stdexcept>

namespace glory {

inline void vkCheck(VkResult result, const char* msg) {
    if (result != VK_SUCCESS) {
        spdlog::error("Vulkan error: {} (code: {})", msg, static_cast<int>(result));
        throw std::runtime_error(msg);
    }
}

} // namespace glory

#define VK_CHECK(result, msg) ::glory::vkCheck(result, msg)
