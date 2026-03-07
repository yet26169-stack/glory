#define GLM_ENABLE_EXPERIMENTAL

#include "animation/AnimationPlayer.h"

#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace glory {

// ── Animation retargeting ────────────────────────────────────────────────────
void retargetClip(AnimationClip &clip, const Skeleton &targetSkeleton) {
  if (clip.restPose.empty() ||
      clip.restPose.size() != targetSkeleton.joints.size())
    return;

  // Quick check: if rest poses already match, skip retargeting.
  const float kEpsilon = 0.001f;
  bool needsRetarget = false;
  for (size_t j = 0; j < clip.restPose.size(); ++j) {
    const auto &src = clip.restPose[j];
    const auto &tgt = targetSkeleton.joints[j];
    float rDot = std::abs(glm::dot(src.rotation, tgt.localRotation));
    float tDist = glm::length(src.translation - tgt.localTranslation);
    if (rDot < 1.0f - kEpsilon || tDist > kEpsilon) {
      needsRetarget = true;
      break;
    }
  }
  if (!needsRetarget)
    return;

  // For each channel, convert keyframes: delta = inverse(srcRest) * key,
  // then retargeted = tgtRest * delta.
  for (auto &channel : clip.channels) {
    int ji = channel.targetJointIndex;
    if (ji < 0 || ji >= static_cast<int>(clip.restPose.size()))
      continue;

    const auto &srcRest = clip.restPose[ji];
    const auto &tgtJoint = targetSkeleton.joints[ji];

    switch (channel.path) {
    case AnimationPath::Rotation: {
      glm::quat srcInv = glm::inverse(srcRest.rotation);
      for (auto &key : channel.rotationKeys) {
        glm::quat delta = srcInv * key;          // deviation from source rest
        key = glm::normalize(tgtJoint.localRotation * delta); // apply to target rest
      }
      break;
    }
    case AnimationPath::Translation:
      for (auto &key : channel.translationKeys) {
        glm::vec3 delta = key - srcRest.translation;
        key = tgtJoint.localTranslation + delta;
      }
      break;
    case AnimationPath::Scale:
      for (auto &key : channel.scaleKeys) {
        glm::vec3 delta;
        for (int c = 0; c < 3; ++c) {
          float s = srcRest.scale[c];
          delta[c] = (std::abs(s) > 1e-6f) ? key[c] / s : 1.0f;
        }
        key = tgtJoint.localScale * delta;
      }
      break;
    }
  }

  // Overwrite clip rest pose with target skeleton's rest.
  for (size_t j = 0; j < clip.restPose.size(); ++j) {
    clip.restPose[j].translation = targetSkeleton.joints[j].localTranslation;
    clip.restPose[j].rotation = targetSkeleton.joints[j].localRotation;
    clip.restPose[j].scale = targetSkeleton.joints[j].localScale;
  }
}

void AnimationPlayer::setSkeleton(const Skeleton *skeleton) {
  m_skeleton = skeleton;
  if (!skeleton)
    return;

  size_t n = skeleton->joints.size();
  m_localPoses.resize(n);
  m_prevPoses.resize(n);
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
    m_isBlending = false;
    m_prevClip = nullptr;
  }
}

void AnimationPlayer::crossfadeTo(const AnimationClip *newClip,
                                  float blendDuration) {
  if (newClip == m_clip)
    return;

  // Snapshot current pose and current time as the blend source
  m_prevClip = m_clip;
  m_prevPoses = m_localPoses;
  m_prevTime = m_time;

  m_clip = newClip;
  m_time = 0.0f;
  m_blendTime = 0.0f;
  m_blendDuration = blendDuration;
  m_isBlending = (m_prevClip != nullptr && blendDuration > 0.0f);
}

