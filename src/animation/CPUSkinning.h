#pragma once

#include "animation/Skeleton.h"
#include "renderer/Buffer.h" // Vertex

#include <glm/glm.hpp>

#include <vector>

namespace glory {

// Transform bind-pose vertices by skinning matrices, writing to outVertices.
// outVertices must be pre-sized to match bindPose.
void applyCPUSkinning(const std::vector<Vertex> &bindPose,
                      const std::vector<SkinVertex> &skinData,
                      const std::vector<glm::mat4> &skinningMatrices,
                      std::vector<Vertex> &outVertices);

} // namespace glory
