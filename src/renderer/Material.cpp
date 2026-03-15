#include "renderer/Material.h"

// Material is now a lightweight data struct — no Vulkan resource management.
// All texture binding is handled by BindlessDescriptors (set 1, binding 0).
// The per-entity MaterialComponent stores indices into the bindless array
// and material parameters, passed to the shader via the instance buffer.

namespace glory {
} // namespace glory
