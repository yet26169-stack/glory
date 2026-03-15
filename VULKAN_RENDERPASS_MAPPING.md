# Glory Engine: VkRenderPass & VkFramebuffer Complete Mapping

## Executive Summary

The Glory engine uses a multi-pass deferred rendering architecture with the following key render passes:

1. **HDR Framebuffer** - Main scene rendering with 2 color attachments + depth
2. **Swapchain Render Pass** - Final tone mapping to swapchain (UI pass)
3. **Shadow Pass** - Depth-only cascaded shadow map atlas
4. **Bloom Pass** - Extract + blur bright areas
5. **Pipeline** - Base graphics pipeline (legacy, usually overridden by HDR)

All pipelines and passes reference one of these render passes for graphics pipeline creation.

---

## 1. RENDER PASS CREATION & DESTRUCTION

### 1.1 HDRFramebuffer::createRenderPass() - LINE 188-270
**File**: `/Users/donkey/Development/1/Glory/src/renderer/HDRFramebuffer.cpp`

**Attachments (3 total)**:
- **Attachment 0**: Color (R16G16B16A16_SFLOAT)
  - Load Op: CLEAR
  - Store Op: STORE
  - Initial Layout: UNDEFINED
  - Final Layout: SHADER_READ_ONLY_OPTIMAL
  
- **Attachment 1**: Depth (D32_SFLOAT or D32_SFLOAT_S8_UINT)
  - Load Op: CLEAR
  - Store Op: STORE
  - Stencil Load: CLEAR (if has stencil) / DONT_CARE
  - Stencil Store: DONT_CARE
  - Initial Layout: UNDEFINED
  - Final Layout: DEPTH_STENCIL_READ_ONLY_OPTIMAL
  
- **Attachment 2**: Character Depth (R32_SFLOAT)
  - Load Op: CLEAR
  - Store Op: STORE
  - Initial Layout: UNDEFINED
  - Final Layout: SHADER_READ_ONLY_OPTIMAL

**Subpass**:
- Bind Point: GRAPHICS
- Color Attachments: 2 (main color + character depth)
- Depth-Stencil: attachment 1

**Dependencies (2 total)**:
```
Dep 0: VK_SUBPASS_EXTERNAL → subpass 0
  Src Stage: BOTTOM_OF_PIPE
  Dst Stage: COLOR_ATTACHMENT_OUTPUT | EARLY_FRAGMENT_TESTS
  Src Access: MEMORY_READ
  Dst Access: COLOR_ATTACHMENT_READ | COLOR_ATTACHMENT_WRITE | DEPTH_STENCIL_ATTACHMENT_WRITE
  Flags: BY_REGION

Dep 1: subpass 0 → VK_SUBPASS_EXTERNAL
  Src Stage: COLOR_ATTACHMENT_OUTPUT | LATE_FRAGMENT_TESTS
  Dst Stage: FRAGMENT_SHADER
  Src Access: COLOR_ATTACHMENT_WRITE | DEPTH_STENCIL_ATTACHMENT_WRITE
  Dst Access: SHADER_READ
  Flags: BY_REGION
```

**Creation Call**: Line 268
```cpp
VK_CHECK(vkCreateRenderPass(m_device->getDevice(), &renderPassInfo, nullptr, &m_renderPass),
         "Create HDR render pass");
```

---

### 1.2 HDRFramebuffer::createLoadRenderPass() - LINE 272-356
**File**: `/Users/donkey/Development/1/Glory/src/renderer/HDRFramebuffer.cpp`

**Purpose**: Load existing framebuffer contents for transparent/VFX pass (LOAD_OP_LOAD)

**Attachments (3 total)**:
- **Attachment 0**: Color (R16G16B16A16_SFLOAT)
  - Load Op: **LOAD** (preserve existing)
  - Store Op: STORE
  - Initial Layout: COLOR_ATTACHMENT_OPTIMAL
  - Final Layout: SHADER_READ_ONLY_OPTIMAL
  
- **Attachment 1**: Depth (matches main depth format)
  - Load Op: **LOAD**
  - Store Op: STORE
  - Initial Layout: DEPTH_STENCIL_READ_ONLY_OPTIMAL
  - Final Layout: DEPTH_STENCIL_READ_ONLY_OPTIMAL
  
