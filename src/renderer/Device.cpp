#include "renderer/Device.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>

#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace glory {

namespace {
#ifdef NDEBUG
constexpr bool kEnableValidation = false;
#else
constexpr bool kEnableValidation = true;
#endif

const std::vector<const char*> kValidationLayers = {
    "VK_LAYER_KHRONOS_validation"
};
} // anonymous namespace

// ── Static ──────────────────────────────────────────────────────────────────
const std::vector<const char*>& Device::getRequiredExtensions() {
    static const std::vector<const char*> extensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
#ifdef __APPLE__
        "VK_KHR_portability_subset",
#endif
    };
    return extensions;
}

// ── Lifecycle ───────────────────────────────────────────────────────────────
Device::Device(VkInstance instance, VkSurfaceKHR surface)
    : m_instance(instance), m_surface(surface)
{
    pickPhysicalDevice();
    createLogicalDevice();
    createAllocator();
    createTransferCommandPool();
    createGraphicsCommandPool();
    createComputeCommandPool();
}

Device::~Device() { cleanup(); }

void Device::cleanup() {
    if (m_cleaned) return;
    m_cleaned = true;
    if (m_transferCommandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_transferCommandPool, nullptr);
    }
    if (m_graphicsCommandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_graphicsCommandPool, nullptr);
    }
    if (m_computeCommandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_computeCommandPool, nullptr);
    }
    if (m_allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(m_allocator);
        spdlog::info("VMA allocator destroyed");
    }
    if (m_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_device, nullptr);
        spdlog::info("Logical device destroyed");
    }
}

// ── Physical device selection ───────────────────────────────────────────────
void Device::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0)
        throw std::runtime_error("Failed to find GPUs with Vulkan support");

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    std::multimap<int, VkPhysicalDevice> candidates;
    for (const auto& dev : devices)
        candidates.insert({ rateDeviceSuitability(dev), dev });

    if (candidates.rbegin()->first > 0) {
        m_physicalDevice = candidates.rbegin()->second;
        m_indices = findQueueFamilies(m_physicalDevice);

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
        spdlog::info("Selected GPU: {} (score: {})",
                     props.deviceName, candidates.rbegin()->first);
    } else {
        throw std::runtime_error("Failed to find a suitable GPU");
    }
}

int Device::rateDeviceSuitability(VkPhysicalDevice device) const {
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceFeatures   feats;
    vkGetPhysicalDeviceProperties(device, &props);
    vkGetPhysicalDeviceFeatures(device, &feats);

    auto indices = findQueueFamilies(device);
    if (!indices.isComplete())              return 0;
    if (!checkDeviceExtensionSupport(device)) return 0;

    auto sc = querySwapchainSupport(device);
    if (sc.formats.empty() || sc.presentModes.empty()) return 0;
    if (props.apiVersion < VK_API_VERSION_1_3)          return 0;

    int score = 0;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        score += 10000;
    else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
        score += 1000;

    score += static_cast<int>(props.limits.maxImageDimension2D);
    if (feats.geometryShader) score += 500;
    if (feats.wideLines)      score += 100;

    spdlog::trace("GPU '{}': score {}", props.deviceName, score);
    return score;
}

bool Device::checkDeviceExtensionSupport(VkPhysicalDevice device) const {
    uint32_t count;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);

    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());

    std::set<std::string> required(getRequiredExtensions().begin(),
                                   getRequiredExtensions().end());
    for (const auto& ext : available)
        required.erase(ext.extensionName);

    return required.empty();
}

QueueFamilyIndices Device::findQueueFamilies(VkPhysicalDevice device) const {
    QueueFamilyIndices indices;

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    for (uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            indices.graphicsFamily = i;

        VkBool32 present = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &present);
        if (present) indices.presentFamily = i;

        // Prefer a queue that supports TRANSFER but NOT GRAPHICS — that's a
        // dedicated DMA engine (common on discrete GPUs: NVIDIA has queue family 2,
        // AMD has queue family 1 for DMA).
        if ((families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
            !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            !indices.transferFamily.has_value()) {
            indices.transferFamily = i;
        }

        // Prefer a queue that supports COMPUTE but NOT GRAPHICS — dedicated
        // async compute (NVIDIA: queue family 2, AMD: queue family 1).
        if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            !indices.computeFamily.has_value()) {
            indices.computeFamily = i;
        }

        if (indices.isComplete()) break;
    }

    // Fall back to graphics family if no dedicated transfer found
    if (!indices.transferFamily.has_value())
        indices.transferFamily = indices.graphicsFamily;

    // Fall back to graphics family if no dedicated compute found
    if (!indices.computeFamily.has_value())
        indices.computeFamily = indices.graphicsFamily;

    return indices;
}

