#include "renderer/WaterRenderer.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#ifndef SHADER_DIR
#  define SHADER_DIR ""
#endif

namespace glory {

// ── Grid mesh generation ──────────────────────────────────────────────────────

static constexpr uint32_t GRID = 64;

void WaterRenderer::createMesh() {
    const uint32_t vertCount  = (GRID + 1) * (GRID + 1);
    const uint32_t indexCount = GRID * GRID * 6;

    std::vector<WaterVertex> verts;
    verts.reserve(vertCount);

    for (uint32_t z = 0; z <= GRID; ++z) {
        for (uint32_t x = 0; x <= GRID; ++x) {
            WaterVertex v{};
            v.position = { static_cast<float>(x) / GRID, 0.0f,
                           static_cast<float>(z) / GRID };
            v.uv       = { static_cast<float>(x) / GRID,
                           static_cast<float>(z) / GRID };
            verts.push_back(v);
        }
    }

    std::vector<uint32_t> indices;
    indices.reserve(indexCount);
    for (uint32_t z = 0; z < GRID; ++z) {
        for (uint32_t x = 0; x < GRID; ++x) {
            uint32_t tl = z * (GRID + 1) + x;
            uint32_t tr = tl + 1;
            uint32_t bl = tl + (GRID + 1);
            uint32_t br = bl + 1;
            indices.insert(indices.end(), { tl, bl, tr, tr, bl, br });
        }
    }

    m_indexCount = static_cast<uint32_t>(indices.size());

    m_vertexBuffer = Buffer::createDeviceLocal(*m_device, m_device->getAllocator(),
        verts.data(), verts.size() * sizeof(WaterVertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    m_indexBuffer = Buffer::createDeviceLocal(*m_device, m_device->getAllocator(),
        indices.data(), indices.size() * sizeof(uint32_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    spdlog::info("WaterRenderer: grid {}x{} ({} verts, {} indices)", GRID, GRID,
                 vertCount, indexCount);
}

// ── Procedural textures ───────────────────────────────────────────────────────

// Encode R,G,B floats [0,1] into a little-endian RGBA uint32_t
// (as expected by VK_FORMAT_R8G8B8A8_UNORM).
static inline uint32_t packRGBA(float r, float g, float b, float a = 1.0f) {
    auto ub = [](float v) { return static_cast<uint32_t>(v * 255.0f + 0.5f) & 0xFF; };
    return ub(r) | (ub(g) << 8) | (ub(b) << 16) | (ub(a) << 24);
}

void WaterRenderer::createTextures(const Device& device,
                                   BindlessDescriptors& bindless) {
    // ── 1. Water surface normal map (128×128) ─────────────────────────────
    // Two overlapping sine waves to simulate wind-driven ripples.
    {
        constexpr uint32_t W = 128, H = 128;
        std::vector<uint32_t> pixels(W * H);
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                float u = static_cast<float>(x) / W;
                float v = static_cast<float>(y) / H;
                // Two-frequency ripple
                float nx = std::sin(u * glm::two_pi<float>() * 4.0f
                                    + std::cos(v * glm::two_pi<float>() * 2.0f)) * 0.35f;
                float ny = std::sin(v * glm::two_pi<float>() * 4.0f
                                    + std::sin(u * glm::two_pi<float>() * 3.0f)) * 0.35f;
                float nz = std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny));
                pixels[y * W + x] = packRGBA(nx * 0.5f + 0.5f,
                                              ny * 0.5f + 0.5f,
                                              nz * 0.5f + 0.5f);
            }
        }
        m_normalMapTex = Texture::createFromPixels(device, pixels.data(),
                                                   W, H, VK_FORMAT_R8G8B8A8_UNORM);
    }

    // ── 2. Flow map (64×64) ───────────────────────────────────────────────
    // A mostly eastward (positive U) flow with gentle swirling variation.
    {
        constexpr uint32_t W = 64, H = 64;
        std::vector<uint32_t> pixels(W * H);
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                float u = static_cast<float>(x) / W;
                float v = static_cast<float>(y) / H;
                float fx = 0.60f + 0.12f * std::sin(v * glm::two_pi<float>() * 1.5f);
                float fy = 0.50f + 0.08f * std::cos(u * glm::two_pi<float>() * 2.0f);
                pixels[y * W + x] = packRGBA(fx, fy, 0.5f);
            }
        }
        m_flowMapTex = Texture::createFromPixels(device, pixels.data(),
                                                 W, H, VK_FORMAT_R8G8B8A8_UNORM);
    }

    // ── 3. Foam / noise texture (64×64) ──────────────────────────────────
    // Value-noise-like pattern mixing two sine octaves for organic foam patches.
    {
        constexpr uint32_t W = 64, H = 64;
        std::vector<uint32_t> pixels(W * H);
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                float u = static_cast<float>(x) / W;
                float v = static_cast<float>(y) / H;
                float n  = std::sin(u * glm::two_pi<float>() * 5.0f
                                    + std::cos(v * glm::two_pi<float>() * 7.0f)) * 0.5f + 0.5f;
                float n2 = std::sin(u * glm::two_pi<float>() * 11.0f
                                    + std::cos(v * glm::two_pi<float>() * 3.0f)) * 0.5f + 0.5f;
                float foam = n * 0.65f + n2 * 0.35f;
                pixels[y * W + x] = packRGBA(foam, foam, foam);
            }
        }
        m_foamTex = Texture::createFromPixels(device, pixels.data(),
                                              W, H, VK_FORMAT_R8G8B8A8_SRGB);
    }

    // ── Register in the bindless array ────────────────────────────────────
    m_normalMapIdx = static_cast<int>(bindless.registerTexture(
        m_normalMapTex.getImageView(), m_normalMapTex.getSampler()));
    m_flowMapIdx   = static_cast<int>(bindless.registerTexture(
        m_flowMapTex.getImageView(), m_flowMapTex.getSampler()));
    m_foamTexIdx   = static_cast<int>(bindless.registerTexture(
        m_foamTex.getImageView(), m_foamTex.getSampler()));

    spdlog::info("WaterRenderer: textures at bindless slots {},{},{}", m_normalMapIdx, m_flowMapIdx, m_foamTexIdx);
}