- **Attachment 2**: Character Depth (R32_SFLOAT)
  - Load Op: **LOAD**
  - Store Op: STORE
  - Layout: GENERAL (allows sampling + attachment simultaneously)
  - Initial Layout: SHADER_READ_ONLY_OPTIMAL
  - Final Layout: SHADER_READ_ONLY_OPTIMAL

**Subpass**: Same structure as main render pass

**Dependencies**: Same 2 dependencies as main pass

**Creation Call**: Line 354
```cpp
VK_CHECK(vkCreateRenderPass(m_device->getDevice(), &renderPassInfo, nullptr, &m_loadRenderPass),
         "Create HDR load render pass");
```

**Shared Framebuffer**: Both render passes use the SAME framebuffer (created with reference to m_renderPass)
- Line 369: `framebufferInfo.renderPass = m_renderPass;`

---

### 1.3 HDRFramebuffer::createFramebuffer() - LINE 358-378
**File**: `/Users/donkey/Development/1/Glory/src/renderer/HDRFramebuffer.cpp`

**Attachments (3 ImageViews)**:
- `m_colorImage.getImageView()` - Main HDR color (R16G16B16A16_SFLOAT)
- `m_depthAttachmentView` - Depth with both DEPTH+STENCIL aspects if needed
- `m_characterDepthImage.getImageView()` - Character depth (R32_SFLOAT)

**Dimensions**: User-specified (typically window resolution)

**Creation Call**: Line 376-377
```cpp
VK_CHECK(vkCreateFramebuffer(m_device->getDevice(), &framebufferInfo, nullptr, &m_framebuffer),
         "Create HDR framebuffer");
```

**Destruction Calls**:
- Line 28, 118, 376: `vkDestroyFramebuffer(m_device->getDevice(), m_framebuffer, nullptr);`

---

### 1.4 ShadowPass::createRenderPass() - LINE 267-315
**File**: `/Users/donkey/Development/1/Glory/src/renderer/ShadowPass.cpp`

**Purpose**: Depth-only render pass for cascaded shadow mapping

**Attachments (1 total)**:
- **Attachment 0**: Depth (D32_SFLOAT)
  - Load Op: CLEAR
  - Store Op: STORE
  - Stencil Load/Store: DONT_CARE
  - Initial Layout: UNDEFINED
  - Final Layout: SHADER_READ_ONLY_OPTIMAL

**Subpass**:
- Bind Point: GRAPHICS
- Color Attachments: 0 (depth-only)
- Depth-Stencil: attachment 0

**Dependencies (2 total)**:
```
Dep 0: VK_SUBPASS_EXTERNAL → subpass 0
  Src Stage: FRAGMENT_SHADER
  Dst Stage: EARLY_FRAGMENT_TESTS
  Src Access: SHADER_READ
  Dst Access: DEPTH_STENCIL_ATTACHMENT_WRITE
  Flags: BY_REGION

Dep 1: subpass 0 → VK_SUBPASS_EXTERNAL
  Src Stage: LATE_FRAGMENT_TESTS
  Dst Stage: FRAGMENT_SHADER
  Src Access: DEPTH_STENCIL_ATTACHMENT_WRITE
  Dst Access: SHADER_READ
  Flags: BY_REGION
```

**Creation Call**: Line 313-314
```cpp
VK_CHECK(vkCreateRenderPass(m_device->getDevice(), &rpCI, nullptr, &m_renderPass),
         "Failed to create shadow render pass");
```

**Destruction Call**: Line 59
```cpp
if (m_renderPass) vkDestroyRenderPass(dev, m_renderPass, nullptr);
```

---

### 1.5 ShadowPass::createFramebuffer() - LINE 317-328
**File**: `/Users/donkey/Development/1/Glory/src/renderer/ShadowPass.cpp`

**Purpose**: Depth atlas for 3 cascaded shadow maps

**Attachments (1 ImageView)**:
- `m_atlasView` - Depth atlas image (CASCADE_COUNT=3 × SHADOW_MAP_SIZE=2048 wide, 2048 tall)
  - Format: D32_SFLOAT
  - Usage: DEPTH_STENCIL_ATTACHMENT_BIT | SAMPLED_BIT

**Dimensions**: 6144 × 2048 (3 cascades × 2048 tiles)