// ── Logical device ──────────────────────────────────────────────────────────
void Device::createLogicalDevice() {
    std::set<uint32_t> uniqueFamilies = {
        m_indices.graphicsFamily.value(),
        m_indices.presentFamily.value(),
        m_indices.transferFamily.value(),  // may equal graphicsFamily — set deduplicates
        m_indices.computeFamily.value()    // may equal graphicsFamily — set deduplicates
    };

    float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueCIs;
    for (uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo ci{};
        ci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        ci.queueFamilyIndex = family;
        ci.queueCount       = 1;
        ci.pQueuePriorities = &priority;
        queueCIs.push_back(ci);
    }

    VkPhysicalDeviceFeatures features{};
    VkPhysicalDeviceFeatures2 baseQuery{};
    baseQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    vkGetPhysicalDeviceFeatures2(m_physicalDevice, &baseQuery);

    features.samplerAnisotropy  = VK_TRUE;
    features.fillModeNonSolid   = baseQuery.features.fillModeNonSolid;
    features.multiDrawIndirect  = baseQuery.features.multiDrawIndirect;
    features.independentBlend   = baseQuery.features.independentBlend;

    // Query which Vulkan 1.2 features are actually available on this device
    // before enabling them — some (e.g. drawIndirectCount) are not supported
    // on MoltenVK / Apple Silicon.
    // Re-use the same query with available12 chained off baseQuery.
    VkPhysicalDeviceVulkan12Features available12{};
    available12.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    available12.pNext  = nullptr;
    baseQuery.pNext    = &available12;
    vkGetPhysicalDeviceFeatures2(m_physicalDevice, &baseQuery);

    // Build the Vulkan 1.2 feature request — only enable what is available.
    // NOTE: VkPhysicalDeviceVulkan12Features already contains all descriptor-
    // indexing bits (they were promoted in 1.2), so VkPhysicalDeviceDescriptorIndexingFeatures
    // must NOT also appear in the pNext chain (VUID-VkDeviceCreateInfo-pNext-02830).
    VkPhysicalDeviceVulkan12Features vk12Features{};
    vk12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vk12Features.pNext = nullptr;

    // drawIndirectCount — optional; gracefully disabled when not available
    m_drawIndirectCountSupported = (available12.drawIndirectCount == VK_TRUE);
    vk12Features.drawIndirectCount = m_drawIndirectCountSupported ? VK_TRUE : VK_FALSE;
    if (!m_drawIndirectCountSupported)
        spdlog::warn("Device: drawIndirectCount not supported — "
                     "GPU culling will fall back to vkCmdDrawIndexedIndirect");

    // Descriptor indexing features (required for bindless textures + SSBOs)
    vk12Features.descriptorBindingSampledImageUpdateAfterBind =
        available12.descriptorBindingSampledImageUpdateAfterBind;
    vk12Features.shaderSampledImageArrayNonUniformIndexing =
        available12.shaderSampledImageArrayNonUniformIndexing;
    vk12Features.descriptorBindingPartiallyBound =
        available12.descriptorBindingPartiallyBound;
    vk12Features.runtimeDescriptorArray =
        available12.runtimeDescriptorArray;
    vk12Features.descriptorBindingStorageBufferUpdateAfterBind =
        available12.descriptorBindingStorageBufferUpdateAfterBind;
    vk12Features.descriptorBindingVariableDescriptorCount =
        available12.descriptorBindingVariableDescriptorCount;
    vk12Features.shaderStorageBufferArrayNonUniformIndexing =
        available12.shaderStorageBufferArrayNonUniformIndexing;

    // Vulkan 1.3 features — dynamic rendering, synchronization2, maintenance4
    VkPhysicalDeviceVulkan13Features vk13Features{};
    vk13Features.sType              = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vk13Features.pNext              = &vk12Features;   // chain 1.2 after 1.3
    vk13Features.dynamicRendering   = VK_TRUE;
    vk13Features.synchronization2   = VK_TRUE;
    vk13Features.maintenance4       = VK_TRUE;

    VkDeviceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext                   = &vk13Features;        // 1.3 → 1.2 chain
    ci.queueCreateInfoCount    = static_cast<uint32_t>(queueCIs.size());
    ci.pQueueCreateInfos       = queueCIs.data();
    ci.pEnabledFeatures        = &features;
    ci.enabledExtensionCount   = static_cast<uint32_t>(getRequiredExtensions().size());
    ci.ppEnabledExtensionNames = getRequiredExtensions().data();

    if (kEnableValidation) {
        ci.enabledLayerCount   = static_cast<uint32_t>(kValidationLayers.size());
        ci.ppEnabledLayerNames = kValidationLayers.data();
    } else {
        ci.enabledLayerCount = 0;
    }

    VK_CHECK(vkCreateDevice(m_physicalDevice, &ci, nullptr, &m_device),
             "Failed to create logical device");

    vkGetDeviceQueue(m_device, m_indices.graphicsFamily.value(), 0, &m_graphicsQueue);
    vkGetDeviceQueue(m_device, m_indices.presentFamily.value(),  0, &m_presentQueue);
    vkGetDeviceQueue(m_device, m_indices.transferFamily.value(), 0, &m_transferQueue);
    vkGetDeviceQueue(m_device, m_indices.computeFamily.value(),  0, &m_computeQueue);
    m_dedicatedTransfer = (m_indices.transferFamily.value() != m_indices.graphicsFamily.value());
    m_dedicatedCompute  = (m_indices.computeFamily.value()  != m_indices.graphicsFamily.value());
    spdlog::info("Transfer queue: family {} ({})",
                 m_indices.transferFamily.value(),
                 m_dedicatedTransfer ? "dedicated DMA" : "shared with graphics");
    spdlog::info("Compute queue: family {} ({})",
                 m_indices.computeFamily.value(),
                 m_dedicatedCompute ? "dedicated async" : "shared with graphics");

    spdlog::info("Logical device created (graphics: {}, present: {})",
                 m_indices.graphicsFamily.value(),
                 m_indices.presentFamily.value());
}