void AnimationPlayer::update(float deltaTime) {
  if (!m_skeleton)
    return;

  // Advance blend timer
  if (m_isBlending) {
    m_blendTime += deltaTime;
    if (m_blendTime >= m_blendDuration) {
      m_isBlending = false;
      m_prevClip = nullptr;
    }
  }

  // Advance and sample current clip
  if (m_clip && m_clip->duration > 0.0f) {
    m_time += deltaTime * m_timeScale;

    if (m_clip->looping) {
      if (m_time > m_clip->duration) {
        m_time = std::fmod(m_time, m_clip->duration);
      }
    } else {
      m_time = std::min(m_time, m_clip->duration);
    }
    sampleInto(m_clip, m_time, m_localPoses);
  }

  // If blending, advance the source clip time and re-sample it so the outgoing
  // animation keeps playing naturally during the transition (avoids the frozen
  // "stuck frame" artifact on longer crossfades).
  if (m_isBlending && m_prevClip && m_prevPoses.size() == m_localPoses.size()) {
    // Advance source clip time (at normal 1x speed during the blend)
    m_prevTime += deltaTime;
    if (m_prevClip->looping && m_prevClip->duration > 0.0f) {
      if (m_prevTime > m_prevClip->duration)
        m_prevTime = std::fmod(m_prevTime, m_prevClip->duration);
    } else {
      m_prevTime = std::min(m_prevTime, m_prevClip->duration);
    }
    sampleInto(m_prevClip, m_prevTime, m_prevPoses);

    float blendFactor = std::clamp(m_blendTime / m_blendDuration, 0.0f, 1.0f);
    for (size_t i = 0; i < m_localPoses.size(); ++i) {
      m_localPoses[i].translation =
          glm::mix(m_prevPoses[i].translation, m_localPoses[i].translation, blendFactor);
      glm::quat q1 = m_prevPoses[i].rotation;
      glm::quat q2 = m_localPoses[i].rotation;
      if (glm::dot(q1, q2) < 0.0f)
        q2 = -q2;
      m_localPoses[i].rotation = glm::normalize(glm::slerp(q1, q2, blendFactor));
      m_localPoses[i].scale =
          glm::mix(m_prevPoses[i].scale, m_localPoses[i].scale, blendFactor);
    }
  }

  computeGlobalTransforms();
}

// Binary search for the keyframe interval containing time t
static size_t findKeyframe(const std::vector<float> &timestamps, float t, bool looping) {
  if (timestamps.size() <= 1)
    return 0;

  if (looping) {
    // If we're beyond the last timestamp, we're in the wrap-around interval [last, first]
    if (t >= timestamps.back())
      return timestamps.size() - 1;
  } else {
    // Clamp to valid range
    if (t <= timestamps.front())
      return 0;
    if (t >= timestamps.back())
      return timestamps.size() - 2;
  }

  // Binary search for the interval [i, i+1] containing t
  auto it = std::upper_bound(timestamps.begin(), timestamps.end(), t);
  size_t idx = static_cast<size_t>(it - timestamps.begin());
  return idx > 0 ? idx - 1 : 0;
}

void AnimationPlayer::sampleChannels() {
  sampleInto(m_clip, m_time, m_localPoses);
}

void AnimationPlayer::sampleInto(const AnimationClip *clip, float t,
                                 std::vector<JointPose> &poses) {
  if (!clip || !m_skeleton)
    return;

  // Reset to the rest pose this clip was authored against.
  bool hasClipRest = !clip->restPose.empty() &&
                     clip->restPose.size() == m_skeleton->joints.size();
  for (size_t i = 0; i < m_skeleton->joints.size(); ++i) {
    if (hasClipRest) {
      poses[i].translation = clip->restPose[i].translation;
      poses[i].rotation    = clip->restPose[i].rotation;
      poses[i].scale       = clip->restPose[i].scale;
    } else {
      poses[i].translation = m_skeleton->joints[i].localTranslation;
      poses[i].rotation    = m_skeleton->joints[i].localRotation;
      poses[i].scale       = m_skeleton->joints[i].localScale;
    }
  }

  for (const auto &channel : clip->channels) {
    if (channel.targetJointIndex < 0 ||
        channel.targetJointIndex >= static_cast<int>(poses.size()))
      continue;

    auto &pose = poses[channel.targetJointIndex];

    if (channel.timestamps.empty())
      continue;

    size_t i0 = findKeyframe(channel.timestamps, t, clip->looping);
    size_t i1 = (i0 + 1) % channel.timestamps.size();

    float t0 = channel.timestamps[i0];
    float t1 = channel.timestamps[i1];

    if (i1 == 0 && clip->looping) {
      t1 = clip->duration;
    }

    float alpha = (i0 == i1) ? 0.0f : (t - t0) / (t1 - t0);
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
        pose.rotation = glm::normalize(
            glm::slerp(channel.rotationKeys[i0],
                       channel.rotationKeys[i1], alpha));
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