**Creation Call**: Line 326-327
```cpp
VK_CHECK(vkCreateFramebuffer(m_device->getDevice(), &fbCI, nullptr, &m_framebuffer),
         "Failed to create shadow framebuffer");
```

**Destruction Call**: Line 58
```cpp
if (m_framebuffer) vkDestroyFramebuffer(dev, m_framebuffer, nullptr);
```

---

### 1.6 BloomPass::createRenderPass() - LINE 170-209
**File**: `/Users/donkey/Development/1/Glory/src/renderer/BloomPass.cpp`

**Purpose**: Render pass for bloom extraction and blur passes

**Attachments (1 total)**:
- **Attachment 0**: Color (R16G16B16A16_SFLOAT)
  - Load Op: CLEAR
  - Store Op: STORE
  - Stencil: DONT_CARE
  - Initial Layout: UNDEFINED
  - Final Layout: SHADER_READ_ONLY_OPTIMAL

**Subpass**:
- Bind Point: GRAPHICS
- Color Attachments: 1
- Depth-Stencil: none

**Dependencies (1 total)**:
```
Dep 0: VK_SUBPASS_EXTERNAL → subpass 0
  Src Stage: COLOR_ATTACHMENT_OUTPUT
  Dst Stage: COLOR_ATTACHMENT_OUTPUT
  Src Access: 0
  Dst Access: COLOR_ATTACHMENT_WRITE
```

**Creation Call**: Line 207-208
```cpp
VK_CHECK(vkCreateRenderPass(m_device->getDevice(), &renderPassInfo, nullptr, &m_renderPass),
         "Create Bloom render pass");
```

**Destruction Call**: Line 80
```cpp
if (m_renderPass != VK_NULL_HANDLE) {
    vkDestroyRenderPass(m_device->getDevice(), m_renderPass, nullptr);
    m_renderPass = VK_NULL_HANDLE;
}
```

---

### 1.7 BloomPass::createFramebuffers() - LINE 406-422
**File**: `/Users/donkey/Development/1/Glory/src/renderer/BloomPass.cpp`

**Purpose**: 2 ping-pong framebuffers for blur passes

**Framebuffers (2 total)**:
- `m_framebuffers[0]` - Blur0 (R16G16B16A16_SFLOAT at 1/2 resolution)
- `m_framebuffers[1]` - Blur1 (R16G16B16A16_SFLOAT at 1/2 resolution)

**Creation Calls**: Lines 419-420
```cpp
VK_CHECK(vkCreateFramebuffer(m_device->getDevice(), &framebufferInfo, nullptr, &m_framebuffers[i]),
         "Create Bloom framebuffer");
```

**Destruction Calls**:
- Line 34-35 (recreate): `vkDestroyFramebuffer(dev, fb, nullptr);`
- Line 55: `vkDestroyFramebuffer(m_device->getDevice(), fb, nullptr);`

---

### 1.8 Renderer::createSwapchainRenderPass() - LINE 2349-2388
**File**: `/Users/donkey/Development/1/Glory/src/renderer/Renderer.cpp`

**Purpose**: Final render pass for ImGui UI + tone mapping to swapchain

**Attachments (1 total)**:
- **Attachment 0**: Color (swapchain format, typically VK_FORMAT_B8G8R8A8_SRGB)
  - Load Op: CLEAR
  - Store Op: STORE
  - Stencil: DONT_CARE
  - Initial Layout: UNDEFINED
  - Final Layout: PRESENT_SRC_KHR

**Subpass**:
- Bind Point: GRAPHICS
- Color Attachments: 1
- Depth-Stencil: none

**Dependencies (1 total)**:
```
Dep 0: VK_SUBPASS_EXTERNAL → subpass 0
  Src Stage: COLOR_ATTACHMENT_OUTPUT
  Dst Stage: COLOR_ATTACHMENT_OUTPUT
  Src Access: 0
  Dst Access: COLOR_ATTACHMENT_WRITE
```

**Creation Call**: Line 2386-2387
```cpp
VK_CHECK(vkCreateRenderPass(m_device->getDevice(), &ci, nullptr, &m_swapchainRenderPass),
         "Failed to create swapchain render pass");
```

**Destruction Call**: Line 2392
```cpp
vkDestroyRenderPass(m_device->getDevice(), m_swapchainRenderPass, nullptr);
```