// ── Swapchain queries ───────────────────────────────────────────────────────
SwapchainSupportDetails Device::querySwapchainSupport() const {
    return querySwapchainSupport(m_physicalDevice);
}

SwapchainSupportDetails Device::querySwapchainSupport(VkPhysicalDevice device) const {
    SwapchainSupportDetails d;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, m_surface, &d.capabilities);

    uint32_t fmtCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &fmtCount, nullptr);
    if (fmtCount) {
        d.formats.resize(fmtCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &fmtCount, d.formats.data());
    }

    uint32_t modeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &modeCount, nullptr);
    if (modeCount) {
        d.presentModes.resize(modeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &modeCount, d.presentModes.data());
    }
    return d;
}

// ── VMA allocator ───────────────────────────────────────────────────────────
void Device::createAllocator() {
    VmaVulkanFunctions vulkanFunctions{};
    vulkanFunctions.vkGetInstanceProcAddr = &vkGetInstanceProcAddr;
    vulkanFunctions.vkGetDeviceProcAddr   = &vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo ci{};
    ci.vulkanApiVersion = VK_API_VERSION_1_3;
    ci.physicalDevice   = m_physicalDevice;
    ci.device           = m_device;
    ci.instance         = m_instance;
    ci.pVulkanFunctions = &vulkanFunctions;

    VK_CHECK(vmaCreateAllocator(&ci, &m_allocator),
             "Failed to create VMA allocator");
    spdlog::info("VMA allocator created");
}

// ── Transfer command pool (one-shot staging copies) ─────────────────────────
void Device::createTransferCommandPool() {
    VkCommandPoolCreateInfo ci{};
    ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    // Use dedicated transfer family when available (overlaps with rendering on DMA engine)
    ci.queueFamilyIndex = m_indices.transferFamily.value();

    VK_CHECK(vkCreateCommandPool(m_device, &ci, nullptr, &m_transferCommandPool),
             "Failed to create transfer command pool");
}

void Device::createGraphicsCommandPool() {
    VkCommandPoolCreateInfo ci{};
    ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                          VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = m_indices.graphicsFamily.value();

    VK_CHECK(vkCreateCommandPool(m_device, &ci, nullptr, &m_graphicsCommandPool),
             "Failed to create graphics command pool");
}

void Device::createComputeCommandPool() {
    VkCommandPoolCreateInfo ci{};
    ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                          VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = m_indices.computeFamily.value();

    VK_CHECK(vkCreateCommandPool(m_device, &ci, nullptr, &m_computeCommandPool),
             "Failed to create compute command pool");
}

VkFormat Device::findSupportedFormat(const std::vector<VkFormat>& candidates,
                                     VkImageTiling tiling,
                                     VkFormatFeatureFlags features) const
{
    for (VkFormat fmt : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, fmt, &props);
        if (tiling == VK_IMAGE_TILING_LINEAR &&
            (props.linearTilingFeatures & features) == features)
            return fmt;
        if (tiling == VK_IMAGE_TILING_OPTIMAL &&
            (props.optimalTilingFeatures & features) == features)
            return fmt;
    }
    throw std::runtime_error("Failed to find supported format");
}

VkFormat Device::findDepthFormat() const {
    return findSupportedFormat(
        { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT },
        VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
}

} // namespace glory
