#include "renderer/passes/PassSetup.h"

#include "renderer/FrameContext.h"
#include "renderer/Descriptors.h"
#include "renderer/BindlessDescriptors.h"
#include "renderer/Pipeline.h"
#include "renderer/HDRFramebuffer.h"
#include "renderer/Swapchain.h"
#include "renderer/ShadowPass.h"
#include "renderer/BloomPass.h"
#include "renderer/ToneMapPass.h"
#include "renderer/InkingPass.h"
#include "renderer/FogOfWarRenderer.h"
#include "renderer/OutlineRenderer.h"
#include "renderer/WaterRenderer.h"
#include "renderer/DistortionRenderer.h"
#include "renderer/ShieldBubbleRenderer.h"
#include "renderer/ExplosionRenderer.h"
#include "renderer/ClickIndicatorRenderer.h"
#include "renderer/GroundDecalRenderer.h"
#include "renderer/ConeAbilityRenderer.h"
#include "renderer/SpriteEffectRenderer.h"
#include "renderer/MegaBuffer.h"
#include "renderer/HiZPass.h"
#include "renderer/GpuTimer.h"
#include "nav/DebugRenderer.h"
#include "renderer/Device.h"
#include "vfx/VFXRenderer.h"
#include "vfx/TrailRenderer.h"
#include "vfx/MeshEffectRenderer.h"
#include "core/ThreadPool.h"
#include "renderer/ThreadedCommandPool.h"
#include "renderer/AsyncComputeManager.h"
#include "renderer/ParallelRecorder.h"
#include "renderer/SSAOPass.h"
#include "renderer/SSRPass.h"

#include <GLFW/glfw3.h>