// ── Pipeline ──────────────────────────────────────────────────────────────────

void WaterRenderer::createPipeline(const RenderFormats& formats,
                                   VkDescriptorSetLayout mainLayout,
                                   VkDescriptorSetLayout bindlessLayout) {
    VkDevice dev = m_device->getDevice();

    auto readFile = [](const std::string& path) -> std::vector<char> {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) throw std::runtime_error("Shader not found: " + path);
        size_t sz = static_cast<size_t>(file.tellg());
        std::vector<char> buf(sz);
        file.seekg(0);
        file.read(buf.data(), static_cast<std::streamsize>(sz));
        return buf;
    };
    auto makeModule = [&](const std::vector<char>& code) {
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = code.size();
        ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule m;
        VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m), "water shader module");
        return m;
    };

    VkShaderModule vert = makeModule(readFile(std::string(SHADER_DIR) + "water.vert.spv"));
    VkShaderModule frag = makeModule(readFile(std::string(SHADER_DIR) + "water.frag.spv"));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                  VK_SHADER_STAGE_VERTEX_BIT, vert, "main" };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                  VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main" };

    // WaterVertex layout: vec3 position (location=0), vec2 uv (location=1)
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = sizeof(WaterVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0;
    attrs[0].binding  = 0;
    attrs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset   = offsetof(WaterVertex, position);
    attrs[1].location = 1;
    attrs[1].binding  = 0;
    attrs[1].format   = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset   = offsetof(WaterVertex, uv);

    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &binding;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1;
    vps.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rast{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.lineWidth   = 1.0f;
    rast.cullMode    = VK_CULL_MODE_BACK_BIT;
    rast.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth test ON (reads scene depth), depth write OFF (transparent surface)
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp   = VK_COMPARE_OP_GREATER;

    // Attachment 0 (HDR color): standard alpha blend
    VkPipelineColorBlendAttachmentState blendHDR{};
    blendHDR.blendEnable         = VK_TRUE;
    blendHDR.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendHDR.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendHDR.colorBlendOp        = VK_BLEND_OP_ADD;
    blendHDR.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendHDR.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendHDR.alphaBlendOp        = VK_BLEND_OP_ADD;
    blendHDR.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    // Attachment 1 (charDepth): no writes from water
    VkPipelineColorBlendAttachmentState blendDepth{};
    blendDepth.colorWriteMask = 0;

    VkPipelineColorBlendAttachmentState blends[2] = { blendHDR, blendDepth };
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 2;
    cb.pAttachments    = blends;

    // Push constant: WaterPC (92 bytes), vertex + fragment
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.size       = sizeof(WaterPC);

    VkDescriptorSetLayout setLayouts[2] = { mainLayout, bindlessLayout };
    VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.setLayoutCount         = 2;
    lci.pSetLayouts            = setLayouts;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges    = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(dev, &lci, nullptr, &m_pipelineLayout), "water pipeline layout");

    VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pci.stageCount          = 2;
    pci.pStages             = stages;
    pci.pVertexInputState   = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState      = &vps;
    pci.pRasterizationState = &rast;
    pci.pMultisampleState   = &ms;
    pci.pDepthStencilState  = &ds;
    pci.pColorBlendState    = &cb;
    pci.pDynamicState       = &dyn;
    VkPipelineRenderingCreateInfo fmtCI = formats.pipelineRenderingCI();
    pci.pNext               = &fmtCI;
    pci.layout              = m_pipelineLayout;
    pci.renderPass          = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &m_pipeline),
             "water pipeline");

    vkDestroyShaderModule(dev, frag, nullptr);
    vkDestroyShaderModule(dev, vert, nullptr);
    spdlog::info("WaterRenderer: pipeline created");
}

