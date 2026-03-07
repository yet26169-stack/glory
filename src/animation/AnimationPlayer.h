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

// Retarget a clip's keyframes from its authored rest pose to a different
// skeleton's rest pose (delta-from-rest approach).  After retargeting, the
// clip's restPose is overwritten with the target skeleton's rest.
void retargetClip(AnimationClip &clip, const Skeleton &targetSkeleton);

class AnimationPlayer {
public:
  void setSkeleton(const Skeleton *skeleton);
  void setClip(const AnimationClip *clip);

  // Smoothly blend from current clip to newClip over blendDuration seconds.
  void crossfadeTo(const AnimationClip *newClip, float blendDuration = 0.15f);

  void update(float deltaTime);

  // Scale animation playback speed (1.0 = normal, 0.0 = frozen).
  void setTimeScale(float scale) { m_timeScale = scale; }

  // Returns globalTransform[i] * inverseBindMatrix[i] for each joint
  const std::vector<glm::mat4> &getSkinningMatrices() const {
    return m_skinningMatrices;
  }

  bool isPlaying() const { return m_clip != nullptr; }
  bool isBlending() const { return m_isBlending; }

  // Re-point to skeleton without resetting animation state.
  // Must be called each frame to guard against EnTT storage reallocation.
  void refreshSkeleton(const Skeleton *skeleton) { m_skeleton = skeleton; }

private:
  const Skeleton *m_skeleton = nullptr;
  const AnimationClip *m_clip = nullptr;
  float m_time = 0.0f;
  float m_timeScale = 1.0f;

  // Crossfade blend state
  const AnimationClip *m_prevClip = nullptr;
  float m_prevTime = 0.0f;   // source clip time continues advancing during blend
  float m_blendTime = 0.0f;
  float m_blendDuration = 0.0f;
  bool m_isBlending = false;

  std::vector<JointPose> m_localPoses;
  std::vector<JointPose> m_prevPoses; // blend source poses
  std::vector<glm::mat4> m_globalTransforms;
  std::vector<glm::mat4> m_skinningMatrices;

  // Sample clip at time t into outPoses (resets to clip rest pose first).
  void sampleInto(const AnimationClip *clip, float t,
                  std::vector<JointPose> &outPoses);
  void sampleChannels();
  void computeGlobalTransforms();
};

} // namespace glory
