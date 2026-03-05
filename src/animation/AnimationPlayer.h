#pragma once

#include "animation/AnimationClip.h"
#include "animation/Skeleton.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>

namespace glory {

struct JointPose {
  glm::vec3 translation{0.0f};
  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
  glm::vec3 scale{1.0f};
};

class AnimationPlayer {
public:
  void setSkeleton(const Skeleton *skeleton);
  void setClip(const AnimationClip *clip);
  void update(float deltaTime);

  // Returns globalTransform[i] * inverseBindMatrix[i] for each joint
  const std::vector<glm::mat4> &getSkinningMatrices() const {
    return m_skinningMatrices;
  }

  bool isPlaying() const { return m_clip != nullptr; }

private:
  const Skeleton *m_skeleton = nullptr;
  const AnimationClip *m_clip = nullptr;
  float m_time = 0.0f;

  std::vector<JointPose> m_localPoses;
  std::vector<glm::mat4> m_globalTransforms;
  std::vector<glm::mat4> m_skinningMatrices;

  void sampleChannels();
  void computeGlobalTransforms();
};

} // namespace glory
