# Glory Engine — Deep-Dive Issue Analysis & Fix Plan

> **Repository:** `donkey-ux/glory`
> **Date:** 2026-03-05
> **Primary symptom:** Textures do not render on Apple Silicon (Mac M4) via MoltenVK.
> **Commit analyzed:** `cfc2e2776b1843f2674f6502eec8637ccbd534a1`

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Architecture Overview](#2-architecture-overview)
3. [CRITICAL — Vulkan Queue Family Ownership Bug (Texture Rendering Failure)](#3-critical--vulkan-queue-family-ownership-bug)
   - 3.1 [Root Cause](#31-root-cause)
   - 3.2 [Affected Files & Code Paths](#32-affected-files--code-paths)
   - 3.3 [Why It Only Fails on Apple Silicon / MoltenVK](#33-why-it-only-fails-on-apple-silicon--moltenvk)
   - 3.4 [Proof: TextureStreamer.cpp Already Does It Right](#34-proof-texturestreamercpp-already-does-it-right)
   - 3.5 [Fix Instructions](#35-fix-instructions)
4. [MEDIUM — Buffer Upload Queue Ownership Gap](#4-medium--buffer-upload-queue-ownership-gap)
5. [MEDIUM — Image Class Lacks Queue Family Awareness](#5-medium--image-class-lacks-queue-family-awareness)
6. [LOW — Missing VMA Memory Flush for Non-Coherent Memory](#6-low--missing-vma-memory-flush-for-non-coherent-memory)
7. [LOW — Pipeline Barrier Stage Mismatch on Transfer Queue](#7-low--pipeline-barrier-stage-mismatch-on-transfer-queue)
8. [INFO — MoltenVK / Apple Silicon Compatibility Notes](#8-info--moltenvk--apple-silicon-compatibility-notes)
9. [Complete Fix Checklist](#9-complete-fix-checklist)
10. [Testing Plan](#10-testing-plan)

---

## 1. Executive Summary

The glory engine's Vulkan renderer has a **critical queue family ownership transfer bug** that causes all textures to render as blank/black on Mac M4 (Apple Silicon via MoltenVK). The engine selects a dedicated DMA transfer queue when available and uploads all texture and buffer data through it, but never properly transfers ownership of those resources to the graphics queue family. On permissive drivers (NVIDIA/AMD desktop) this works by accident; on MoltenVK it causes undefined resource visibility.

**The single most impactful fix is changing `Image.cpp` to use `VK_SHARING_MODE_CONCURRENT` when the transfer queue family differs from the graphics queue family.** This pattern is already correctly implemented in `TextureStreamer.cpp` but was never applied to the core `Image` and `Buffer` classes.

---

## 2. Architecture Overview

### Queue Family Setup (`Device.cpp` lines 138-171)

```
Device::findQueueFamilies():
  1. Find GRAPHICS queue family → graphicsFamily
  2. Find PRESENT queue family  → presentFamily
  3. Find TRANSFER-only family  → transferFamily (dedicated DMA)
  4. Fallback: transferFamily = graphicsFamily
```

### Texture Upload Flow

```
Texture::create*() / Texture::Texture(filepath)
  └─ staging buffer (CPU_ONLY) ← pixel data via memcpy
  └─ Image() constructor        ← VK_SHARING_MODE_EXCLUSIVE  ⚠️
  └─ transitionImageLayout()    ← submitted on TRANSFER queue
  │    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED  ⚠️
  │    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED  ⚠️
  └─ copyBufferToImage()        ← submitted on TRANSFER queue
  └─ transitionImageLayout()    ← submitted on TRANSFER queue
  └─ createSampler()
  └─ writeBindlessTexture()     ← descriptor expects SHADER_READ_ONLY
  └─ Fragment shader samples    ← on GRAPHICS queue → UNDEFINED DATA
```

### Buffer Upload Flow

```
Buffer::createDeviceLocal()
  └─ staging buffer (CPU_ONLY) ← data via memcpy
  └─ target buffer (GPU_ONLY, EXCLUSIVE sharing)  ⚠️
  └─ vkCmdCopyBuffer           ← submitted on TRANSFER queue
  └─ vkQueueWaitIdle(transferQueue)
  └─ Vertex shader reads       ← on GRAPHICS queue → UNDEFINED DATA
```

---

## 3. CRITICAL — Vulkan Queue Family Ownership Bug

### 3.1 Root Cause

When `Device::findQueueFamilies()` finds a **dedicated transfer queue family** (one that has `VK_QUEUE_TRANSFER_BIT` but NOT `VK_QUEUE_GRAPHICS_BIT`), the transfer and graphics families have **different indices**. Under Vulkan's rules:

- Resources created with `VK_SHARING_MODE_EXCLUSIVE` are **owned by one queue family at a time**.
- To use them from a different family, an explicit **queue family ownership transfer** must be performed via paired release/acquire barriers.
- `VK_QUEUE_FAMILY_IGNORED` in barriers is **only valid when no ownership transfer is needed** (i.e., same family or `CONCURRENT` sharing mode).

The engine violates all three rules.

### 3.2 Affected Files & Code Paths

#### File 1: `src/renderer/Image.cpp` (line 27) — **PRIMARY FIX TARGET**

```cpp
// CURRENT (BROKEN):
imgCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
// No queue family indices set
```

**Every** `Image` created via this constructor — which includes all procedural textures (checkerboard, marble, wood, lava, rock, brick normal, tiles, circuit, hexgrid, gradient, noise), all file-loaded textures, all GLB textures, and the default 1×1 white/flat-normal textures — inherits this bug.

URL: https://github.com/donkey-ux/glory/blob/cfc2e2776b1843f2674f6502eec8637ccbd534a1/src/renderer/Image.cpp#L27

#### File 2: `src/renderer/Texture.cpp` (lines 18-79) — Barrier functions

```cpp
// transitionImageLayout() — lines 38-39:
barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

// Submitted on transfer queue — line 75:
vkQueueSubmit(device.getTransferQueue(), 1, &submitInfo, VK_NULL_HANDLE);
```

URL: https://github.com/donkey-ux/glory/blob/cfc2e2776b1843f2674f6502eec8637ccbd534a1/src/renderer/Texture.cpp#L18-L79

#### File 3: `src/renderer/Buffer.cpp` (lines 210-261) — `createDeviceLocal()`

```cpp
// Line 237: command pool from transfer family
allocInfo.commandPool = device.getTransferCommandPool();

// Line 255: submitted on transfer queue
vkQueueSubmit(device.getTransferQueue(), 1, &submitInfo, VK_NULL_HANDLE);
```

URL: https://github.com/donkey-ux/glory/blob/cfc2e2776b1843f2674f6502eec8637ccbd534a1/src/renderer/Buffer.cpp#L210-L261

#### File 4: `src/renderer/Device.cpp` (lines 154-161) — Queue family selection

```cpp
// Selects dedicated DMA queue:
if ((families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
    !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
    !indices.transferFamily.has_value()) {
    indices.transferFamily = i;
}
```

URL: https://github.com/donkey-ux/glory/blob/cfc2e2776b1843f2674f6502eec8637ccbd534a1/src/renderer/Device.cpp#L154-L161

### 3.3 Why It Only Fails on Apple Silicon / MoltenVK

| Platform | Queue Families | Behavior |
|----------|---------------|----------|
| **NVIDIA (desktop)** | Family 0: Graphics+Compute+Transfer, Family 1: Transfer+Compute, Family 2: Transfer-only | Dedicated transfer queue found → bug exists but NVIDIA driver is lenient about ownership |
| **AMD (desktop)** | Family 0: Graphics+Compute+Transfer, Family 1: Compute+Transfer, Family 2: Transfer-only | Same — AMD driver hides the bug |
| **Apple Silicon (MoltenVK)** | Typically 1 family: Graphics+Compute+Transfer | **Usually no dedicated transfer** → `transferFamily == graphicsFamily` → bug is masked. BUT some MoltenVK versions / configs expose 2 families → **bug triggers** |
| **Apple M4 specifically** | MoltenVK may expose a separate transfer family depending on version | **Bug triggers → textures blank** |

Key insight: MoltenVK translates Vulkan to Metal. Metal has **no concept of queue family ownership**. When MoltenVK encounters an image that was written by one "queue family" and read by another without proper transfer, it may not synchronize the Metal command buffers properly, resulting in the GPU reading stale/zero data.

### 3.4 Proof: TextureStreamer.cpp Already Does It Right

`TextureStreamer.cpp` (lines 130-140) correctly handles this case:

```cpp
// From TextureStreamer::processRequest():
uint32_t familyIndices[2] = {xferFamily, gfxFamily};
if (dedicatedXfer) {
    ici.sharingMode           = VK_SHARING_MODE_CONCURRENT;
    ici.queueFamilyIndexCount = 2;
    ici.pQueueFamilyIndices   = familyIndices;
}
```

This proves the developer was aware of the pattern but did not apply it to `Image.cpp`, `Texture.cpp`, or `Buffer.cpp`.

### 3.5 Fix Instructions

#### Fix A — `src/renderer/Image.h` (add Device forward decl storage)

The `Image` constructor already receives `const Device&`. The fix modifies `Image.cpp` only.

#### Fix B — `src/renderer/Image.cpp` (PRIMARY FIX)

Replace lines 16-27 with:

```cpp
VkImageCreateInfo imgCI{};
imgCI.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
imgCI.imageType     = VK_IMAGE_TYPE_2D;
imgCI.extent        = { width, height, 1 };
imgCI.mipLevels     = 1;
imgCI.arrayLayers   = 1;
imgCI.format        = format;
imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
imgCI.usage         = usage;
imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;

// When a dedicated transfer queue exists, use CONCURRENT sharing so that
// both the transfer queue (uploads) and graphics queue (rendering) can
// access this image without explicit ownership transfers.
auto families = device.getQueueFamilies();
uint32_t queueFamilyIndices[] = {
    families.graphicsFamily.value(),
    families.transferFamily.value()
};

if (device.hasDedicatedTransfer()) {
    imgCI.sharingMode           = VK_SHARING_MODE_CONCURRENT;
    imgCI.queueFamilyIndexCount = 2;
    imgCI.pQueueFamilyIndices   = queueFamilyIndices;
} else {
    imgCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
}
```

This matches the pattern in `TextureStreamer.cpp`.

#### Fix C — `src/renderer/Texture.cpp` — No changes needed

Once images use `VK_SHARING_MODE_CONCURRENT`, `VK_QUEUE_FAMILY_IGNORED` becomes valid in the barriers (the Vulkan spec says IGNORED is correct for concurrent-mode images). The existing `transitionImageLayout()` and `copyBufferToImage()` code becomes correct as-is.

---

## 4. MEDIUM — Buffer Upload Queue Ownership Gap

### Location

`src/renderer/Buffer.cpp`, `Buffer::createDeviceLocal()` (lines 210-261)

URL: https://github.com/donkey-ux/glory/blob/cfc2e2776b1843f2674f6502eec8637ccbd534a1/src/renderer/Buffer.cpp#L210-L261

### Problem

Device-local buffers (vertex buffers, index buffers) are created with default `VK_SHARING_MODE_EXCLUSIVE` and uploaded via the transfer queue. The graphics queue then reads them without ownership transfer.

### Impact

On Apple Silicon this can cause **missing or corrupted geometry** (vertices reading as zero → degenerate triangles). This may be masked if MoltenVK happens to use unified memory, but is still spec-invalid.

### Fix

Modify `Buffer::createDeviceLocal()` to create the target buffer with concurrent sharing when dedicated transfer exists:

```cpp
// After creating the target buffer, before the copy:
Buffer target(allocator, size,
              usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              VMA_MEMORY_USAGE_GPU_ONLY);
```

The `Buffer` constructor doesn't have access to queue family info. Two options:

**Option A (recommended):** Pass `const Device&` to the `Buffer` constructor or add an overload of `createDeviceLocal` that sets sharing mode:

```cpp
// In the createDeviceLocal function, after creating allocCI for target:
VkBufferCreateInfo bufCI{};
bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
bufCI.size  = size;
bufCI.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

auto families = device.getQueueFamilies();
uint32_t familyIndices[] = {
    families.graphicsFamily.value(),
    families.transferFamily.value()
};
if (device.hasDedicatedTransfer()) {
    bufCI.sharingMode           = VK_SHARING_MODE_CONCURRENT;
    bufCI.queueFamilyIndexCount = 2;
    bufCI.pQueueFamilyIndices   = familyIndices;
} else {
    bufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
}
```

**Option B (simpler):** Use VMA's buffer creation directly with the concurrent sharing info, or refactor `Buffer` to accept a sharing mode parameter.

---

## 5. MEDIUM — Image Class Lacks Queue Family Awareness

### Location

`src/renderer/Image.h` and `src/renderer/Image.cpp`

URL: https://github.com/donkey-ux/glory/blob/cfc2e2776b1843f2674f6502eec8637ccbd534a1/src/renderer/Image.h
URL: https://github.com/donkey-ux/glory/blob/cfc2e2776b1843f2674f6502eec8637ccbd534a1/src/renderer/Image.cpp

### Problem

The `Image` constructor takes `const Device&` but only extracts `getDevice()` and `getAllocator()`. It never queries `getQueueFamilies()` or `hasDedicatedTransfer()`. This is the architectural root cause — the `Image` class was designed without considering multi-queue-family scenarios.

### Fix

This is addressed by Fix B in Section 3.5. The constructor already has `const Device&` so no signature change is needed. Just add the queue family logic to the `VkImageCreateInfo` setup.

---

## 6. LOW — Missing VMA Memory Flush for Non-Coherent Memory

### Location

`src/renderer/Texture.cpp` — all `staging.map()` / `staging.unmap()` pairs
`src/renderer/Buffer.cpp` — `createDeviceLocal()` staging buffer

### Problem

After writing to a `VMA_MEMORY_USAGE_CPU_ONLY` mapped buffer and before submitting a GPU copy, the code does not call `vmaFlushAllocation()`. On most desktop GPUs with host-coherent memory this is fine. On some mobile/Apple GPUs, VMA may allocate non-coherent memory where flushes are required for the GPU to see the writes.

### Impact

Low — VMA typically selects coherent memory types for `CPU_ONLY`. But on exotic configurations or future MoltenVK changes, this could cause partial/corrupt texture uploads.

### Fix

After `staging.unmap()`, add:

```cpp
vmaFlushAllocation(device.getAllocator(), staging.getAllocation(), 0, VK_WHOLE_SIZE);
```

Or use `VMA_MEMORY_USAGE_CPU_TO_GPU` which VMA documentation recommends for staging buffers (it prefers coherent memory types).

---

## 7. LOW — Pipeline Barrier Stage Mismatch on Transfer Queue

### Location

`src/renderer/Texture.cpp`, `transitionImageLayout()` lines 54-60

### Problem

The second barrier transition (`TRANSFER_DST → SHADER_READ_ONLY`) uses:
```cpp
dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
```

But this barrier is submitted to the **transfer queue**, which does NOT support the `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT` stage. According to the Vulkan spec, pipeline stage flags must be valid for the queue family the command buffer is submitted to.

### Impact

On desktop GPUs, this is silently accepted. On strict implementations (validation layers, some MoltenVK builds), this may trigger warnings or undefined behavior.

### Fix

If keeping the transfer queue submission model (with concurrent sharing), replace the dst stage with `VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT` for the transfer queue submission:

```cpp
} else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
           newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0; // No dst access on transfer queue
    srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
}
```

The actual graphics-side synchronization is handled by the implicit semaphore/fence waits before rendering begins. Alternatively, split this into two barriers (release on transfer queue, acquire on graphics queue).

---

## 8. INFO — MoltenVK / Apple Silicon Compatibility Notes

### Features Already Handled Correctly

The codebase already has several MoltenVK accommodations:

1. **`VK_KHR_portability_subset`** — Enabled in `Device.cpp` line 30 under `#ifdef __APPLE__`.
2. **`drawIndirectCount`** — Gracefully disabled when not supported (line 220-224).
3. **Descriptor indexing features** — Queried before enabling (lines 226-234).
4. **`TextureStreamer.cpp`** — Correctly uses `VK_SHARING_MODE_CONCURRENT`.
5. **`Swapchain.cpp`** — Correctly checks graphics vs present family for concurrent sharing.

### Features NOT Handled

| Feature | Status | Risk |
|---------|--------|------|
| Queue family ownership for Image | ❌ Missing | **CRITICAL** — causes blank textures |
| Queue family ownership for Buffer | ❌ Missing | **MEDIUM** — may cause geometry issues |
| `geometryShader` feature scoring | ⚠️ Scores +500 but Apple doesn't support it | LOW — doesn't affect functionality, just device scoring |
| `wideLines` feature scoring | ⚠️ Scores +100 but Apple doesn't support it | LOW — same |
| Pipeline barrier stage validation on transfer queue | ❌ Missing | LOW — may cause validation warnings |
| VMA flush for staging buffers | ❌ Missing | LOW — typically masked by coherent memory |

### `geometryShader` and `wideLines` Scoring Note

In `Device.cpp` `rateDeviceSuitability()` (lines 116-117):
```cpp
if (feats.geometryShader) score += 500;
if (feats.wideLines)      score += 100;
```

Apple Silicon does NOT support geometry shaders or wide lines. This means the scoring function gives +600 to GPUs that will never be Apple Silicon, but since there's typically only one GPU on a Mac, this is a non-issue functionally. However, it could matter in eGPU setups.

---

## 9. Complete Fix Checklist

### Priority 1 — CRITICAL (fixes blank textures on Mac M4)

- [ ] **`src/renderer/Image.cpp`** — Add concurrent sharing mode when `device.hasDedicatedTransfer()` is true. Use `getQueueFamilies()` to get graphics + transfer family indices. See Section 3.5 Fix B for exact code.

### Priority 2 — MEDIUM (fixes potential geometry issues)

- [ ] **`src/renderer/Buffer.cpp`** — Modify `Buffer::createDeviceLocal()` to create the target buffer with `VK_SHARING_MODE_CONCURRENT` and the two queue family indices when dedicated transfer exists. This requires either:
  - Refactoring `Buffer` constructor to accept sharing mode info, OR
  - Using raw `vmaCreateBuffer` with a custom `VkBufferCreateInfo` in `createDeviceLocal()`

### Priority 3 — LOW (correctness & future-proofing)

- [ ] **`src/renderer/Texture.cpp`** — Fix `transitionImageLayout()` to use `VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT` as `dstStage` for the `TRANSFER_DST → SHADER_READ_ONLY` transition when submitted on the transfer queue.

- [ ] **`src/renderer/Texture.cpp` / `src/renderer/Buffer.cpp`** — Add `vmaFlushAllocation()` after `staging.unmap()` calls, or switch staging buffers to `VMA_MEMORY_USAGE_CPU_TO_GPU`.

### Priority 4 — CLEANUP (non-functional)

- [ ] **`src/renderer/Device.cpp`** — Consider guarding the dedicated transfer queue selection with a comment noting the concurrent sharing requirement, or adding a helper method `Device::getImageSharingInfo()` that returns the correct `sharingMode`, `queueFamilyIndexCount`, and `pQueueFamilyIndices` for any code that creates images/buffers.

---

## 10. Testing Plan

### Verification Steps

1. **Build on Mac M4** — Verify textures render correctly (checkerboard, procedural, file-loaded, GLB-embedded).

2. **Check log output** — Look for:
   ```
   Transfer queue: family X (dedicated DMA)
   ```
   If X differs from the graphics family, the fix was needed. If it says "shared with graphics", the bug was masked on this particular system.

3. **Enable Vulkan Validation Layers** — Run with `VK_LAYER_KHRONOS_validation` and verify no errors about:
   - Queue family ownership transfer
   - Invalid pipeline stage flags for queue family
   - Image layout transition on wrong queue

4. **Test on NVIDIA/AMD** — Verify no regression. The concurrent sharing mode has a minor performance cost (shared memory bus instead of exclusive) but is functionally identical.

5. **Test `TextureStreamer`** — Verify async-loaded textures still work correctly (they should — `TextureStreamer` was already correct).

### Quick Smoke Test

Before the full fix, you can verify the diagnosis by adding this one-liner to `Device.cpp` line 167:

```cpp
// TEMPORARY: Force shared queue to confirm diagnosis
indices.transferFamily = indices.graphicsFamily;
```

If textures appear immediately after this change, the diagnosis is confirmed and the proper fix (concurrent sharing mode) should be applied.

---

## Appendix: File Reference Table

| File | Lines | Role | Bug? |
|------|-------|------|------|
| `src/renderer/Device.h` | 1-87 | Queue family indices, device accessors | ✅ OK |
| `src/renderer/Device.cpp` | 1-359 | Physical device selection, queue family discovery, logical device creation | ✅ OK (source of dedicated transfer) |
| `src/renderer/Image.h` | 1-38 | Image RAII wrapper declaration | ✅ OK |
| `src/renderer/Image.cpp` | 1-95 | Image creation with **hardcoded EXCLUSIVE sharing** | ❌ **FIX** |
| `src/renderer/Texture.h` | 1-66 | Texture class with sampler | ✅ OK |
| `src/renderer/Texture.cpp` | 1-1251 | All texture creation, `transitionImageLayout()`, `copyBufferToImage()` | ⚠️ Barrier stage, but OK after Image fix |
| `src/renderer/Buffer.h` | 1-95 | Buffer RAII wrapper, Vertex/InstanceData structs | ✅ OK |
| `src/renderer/Buffer.cpp` | 1-261 | Buffer creation, `createDeviceLocal()` with **transfer queue upload** | ❌ **FIX** |
| `src/renderer/TextureStreamer.h` | 1-104 | Async texture streaming declaration | ✅ OK |
| `src/renderer/TextureStreamer.cpp` | 1-340 | Async streaming with **CORRECT concurrent sharing** | ✅ Reference impl |
| `src/renderer/Swapchain.cpp` | 1-159 | Swapchain with correct graphics/present concurrent check | ✅ OK |
| `src/renderer/Descriptors.cpp` | 1-250+ | Bindless texture array, descriptor management | ✅ OK |
| `src/renderer/GLBLoader.cpp` | 1-926 | GLB/glTF loading, calls `Texture::createFromPixels()` | ✅ OK (inherits Image fix) |
| `src/renderer/Renderer.cpp` | 1-800+ | Main renderer, scene building, draw loop | ✅ OK |
| `shaders/triangle.frag` | 1-200+ | Fragment shader with bindless `nonuniformEXT` sampling | ✅ OK |
| `shaders/triangle.vert` | 1-80+ | Vertex shader with instanced rendering | ✅ OK |