#include "editor/DebugOverlay.h"
#include "renderer/Device.h"
#include "renderer/Swapchain.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <spdlog/spdlog.h>

#include <array>
#include <stdexcept>

namespace glory {

DebugOverlay::~DebugOverlay() { shutdown(); }

void DebugOverlay::init(GLFWwindow* window, VkInstance instance, const Device& device,
                        const Swapchain& swapchain, VkRenderPass renderPass) {
    m_vkDevice = device.getDevice();

    // Dedicated descriptor pool for ImGui
    std::array<VkDescriptorPoolSize, 1> poolSizes{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolCI.pPoolSizes    = poolSizes.data();

    if (vkCreateDescriptorPool(m_vkDevice, &poolCI, nullptr, &m_imguiPool) != VK_SUCCESS)
        throw std::runtime_error("Failed to create ImGui descriptor pool");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // no imgui.ini

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 6.0f;
    style.FrameRounding     = 4.0f;
    style.GrabRounding      = 3.0f;
    style.Alpha             = 0.92f;

    ImGui_ImplGlfw_InitForVulkan(window, true);

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance        = instance;
    initInfo.PhysicalDevice  = device.getPhysicalDevice();
    initInfo.Device          = device.getDevice();
    initInfo.QueueFamily     = device.getQueueFamilies().graphicsFamily.value();
    initInfo.Queue           = device.getGraphicsQueue();
    initInfo.DescriptorPool  = m_imguiPool;
    initInfo.MinImageCount   = swapchain.getImageCount();
    initInfo.ImageCount      = swapchain.getImageCount();
    initInfo.MSAASamples     = VK_SAMPLE_COUNT_1_BIT;
    initInfo.RenderPass      = renderPass;
    initInfo.Subpass         = 0;

    ImGui_ImplVulkan_Init(&initInfo);

    // Upload fonts
    ImGui_ImplVulkan_CreateFontsTexture();

    m_initialized = true;
    spdlog::info("Debug overlay initialized (Dear ImGui)");
}

void DebugOverlay::shutdown() {
    if (!m_initialized) return;
    m_initialized = false;

    vkDeviceWaitIdle(m_vkDevice);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    if (m_imguiPool) {
        vkDestroyDescriptorPool(m_vkDevice, m_imguiPool, nullptr);
        m_imguiPool = VK_NULL_HANDLE;
    }

    spdlog::info("Debug overlay shut down");
}

void DebugOverlay::onSwapchainRecreate() {
    // ImGui Vulkan backend handles internal resize automatically
}

void DebugOverlay::beginFrame() {
    if (!m_initialized || !m_visible) return;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Build the overlay window
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(280, 240), ImGuiCond_FirstUseEver);
    ImGui::Begin("Glory Engine", nullptr,
                 ImGuiWindowFlags_NoCollapse);

    ImGui::Text("FPS: %.1f", m_fps);
    ImGui::Text("Frame: %.2f ms", m_frameTimeMs);
    ImGui::Text("GPU: %s", m_gpuTimingSummary);
    ImGui::Text("CPU: %s", m_cpuTimingSummary);
    ImGui::Separator();    ImGui::Separator();
    ImGui::Text("Entities: %u", m_entityCount);
    ImGui::Text("Draw calls: %u (indirect)", m_drawCalls);
    ImGui::Text("Indirect cmds: %u | Instances: %u", m_indirectCommands, m_totalInstances);
    ImGui::Text("Culled: %u", m_culled);
    ImGui::Text("Particles: %u | Meshes: %u | Textures: %u",
                m_particleCount, m_meshCount, m_textureCount);
    if (m_wireframe) ImGui::TextColored(ImVec4(1,1,0,1), "WIREFRAME");
    ImGui::Separator();
    ImGui::Text("Camera: (%.1f, %.1f, %.1f)", m_camX, m_camY, m_camZ);
    ImGui::Separator();

    ImGui::SliderFloat("Exposure", &m_exposure, 0.1f, 5.0f);
    ImGui::Checkbox("Auto Exposure", &m_autoExposure);
    ImGui::SliderFloat("Gamma", &m_gamma, 1.0f, 3.0f);
    ImGui::SliderFloat("Bloom", &m_bloomIntensity, 0.0f, 2.0f);
    ImGui::SliderFloat("Bloom Thr", &m_bloomThreshold, 0.1f, 5.0f);
    ImGui::SliderFloat("Vignette", &m_vignetteStrength, 0.0f, 1.0f);
    ImGui::SliderFloat("Vig Radius", &m_vignetteRadius, 0.3f, 1.2f);
    ImGui::SliderFloat("Fog", &m_fogDensity, 0.0f, 0.15f);
    ImGui::SliderFloat("Chroma", &m_chromaStrength, 0.0f, 3.0f);
    ImGui::SliderFloat("SSAO Rad", &m_ssaoRadius, 0.1f, 2.0f);
    ImGui::SliderFloat("SSAO Int", &m_ssaoIntensity, 0.0f, 3.0f);
    ImGui::SliderFloat("Film Grain", &m_filmGrain, 0.0f, 0.15f);

    const char* toneMapNames[] = {"ACES", "Reinhard", "Uncharted2"};
    ImGui::Combo("Tone Map", &m_toneMapMode, toneMapNames, 3);
    ImGui::Checkbox("FXAA", &m_fxaaEnabled);
    ImGui::SliderFloat("Sharpen", &m_sharpenStrength, 0.0f, 1.0f);
    ImGui::SliderFloat("DoF", &m_dofStrength, 0.0f, 1.0f);
    ImGui::SliderFloat("Focus Dist", &m_dofFocusDist, 0.5f, 20.0f);
    ImGui::SliderFloat("DoF Range", &m_dofRange, 0.5f, 10.0f);
    ImGui::SliderFloat("Saturation", &m_saturation, 0.0f, 2.0f);
    ImGui::SliderFloat("Color Temp", &m_colorTemp, -1.0f, 1.0f);
    ImGui::SliderFloat("Outline",    &m_outlineStrength, 0.0f, 1.0f);
    ImGui::SliderFloat("Outline Thr", &m_outlineThreshold, 0.01f, 0.5f);
    ImGui::SliderFloat("God Rays",   &m_godRaysStrength, 0.0f, 1.0f);
    if (m_godRaysStrength > 0.0f) {
        ImGui::SliderFloat("GR Decay",   &m_godRaysDecay, 0.9f, 1.0f);
        ImGui::SliderFloat("GR Density", &m_godRaysDensity, 0.1f, 1.0f);
    }
    ImGui::SliderFloat("Heat Dist",  &m_heatDistortion, 0.0f, 1.0f);
    ImGui::SliderFloat("Dithering",  &m_ditheringStrength, 0.0f, 2.0f);

    ImGui::Separator();
    ImGui::TextDisabled("F1 — overlay | F2 — wireframe | F3 — grid");
    ImGui::TextDisabled("RMB — capture mouse | ESC — release");

    ImGui::End();
}

void DebugOverlay::endFrame() {
    if (!m_initialized || !m_visible) return;
    ImGui::Render();
}

void DebugOverlay::render(VkCommandBuffer cmd) {
    if (!m_initialized || !m_visible) return;
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

} // namespace glory
