#pragma once

#ifndef GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>

#include <string>

namespace glory {

class Window {
public:
    Window(int width, int height, const std::string& title);
    ~Window();

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;

    bool shouldClose() const;
    void pollEvents();

    void createSurface(VkInstance instance);
    void destroySurface(VkInstance instance);

    GLFWwindow*  getHandle()  const { return m_window; }
    VkSurfaceKHR getSurface() const { return m_surface; }
    VkExtent2D   getExtent()  const;

    bool wasResized()      const { return m_framebufferResized; }
    void resetResizedFlag()      { m_framebufferResized = false; }

private:
    GLFWwindow*  m_window  = nullptr;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    int  m_width;
    int  m_height;
    bool m_framebufferResized = false;

    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
};

} // namespace glory
