#include "renderer/Renderer.h"
#include "renderer/VkCheck.h"
#include "renderer/Model.h"
#include "renderer/StaticSkinnedMesh.h"
#include "renderer/Frustum.h"
#include "renderer/passes/PassSetup.h"
#include "scene/Components.h"
#include "combat/CombatComponents.h"
#include "ability/AbilityComponents.h"

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace glory {

void Renderer::recordShadowPass(VkCommandBuffer cmd, const FrameContext& ctx) {
    VkDescriptorSet ds = m_descriptors->getSet(m_currentFrame);

    // Pre-collect static entities for random-access indexing
    struct ShadowEntity { uint32_t meshIndex; glm::mat4 model; };
    std::vector<ShadowEntity> shadowEntities;
    {
        auto staticView = m_scene.getRegistry()
            .view<TransformComponent, MeshComponent, MaterialComponent>(
                entt::exclude<GPUSkinnedMeshComponent>);
        for (auto&& [e, t, mc, mat] : staticView.each()) {
            if (shadowEntities.size() >= MAX_INSTANCES) break;
            shadowEntities.push_back({mc.meshIndex, t.getInterpolatedModelMatrix(m_renderAlpha)});
        }
    }

    // Pre-fill instance buffer model matrices for shadow
    auto* instances = static_cast<InstanceData*>(m_instanceMapped[m_currentFrame]);
    for (uint32_t i = 0; i < shadowEntities.size(); ++i) {
        instances[i].model = shadowEntities[i].model;
    }

    if (m_gpuTimer) m_gpuTimer->beginZone(cmd, m_currentFrame, "Shadow");

    if (shadowEntities.size() >= PARALLEL_THRESHOLD) {
        auto shadowFormats = RenderFormats::depthOnly(ShadowPass::DEPTH_FORMAT);
        VkBuffer instBuf = m_instanceBuffers[m_currentFrame].getBuffer();

        auto parallelStaticFn = [&](uint32_t /*cascade*/, const glm::mat4& lvp,
                                    VkViewport vp, VkRect2D sc) -> std::vector<VkCommandBuffer> {
            auto result = ParallelRecorder::record(
                m_threadPool, m_cmdPools, m_currentFrame, shadowFormats,
                static_cast<uint32_t>(shadowEntities.size()),
                [&](VkCommandBuffer scmd, uint32_t start, uint32_t end) {
                    vkCmdSetViewport(scmd, 0, 1, &vp);
                    vkCmdSetScissor(scmd, 0, 1, &sc);
                    vkCmdBindPipeline(scmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      m_shadowPass.getStaticPipeline());
                    vkCmdPushConstants(scmd, m_shadowPass.getPipelineLayout(),
                                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &lvp);
                    for (uint32_t i = start; i < end; ++i) {
                        VkDeviceSize instOffset = i * sizeof(InstanceData);
                        vkCmdBindVertexBuffers(scmd, 1, 1, &instBuf, &instOffset);
                        m_scene.getMesh(shadowEntities[i].meshIndex).draw(scmd);
                    }
                });
            return result.secondaryBuffers;
        };

        m_shadowPass.recordCommandsParallel(cmd, parallelStaticFn, nullptr);
    } else {
        auto drawStaticShadows = [&](VkCommandBuffer scmd, uint32_t /*cascade*/) {
            VkBuffer instBuf = m_instanceBuffers[m_currentFrame].getBuffer();
            for (uint32_t i = 0; i < static_cast<uint32_t>(shadowEntities.size()); ++i) {
                VkDeviceSize instOffset = i * sizeof(InstanceData);
                vkCmdBindVertexBuffers(scmd, 1, 1, &instBuf, &instOffset);
                m_scene.getMesh(shadowEntities[i].meshIndex).draw(scmd);
            }
        };
        m_shadowPass.recordCommands(cmd, drawStaticShadows, nullptr);
    }

    if (m_gpuTimer) m_gpuTimer->endZone(cmd, m_currentFrame, "Shadow");
}

// ── recordGBufferPass ────────────────────────────────────────────────────────
void Renderer::recordGBufferPass(VkCommandBuffer cmd, const FrameContext& ctx) {
    auto ext = ctx.extent;
    float aspect = ctx.aspect;

    // ── Transition HDR attachments to attachment-optimal layouts ─────────────
    {
        VkImageMemoryBarrier2 barriers[3]{};
        barriers[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barriers[0].srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
        barriers[0].srcAccessMask = VK_ACCESS_2_NONE;
        barriers[0].dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barriers[0].image = m_hdrFB->colorImage();
        barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        barriers[1] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barriers[1].srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
        barriers[1].srcAccessMask = VK_ACCESS_2_NONE;
        barriers[1].dstStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barriers[1].image = m_hdrFB->depthImage();
        barriers[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};

        barriers[2] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barriers[2].srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
        barriers[2].srcAccessMask = VK_ACCESS_2_NONE;
        barriers[2].dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barriers[2].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[2].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barriers[2].image = m_hdrFB->charDepthImage();
        barriers[2].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        depInfo.imageMemoryBarrierCount = 3;
        depInfo.pImageMemoryBarriers    = barriers;
        vkCmdPipelineBarrier2(cmd, &depInfo);
    }

    VkRenderingAttachmentInfo hdrColorAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    hdrColorAttach.imageView   = m_hdrFB->colorView();
    hdrColorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    hdrColorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    hdrColorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    hdrColorAttach.clearValue.color = {{ 0.08f, 0.10f, 0.14f, 1.0f }};

    VkRenderingAttachmentInfo hdrCharDepthAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    hdrCharDepthAttach.imageView   = m_hdrFB->characterDepthView();
    hdrCharDepthAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    hdrCharDepthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    hdrCharDepthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    hdrCharDepthAttach.clearValue.color = {{ 0.0f, 0.0f, 0.0f, 0.0f }};

    VkRenderingAttachmentInfo hdrColorAttachments[2] = {hdrColorAttach, hdrCharDepthAttach};

    VkRenderingAttachmentInfo hdrDepthAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    hdrDepthAttach.imageView   = m_hdrFB->depthAttachmentView();
    hdrDepthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    hdrDepthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    hdrDepthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    hdrDepthAttach.clearValue.depthStencil = { 0.0f, 0 }; // reversed-Z: far = 0.0

    VkRenderingInfo hdrRenderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    hdrRenderInfo.renderArea          = { {0, 0}, ext };
    hdrRenderInfo.layerCount          = 1;
    hdrRenderInfo.colorAttachmentCount = 2;
    hdrRenderInfo.pColorAttachments   = hdrColorAttachments;
    hdrRenderInfo.pDepthAttachment    = &hdrDepthAttach;
    hdrRenderInfo.pStencilAttachment  = &hdrDepthAttach;

    VkViewport vp{ 0, 0, static_cast<float>(ext.width), static_cast<float>(ext.height), 0, 1 };
    VkRect2D   sc{ {0, 0}, ext };

    // ── Phase 1: CPU data fill (sequential, before rendering) ───────────────
    struct SkinnedDrawInfo {
        entt::entity entity;
        VkBuffer     instBuf;
        VkDeviceSize instOffset;
        uint32_t     boneBase;
        uint32_t     meshIdx;
    };
    // Use frame allocator instead of std::vector for transient draw list
    constexpr uint32_t MAX_SKINNED_DRAWS = 512;
    auto* skinnedDrawData = m_frameAllocator.alloc<SkinnedDrawInfo>(MAX_SKINNED_DRAWS);
    uint32_t skinnedDrawCount = 0;
    uint32_t objectCount   = 0;
    uint32_t instanceIndex = 0;

    if (!m_menuMode) {
        auto* instances = static_cast<InstanceData*>(m_instanceMapped[m_currentFrame]);
        auto* sceneData = static_cast<GpuObjectData*>(m_sceneMapped[m_currentFrame]);
        VkDescriptorSet ds = m_descriptors->getSet(m_currentFrame);

        // ── Static entities ─────────────────────────────────────────────────
        glm::vec3 camPos = m_isoCam.getPosition();

        auto staticView = m_scene.getRegistry()
            .view<TransformComponent, MeshComponent, MaterialComponent>(
                entt::exclude<GPUSkinnedMeshComponent>);
        for (auto&& [e, t, mc, mat] : staticView.each()) {
            if (objectCount >= MAX_INSTANCES) break;

            // FoW: skip enemies not in vision
            if (!FogOfWarGameplay::shouldRender(m_scene.getRegistry(), e, Team::PLAYER))
                continue;

            glm::mat4 model = t.getInterpolatedModelMatrix(m_renderAlpha);
            glm::vec3 worldPos = glm::vec3(model[3]);
            float dist = glm::length(worldPos - camPos);

            glm::vec4 tint(1.0f);
            if (e == m_hoveredEntity) tint = glm::vec4(1.0f, 0.4f, 0.4f, 1.0f);
            else if (auto* tc = m_scene.getRegistry().try_get<TintComponent>(e)) tint = tc->color;

            // FoW fade: modulate alpha for enemies fading in/out
            float fowAlpha = FogOfWarGameplay::getRenderAlpha(m_scene.getRegistry(), e);
            tint.a *= fowAlpha;

            uint32_t mi = mc.meshIndex;
            if (mi >= m_meshHandles.size()) continue;

            // Determine sub-mesh range: -1 means draw all sub-meshes
            uint32_t subCount = static_cast<uint32_t>(m_meshHandles[mi].size());
            uint32_t siStart  = (mc.subMeshIndex < 0) ? 0 : static_cast<uint32_t>(mc.subMeshIndex);
            uint32_t siEnd    = (mc.subMeshIndex < 0) ? subCount : siStart + 1;

            glm::mat4 normalMat = glm::transpose(glm::inverse(model));
            AABB worldAABB = mc.localAABB.transformed(model);
            glm::vec4 params(mat.shininess, mat.metallic, mat.roughness, mat.emissive);

            for (uint32_t si = siStart; si < siEnd && objectCount < MAX_INSTANCES; ++si) {
                // Per-sub-mesh texture: prefer subMeshAlbedo[si] when available
                uint32_t diffuseTex = (si < mat.subMeshAlbedo.size())
                    ? mat.subMeshAlbedo[si]
                    : mat.materialIndex;

                instances[objectCount].model        = model;
                instances[objectCount].normalMatrix = normalMat;
                instances[objectCount].tint         = tint;
                instances[objectCount].params       = params;
                instances[objectCount].texIndices   = glm::vec4(
                    static_cast<float>(diffuseTex),
                    static_cast<float>(mat.normalMapIndex), 0.0f, 0.0f);

                const auto& mh = m_meshHandles[mi][si];
                LODLevel lod = m_lodSystem.getLODLevel(mi, si, dist);
                uint32_t vOff = (lod.indexCount > 0) ? lod.vertexOffset : mh.vertexOffset;
                uint32_t iOff = (lod.indexCount > 0) ? lod.indexOffset  : mh.indexOffset;
                uint32_t iCnt = (lod.indexCount > 0) ? lod.indexCount   : mh.indexCount;

                auto& obj = sceneData[objectCount];
                obj.model            = model;
                obj.normalMatrix     = normalMat;
                obj.aabbMin          = glm::vec4(worldAABB.min, 0.0f);
                obj.aabbMax          = glm::vec4(worldAABB.max, 0.0f);
                obj.tint             = tint;
                obj.params           = params;
                obj.texIndices       = glm::vec4(
                    static_cast<float>(diffuseTex),
                    static_cast<float>(mat.normalMapIndex), 0.0f, 0.0f);
                obj.meshVertexOffset = vOff;
                obj.meshIndexOffset  = iOff;
                obj.meshIndexCount   = iCnt;
                obj._pad             = 0;

                ++objectCount;
            }
        }
        instanceIndex = objectCount;

        // Flush scene buffer descriptor
        if (objectCount > 0) {
            VkDeviceSize usedSize = objectCount * sizeof(GpuObjectData);
            m_sceneBuffers[m_currentFrame].flush();
            m_descriptors->writeSceneBuffer(m_currentFrame,
                m_sceneBuffers[m_currentFrame].getBuffer(), usedSize);
        }

        // ── Skinned entities ────────────────────────────────────────────────
        auto skinnedView = m_scene.getRegistry()
            .view<TransformComponent, MaterialComponent, GPUSkinnedMeshComponent>();
        for (auto&& [e, t, mat, ssm] : skinnedView.each()) {
            if (instanceIndex >= MAX_INSTANCES) break;

            // FoW: skip enemies not in vision
            if (!FogOfWarGameplay::shouldRender(m_scene.getRegistry(), e, Team::PLAYER))
                continue;

            glm::mat4 model = t.getInterpolatedModelMatrix(m_renderAlpha);
            instances[instanceIndex].model        = model;
            instances[instanceIndex].normalMatrix = glm::transpose(glm::inverse(model));

            glm::vec4 tint(1.0f);
            if (e == m_hoveredEntity) {
                tint = glm::vec4(1.0f, 0.6f, 0.6f, 1.0f);
            } else if (auto* tc = m_scene.getRegistry().try_get<TintComponent>(e)) {
                tint = tc->color;
            }

            glm::vec3 worldPos = t.position;
            float dist = glm::distance(worldPos, camPos);

            // FoW fade: modulate alpha for enemies fading in/out
            float fowAlpha = FogOfWarGameplay::getRenderAlpha(m_scene.getRegistry(), e);
            tint.a *= fowAlpha;

            instances[instanceIndex].tint = tint;
            instances[instanceIndex].params = glm::vec4(mat.shininess, mat.metallic, mat.roughness, mat.emissive);
            instances[instanceIndex].texIndices = glm::vec4(
                static_cast<float>(mat.materialIndex), static_cast<float>(mat.normalMapIndex), 0.0f, 0.0f);

            VkBuffer     instBuf    = m_instanceBuffers[m_currentFrame].getBuffer();
            VkDeviceSize instOffset = instanceIndex * sizeof(InstanceData);
            uint32_t boneBase = ssm.boneSlot * Descriptors::MAX_BONES;

            if (skinnedDrawCount < MAX_SKINNED_DRAWS)
                skinnedDrawData[skinnedDrawCount++] = {e, instBuf, instOffset, boneBase, ssm.staticSkinnedMeshIndex};
            ++instanceIndex;
        }
    }

    // ── Phase 2: HDR Scope 1 — parallel geometry draws ──────────────────────
    if (m_gpuTimer) m_gpuTimer->beginZone(cmd, m_currentFrame, "Geometry");

    if (!m_menuMode) {
        VkDescriptorSet ds = m_descriptors->getSet(m_currentFrame);
        auto* sceneData = static_cast<GpuObjectData*>(m_sceneMapped[m_currentFrame]);
        RenderFormats hdrFormats = m_hdrFB->mainFormats();

        // Record static mesh draws in parallel
        ParallelRecordResult staticResult;
        if (objectCount > 0) {
            staticResult = ParallelRecorder::record(
                m_threadPool, m_cmdPools, m_currentFrame, hdrFormats, objectCount,
                [&](VkCommandBuffer scmd, uint32_t start, uint32_t end) {
                    VkViewport lvp{ 0, 0, static_cast<float>(ext.width), static_cast<float>(ext.height), 0, 1 };
                    VkRect2D lsc{ {0,0}, ext };
                    vkCmdSetViewport(scmd, 0, 1, &lvp);
                    vkCmdSetScissor(scmd, 0, 1, &lsc);
                    VkPipeline mainPipeline = m_wireframe ? m_pipeline->getWireframePipeline()
                                                          : m_pipeline->getGraphicsPipeline();
                    vkCmdBindPipeline(scmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mainPipeline);
                    VkDescriptorSet sets[2] = { ds, m_bindless->getSet() };
                    vkCmdBindDescriptorSets(scmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            m_pipeline->getPipelineLayout(), 0, 2, sets, 0, nullptr);
                    m_megaBuffer->bind(scmd);
                    for (uint32_t i = start; i < end; ++i) {
                        vkCmdDrawIndexed(scmd, sceneData[i].meshIndexCount, 1,
                            sceneData[i].meshIndexOffset,
                            static_cast<int32_t>(sceneData[i].meshVertexOffset), i);
                    }
                });
        }

        // Record skinned mesh draws in parallel
        ParallelRecordResult skinnedResult;
        if (skinnedDrawCount > 0) {
            skinnedResult = ParallelRecorder::record(
                m_threadPool, m_cmdPools, m_currentFrame, hdrFormats,
                skinnedDrawCount,
                [&](VkCommandBuffer scmd, uint32_t start, uint32_t end) {
                    VkViewport lvp{ 0, 0, static_cast<float>(ext.width), static_cast<float>(ext.height), 0, 1 };
                    VkRect2D lsc{ {0,0}, ext };
                    vkCmdSetViewport(scmd, 0, 1, &lvp);
                    vkCmdSetScissor(scmd, 0, 1, &lsc);
                    vkCmdBindPipeline(scmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedPipeline);
                    VkDescriptorSet sets[2] = { ds, m_bindless->getSet() };
                    vkCmdBindDescriptorSets(scmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            m_skinnedPipelineLayout, 0, 2, sets, 0, nullptr);
                    for (uint32_t i = start; i < end; ++i) {
                        auto& info = skinnedDrawData[i];
                        vkCmdBindVertexBuffers(scmd, 1, 1, &info.instBuf, &info.instOffset);
                        vkCmdPushConstants(scmd, m_skinnedPipelineLayout,
                                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                           0, sizeof(uint32_t), &info.boneBase);
                        const auto& mesh = m_scene.getStaticSkinnedMesh(info.meshIdx);
                        mesh.bind(scmd);
                        mesh.draw(scmd);
                    }
                });
        }

        bool hasSecondaries = !staticResult.secondaryBuffers.empty() ||
                              !skinnedResult.secondaryBuffers.empty();

        if (hasSecondaries)
            hdrRenderInfo.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;

        vkCmdBeginRendering(cmd, &hdrRenderInfo);
        if (!staticResult.secondaryBuffers.empty())
            vkCmdExecuteCommands(cmd, static_cast<uint32_t>(staticResult.secondaryBuffers.size()),
                                 staticResult.secondaryBuffers.data());
        if (!skinnedResult.secondaryBuffers.empty())
            vkCmdExecuteCommands(cmd, static_cast<uint32_t>(skinnedResult.secondaryBuffers.size()),
                                 skinnedResult.secondaryBuffers.data());
        vkCmdEndRendering(cmd);

        // ── Self-synchronizing barrier between scope 1 and scope 2 ──────────
        {
            VkMemoryBarrier2 memBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            memBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
                                     | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            memBarrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                                     | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            memBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
                                     | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
            memBarrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                                     | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT
                                     | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                     | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &memBarrier;
            vkCmdPipelineBarrier2(cmd, &dep);
        }

        // ── Phase 3: HDR Scope 2 (inline, LOAD_OP_LOAD) — outlines + extras ─
        {
            VkRenderingAttachmentInfo loadColorAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            loadColorAttach.imageView   = m_hdrFB->colorView();
            loadColorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            loadColorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
            loadColorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingAttachmentInfo loadCharDepthAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            loadCharDepthAttach.imageView   = m_hdrFB->characterDepthView();
            loadCharDepthAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            loadCharDepthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
            loadCharDepthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingAttachmentInfo loadColorAttachments[2] = {loadColorAttach, loadCharDepthAttach};

            VkRenderingAttachmentInfo loadDepthAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            loadDepthAttach.imageView   = m_hdrFB->depthAttachmentView();
            loadDepthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            loadDepthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
            loadDepthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo loadRenderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
            loadRenderInfo.renderArea          = { {0, 0}, ext };
            loadRenderInfo.layerCount          = 1;
            loadRenderInfo.colorAttachmentCount = 2;
            loadRenderInfo.pColorAttachments   = loadColorAttachments;
            loadRenderInfo.pDepthAttachment    = &loadDepthAttach;
            loadRenderInfo.pStencilAttachment  = &loadDepthAttach;

            vkCmdBeginRendering(cmd, &loadRenderInfo);

            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);

            VkDescriptorSet sets[2] = { ds, m_bindless->getSet() };

            // ── Team-colored outlines (LoL Stage 8) ─────────────────────────
            if (m_outlineRenderer && skinnedDrawCount > 0) {
                auto& reg = m_scene.getRegistry();

                entt::entity attackTarget = entt::null;
                if (reg.valid(m_playerEntity) &&
                    reg.all_of<CombatComponent>(m_playerEntity)) {
                    attackTarget = reg.get<CombatComponent>(m_playerEntity).targetEntity;
                    if (!reg.valid(attackTarget)) attackTarget = entt::null;
                }

                for (uint32_t si = 0; si < skinnedDrawCount; ++si) {
                    const auto& info = skinnedDrawData[si];
                    entt::entity e      = info.entity;
                    bool isHovered      = (e == m_hoveredEntity);
                    bool isSelected     = reg.all_of<SelectableComponent>(e) &&
                                          reg.get<SelectableComponent>(e).isSelected;
                    bool isAttackTarget = (e == attackTarget);

                    const auto& mesh = m_scene.getStaticSkinnedMesh(info.meshIdx);

                    if (isHovered) {
                        m_outlineRenderer->renderOutline(
                            cmd, ds, info.instBuf, info.instOffset,
                            info.boneBase, mesh,
                            0.035f, glm::vec4(1.0f, 0.85f, 0.0f, 1.0f)); // gold
                    } else if (isAttackTarget) {
                        m_outlineRenderer->renderOutline(
                            cmd, ds, info.instBuf, info.instOffset,
                            info.boneBase, mesh,
                            0.030f, glm::vec4(1.0f, 0.25f, 0.1f, 1.0f)); // enemy red
                    } else if (isSelected) {
                        m_outlineRenderer->renderOutline(
                            cmd, ds, info.instBuf, info.instOffset,
                            info.boneBase, mesh,
                            0.030f, glm::vec4(0.2f, 0.5f, 1.0f, 1.0f)); // blue team
                    }
                }

                // Restore the skinned pipeline for anything that follows
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedPipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_skinnedPipelineLayout, 0, 2, sets, 0, nullptr);
            }

            // ── Debug grid ──────────────────────────────────────────────────
            if (m_showGrid && m_gridPipeline != VK_NULL_HANDLE) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gridPipeline);
                struct GridPC { glm::mat4 viewProj; float gridY; } gridPC;
                gridPC.viewProj = ctx.viewProj;
                gridPC.gridY    = 0.0f;
                vkCmdPushConstants(cmd, m_gridPipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(GridPC), &gridPC);
                vkCmdDraw(cmd, 6, 1, 0, 0);
            }

            // ── Ground decals (depth test ON, depth write OFF, alpha blend) ──
            if (m_groundDecalRenderer) {
                float appTime = static_cast<float>(glfwGetTime());
                m_groundDecalRenderer->render(cmd, ctx.viewProj, appTime);
            }

            // ── Click indicator ─────────────────────────────────────────────
            if (m_clickAnim && m_clickIndicatorRenderer) {
                float t_norm = m_clickAnim->lifetime / m_clickAnim->maxLife;
                m_clickIndicatorRenderer->render(cmd, ctx.viewProj, m_clickAnim->position, t_norm, 1.5f);
            }

            // ── Debug Renderer (Marquee Box) ────────────────────────────────
            m_debugRenderer.render(cmd, ctx.viewProj);

            // ── Water surface (alpha-blend, depth-test ON, depth-write OFF) ──
            if (m_waterRenderer) {
                float appTime = static_cast<float>(glfwGetTime());
                glm::mat4 waterModel = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.25f, 0.0f))
                                     * glm::scale(glm::mat4(1.0f), glm::vec3(200.0f, 1.0f, 200.0f));
                m_waterRenderer->render(cmd, m_descriptors->getSet(m_currentFrame),
                                        m_bindless->getSet(), appTime, waterModel);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedPipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_skinnedPipelineLayout, 0, 2, sets, 0, nullptr);
            }

            vkCmdEndRendering(cmd);
        }
    } else {
        // Launcher mode: just clear the HDR framebuffer
        vkCmdBeginRendering(cmd, &hdrRenderInfo);
        vkCmdEndRendering(cmd);
    }

    if (m_gpuTimer) m_gpuTimer->endZone(cmd, m_currentFrame, "Geometry");

    // ── Transition HDR attachments to read-only layouts ─────────────────────
    {
        VkImageMemoryBarrier2 barriers[3]{};
        barriers[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barriers[0].srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barriers[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[0].dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[0].image = m_hdrFB->colorImage();
        barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        barriers[1] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barriers[1].srcStageMask  = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        barriers[1].srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barriers[1].dstStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        barriers[1].image = m_hdrFB->depthImage();
        barriers[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};

        barriers[2] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barriers[2].srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barriers[2].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[2].dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barriers[2].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barriers[2].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barriers[2].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[2].image = m_hdrFB->charDepthImage();
        barriers[2].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        depInfo.imageMemoryBarrierCount = 3;
        depInfo.pImageMemoryBarriers    = barriers;
        vkCmdPipelineBarrier2(cmd, &depInfo);
    }
}

// ── recordTransparentVFXPass ─────────────────────────────────────────────────
void Renderer::recordTransparentVFXPass(VkCommandBuffer cmd, const FrameContext& ctx) {
    auto ext = ctx.extent;
    float aspect = ctx.aspect;

    // Copy scene color for distortion effects before rendering transparents over it
    bool copiedColor = false;
    if (m_distortionRenderer) {
        m_hdrFB->copyColor(cmd);
        copiedColor = true;
    }

    // Transition color back to attachment for load pass writing
    // (copyColor already leaves color in COLOR_ATTACHMENT_OPTIMAL — skip if done)
    if (!copiedColor) {
        VkImageMemoryBarrier2 colorToAttach{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        colorToAttach.srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        colorToAttach.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        colorToAttach.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        colorToAttach.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
        colorToAttach.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        colorToAttach.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorToAttach.image = m_hdrFB->colorImage();
        colorToAttach.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        depInfo.imageMemoryBarrierCount = 1;
        depInfo.pImageMemoryBarriers    = &colorToAttach;
        vkCmdPipelineBarrier2(cmd, &depInfo);
    }

    // Load pass: color LOAD (preserve geometry), charDepth read-only (inking reads it),
    // depth read-only (depth test, no write)
    VkRenderingAttachmentInfo loadColorAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    loadColorAttach.imageView   = m_hdrFB->colorView();
    loadColorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    loadColorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
    loadColorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingAttachmentInfo loadCharDepthAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    loadCharDepthAttach.imageView   = m_hdrFB->characterDepthView();
    loadCharDepthAttach.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    loadCharDepthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
    loadCharDepthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_NONE;

    VkRenderingAttachmentInfo loadColorAttachments[2] = {loadColorAttach, loadCharDepthAttach};

    VkRenderingAttachmentInfo loadDepthAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    loadDepthAttach.imageView   = m_hdrFB->depthAttachmentView();
    loadDepthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    loadDepthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
    loadDepthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_NONE;

    VkRenderingInfo loadRenderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    loadRenderInfo.renderArea           = { {0, 0}, ext };
    loadRenderInfo.layerCount           = 1;
    loadRenderInfo.colorAttachmentCount = 2;
    loadRenderInfo.pColorAttachments    = loadColorAttachments;
    loadRenderInfo.pDepthAttachment     = &loadDepthAttach;
    loadRenderInfo.pStencilAttachment   = &loadDepthAttach;

    vkCmdBeginRendering(cmd, &loadRenderInfo);
    if (m_gpuTimer) m_gpuTimer->beginZone(cmd, m_currentFrame, "VFX/Trans");

    VkViewport vp{ 0, 0, static_cast<float>(ext.width), static_cast<float>(ext.height), 0, 1 };
    VkRect2D   sc{ {0, 0}, ext };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    // ── Character outline (Sobel inking pass) ───────────────────────────────
    if (m_inkingPass) {
        m_inkingPass->render(cmd, 0.02f, 1.5f, glm::vec4(0.05f, 0.03f, 0.02f, 1.0f));
    }

    glm::mat4 viewProj = ctx.viewProj;

    // ── VFX particle render pass ────────────────────────────────────────────
    if (m_vfxRenderer) {
        const glm::mat4& view = m_isoCam.getViewMatrix();
        glm::vec3 camRight{view[0][0], view[1][0], view[2][0]};
        glm::vec3 camUp   {view[0][1], view[1][1], view[2][1]};
        glm::vec2 screenSize(static_cast<float>(ext.width), static_cast<float>(ext.height));
        m_vfxRenderer->render(cmd, viewProj, camRight, camUp,
                              screenSize, m_isoCam.getNear(), m_isoCam.getFar());
    }

    // ── Trail ribbon render pass ────────────────────────────────────────────
    if (m_trailRenderer) {
        const glm::mat4& view = m_isoCam.getViewMatrix();
        glm::vec3 camRight{view[0][0], view[1][0], view[2][0]};
        glm::vec3 camUp   {view[0][1], view[1][1], view[2][1]};
        m_trailRenderer->render(cmd, viewProj, camRight, camUp);
    }

    // ── Glass shield bubble ─────────────────────────────────────────────────
    if (m_shieldBubble) {
        auto& reg = m_scene.getRegistry();
        if (reg.all_of<CombatComponent, TransformComponent>(m_playerEntity)) {
            const auto& combat = reg.get<CombatComponent>(m_playerEntity);
            if (combat.state == CombatState::SHIELDING) {
                const auto& t = reg.get<TransformComponent>(m_playerEntity);
                float elapsed  = combat.shieldDuration - combat.stateTimer;
                float fadeIn   = std::min(elapsed / 0.25f, 1.0f);
                float fadeOut  = std::min(combat.stateTimer / 0.25f, 1.0f);
                float alpha    = fadeIn * fadeOut;
                glm::vec3 center    = t.position + glm::vec3(0.0f, 2.5f, 0.0f);
                glm::vec3 cameraPos = m_isoCam.getPosition();
                float appTime = static_cast<float>(glfwGetTime());
                m_shieldBubble->render(cmd, viewProj, center, cameraPos,
                                       3.2f, appTime, alpha);
            }
        }
    }

    // ── E-ability cone effect (Molten Shield) ───────────────────────────────
    if (m_coneEffectTimer > 0.0f && m_coneEffect) {
        float elapsed    = CONE_DURATION - m_coneEffectTimer;
        glm::vec3 camPos = m_isoCam.getPosition();
        float     t      = static_cast<float>(glfwGetTime());
        m_coneEffect->render(cmd, viewProj, m_coneApex, m_coneDirection,
                             CONE_HALF_ANGLE, CONE_RANGE,
                             camPos, t, elapsed, 1.0f);
    }

    // ── R-ability explosion effects (Incendiary Bomb) ───────────────────────
    if (m_explosionRenderer) {
        glm::vec3 camPos = m_isoCam.getPosition();
        float appTime    = static_cast<float>(glfwGetTime());
        m_explosionRenderer->render(cmd, viewProj, camPos, appTime);
    }

    if (m_meshEffectRenderer) {
        float appTime = static_cast<float>(glfwGetTime());
        m_meshEffectRenderer->render(cmd, viewProj, appTime);
    }

    // ── Sprite-atlas VFX effects (W cone, E explosion) ──────────────────────
    if (m_spriteEffectRenderer) {
        glm::vec3 camPos = m_isoCam.getPosition();
        m_spriteEffectRenderer->render(cmd, viewProj, camPos);
    }

    // ── Distortion pass (rendered on top of transparents) ───────────────────
    if (m_distortionRenderer) {
        float appTime = static_cast<float>(glfwGetTime());
        m_distortionRenderer->render(cmd, viewProj, appTime, ext.width, ext.height);
    }

    if (m_gpuTimer) m_gpuTimer->endZone(cmd, m_currentFrame, "VFX/Trans");
    vkCmdEndRendering(cmd);

    // Transition HDR color to SHADER_READ_ONLY for bloom/tonemap sampling
    {
        VkImageMemoryBarrier2 colorBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        colorBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        colorBarrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        colorBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        colorBarrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        colorBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        colorBarrier.image = m_hdrFB->colorImage();
        colorBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        depInfo.imageMemoryBarrierCount = 1;
        depInfo.pImageMemoryBarriers    = &colorBarrier;
        vkCmdPipelineBarrier2(cmd, &depInfo);
    }
}

// ── recordTonemapPass ────────────────────────────────────────────────────────
void Renderer::recordTonemapPass(VkCommandBuffer cmd, const FrameContext& ctx) {
    auto ext = ctx.extent;

    // ── Transition swapchain image to COLOR_ATTACHMENT_OPTIMAL ───────────────
    {
        VkImageMemoryBarrier2 swapBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        swapBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
        swapBarrier.srcAccessMask = VK_ACCESS_2_NONE;
        swapBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        swapBarrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        swapBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        swapBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        swapBarrier.image = m_swapchain->getImages()[ctx.imageIndex];
        swapBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        depInfo.imageMemoryBarrierCount = 1;
        depInfo.pImageMemoryBarriers    = &swapBarrier;
        vkCmdPipelineBarrier2(cmd, &depInfo);
    }

    VkRenderingAttachmentInfo swapColorAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    swapColorAttach.imageView   = m_swapchain->getImageViews()[ctx.imageIndex];
    swapColorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    swapColorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    swapColorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    swapColorAttach.clearValue.color = {{ 0.0f, 0.0f, 0.0f, 1.0f }};

    VkRenderingInfo swapRenderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    swapRenderInfo.renderArea          = { {0, 0}, ext };
    swapRenderInfo.layerCount          = 1;
    swapRenderInfo.colorAttachmentCount = 1;
    swapRenderInfo.pColorAttachments   = &swapColorAttach;

    vkCmdBeginRendering(cmd, &swapRenderInfo);
    if (m_gpuTimer) m_gpuTimer->beginZone(cmd, m_currentFrame, "Tonemap");

    {
        VkViewport vp{0, 0, static_cast<float>(ext.width), static_cast<float>(ext.height), 0, 1};
        VkRect2D   sc{{0, 0}, ext};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
    }
    float deathDesat = 0.0f;
    if (RespawnSystem::isDead(m_scene.getRegistry(), m_playerEntity))
        deathDesat = 0.7f; // heavy desaturation for death screen
    m_toneMap->render(cmd, /*exposure=*/1.0f, /*bloomStrength=*/0.3f,
                      /*vignette=*/1, /*colorGrade=*/1, /*chromatic=*/0.003f,
                      deathDesat);

    if (m_gpuTimer) m_gpuTimer->endZone(cmd, m_currentFrame, "Tonemap");

    // ── ImGui render draw data (last thing inside render pass) ──────────────
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

    vkCmdEndRendering(cmd);

    // Transition swapchain image to PRESENT_SRC_KHR for presentation
    {
        VkImageMemoryBarrier2 presentBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        presentBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        presentBarrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        presentBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_NONE;
        presentBarrier.dstAccessMask = VK_ACCESS_2_NONE;
        presentBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        presentBarrier.image = m_swapchain->getImages()[ctx.imageIndex];
        presentBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        depInfo.imageMemoryBarrierCount = 1;
        depInfo.pImageMemoryBarriers    = &presentBarrier;
        vkCmdPipelineBarrier2(cmd, &depInfo);
    }
}


// ── Async scene loading helpers ───────────────────────────────────────────

void Renderer::createInstanceBuffers() {
    m_instanceBuffers.clear();
    m_instanceMapped.clear();
    m_instanceBuffers.reserve(Sync::MAX_FRAMES_IN_FLIGHT);
    m_instanceMapped.reserve(Sync::MAX_FRAMES_IN_FLIGHT);

    VkDeviceSize size = MAX_INSTANCES * sizeof(InstanceData);
    for (uint32_t i = 0; i < Sync::MAX_FRAMES_IN_FLIGHT; ++i) {
        m_instanceBuffers.emplace_back(
            m_device->getAllocator(), size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        m_instanceMapped.push_back(m_instanceBuffers.back().map());
    }
}

void Renderer::destroyInstanceBuffers() {
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_instanceBuffers.size()); ++i) {
        if (m_instanceMapped[i]) {
            m_instanceBuffers[i].unmap();
            m_instanceMapped[i] = nullptr;
        }
    }
    m_instanceBuffers.clear();
    m_instanceMapped.clear();
}

// ── Grid pipeline ─────────────────────────────────────────────────────────────
void Renderer::createGridPipeline() {
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
        VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m), "shader module");
        return m;
    };

    VkShaderModule vert = makeModule(readFile(std::string(SHADER_DIR) + "grid.vert.spv"));
    VkShaderModule frag = makeModule(readFile(std::string(SHADER_DIR) + "grid.frag.spv"));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,   vert, "main" };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main" };

    VkPipelineVertexInputStateCreateInfo   vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

    VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rast{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rast.polygonMode = VK_POLYGON_MODE_FILL; rast.lineWidth = 1.0f; rast.cullMode = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_FALSE; ds.depthCompareOp = VK_COMPARE_OP_GREATER;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable         = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp        = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp        = VK_BLEND_OP_ADD;
    blend.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState gridBlends[2] = {blend, {}};
    cb.attachmentCount = 2; cb.pAttachments = gridBlends;

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.size       = sizeof(glm::mat4) + sizeof(float);
    VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.pushConstantRangeCount = 1; lci.pPushConstantRanges = &pc;
    VK_CHECK(vkCreatePipelineLayout(dev, &lci, nullptr, &m_gridPipelineLayout), "grid layout");

    VkPipelineRenderingCreateInfo gridDynCI = m_pipeline->getRenderFormats().pipelineRenderingCI();

    VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pci.pNext               = &gridDynCI;
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
    pci.layout              = m_gridPipelineLayout;
    pci.renderPass          = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &m_gridPipeline), "grid pipeline");

    vkDestroyShaderModule(dev, frag, nullptr);
    vkDestroyShaderModule(dev, vert, nullptr);
    spdlog::info("Grid pipeline created");
}

