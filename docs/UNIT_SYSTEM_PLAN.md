# Unit Selection & Command System — Implementation Plan

This plan describes how to implement a marquee selection system and a multi-unit command system. The system allows spawning minions with the 'X' key, selecting them via a left-click-and-drag box, and commanding them to move with a subsequent left-click.

## 1. Components & Data Structures

### 1.1 New Components
**File:** `src/scene/Components.h` (or a new `src/scene/UnitComponents.h`)

```cpp
namespace glory {

struct SelectableComponent {
    bool isSelected = false;
    float selectionRadius = 1.0f;
};

struct UnitComponent {
    enum class State { IDLE, MOVING, ATTACKING };
    State state = State.IDLE;
    glm::vec3 targetPosition{0.0f};
    float moveSpeed = 5.0f;
};

// Attached to the Player/Camera entity to track selection state
struct SelectionState {
    bool isDragging = false;
    glm::vec2 dragStart{0.0f}; // Screen space
    glm::vec2 dragEnd{0.0f};   // Screen space
    std::vector<entt::entity> selectedEntities;
};

}
```

## 2. Input Handling Enhancements

### 2.1 Track Dragging
**File:** `src/input/InputManager.h/cpp`
Ensure the `InputManager` tracks `isLeftMouseDown`, `getMousePos()`, and provides `wasLeftReleased()`.

## 3. Implementation Phases

### Phase 1: Spawning (The 'X' Key)
In `Renderer::update()` or a dedicated `UnitSystem`:
1. Check if `glfwGetKey(window, GLFW_KEY_X) == GLFW_PRESS` (with a debounce timer).
2. Create an entity with:
   - `TransformComponent` (at mouse world position).
   - `GPUSkinnedMeshComponent` (using `melee_minion.glb`).
   - `SelectableComponent`.
   - `UnitComponent`.
   - `CharacterComponent` (for physics/navigation).

### Phase 2: Marquee Selection (Left-Click & Drag)
In `Renderer::recordCommandBuffer` or a UI pass:
1. **Detection**:
   - On Left-Down: Store `dragStart`.
   - While Left-Held: Update `dragEnd`. Set `isDragging = true`.
2. **Selection Logic** (On Left-Release):
   - If distance between `dragStart` and `dragEnd` is small, it's a single click.
   - Otherwise, create a frustum or use screen-to-world projection for the four corners of the box.
   - Simple Approach: Project every `SelectableComponent` entity's world position to screen space. If it falls within the `min/max` of `dragStart` and `dragEnd`, set `isSelected = true`.
3. **Visuals**:
   - Use `ImGui::GetForegroundDrawList()->AddRect()` to draw the selection box on screen.
   - For selected units, use `DebugRenderer::drawCircle()` at their feet (green ring).

### Phase 3: Movement Commands (Left-Click)
1. If the player left-clicks **without dragging** and has units selected:
   - Raycast to the floor to find `targetWorldPos`.
   - Update `UnitComponent::targetPosition` for all `isSelected` entities.
   - Set their `UnitComponent::state` to `MOVING`.

### Phase 4: Unit Movement System
A system that iterates over all entities with `UnitComponent` and `TransformComponent`:
1. If `state == MOVING`:
   - Calculate direction to `targetPosition`.
   - Update position based on `moveSpeed * deltaTime`.
   - Rotate transform to face movement direction.
   - If distance < 0.1m, set `state = State.IDLE`.
   - Trigger "walk" animation if moving, "idle" otherwise.

## 4. Code Snippets

### 4.1 Drawing the Marquee
```cpp
// In Renderer::recordCommandBuffer
if (m_selection.isDragging) {
    ImGui::GetForegroundDrawList()->AddRect(
        ImVec2(m_selection.dragStart.x, m_selection.dragStart.y),
        ImVec2(m_selection.dragEnd.x, m_selection.dragEnd.y),
        IM_COL32(0, 255, 0, 255), 0.0f, 0, 2.0f
    );
}
```

### 4.2 Selection Check (Screen Space)
```cpp
void performSelection(const glm::vec2& start, const glm::vec2& end) {
    auto view = registry.view<TransformComponent, SelectableComponent>();
    glm::vec2 min = glm::min(start, end);
    glm::vec2 max = glm::max(start, end);

    for (auto entity : view) {
        auto& t = view.get<TransformComponent>(entity);
        auto& s = view.get<SelectableComponent>(entity);
        
        glm::vec2 screenPos = worldToScreen(t.position);
        if (screenPos.x >= min.x && screenPos.x <= max.x &&
            screenPos.y >= min.y && screenPos.y <= max.y) {
            s.isSelected = true;
        } else {
            s.isSelected = false;
        }
    }
}
```

## 5. Verification Checklist
- [ ] Pressing 'X' spawns a minion at the cursor.
- [ ] Left-click and drag creates a visible green box.
- [ ] Releasing the drag selects all minions inside the box (they get a green circle at their feet).
- [ ] Left-clicking a location makes all selected minions walk to that point.
- [ ] Minions stop and play idle animation when reaching the target.
