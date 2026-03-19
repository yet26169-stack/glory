#pragma once
// RendererTypes.h — single header for common Vulkan/VMA/GLM renderer includes.
// Include this in renderer implementation files in place of repeating the same
// platform headers across every .cpp.
//
// Forward declarations are provided for engine-owned types so that consumers of
// this header do not pull in their full definitions; include the specific headers
// when the complete definition is required.

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>

// Forward declarations for engine-owned renderer types.
namespace glory {
    class  Device;
    class  Buffer;
    class  Image;
    class  Texture;
    class  Mesh;
    class  Model;
    class  StaticSkinnedMesh;
    struct RenderFormats;
    class  Descriptors;
    class  BindlessDescriptors;
} // namespace glory