void Renderer::destroyGridPipeline() {
    VkDevice dev = m_device->getDevice();
    if (m_gridPipeline)       { vkDestroyPipeline(dev, m_gridPipeline, nullptr);       m_gridPipeline       = VK_NULL_HANDLE; }
    if (m_gridPipelineLayout) { vkDestroyPipelineLayout(dev, m_gridPipelineLayout, nullptr); m_gridPipelineLayout = VK_NULL_HANDLE; }
}

// ── GPU-skinned pipeline ──────────────────────────────────────────────────────
void Renderer::createSkinnedPipeline() {
    VkDevice dev = m_device->getDevice();

    auto readFile = [](const std::string& path) -> std::vector<char> {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) throw std::runtime_error("Shader not found: " + path);
        size_t sz = static_cast<size_t>(file.tellg());
        std::vector<char> buf(sz);
        file.seekg(0); file.read(buf.data(), static_cast<std::streamsize>(sz));
        return buf;
    };
    auto makeModule = [&](const std::vector<char>& code) {
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = code.size();
        ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule m;
        VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m), "shader module");
        return m;
    };

    VkShaderModule vert = makeModule(readFile(std::string(SHADER_DIR) + "skinned.vert.spv"));
    VkShaderModule frag = makeModule(readFile(std::string(SHADER_DIR) + "triangle.frag.spv"));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,   vert, "main" };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main" };

    // SkinnedVertex (binding 0) + InstanceData (binding 1, locations shifted +2)
    auto skinnedBind  = SkinnedVertex::getBindingDescription();
    auto skinnedAttrs = SkinnedVertex::getAttributeDescriptions();
    auto instBind     = InstanceData::getBindingDescription();
    auto instAttrs    = InstanceData::getAttributeDescriptions();
    instBind.binding = 1;
    for (auto& a : instAttrs) { a.binding = 1; a.location += 2; } // 4→6, …, 14→16

    std::array<VkVertexInputBindingDescription, 2> bindings{ skinnedBind, instBind };
    std::vector<VkVertexInputAttributeDescription> allAttrs(skinnedAttrs.begin(), skinnedAttrs.end());
    allAttrs.insert(allAttrs.end(), instAttrs.begin(), instAttrs.end());

    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = 2;
    vi.pVertexBindingDescriptions      = bindings.data();
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(allAttrs.size());
    vi.pVertexAttributeDescriptions    = allAttrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

    VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rast{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rast.polygonMode = VK_POLYGON_MODE_FILL; rast.lineWidth = 1.0f;
    rast.cullMode = VK_CULL_MODE_NONE; rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; // doubleSided:true

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_GREATER;

    VkPipelineColorBlendAttachmentState blendAttach{};
    blendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendAttachmentState charDepthBlend{};
    charDepthBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendAttachmentState skinnedBlends[2] = {blendAttach, charDepthBlend};
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 2; cb.pAttachments = skinnedBlends;

    VkDescriptorSetLayout setLayouts[2] = { m_descriptors->getLayout(), m_bindless->getLayout() };
    VkPushConstantRange pc{};
    // Include FRAGMENT_BIT so this range is compatible with the water renderer's
    // VERTEX|FRAGMENT push constant range that overlaps at bytes [0..4].
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.size       = sizeof(uint32_t); // boneBaseIndex
    VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.setLayoutCount         = 2;
    lci.pSetLayouts            = setLayouts;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges    = &pc;
    VK_CHECK(vkCreatePipelineLayout(dev, &lci, nullptr, &m_skinnedPipelineLayout), "skinned layout");

    VkPipelineRenderingCreateInfo skinnedDynCI = m_pipeline->getRenderFormats().pipelineRenderingCI();

    VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pci.pNext               = &skinnedDynCI;
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
    pci.layout              = m_skinnedPipelineLayout;
    pci.renderPass          = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &m_skinnedPipeline), "skinned pipeline");

    vkDestroyShaderModule(dev, frag, nullptr);
    vkDestroyShaderModule(dev, vert, nullptr);
    spdlog::info("GPU-skinned pipeline created");
}

void Renderer::destroySkinnedPipeline() {
    VkDevice dev = m_device->getDevice();
    if (m_skinnedPipeline)       { vkDestroyPipeline(dev, m_skinnedPipeline, nullptr);            m_skinnedPipeline       = VK_NULL_HANDLE; }
    if (m_skinnedPipelineLayout) { vkDestroyPipelineLayout(dev, m_skinnedPipelineLayout, nullptr); m_skinnedPipelineLayout = VK_NULL_HANDLE; }
}

} // namespace glory