---

### 1.9 Renderer::createSwapchainFramebuffers() - LINE 2397-2414
**File**: `/Users/donkey/Development/1/Glory/src/renderer/Renderer.cpp`

**Purpose**: One framebuffer per swapchain image for present

**Framebuffers (N total, one per swapchain image)**:
- `m_swapchainFramebuffers[i]` - One per swapchain image view

**Creation Calls**: Line 2411-2412
```cpp
VK_CHECK(vkCreateFramebuffer(m_device->getDevice(), &ci, nullptr, &m_swapchainFramebuffers[i]),
         "Failed to create swapchain framebuffer");
```

**Destruction Call**: Line 2418
```cpp
vkDestroyFramebuffer(m_device->getDevice(), fb, nullptr);
```

---

### 1.10 Pipeline::createRenderPass() - LINE 64-124
**File**: `/Users/donkey/Development/1/Glory/src/renderer/Pipeline.cpp`

**Purpose**: Main graphics pipeline render pass (used when no external render pass provided)

**Attachments (2 total)**:
- **Attachment 0**: Color (swapchain format)
  - Load Op: CLEAR
  - Store Op: STORE
  - Final Layout: PRESENT_SRC_KHR
  
- **Attachment 1**: Depth (device-specific format)
  - Load Op: CLEAR
  - Store Op: DONT_CARE
  - Final Layout: DEPTH_STENCIL_ATTACHMENT_OPTIMAL

**Subpass**: Includes both color and depth

**Creation Call**: Line 121-122
```cpp
VK_CHECK(vkCreateRenderPass(m_device.getDevice(), &ci, nullptr, &m_renderPass),
         "Failed to create render pass");
```

**Destruction Call**: Line 50
```cpp
if (m_ownsRenderPass && m_renderPass != VK_NULL_HANDLE)
    vkDestroyRenderPass(m_device.getDevice(), m_renderPass, nullptr);
```

---

### 1.11 Pipeline::createFramebuffers() - LINE 282-304
**File**: `/Users/donkey/Development/1/Glory/src/renderer/Pipeline.cpp`

**Purpose**: Framebuffers for the Pipeline's render pass (typically not used since HDR framebuffer takes precedence)

**Framebuffers (N total)**:
- One per swapchain image

**Creation Call**: Line 300-301
```cpp
VK_CHECK(vkCreateFramebuffer(m_device.getDevice(), &ci, nullptr, &m_framebuffers[i]),
         "Failed to create framebuffer");
```

**Destruction Call**: Line 309
```cpp
vkDestroyFramebuffer(m_device.getDevice(), fb, nullptr);
```

---

## 2. RENDER PASS USAGE (vkCmdBeginRenderPass / vkCmdEndRenderPass)

### 2.1 HDR Main Geometry Pass
**File**: `/Users/donkey/Development/1/Glory/src/renderer/Renderer.cpp`
**Lines**: 1141-1147, 1343

```cpp
VkRenderPassBeginInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
rp.renderPass       = m_hdrFB->renderPass();       // m_renderPass
rp.framebuffer      = m_hdrFB->framebuffer();      // m_framebuffer
rp.renderArea       = { {0, 0}, ext };
rp.clearValueCount  = 3;
rp.pClearValues     = clears.data();  // Color (0.08, 0.10, 0.14), Depth (0.0), CharDepth (0, 0, 0, 0)

vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);  // Line 1147
  // Geometry rendering: static meshes, skinned meshes, water
vkCmdEndRenderPass(cmd);  // Line 1343
```

**Clear Values**:
- Clear[0].color = {0.08f, 0.10f, 0.14f, 1.0f} (dark blue background)
- Clear[1].depthStencil = {0.0f, 0} (reversed-Z: 0 = far, 1 = near)
- Clear[2].color = {0.0f, 0.0f, 0.0f, 0.0f} (character depth = 0.0)

**Pipelines Bound Inside**:
- m_pipeline->getGraphicsPipeline() or getWireframePipeline() (static mesh pass)
- m_skinnedPipeline (skinned mesh pass)
- m_waterRenderer pipeline (water pass)

---

### 2.2 HDR Transparent / VFX Pass (LOAD_OP_LOAD)
**File**: `/Users/donkey/Development/1/Glory/src/renderer/Renderer.cpp`
**Lines**: 1352-1356, 1442

