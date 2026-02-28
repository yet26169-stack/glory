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
}

Device::~Device() { cleanup(); }

void Device::cleanup() {
    if (m_cleaned) return;
    m_cleaned = true;
    if (m_transferCommandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_transferCommandPool, nullptr);
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

        if (indices.isComplete()) break;
    }
    return indices;
}

// ── Logical device ──────────────────────────────────────────────────────────
void Device::createLogicalDevice() {
    std::set<uint32_t> uniqueFamilies = {
        m_indices.graphicsFamily.value(),
        m_indices.presentFamily.value()
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
    features.samplerAnisotropy   = VK_TRUE;
    features.fillModeNonSolid    = VK_TRUE;  // needed for wireframe rendering

    // Descriptor indexing features (Vulkan 1.2 core) — bindless resources
    VkPhysicalDeviceDescriptorIndexingFeatures indexingFeatures{};
    indexingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    indexingFeatures.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    indexingFeatures.shaderSampledImageArrayNonUniformIndexing    = VK_TRUE;
    indexingFeatures.descriptorBindingPartiallyBound              = VK_TRUE;

    VkDeviceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext                   = &indexingFeatures;
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
    ci.queueFamilyIndex = m_indices.graphicsFamily.value();

    VK_CHECK(vkCreateCommandPool(m_device, &ci, nullptr, &m_transferCommandPool),
             "Failed to create transfer command pool");
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
