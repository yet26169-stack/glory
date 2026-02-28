#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

struct GLFWwindow;

namespace glory {

class Device;
class Swapchain;

class DebugOverlay {
public:
    DebugOverlay() = default;
    ~DebugOverlay();

    DebugOverlay(const DebugOverlay&)            = delete;
    DebugOverlay& operator=(const DebugOverlay&) = delete;

    void init(GLFWwindow* window, VkInstance instance, const Device& device,
              const Swapchain& swapchain, VkRenderPass renderPass);
    void shutdown();
    void onSwapchainRecreate();

    void beginFrame();
    void endFrame();
    void render(VkCommandBuffer cmd);

    void setFPS(float fps) { m_fps = fps; }
    void setFrameTime(float ms) { m_frameTimeMs = ms; }
    void setEntityCount(uint32_t count) { m_entityCount = count; }
    void setDrawCallCount(uint32_t count) { m_drawCalls = count; }
    void setInstancedDraws(uint32_t count) { m_instancedDraws = count; }
    void setTotalInstances(uint32_t count) { m_totalInstances = count; }
    void setIndirectCommands(uint32_t count) { m_indirectCommands = count; }
    void setCulledCount(uint32_t count) { m_culled = count; }
    void setCameraPos(float x, float y, float z) { m_camX=x; m_camY=y; m_camZ=z; }

    void setWireframe(bool w) { m_wireframe = w; }

    bool isVisible() const { return m_visible; }
    void toggleVisible() { m_visible = !m_visible; }

    float* exposurePtr() { return &m_exposure; }
    float* gammaPtr()    { return &m_gamma; }
    float  getExposure()       const { return m_exposure; }
    float  getGamma()          const { return m_gamma; }
    float  getBloomIntensity()    const { return m_bloomIntensity; }
    float  getBloomThreshold()    const { return m_bloomThreshold; }
    float  getVignetteStrength()  const { return m_vignetteStrength; }
    float  getVignetteRadius()    const { return m_vignetteRadius; }
    float  getFogDensity()        const { return m_fogDensity; }
    float  getChromaStrength()    const { return m_chromaStrength; }
    float  getSSAORadius()        const { return m_ssaoRadius; }
    float  getSSAOBias()          const { return m_ssaoBias; }
    float  getSSAOIntensity()     const { return m_ssaoIntensity; }
    float  getFilmGrain()         const { return m_filmGrain; }
    int    getToneMapMode()       const { return m_toneMapMode; }
    bool   getFXAAEnabled()       const { return m_fxaaEnabled; }
    float  getSharpenStrength()   const { return m_sharpenStrength; }
    float  getDofStrength()       const { return m_dofStrength; }
    float  getDofFocusDist()      const { return m_dofFocusDist; }
    float  getDofRange()          const { return m_dofRange; }
    float  getSaturation()        const { return m_saturation; }
    float  getColorTemp()         const { return m_colorTemp; }
    float  getOutlineStrength()   const { return m_outlineStrength; }
    float  getOutlineThreshold()  const { return m_outlineThreshold; }
    float  getGodRaysStrength()   const { return m_godRaysStrength; }
    float  getGodRaysDecay()      const { return m_godRaysDecay; }
    float  getGodRaysDensity()    const { return m_godRaysDensity; }
    bool   getAutoExposure()      const { return m_autoExposure; }
    float  getHeatDistortion()    const { return m_heatDistortion; }
    float  getDitheringStrength() const { return m_ditheringStrength; }

    void setParticleCount(uint32_t count) { m_particleCount = count; }
    void setMeshCount(uint32_t count)     { m_meshCount = count; }
    void setTextureCount(uint32_t count)  { m_textureCount = count; }

private:
    VkDevice         m_vkDevice    = VK_NULL_HANDLE;
    VkDescriptorPool m_imguiPool   = VK_NULL_HANDLE;
    bool             m_initialized = false;
    bool             m_visible     = true;

    float    m_fps         = 0.0f;
    float    m_frameTimeMs = 0.0f;
    uint32_t m_entityCount = 0;
    uint32_t m_drawCalls      = 0;
    uint32_t m_instancedDraws = 0;
    uint32_t m_totalInstances = 0;
    uint32_t m_indirectCommands = 0;
    uint32_t m_culled      = 0;
    float    m_camX = 0, m_camY = 0, m_camZ = 0;
    float    m_exposure = 1.0f;
    float    m_gamma    = 2.2f;
    float    m_bloomIntensity = 0.3f;
    float    m_bloomThreshold = 1.0f;
    float    m_vignetteStrength = 0.4f;
    float    m_vignetteRadius   = 0.75f;
    float    m_fogDensity       = 0.03f;
    float    m_chromaStrength   = 0.0f;
    float    m_ssaoRadius       = 0.5f;
    float    m_ssaoBias         = 0.025f;
    float    m_ssaoIntensity    = 1.0f;
    float    m_filmGrain        = 0.03f;
    int      m_toneMapMode      = 0; // 0=ACES, 1=Reinhard, 2=Uncharted2
    bool     m_fxaaEnabled      = true;
    float    m_sharpenStrength  = 0.0f;
    float    m_dofStrength      = 0.0f;
    float    m_dofFocusDist     = 5.0f;
    float    m_dofRange         = 3.0f;
    float    m_saturation       = 1.0f;
    float    m_colorTemp        = 0.0f;
    float    m_outlineStrength  = 0.0f;
    float    m_outlineThreshold = 0.1f;
    float    m_godRaysStrength  = 0.0f;
    float    m_godRaysDecay     = 0.97f;
    float    m_godRaysDensity   = 0.5f;
    bool     m_autoExposure     = false;
    float    m_heatDistortion   = 0.0f;
    float    m_ditheringStrength = 0.0f;
    uint32_t m_particleCount    = 0;
    uint32_t m_meshCount        = 0;
    uint32_t m_textureCount     = 0;
    bool     m_wireframe = false;
};

} // namespace glory
