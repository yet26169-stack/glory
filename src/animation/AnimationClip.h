#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace glory {

enum class AnimationPath { Translation, Rotation, Scale };

struct AnimationChannel {
  int targetJointIndex = -1;
  AnimationPath path = AnimationPath::Translation;

  std::vector<float> timestamps;

  // Only one of these is populated, depending on path
  std::vector<glm::vec3> translationKeys;
  std::vector<glm::quat> rotationKeys; // GLM order: w,x,y,z
  std::vector<glm::vec3> scaleKeys;
};

// Per-joint rest pose that an animation was authored against.
// When an animation from a different GLB file is retargeted onto a skeleton,
// its rest pose may differ slightly from the host skeleton's rest pose.
// Storing it here lets the player reset to the correct rest before sampling.
struct ClipRestPose {
  glm::vec3 translation{0.0f};
  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
  glm::vec3 scale{1.0f};
};

struct AnimationClip {
  std::string name;
  float duration = 0.0f;
  bool looping = true;
  // Physical distance (engine units) the character covers in one full animation
  // cycle, used for stride-length-based speed synchronization to eliminate foot
  // sliding.  Set to 0 to fall back to moveSpeed-ratio scaling.
  float strideLength = 0.0f;
  std::vector<AnimationChannel> channels;

  // Optional: the rest pose this clip was authored against.
  // If non-empty, sampleChannels() uses these instead of the skeleton's rest.
  std::vector<ClipRestPose> restPose;
};

} // namespace glory
