#include "window/Window.h"

#include <spdlog/spdlog.h>
#include <stdexcept>

namespace glory {

Window::Window(int width, int height, const std::string& title)
    : m_width(width), m_height(height)
{
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    m_window = glfwCreateWindow(m_width, m_height, title.c_str(), nullptr, nullptr);
    if (!m_window) {
        glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, framebufferResizeCallback);

    spdlog::info("Window created ({}x{})", m_width, m_height);
}

Window::~Window() {
    if (m_window) {
        glfwDestroyWindow(m_window);
        glfwTerminate();
        spdlog::info("Window destroyed");
    }
}

bool Window::shouldClose() const {
    return glfwWindowShouldClose(m_window);
}

void Window::pollEvents() {
    glfwPollEvents();
}

void Window::createSurface(VkInstance instance) {
    VkResult result = glfwCreateWindowSurface(instance, m_window, nullptr, &m_surface);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to create window surface");
    }
    spdlog::info("Vulkan surface created");
}

void Window::destroySurface(VkInstance instance) {
    if (m_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
        spdlog::info("Vulkan surface destroyed");
    }
}

VkExtent2D Window::getExtent() const {
    int width, height;
    glfwGetFramebufferSize(m_window, &width, &height);
    return { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
}

void Window::framebufferResizeCallback(GLFWwindow* window, int width, int height) {
    auto* self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
    self->m_framebufferResized = true;
    self->m_width  = width;
    self->m_height = height;
    spdlog::info("Window resized to {}x{}", width, height);
}

} // namespace glory