```cpp
VkRenderPassBeginInfo loadRP{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
loadRP.renderPass  = m_hdrFB->loadRenderPass();    // m_loadRenderPass
loadRP.framebuffer = m_hdrFB->framebuffer();       // SAME framebuffer
loadRP.renderArea  = { {0, 0}, ext };
// No clear values needed (LOAD_OP_LOAD preserves contents)

vkCmdBeginRenderPass(cmd, &loadRP, VK_SUBPASS_CONTENTS_INLINE);  // Line 1356
  // Inking pass (character outline)
  // VFX particle rendering
  // Trail rendering
  // Distortion rendering
vkCmdEndRenderPass(cmd);  // Line 1442
```

**Pipelines Bound Inside**:
- m_inkingPass pipeline
- m_vfxRenderer pipeline
- m_trailRenderer pipeline
- m_distortionRenderer pipeline
- m_clickIndicatorRenderer pipeline
- Various ability renderers

---

### 2.3 Bloom Dispatch Passes
**File**: `/Users/donkey/Development/1/Glory/src/renderer/BloomPass.cpp`
**Lines**: 87-159

**Pass 1: Extract Bright Areas**
```cpp
VkRenderPassBeginInfo rpBegin{};
rpBegin.renderPass  = m_renderPass;
rpBegin.framebuffer = m_framebuffers[0];  // Blur0

vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);  // Line 101
  vkCmdBindPipeline(cmd, ..., m_extractPipeline);
  // Sample HDR color, threshold & output to Blur0
vkCmdEndRenderPass(cmd);  // Line 109
```

**Pass 2: Horizontal Blur (Blur0 → Blur1)**
```cpp
rpBegin.framebuffer = m_framebuffers[1];  // Blur1
vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);  // Line 126
  vkCmdBindPipeline(cmd, ..., m_blurPipeline);
  vkCmdPushConstants(..., {horizontal=1, ...});
vkCmdEndRenderPass(cmd);  // Line 134
```

**Pass 3: Vertical Blur (Blur1 → Blur0)**
```cpp
rpBegin.framebuffer = m_framebuffers[0];  // Blur0
vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);  // Line 148
  vkCmdBindPipeline(cmd, ..., m_blurPipeline);
  vkCmdPushConstants(..., {horizontal=0, ...});
vkCmdEndRenderPass(cmd);  // Line 156
```

**Loop**: Repeats for multiple blur passes (typically 5)

---

### 2.4 Shadow Pass Recording
**File**: `/Users/donkey/Development/1/Glory/src/renderer/ShadowPass.cpp`
**Lines**: 157-206

```cpp
VkClearValue clear{};
clear.depthStencil = {1.0f, 0};  // Reversed-Z: 1 = far

VkRenderPassBeginInfo rpBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
rpBegin.renderPass  = m_renderPass;
rpBegin.framebuffer = m_framebuffer;
rpBegin.renderArea  = {{0, 0}, {SHADOW_MAP_SIZE * CASCADE_COUNT, SHADOW_MAP_SIZE}};
rpBegin.clearValueCount = 1;
rpBegin.pClearValues = &clear;

vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);  // Line 170

for (uint32_t c = 0; c < CASCADE_COUNT; ++c) {
    // Set viewport/scissor to cascade tile
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    
    // Static mesh shadow pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_staticPipeline);
    vkCmdPushConstants(cmd, ..., sizeof(glm::mat4), &m_cascades[c].lightViewProj);
    staticDrawFn(cmd, c);
    
    // Skinned mesh shadow pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedPipeline);
    vkCmdPushConstants(cmd, ..., sizeof(glm::mat4), &m_cascades[c].lightViewProj);
    skinnedDrawFn(cmd, c);
}

vkCmdEndRenderPass(cmd);  // Line 205
```

---

### 2.5 Swapchain Tonemap Pass
**File**: `/Users/donkey/Development/1/Glory/src/renderer/Renderer.cpp`
**Lines**: 1451-1470

