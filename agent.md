# Project: Vulkan Engine Base (Codename: Glory)

## 1. Tech Stack
- **Language:** C++20
- **Build System:** CMake (3.20+)
- **Graphics API:** Vulkan 1.3 (using VMA - Vulkan Memory Allocator)
- **Windowing:** GLFW
- **Math:** GLM
- **Logging:** spdlog (for clean, categorized console output)

## 2. Architecture Overview


The engine must be split into the following logical modules:
- **Core:** Application entry point and main loop.
- **Windowing:** Wrapper for GLFW and Vulkan Surface.
- **Renderer/Backend:** - `Context`: Manages Instance and Validation Layers.
    - `Device`: Handles Physical Device selection (picking the best GPU) and Logical Device creation.
    - `Swapchain`: Handles image acquisition and presentation.
    - `Pipeline`: Abstracted Graphics Pipeline (Shaders, Fixed Functions).
    - `Sync`: Manages Fences and Semaphores for "Frames in Flight."

## 3. Coding Standards
- **Naming:** `PascalCase` for Classes, `camelCase` for methods/variables, `m_` prefix for private members.
- **Error Handling:** Use `std::runtime_error` for unrecoverable Vulkan initialization failures.
- **Modern C++:** Use `std::optional`, `std::vector`, and `std::unique_ptr` appropriately. Avoid raw pointers for ownership.

## 4. Implementation Phase 1: The Core Loop
1. Initialize GLFW and create a window.
2. Create `VulkanInstance` with required extensions and Validation Layers.
3. Pick a `PhysicalDevice` based on a scoring system (Discrete GPU > Integrated).
4. Create a `LogicalDevice` with Graphics and Presentation queue families.
5. Setup the `Swapchain` and `ImageViews`.
6. Implement the "Frame in Flight" synchronization (Double or Triple Buffering).

## Lifecycle Contract
Vulkan resources MUST be destroyed in this exact reverse-initialization order.
Class member declaration order MUST mirror this. Use a central `Renderer` or
`RenderContext` class that owns all Vulkan subsystems as members declared in
initialization order, so that C++ automatic destruction handles teardown correctly.

Destruction order (last created → first destroyed):
1. Sync objects (Fences, Semaphores)
2. Command Pools / Command Buffers
3. Framebuffers
4. Pipeline + PipelineLayout
5. RenderPass
6. ImageViews
7. Swapchain
8. Logical Device
9. Surface
10. Debug Messenger
11. VkInstance

## Error Handling Strategy
- **Fatal (throw std::runtime_error):** Instance creation, Device creation,
  Pipeline compilation failures, missing required extensions/layers.
- **Recoverable (handle inline with return codes):** Swapchain out-of-date
  (`VK_ERROR_OUT_OF_DATE_KHR`), suboptimal presentation (`VK_SUBOPTIMAL_KHR`).
  These must trigger swapchain recreation, NOT exceptions.
- **Validation Layer Errors:** Logged via spdlog at `error` level through the
  `VK_EXT_debug_utils` messenger callback. Never silently ignored.

Provide a helper macro or inline function:
  `VK_CHECK(result, "context message")` — that logs and throws only on true failure.

## Frames in Flight Specification
- `MAX_FRAMES_IN_FLIGHT = 2` (constant, configurable).
- This is INDEPENDENT of swapchain image count (which may be 2 or 3).
- Each "frame in flight" owns:
  - 1 `VkCommandBuffer` (from a per-frame command pool, or reset per frame)
  - 1 `VkSemaphore` for "image available"
  - 1 `VkSemaphore` for "render finished"
  - 1 `VkFence` for CPU-GPU sync (waited on before reuse)
- Frame index is: `currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT`
- Swapchain image index comes from `vkAcquireNextImageKHR` and is NOT the frame index.

## Dependency Direction (Strict)

Core::Application
  └──→ Window        (owns, creates first)
  └──→ Renderer      (owns, receives Window reference)
         └──→ Context    (VkInstance, DebugMessenger)
         └──→ Device     (Physical + Logical, receives VkInstance + VkSurfaceKHR)
         └──→ Swapchain  (receives Device + Surface)
         └──→ Pipeline   (receives Device + Swapchain format)
         └──→ Sync       (receives Device)

Rules:
- NO module may depend on a module above it or at the same level.
- `Window` exposes a `VkSurfaceKHR` getter but NEVER includes Renderer headers.
- `Device` receives `VkInstance` and `VkSurfaceKHR` as constructor arguments, never
  fetches them from globals.
- ALL Vulkan handles are passed explicitly through constructors — NO singletons,
  NO global state, NO `extern` Vulkan handles.

## Directory Structure

Glory/
├── CMakeLists.txt                  # Root CMake
├── extern/                         # Third-party (VMA, GLM, GLFW, spdlog as submodules)
├── shaders/
│   ├── triangle.vert
│   └── triangle.frag
├── src/
│   ├── main.cpp                    # Entry point
│   ├── core/
│   │   ├── Application.h / .cpp    # Main loop, owns Window + Renderer
│   │   └── Log.h / .cpp            # spdlog initialization wrapper
│   ├── window/
│   │   └── Window.h / .cpp         # GLFW + VkSurfaceKHR
│   └── renderer/
│       ├── Context.h / .cpp        # VkInstance + Debug Messenger
│       ├── Device.h / .cpp         # Physical + Logical Device
│       ├── Swapchain.h / .cpp      # Swapchain + ImageViews
│       ├── Pipeline.h / .cpp       # Graphics Pipeline
│       ├── Sync.h / .cpp           # Fences + Semaphores per frame
│       └── Renderer.h / .cpp       # Orchestrator: owns all above

## Shader Handling
- Shaders are written in GLSL (Vulkan dialect with `#version 450`).
- Compiled to SPIR-V offline using `glslc` (from the Vulkan SDK).
- CMake custom command compiles `.vert` / `.frag` → `.spv` at build time.
- `Pipeline` class loads `.spv` from disk as `std::vector<char>`, creates
  `VkShaderModule`, uses it, then immediately destroys it after pipeline creation
  (shader modules are not needed at runtime).