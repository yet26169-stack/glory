#include "animation/CPUSkinning.h"
#include <thread>

namespace glory {

void applyCPUSkinning(const std::vector<Vertex> &bindPose,
                      const std::vector<SkinVertex> &skinData,
                      const std::vector<glm::mat4> &skinningMatrices,
                      std::vector<Vertex> &outVertices) {
  size_t vertCount = bindPose.size();
  outVertices.resize(vertCount);

  unsigned int numThreads = std::thread::hardware_concurrency();
  if (numThreads == 0)
    numThreads = 4;
  // If the mesh is small-to-mid-sized, don't bother threading (avoids thread
  // creation overhead when many entities are skinned per frame).
  if (vertCount < 50000)
    numThreads = 1;

  size_t chunkSize = vertCount / numThreads;
  std::vector<std::thread> threads;

  for (unsigned int t = 0; t < numThreads; ++t) {
    size_t startIdx = t * chunkSize;
    size_t endIdx = (t == numThreads - 1) ? vertCount : startIdx + chunkSize;

    threads.emplace_back([&, startIdx, endIdx]() {
      for (size_t i = startIdx; i < endIdx; ++i) {
        const auto &src = bindPose[i];
        const auto &skin = skinData[i];

        // Build blended skinning matrix from up to 4 joint influences
        glm::mat4 skinMat(0.0f);
        for (int j = 0; j < 4; ++j) {
          float w = skin.weights[j];
          if (w <= 0.0f)
            continue;
          int jointIdx = skin.joints[j];
          if (jointIdx >= 0 &&
              jointIdx < static_cast<int>(skinningMatrices.size())) {
            skinMat += skinningMatrices[jointIdx] * w;
          }
        }

        // Transform position (w=1)
        glm::vec4 skinnedPos = skinMat * glm::vec4(src.position, 1.0f);
        outVertices[i].position = glm::vec3(skinnedPos);

        // Transform normal (mat3, then normalize)
        glm::mat3 normalMat = glm::mat3(skinMat);
        outVertices[i].normal = glm::normalize(normalMat * src.normal);

        // Pass through color and texCoord unchanged
        outVertices[i].color = src.color;
        outVertices[i].texCoord = src.texCoord;
      }
    });
  }

  for (auto &th : threads) {
    if (th.joinable()) {
      th.join();
    }
  }
}

} // namespace glory
