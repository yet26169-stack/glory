#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace glory {

struct Joint {
  std::string name;
  int parentIndex = -1; // -1 = root
  glm::mat4 inverseBindMatrix{1.0f};
  glm::vec3 localTranslation{0.0f};
  glm::quat localRotation{1.0f, 0.0f, 0.0f, 0.0f}; // GLM order: w,x,y,z
  glm::vec3 localScale{1.0f};
};

struct Skeleton {
  std::vector<Joint> joints;
  std::vector<int> jointNodeIndices; // glTF node index per joint

  // The world-space transform of the armature root node (parent of all joints).
  // glTF stores this on the scene-graph node above the skin's skeleton root;
  // the loader copies it here so the skinning pipeline can include it when
  // computing global joint transforms.  Defaults to identity.
  glm::mat4 armatureTransform{1.0f};

  int findJoint(const std::string &name) const {
    for (size_t i = 0; i < joints.size(); ++i) {
      if (joints[i].name == name)
        return static_cast<int>(i);
    }
    return -1;
  }
};

// Per-vertex bone influences (up to 4 joints)
struct SkinVertex {
  glm::ivec4 joints{0};  // joint indices
  glm::vec4 weights{0.0f}; // joint weights (should sum to 1.0)
};

} // namespace glory
