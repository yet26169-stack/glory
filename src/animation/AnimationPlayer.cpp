#define GLM_ENABLE_EXPERIMENTAL

#include "animation/AnimationPlayer.h"

#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace glory {

void AnimationPlayer::setSkeleton(const Skeleton *skeleton) {
  m_skeleton = skeleton;
  if (!skeleton)
    return;

  size_t n = skeleton->joints.size();
  m_localPoses.resize(n);
  m_globalTransforms.resize(n, glm::mat4(1.0f));
  m_skinningMatrices.resize(n, glm::mat4(1.0f));

  // Initialize local poses from skeleton rest pose
  for (size_t i = 0; i < n; ++i) {
    m_localPoses[i].translation = skeleton->joints[i].localTranslation;
    m_localPoses[i].rotation = skeleton->joints[i].localRotation;
    m_localPoses[i].scale = skeleton->joints[i].localScale;
  }
}

void AnimationPlayer::setClip(const AnimationClip *clip) {
  if (m_clip != clip) {
    m_clip = clip;
    m_time = 0.0f;
  }
}

void AnimationPlayer::update(float deltaTime) {
  if (!m_skeleton)
    return;

  if (m_clip && m_clip->duration > 0.0f) {
    m_time += deltaTime;
    if (m_clip->looping) {
      m_time = std::fmod(m_time, m_clip->duration);
      if (m_time < 0.0f)
        m_time += m_clip->duration;
    } else {
      m_time = std::min(m_time, m_clip->duration);
    }
    sampleChannels();
  }

  computeGlobalTransforms();
}

// Binary search for the keyframe interval containing time t
static size_t findKeyframe(const std::vector<float> &timestamps, float t) {
  if (timestamps.size() <= 1)
    return 0;
  // Clamp to valid range
  if (t <= timestamps.front())
    return 0;
  if (t >= timestamps.back())
    return timestamps.size() - 2;

  // Binary search for the interval [i, i+1] containing t
  auto it = std::upper_bound(timestamps.begin(), timestamps.end(), t);
  size_t idx = static_cast<size_t>(it - timestamps.begin());
  return idx > 0 ? idx - 1 : 0;
}

void AnimationPlayer::sampleChannels() {
  if (!m_clip || !m_skeleton)
    return;

  // Reset to the rest pose this clip was authored against.  When a clip was
  // loaded from a different GLB file the joint rest transforms may differ
  // slightly from the host skeleton's; using the clip's own rest pose prevents
  // the animation deltas from being applied in the wrong reference frame.
  bool hasClipRest = !m_clip->restPose.empty() &&
                     m_clip->restPose.size() == m_skeleton->joints.size();
  for (size_t i = 0; i < m_skeleton->joints.size(); ++i) {
    if (hasClipRest) {
      m_localPoses[i].translation = m_clip->restPose[i].translation;
      m_localPoses[i].rotation    = m_clip->restPose[i].rotation;
      m_localPoses[i].scale       = m_clip->restPose[i].scale;
    } else {
      m_localPoses[i].translation = m_skeleton->joints[i].localTranslation;
      m_localPoses[i].rotation    = m_skeleton->joints[i].localRotation;
      m_localPoses[i].scale       = m_skeleton->joints[i].localScale;
    }
  }

  for (const auto &channel : m_clip->channels) {
    if (channel.targetJointIndex < 0 ||
        channel.targetJointIndex >= static_cast<int>(m_localPoses.size()))
      continue;

    auto &pose = m_localPoses[channel.targetJointIndex];

    if (channel.timestamps.empty())
      continue;

    size_t i0 = findKeyframe(channel.timestamps, m_time);
    size_t i1 = std::min(i0 + 1, channel.timestamps.size() - 1);

    float t0 = channel.timestamps[i0];
    float t1 = channel.timestamps[i1];
    float alpha = (i0 == i1) ? 0.0f : (m_time - t0) / (t1 - t0);
    alpha = std::clamp(alpha, 0.0f, 1.0f);

    switch (channel.path) {
    case AnimationPath::Translation:
      if (i0 < channel.translationKeys.size() &&
          i1 < channel.translationKeys.size()) {
        pose.translation =
            glm::mix(channel.translationKeys[i0],
                     channel.translationKeys[i1], alpha);
      }
      break;
    case AnimationPath::Rotation:
      if (i0 < channel.rotationKeys.size() &&
          i1 < channel.rotationKeys.size()) {
        pose.rotation =
            glm::slerp(channel.rotationKeys[i0],
                       channel.rotationKeys[i1], alpha);
      }
      break;
    case AnimationPath::Scale:
      if (i0 < channel.scaleKeys.size() && i1 < channel.scaleKeys.size()) {
        pose.scale = glm::mix(channel.scaleKeys[i0],
                              channel.scaleKeys[i1], alpha);
      }
      break;
    }
  }
}

void AnimationPlayer::computeGlobalTransforms() {
  if (!m_skeleton)
    return;

  size_t n = m_skeleton->joints.size();

  // Walk joints in topological order (parents first, guaranteed by loader)
  for (size_t i = 0; i < n; ++i) {
    const auto &pose = m_localPoses[i];

    // Build local TRS matrix
    glm::mat4 T = glm::translate(glm::mat4(1.0f), pose.translation);
    glm::mat4 R = glm::toMat4(pose.rotation);
    glm::mat4 S = glm::scale(glm::mat4(1.0f), pose.scale);
    glm::mat4 localMat = T * R * S;

    int parent = m_skeleton->joints[i].parentIndex;
    if (parent >= 0 && parent < static_cast<int>(n)) {
      m_globalTransforms[i] = m_globalTransforms[parent] * localMat;
    } else {
      // Root joint: include armature transform (handles Z-up→Y-up, scale, etc.)
      m_globalTransforms[i] = m_skeleton->armatureTransform * localMat;
    }

    // Skinning matrix = globalTransform * inverseBindMatrix
    m_skinningMatrices[i] =
        m_globalTransforms[i] * m_skeleton->joints[i].inverseBindMatrix;
  }
}

} // namespace glory
