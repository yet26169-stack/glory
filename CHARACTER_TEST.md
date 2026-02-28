# Click-to-Move Character Implementation Plan

## Overview
Add a capsule-shaped player character to the MOBA terrain view that moves to where the player right-clicks. The character smoothly rotates to face its movement direction and snaps to terrain height.

## Components

### 1. CharacterComponent (Components.h)
New ECS component storing movement state:
- `targetPosition` — world-space destination
- `moveSpeed` — units per second (default 6.0)
- `hasTarget` — whether the character is currently moving

### 2. InputManager Right-Click Tracking (InputManager.h/.cpp)
- `m_rightClicked` bool + `m_rightClickPos` glm::vec2 members
- In `mouseButtonCallback`: when MOBA mode (capture disabled) and right-click pressed, store cursor position and set flag
- `wasRightClicked()` — consume-once pattern (returns true once, then resets)
- `getLastClickPos()` — returns stored screen coordinates

### 3. IsometricCamera screenToWorldRay() (IsometricCamera.h/.cpp)
- Takes screen coords + window dimensions
- Returns ray origin + direction in world space
- Uses `glm::inverse(proj * view)` to unproject NDC near/far points
- Handles Vulkan Y-flip in NDC conversion

### 4. Scene Character Movement System (Scene.h/.cpp)
- `setTerrainSystem()` stores a pointer to TerrainSystem for height queries
- In `update()`: iterate entities with CharacterComponent + TransformComponent
- Move toward target at moveSpeed * deltaTime
- Rotate Y axis using atan2 to face movement direction (lerp for smooth turning)
- Snap Y position to terrain height via `TerrainSystem::GetHeightAt()`
- Stop when within 0.1 units of target

### 5. Renderer Integration (Renderer.h/.cpp)
- **Entity creation**: In `buildScene()`, create a capsule entity at map center (100, 0, 100) with CharacterComponent
- **Right-click handling**: In `drawFrame()` (MOBA mode), check `wasRightClicked()` → `screenToWorldRay()` → ray-plane intersection → set character target
- **MOBA rendering**: After terrain renders, also run instanced scene entity rendering using isometric camera matrices

## Verification
1. Build with CMake — no compile errors
2. Run app (starts in MOBA mode by default)
3. Capsule character visible at map center on terrain
4. Right-click on terrain → character moves toward click position
5. Character rotates to face movement direction
6. Character height follows terrain
7. Character stops at target position