// ── Public interface ──────────────────────────────────────────────────────────

void WaterRenderer::init(const Device& device,
                         const RenderFormats& formats,
                         VkDescriptorSetLayout mainLayout,
                         VkDescriptorSetLayout bindlessLayout,
                         BindlessDescriptors& bindless) {
    m_device = &device;
    createMesh();
    createTextures(device, bindless);
    createPipeline(formats, mainLayout, bindlessLayout);
}

void WaterRenderer::render(VkCommandBuffer cmd, VkDescriptorSet mainSet,
                           VkDescriptorSet bindlessSet,
                           float time, const glm::mat4& model) {
    WaterPC pc{};
    pc.model               = model;
    pc.time                = time;
    pc.flowSpeed           = flowSpeed;
    pc.distortionStrength  = distortionStrength;
    pc.foamScale           = foamScale;
    pc.normalMapIdx        = m_normalMapIdx;
    pc.flowMapIdx          = m_flowMapIdx;
    pc.foamTexIdx          = m_foamTexIdx;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    VkDescriptorSet sets[2] = { mainSet, bindlessSet };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(WaterPC), &pc);

    VkBuffer     vb  = m_vertexBuffer.getBuffer();
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.getBuffer(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

void WaterRenderer::destroy() {
    if (!m_device) return;
    VkDevice dev = m_device->getDevice();

    if (m_pipeline)       { vkDestroyPipeline(dev, m_pipeline, nullptr);             m_pipeline       = VK_NULL_HANDLE; }
    if (m_pipelineLayout) { vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }

    m_normalMapTex = Texture{};
    m_flowMapTex   = Texture{};
    m_foamTex      = Texture{};
    m_vertexBuffer.destroy();
    m_indexBuffer.destroy();
    m_device = nullptr;
}

} // namespace glory
