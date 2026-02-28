Technical Implementation Document: MOBA-Style Edge Panning Camera
1. Overview & Requirements
This document outlines the implementation of a top-down, fixed-angle, edge-panning camera suitable for a MOBA (like League of Legends). The camera operates by moving the world-space view when the user's cursor enters a defined margin near the edges of the screen or window.

Core Objectives:

Translate 2D screen-space cursor coordinates into 3D world-space camera translations.

Normalize diagonal movement to prevent speed anomalies.

Generate the View and Projection matrices formatted for Vulkan's coordinate system.

Handle edge cases like window focus and multi-monitor setups.

2. Configuration Parameters
Your agent should encapsulate the camera state in a dedicated struct or class. Here are the recommended configurable parameters:

C++
struct CameraConfig {
    // Positioning
    glm::vec3 position;       // Current world position (usually tracks the ground target)
    float heightOffset;       // How high the camera is above the ground (Zoom level)
    float pitchAngle;         // Fixed looking-down angle (e.g., 55 to 65 degrees)
    float yawAngle;           // Fixed rotation around the Y axis
    
    // Panning Logic
    float panSpeed;           // World units per second
    int edgeMarginPixels;     // Thickness of the trigger zone (e.g., 10-30 pixels)
    
    // Bounds (Optional but recommended)
    glm::vec2 mapMinBounds;   // Minimum X and Z map coordinates
    glm::vec2 mapMaxBounds;   // Maximum X and Z map coordinates
};
3. The Edge Panning Algorithm
The core logic requires reading the mouse position every frame and comparing it against the window dimensions.

Step 1: Input Detection

Retrieve the current mouse coordinates (x,y) and the window dimensions (W,H).

Step 2: Determine Direction Vectors

We calculate a 2D direction vector v 
dir
​	
 =⟨x 
dir
​	
 ,y 
dir
​	
 ⟩ based on the cursor's position relative to the edgeMarginPixels (M).

Left: If x≤M, then x 
dir
​	
 =−1

Right: If x≥W−M, then x 
dir
​	
 =1

Top: If y≤M, then y 
dir
​	
 =1 (Assuming top is Y=0 in screen space, panning up means moving forward in world Z or Y)

Bottom: If y≥H−M, then y 
dir
​	
 =−1

Step 3: Normalization and Time Integration

If the camera pans diagonally, the raw direction vector length would be  
1 
2
 +1 
2
 

​	
 ≈1.414, making diagonal movement 41% faster. The agent must normalize this vector.

Using LaTeX for the mathematical update:

v 
norm
​	
 = 
∥v 
dir
​	
 ∥
v 
dir
​	
 
​	
 
The new camera target position P 
new
​	
  is updated using the normalized vector, the configured speed S, and the frame delta time Δt:

P 
new
​	
 =P 
old
​	
 +(v 
norm
​	
 ×S×Δt)
Pseudo-code for the Agent:

C++
glm::vec2 panDirection(0.0f);
bool isPanning = false;

// Check X axis
if (mouseX <= edgeMarginPixels) {
    panDirection.x = -1.0f;
    isPanning = true;
} else if (mouseX >= windowWidth - edgeMarginPixels) {
    panDirection.x = 1.0f;
    isPanning = true;
}

// Check Y axis (Note: screen Y=0 is usually top, world forward is usually -Z)
if (mouseY <= edgeMarginPixels) {
    panDirection.y = -1.0f; // Move forward in world
    isPanning = true;
} else if (mouseY >= windowHeight - edgeMarginPixels) {
    panDirection.y = 1.0f;  // Move backward in world
    isPanning = true;
}

if (isPanning) {
    panDirection = glm::normalize(panDirection);
    
    // Translate 2D pan direction to 3D world movement
    // Assuming Y is UP, X is RIGHT, and -Z is FORWARD
    camera.targetPosition.x += panDirection.x * camera.panSpeed * deltaTime;
    camera.targetPosition.z += panDirection.y * camera.panSpeed * deltaTime;
    
    // Clamp to map bounds here
}
4. Vulkan-Specific Matrix Generation
Vulkan handles coordinate systems differently than OpenGL. Specifically, Vulkan's Y-axis points downwards in clip space, and its depth range is [0,1] (unlike OpenGL's [−1,1]).

If your engine uses GLM, the agent must define #define GLM_FORCE_DEPTH_ZERO_TO_ONE before including GLM.

The View Matrix

For a MOBA, the camera doesn't usually physically move "forward" into the ground; instead, it looks at a "target" on the ground plane.

C++
// Calculate the actual camera position based on target, height, and pitch
glm::vec3 cameraPos = camera.targetPosition;
cameraPos.y += camera.heightOffset;
// Offset the camera backward along the Z axis based on the pitch angle
// (e.g., if pitch is 60 degrees looking down)
cameraPos.z += camera.heightOffset / tan(glm::radians(camera.pitchAngle));

// Generate View Matrix
glm::mat4 viewMatrix = glm::lookAt(cameraPos, camera.targetPosition, glm::vec3(0.0f, 1.0f, 0.0f));
The Projection Matrix & Vulkan Y-Flip

C++
glm::mat4 projMatrix = glm::perspective(glm::radians(camera.fov), 
                                        windowWidth / (float)windowHeight, 
                                        0.1f, 1000.0f);

// CRITICAL VULKAN STEP: Flip the Y axis to account for Vulkan's coordinate system
projMatrix[1][1] *= -1.0f; 
Uniform Buffer Object (UBO) Update

The agent should package these matrices into a UBO structure that matches the vertex shader's layout, and use vkCmdUpdateBuffer or map memory to push this to the GPU every frame.

5. Crucial Edge Cases & Polish
Instruct your agent to implement the following safeguards to ensure a professional feel:

Window Focus / Confine Cursor: If the game is played in windowed or borderless mode, edge panning should only trigger if the window has OS focus. The cursor should ideally be locked (confined) to the window bounds using the platform API (e.g., ClipCursor on Windows or glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_CONFINED) if using GLFW).

Smoothing/Interpolation: Instead of instantly moving the camera at max speed, apply a brief easing function (like Lerp) to the velocity to make starting and stopping feel less jerky.

Mouse Delta Fallback: While middle-mouse click-and-drag panning wasn't explicitly requested, it is a standard MOBA feature. It's recommended to map raw mouse deltas to camera translation when the middle mouse button is held down.