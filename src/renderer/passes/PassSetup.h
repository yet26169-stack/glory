#pragma once

// Pass setup functions for the modular render graph.
// Each function creates a RenderPassNode that wraps an existing pass class,
// declaring its resource reads/writes for automatic barrier deduction.

#include "renderer/RenderGraph.h"

namespace glory {

// Individual pass setup declarations
RenderPassNode createFogOfWarPass();
RenderPassNode createShadowRenderPass();
RenderPassNode createGBufferRenderPass();
RenderPassNode createLightingRenderPass();   // placeholder — currently merged into GBuffer scope
RenderPassNode createHiZRenderPass();        // placeholder — HiZ generation after geometry
RenderPassNode createOcclusionCullPass();    // placeholder — GPU occlusion cull
RenderPassNode createVFXAcquirePass();
RenderPassNode createTransparentVFXPass();
RenderPassNode createBloomRenderPass();
RenderPassNode createTonemapRenderPass();

// Utility: create the full default pipeline of passes and add them to a graph
void buildDefaultRenderGraph(RenderGraph& graph);

} // namespace glory
