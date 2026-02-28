#pragma once

#include <vulkan/vulkan.h>

#include <vector>

namespace glory {

class Context {
public:
    Context();
    ~Context();

    Context(const Context&)            = delete;
    Context& operator=(const Context&) = delete;

    void cleanup();

    VkInstance getInstance() const { return m_instance; }

    static bool validationLayersEnabled();

private:
    VkInstance               m_instance       = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    bool m_cleaned = false;

    void createInstance();
    void setupDebugMessenger();

    bool checkValidationLayerSupport() const;
    std::vector<const char*> getRequiredExtensions() const;

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT      messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT             messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void*                                       pUserData);

    static void populateDebugMessengerCreateInfo(
        VkDebugUtilsMessengerCreateInfoEXT& createInfo);
};

} // namespace glory