```cpp
VkRenderPassBeginInfo swapRP{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
swapRP.renderPass  = m_swapchainRenderPass;
swapRP.framebuffer = m_swapchainFramebuffers[imageIndex];
swapRP.renderArea  = { {0, 0}, ext };
std::array<VkClearValue, 1> swapClears{};
swapClears[0].color = {{ 0.0f, 0.0f, 0.0f, 1.0f }};
swapRP.clearValueCount = 1;
swapRP.pClearValues = swapClears.data();

vkCmdBeginRenderPass(cmd, &swapRP, VK_SUBPASS_CONTENTS_INLINE);  // Line 1460
  m_toneMap->render(cmd, /*exposure=*/1.0f, /*bloomStrength=*/0.3f);
  ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);  // ImGui UI
vkCmdEndRenderPass(cmd);  // Line 1470
```

**Pipelines**:
- ToneMapPass: Samples HDR + Bloom, outputs to swapchain
- ImGui: Renders UI on top

---

## 3. PIPELINE REFERENCES

### Graphics Pipeline Creation

All graphics pipelines are created with a specific render pass:

| Component | Render Pass | File | Line |
|-----------|-------------|------|------|
| Main Graphics Pipeline | m_renderPass (or external) | Pipeline.cpp | 249 |
| Wireframe Pipeline | m_renderPass | Pipeline.cpp | 256 |
| Skinned Pipeline | (HDR) | Renderer.cpp | ~2200 |
| Shadow Static Pipeline | m_renderPass | ShadowPass.cpp | 430 |
| Shadow Skinned Pipeline | m_renderPass | ShadowPass.cpp | 481 |
| Bloom Extract Pipeline | m_renderPass | BloomPass.cpp | 333 |
| Bloom Blur Pipeline | m_renderPass | BloomPass.cpp | 337 |
| ToneMap Pipeline | m_swapchainRenderPass | ToneMapPass.cpp | 232 |
| Inking Pipeline | HDR renderPass | InkingPass.cpp | ~TBD |
| VFX Renderer Pipeline | HDR renderPass | VFXRenderer.cpp | ~TBD |
| Trail Renderer Pipeline | HDR renderPass | TrailRenderer.cpp | ~TBD |

---

## 4. RENDER PASS ATTACHMENT REFERENCE

### HDR Render Pass Attachments
```
Layout Dependencies:
  External → Subpass 0 → External

Attachment 0 (Color):
  - Format: R16G16B16A16_SFLOAT
  - Transitions: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
  - Reference in subpass: colorRefs[0]

Attachment 1 (Depth):
  - Format: D32_SFLOAT (or with stencil)
  - Transitions: UNDEFINED → DEPTH_STENCIL_ATTACHMENT_OPTIMAL → DEPTH_STENCIL_READ_ONLY_OPTIMAL
  - Reference in subpass: depthAttachmentRef

Attachment 2 (Character Depth):
  - Format: R32_SFLOAT
  - Transitions: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
  - Reference in subpass: colorRefs[1]
```

### Shadow Render Pass Attachments
```
Attachment 0 (Depth):
  - Format: D32_SFLOAT
  - Transitions: UNDEFINED → DEPTH_STENCIL_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
  - Reference in subpass: depthRef
  - Sample binding: Descriptor binding 3 in main pass shaders
```

### Bloom Render Pass Attachments
```
Attachment 0 (Color):
  - Format: R16G16B16A16_SFLOAT
  - Transitions: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
  - Used for Extract and Blur ping-pong passes
```

---

## 5. SUBPASS STRUCTURES

### HDR Render Pass Subpass
```cpp
VkSubpassDescription subpass{};
subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
subpass.colorAttachmentCount    = 2;  // Color + CharDepth
subpass.pColorAttachments       = colorRefs;
subpass.pDepthStencilAttachment = &depthAttachmentRef;
```

### Shadow Render Pass Subpass
```cpp
VkSubpassDescription subpass{};
subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
subpass.colorAttachmentCount    = 0;  // Depth-only
subpass.pDepthStencilAttachment = &depthRef;
```

### Bloom & ToneMap Subpass
```cpp
VkSubpassDescription subpass{};
subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
subpass.colorAttachmentCount = 1;  // Single color
subpass.pColorAttachments    = &colorRef;
```

---

## 6. IMAGE LAYOUT TRANSITIONS

### HDR Color Image
```
Initial:     UNDEFINED
Attachment:  COLOR_ATTACHMENT_OPTIMAL (during render)
Final:       SHADER_READ_ONLY_OPTIMAL (for sampling in subsequent passes)
```

