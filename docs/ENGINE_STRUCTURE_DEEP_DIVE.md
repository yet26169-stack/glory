# Glory Engine: Deep Technical Research & Architectural Analysis

## 1. Executive Summary
The **Glory Engine** is a high-performance, custom-built C++ game engine utilizing the **Vulkan API**. It is architected specifically for MOBA (Multiplayer Online Battle Arena) and RTS (Real-Time Strategy) games, focusing on high entity counts (hundreds of units), low-latency simulation, and a modern GPU-driven rendering pipeline.

The engine leverages **EnTT (ECS)** for its core logic, ensuring cache-friendly data access and modular system design. It employs advanced rendering techniques like **GPU Frustum Culling**, **Compute Shader Skinning**, and **Cascaded Shadow Maps (CSM)**, which are typically found in AAA-tier engines.

---

## 2. Core Architecture

### 2.1 Main Loop & Simulation
The engine follows a decoupled simulation/render loop:
- **Application (`src/core/Application.cpp`):** Manages the window and the high-level loop.
- **Fixed-Timestep Simulation:** The simulation (`Renderer::drawFrame`) runs at a fixed **30 Hz** frequency using an accumulator. This ensures deterministic behavior for gameplay systems (AI, projectiles, physics) regardless of the rendering frame rate.
- **Vulkan Synchronization:** Uses `Sync.h` to manage "Frames in Flight" (typically 2 or 3), allowing the CPU to record commands for the next frame while the GPU is still rendering the previous one.

### 2.2 ECS (Entity Component System)
Powered by **EnTT**, the engine organizes data into components (`src/scene/Components.h`) and logic into systems:
- **Efficiency:** Data-oriented design minimizes cache misses.
- **Modularity:** Systems like `MinionSystem`, `AbilitySystem`, and `ProjectileSystem` operate independently on the registry.

---

## 3. Rendering Pipeline

### 3.1 Deferred Shading
The engine uses a **Deferred Rendering** path (`GBuffer.cpp`):
- **G-Buffer Pass:** Renders geometry properties (Position, Normal, Albedo, Material) into multiple render targets.
- **Lighting Pass:** Computes lighting once per pixel using the G-Buffer, allowing for hundreds of light sources without the exponential cost of forward rendering.
- **MOBA Specialization:** In MOBA mode, expensive effects like SSAO and Bloom are bypassed via specialization constants and dummy descriptors to maximize performance for top-down views.

### 3.2 GPU-Driven Rendering (`src/renderer/GpuCuller.h`)
This is a "Triple-A" feature:
- **Compute Culling:** A compute shader (`cull.comp`) tests every object's AABB against the camera frustum on the GPU.
- **Indirect Drawing:** Uses `vkCmdDrawIndexedIndirectCount`. The GPU writes its own draw commands into an indirect buffer, eliminating the "Draw Call" CPU bottleneck. Only visible objects are ever processed by the graphics pipeline.

### 3.3 Cascaded Shadow Maps (CSM)
Implemented in `CascadeShadow.cpp`, it splits the view frustum into multiple zones (cascades) to provide high-resolution shadows near the camera while maintaining coverage for distant terrain—essential for the wide view area of a MOBA.

### 3.4 Animation & Skinning
- **Vertex Shader Skinning:** Standard path for low-to-medium entity counts.
- **Compute Skinner (`ComputeSkinner.cpp`):** Used when the number of skinned entities exceeds a threshold. It pre-skins vertices in a compute pass and writes to a vertex buffer, allowing hundreds of minions to be animated with minimal overhead.
- **LOD System:** `SkinnedLODComponent` switches between different mesh resolutions based on camera distance, significantly reducing the vertex processing load.

---

## 4. Game Systems

### 4.1 Minion & NPC AI (`src/minion/MinionSystem.h`)
- **Spatial Hashing:** Instead of O(N²) collision/aggro checks, it uses a grid-based spatial hash to quickly find nearby targets.
- **State Machine:** Minions transition between Idle, Move-to-Lane, Aggro, and Combat states.
- **Scaling:** Stat scaling and reward logic are built-in, supporting standard MOBA progression.

### 4.2 Terrain & Navigation
- **Heightmap System:** Generates height data from GLB map meshes, allowing units to "snap" to the terrain surface accurately.
- **Isometric Camera:** Constraints are applied to maintain a consistent top-down perspective with bounds checking.

### 4.3 Ability & Combat System
- **Modular Projectiles:** `ProjectileSystem` handles linear and targeted projectiles with customizable on-hit effects.
- **VFX Integration:** Uses a `VFXEventQueue` to decouple gameplay logic from visual effect triggering (particles, impacts).

---

## 5. Asset Management

### 5.1 GLB Loading & Optimization
- **TinyGLTF:** Used for loading industry-standard `.glb` files.
- **Meshoptimizer:** Integrated into the loader to simplify meshes (decimation) and optimize vertex caches for the GPU.
- **Texture Streamer:** An asynchronous loader (`TextureStreamer.cpp`) that uploads textures to the GPU on a background transfer queue, preventing frame hitches during gameplay.

---

## 6. Diagnosis & Performance Tooling
- **GpuProfiler:** Real-time GPU timing per-pass (Shadow, G-Buffer, Lighting, Post).
- **CPU Profiler:** Measures execution time of simulation and command recording blocks.
- **Debug Overlay:** Integrated **ImGui** for hot-swapping configurations and visualizing ECS state.

---

## 7. Recommendations for AAA Scaling

To reach the efficiency of top-tier MOBA/RTS titles (League of Legends, Dota 2, StarCraft II), the following improvements are suggested:

1.  **Multi-Threaded Command Recording:** Utilize Vulkan's secondary command buffers to record draw commands across all CPU cores.
2.  **Navigation Mesh (NavMesh):** Replace current waypoint-based movement with a robust NavMesh (e.g., Recast/Detour) for complex pathfinding and obstacle avoidance.
3.  **Network Architecture:** Implement a "Deterministic Lockstep" or "Server-Authoritative with Client-Side Prediction" networking model, as MOBA games require strict synchronization.
4.  **Advanced Culling:** Implement **Occlusion Culling** (Hierarchical Z-Buffer) in the `GpuCuller` to skip rendering objects hidden behind terrain or structures.
5.  **Vulkan 1.3 Features:** Adopt Dynamic Rendering and Descriptor Indexing (Bindless) more aggressively to further reduce descriptor set management overhead.
6.  **Asset Pipeline:** Move from runtime GLB loading to a custom binary format ("Cooked" assets) to drastically improve startup times and memory layout.