namespace glory {

// ═══════════════════════════════════════════════════════════════════════════════
// Fog of War compute pass
// ═══════════════════════════════════════════════════════════════════════════════
RenderPassNode createFogOfWarPass() {
    RenderPassNode node;
    node.name = "FogOfWar";
    node.writes(res::FowMask,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_IMAGE_LAYOUT_GENERAL);
    node.execute = [](VkCommandBuffer cmd, const FrameContext& ctx) {
        if (!ctx.fogOfWar || ctx.isLauncher) return;
        if (ctx.gpuTimer) ctx.gpuTimer->beginZone(cmd, ctx.frameIndex, "FoW");
        ctx.fogOfWar->dispatch(cmd);
        if (ctx.gpuTimer) ctx.gpuTimer->endZone(cmd, ctx.frameIndex, "FoW");
    };
    return node;
}

// ═══════════════════════════════════════════════════════════════════════════════
// VFX acquire (queue family ownership transfer from async compute)
// ═══════════════════════════════════════════════════════════════════════════════
RenderPassNode createVFXAcquirePass() {
    RenderPassNode node;
    node.name = "VFXAcquire";
    node.writes(res::ParticleBuffer,
                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED);
    node.execute = [](VkCommandBuffer cmd, const FrameContext& ctx) {
        if (!ctx.vfxRenderer || ctx.isLauncher || !ctx.asyncCompute) return;
        if (ctx.gpuTimer) ctx.gpuTimer->beginZone(cmd, ctx.frameIndex, "VFX Acquire");
        uint32_t computeFamily  = ctx.asyncCompute->getQueueFamilyIndex();
        uint32_t graphicsFamily = ctx.device->getQueueFamilies().graphicsFamily.value();
        ctx.vfxRenderer->acquireFromCompute(cmd, computeFamily, graphicsFamily);
        if (ctx.gpuTimer) ctx.gpuTimer->endZone(cmd, ctx.frameIndex, "VFX Acquire");
    };
    return node;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Shadow pass
// ═══════════════════════════════════════════════════════════════════════════════
RenderPassNode createShadowRenderPass() {
    RenderPassNode node;
    node.name = "Shadow";
    node.writes(res::ShadowMap,
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    node.execute = [](VkCommandBuffer cmd, const FrameContext& ctx) {
        if (ctx.isLauncher || !ctx.shadowPass) return;
        if (ctx.gpuTimer) ctx.gpuTimer->beginZone(cmd, ctx.frameIndex, "Shadow");
        // Shadow recording is handled by the Renderer since it involves
        // entity iteration and parallel recording — delegated via callback
        // stored in FrameContext by the orchestrator.
        if (ctx.gpuTimer) ctx.gpuTimer->endZone(cmd, ctx.frameIndex, "Shadow");
    };
    return node;
}

// ═══════════════════════════════════════════════════════════════════════════════
// G-Buffer / HDR geometry pass (opaque static + skinned mesh rendering)
// ═══════════════════════════════════════════════════════════════════════════════
RenderPassNode createGBufferRenderPass() {
    RenderPassNode node;
    node.name = "GBuffer";
    node.reads(res::ShadowMap,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    node.reads(res::FowMask,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    node.writes(res::HdrColor,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    node.writes(res::HdrDepth,
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    node.writes(res::CharDepth,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    node.execute = [](VkCommandBuffer /*cmd*/, const FrameContext& /*ctx*/) {
        // Entity iteration + parallel recording is handled by the orchestrator
        // via dedicated callback since it needs Renderer's entity state.
    };
    return node;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Lighting (placeholder — currently fused into triangle.frag in the GBuffer pass)
// ═══════════════════════════════════════════════════════════════════════════════
RenderPassNode createLightingRenderPass() {
    RenderPassNode node;
    node.name = "Lighting";
    node.reads(res::HdrColor);
    node.reads(res::HdrDepth);
    node.enabled = false; // fused into GBuffer for now
    node.execute = [](VkCommandBuffer, const FrameContext&) {};
    return node;
}

// ═══════════════════════════════════════════════════════════════════════════════
// HiZ pyramid generation (placeholder — init'd but not dispatched in current frame)
// ═══════════════════════════════════════════════════════════════════════════════
RenderPassNode createHiZRenderPass() {
    RenderPassNode node;
    node.name = "HiZ";
    node.reads(res::HdrDepth,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    node.writes(res::HiZPyramid,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_IMAGE_LAYOUT_GENERAL);
    node.enabled = false; // activated when GPU-driven culling is enabled
    node.execute = [](VkCommandBuffer, const FrameContext&) {};
    return node;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Occlusion cull compute (placeholder — occlusion_cull.comp exists but not wired)
// ═══════════════════════════════════════════════════════════════════════════════
RenderPassNode createOcclusionCullPass() {
    RenderPassNode node;
    node.name = "OcclusionCull";
    node.reads(res::HiZPyramid,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    node.writes(res::IndirectBuffer,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    node.enabled = false; // activated when GPU-driven culling is enabled
    node.execute = [](VkCommandBuffer, const FrameContext&) {};
    return node;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Transparent / VFX render pass (inking, particles, water, effects, distortion)
// ═══════════════════════════════════════════════════════════════════════════════
RenderPassNode createTransparentVFXPass() {
    RenderPassNode node;
    node.name = "TransparentVFX";
    node.reads(res::HdrDepth,
               VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
               VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    node.reads(res::CharDepth,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    node.reads(res::ParticleBuffer,
               VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
               VK_ACCESS_2_SHADER_READ_BIT);
    node.writes(res::HdrColor,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    node.execute = [](VkCommandBuffer /*cmd*/, const FrameContext& /*ctx*/) {
        // Transparent/VFX rendering is handled by the orchestrator
        // since it involves many subsystem calls with specific setup.
    };
    return node;
}

// ═══════════════════════════════════════════════════════════════════════════════
// SSAO compute pass
// ═══════════════════════════════════════════════════════════════════════════════
RenderPassNode createSSAORenderPass() {
    RenderPassNode node;
    node.name = "SSAO";
    node.reads(res::HdrDepth,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    node.writes(res::AOTexture,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_IMAGE_LAYOUT_GENERAL);
    node.execute = [](VkCommandBuffer cmd, const FrameContext& ctx) {
        if (!ctx.ssaoPass || ctx.isLauncher) return;
        if (ctx.gpuTimer) ctx.gpuTimer->beginZone(cmd, ctx.frameIndex, "SSAO");
        glm::mat4 invProj = glm::inverse(ctx.proj);
        ctx.ssaoPass->dispatch(cmd, invProj);
        if (ctx.gpuTimer) ctx.gpuTimer->endZone(cmd, ctx.frameIndex, "SSAO");
    };
    return node;
}

// ═══════════════════════════════════════════════════════════════════════════════
// SSR compute pass (reads scene color + depth, writes half-res reflection)
// ═══════════════════════════════════════════════════════════════════════════════
RenderPassNode createSSRRenderPass() {
    RenderPassNode node;
    node.name = "SSR";
    node.reads(res::HdrColor,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    node.reads(res::HdrDepth,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    node.writes(res::SSRTexture,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_IMAGE_LAYOUT_GENERAL);
    node.execute = [](VkCommandBuffer cmd, const FrameContext& ctx) {
        if (!ctx.ssrPass || ctx.isLauncher) return;
        if (ctx.gpuTimer) ctx.gpuTimer->beginZone(cmd, ctx.frameIndex, "SSR");
        glm::mat4 invViewProj = glm::inverse(ctx.viewProj);
        ctx.ssrPass->dispatch(cmd, ctx.viewProj, invViewProj, ctx.cameraPos);
        if (ctx.gpuTimer) ctx.gpuTimer->endZone(cmd, ctx.frameIndex, "SSR");
    };
    return node;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Bloom compute pass
// ═══════════════════════════════════════════════════════════════════════════════
RenderPassNode createBloomRenderPass() {
    RenderPassNode node;
    node.name = "Bloom";
    node.reads(res::HdrColor,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    node.writes(res::BloomMips,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_IMAGE_LAYOUT_GENERAL);
    node.execute = [](VkCommandBuffer cmd, const FrameContext& ctx) {
        if (!ctx.bloom) return;
        if (ctx.gpuTimer) ctx.gpuTimer->beginZone(cmd, ctx.frameIndex, "Bloom");
        ctx.bloom->dispatch(cmd);
        if (ctx.gpuTimer) ctx.gpuTimer->endZone(cmd, ctx.frameIndex, "Bloom");
    };
    return node;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Tonemap + ImGui → swapchain
// ═══════════════════════════════════════════════════════════════════════════════
RenderPassNode createTonemapRenderPass() {
    RenderPassNode node;
    node.name = "Tonemap";
    node.reads(res::HdrColor,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    node.reads(res::BloomMips,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    node.writes(res::SwapchainImage,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    node.execute = [](VkCommandBuffer /*cmd*/, const FrameContext& /*ctx*/) {
        // Tonemap + present barrier handled by orchestrator (needs swapchain image)
    };
    return node;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Build the default render graph with all passes in correct order
// ═══════════════════════════════════════════════════════════════════════════════
void buildDefaultRenderGraph(RenderGraph& graph) {
    graph.clear();

    graph.addPass(createFogOfWarPass());
    graph.addPass(createVFXAcquirePass());
    graph.addPass(createShadowRenderPass());
    graph.addPass(createGBufferRenderPass());
    graph.addPass(createLightingRenderPass());
    graph.addPass(createHiZRenderPass());
    graph.addPass(createOcclusionCullPass());
    graph.addPass(createTransparentVFXPass());
    graph.addPass(createSSAORenderPass());
    graph.addPass(createSSRRenderPass());
    graph.addPass(createBloomRenderPass());
    graph.addPass(createTonemapRenderPass());

    graph.compile();
}

} // namespace glory