### HDR Depth Image
```
Initial:     UNDEFINED
Attachment:  DEPTH_STENCIL_ATTACHMENT_OPTIMAL (during render)
Final:       DEPTH_STENCIL_READ_ONLY_OPTIMAL (for sampling / next load pass)
```

### Shadow Depth Atlas
```
Initial:     UNDEFINED
Attachment:  DEPTH_STENCIL_ATTACHMENT_OPTIMAL (during shadow pass)
Final:       SHADER_READ_ONLY_OPTIMAL (for sampling in main pass)
ClearLayout: Maintains SHADER_READ_ONLY_OPTIMAL across frames via loadRenderPass
```

### Bloom Images
```
Blur0:
  Extract:   UNDEFINED → COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
  H-Blur:    SHADER_READ_ONLY_OPTIMAL (input) → UNDEFINED → COLOR_ATTACHMENT_OPTIMAL (output)
  V-Blur:    (output from H) → SHADER_READ_ONLY_OPTIMAL (for final blend)

Blur1:
  H-Blur:    UNDEFINED → COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
  V-Blur:    SHADER_READ_ONLY_OPTIMAL (input) → UNDEFINED → COLOR_ATTACHMENT_OPTIMAL (output)
```

---

## 7. SHADER COMPILATION (CMakeLists.txt)

**File**: `/Users/donkey/Development/1/Glory/CMakeLists.txt`
**Lines**: 100-119

```cmake
set(SHADER_SOURCES
    ${CMAKE_SOURCE_DIR}/shaders/triangle.vert
    ${CMAKE_SOURCE_DIR}/shaders/triangle.frag
    ${CMAKE_SOURCE_DIR}/shaders/tonemap.vert
    ${CMAKE_SOURCE_DIR}/shaders/tonemap.frag
    ${CMAKE_SOURCE_DIR}/shaders/bloom_extract.frag
    ${CMAKE_SOURCE_DIR}/shaders/bloom_blur.frag
    # ... more shaders ...
)

foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPV_OUTPUT ${CMAKE_BINARY_DIR}/shaders/${SHADER_NAME}.spv)
    add_custom_command(
        OUTPUT  ${SPV_OUTPUT}
        COMMAND ${GLSLC} ${SHADER} -o ${SPV_OUTPUT}    # Line 112
        DEPENDS ${SHADER}
        COMMENT "Compiling shader ${SHADER_NAME}"
    )
    list(APPEND SPV_SHADERS ${SPV_OUTPUT})
endforeach()

add_custom_target(Shaders ALL DEPENDS ${SPV_SHADERS})
```

**Shader Compilation Command**:
```bash
${GLSLC} <shader.glsl> -o <shader.glsl>.spv
```

Where `${GLSLC}` is typically `glslc` from the Khronos GLSL compiler.

---

## 8. FRAMEBUFFER LIFECYCLE

### HDR Framebuffer
```
Creation: HDRFramebuffer::init()
  → createImages()
  → createRenderPass()
  → createLoadRenderPass()
  → createFramebuffer()

Usage:
  Frame 0: vkCmdBeginRenderPass(renderPass, framebuffer) → Geometry
           vkCmdEndRenderPass()
           
           vkCmdBeginRenderPass(loadRenderPass, framebuffer) → VFX/Transparent
           vkCmdEndRenderPass()

Recreation: HDRFramebuffer::recreate()
  → vkDestroyFramebuffer()
  → createImages()  (depth & color recreated)
  → createFramebuffer()

Destruction: HDRFramebuffer::destroy()
  → vkDestroyFramebuffer()
  → vkDestroyRenderPass() (both renderPass & loadRenderPass)
  → Destroy images
```

### Shadow Framebuffer
```
Creation: ShadowPass::init()
  → createAtlasImage()
  → createSampler()
  → createRenderPass()
  → createFramebuffer()

Usage:
  Each frame: vkCmdBeginRenderPass(renderPass, framebuffer) → Shadow depth rendering
              vkCmdEndRenderPass()

Destruction: ShadowPass::destroy()
  → vkDestroyFramebuffer()
  → vkDestroyRenderPass()
```

### Swapchain Framebuffers
```
Creation: Renderer::createSwapchainFramebuffers()
  One per swapchain image

Usage:
  Frame N: vkCmdBeginRenderPass(..., swapchainFramebuffers[imageIndex])
           → ToneMap + ImGui rendering
           vkCmdEndRenderPass()

Recreation: Renderer::recreateSwapchain()
  → destroySwapchainFramebuffers()
  → destroySwapchainRenderPass()
  → createSwapchainRenderPass()
  → createSwapchainFramebuffers()
```

---

## 9. RENDER PASS COMPATIBILITY

Render passes must match for framebuffers:

- **HDR Framebuffer** compatible with:
  - `m_renderPass` (CLEAR loads)
  - `m_loadRenderPass` (LOAD preserves)
  - Same subpass structure, same attachment count & order

- **Bloom Framebuffers** compatible with:
  - `m_renderPass` only
  - Both Extract and Blur use same render pass

- **Shadow Framebuffer** compatible with:
  - `m_renderPass` only
  - Used across CASCADE_COUNT viewport/scissor regions

- **Swapchain Framebuffers** compatible with:
  - `m_swapchainRenderPass` only
  - One framebuffer per swapchain image view

---

## 10. COMPLETE FILE LISTING

### Files Using VkRenderPass & VkFramebuffer:

**Core Rendering**:
- `/Users/donkey/Development/1/Glory/src/renderer/Renderer.h` (declarations)
- `/Users/donkey/Development/1/Glory/src/renderer/Renderer.cpp` (HDR + swapchain)
- `/Users/donkey/Development/1/Glory/src/renderer/Pipeline.h` (declarations)
- `/Users/donkey/Development/1/Glory/src/renderer/Pipeline.cpp` (base pipeline)
- `/Users/donkey/Development/1/Glory/src/renderer/HDRFramebuffer.h` (declarations)
- `/Users/donkey/Development/1/Glory/src/renderer/HDRFramebuffer.cpp` (HDR setup)

**Pass Implementations**:
- `/Users/donkey/Development/1/Glory/src/renderer/ShadowPass.h`
- `/Users/donkey/Development/1/Glory/src/renderer/ShadowPass.cpp` (shadow atlas)
- `/Users/donkey/Development/1/Glory/src/renderer/BloomPass.h`
- `/Users/donkey/Development/1/Glory/src/renderer/BloomPass.cpp` (bloom blur)
- `/Users/donkey/Development/1/Glory/src/renderer/ToneMapPass.h`
- `/Users/donkey/Development/1/Glory/src/renderer/ToneMapPass.cpp` (tone mapping)

**Effect Renderers** (all pass VkRenderPass to ctor):
- `/Users/donkey/Development/1/Glory/src/renderer/InkingPass.cpp`
- `/Users/donkey/Development/1/Glory/src/renderer/DistortionRenderer.cpp`
- `/Users/donkey/Development/1/Glory/src/vfx/VFXRenderer.cpp`
- `/Users/donkey/Development/1/Glory/src/vfx/TrailRenderer.cpp`
- `/Users/donkey/Development/1/Glory/src/renderer/OutlineRenderer.cpp`
- `/Users/donkey/Development/1/Glory/src/renderer/SpriteEffectRenderer.cpp`
- `/Users/donkey/Development/1/Glory/src/renderer/ConeAbilityRenderer.cpp`
- `/Users/donkey/Development/1/Glory/src/renderer/ExplosionRenderer.cpp`
- `/Users/donkey/Development/1/Glory/src/renderer/ShieldBubbleRenderer.cpp`
- `/Users/donkey/Development/1/Glory/src/renderer/ClickIndicatorRenderer.cpp`
- `/Users/donkey/Development/1/Glory/src/renderer/GroundDecalRenderer.cpp`
- `/Users/donkey/Development/1/Glory/src/renderer/WaterRenderer.cpp`
- `/Users/donkey/Development/1/Glory/src/renderer/MeshEffectRenderer.cpp`
- `/Users/donkey/Development/1/Glory/src/nav/DebugRenderer.cpp`

**Utilities**:
- `/Users/donkey/Development/1/Glory/src/renderer/ParallelRecorder.h` (render pass pass-through)
- `/Users/donkey/Development/1/Glory/src/renderer/ThreadedCommandPool.h` (render pass pass-through)

---

