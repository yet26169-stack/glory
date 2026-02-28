#pragma once

#include <glm/glm.hpp>
#include <array>

namespace glory {

// Axis-aligned bounding box
struct AABB {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};

    // Transform an AABB by a model matrix (conservative expansion)
    AABB transformed(const glm::mat4& m) const {
        glm::vec3 corners[8] = {
            {min.x, min.y, min.z}, {max.x, min.y, min.z},
            {min.x, max.y, min.z}, {max.x, max.y, min.z},
            {min.x, min.y, max.z}, {max.x, min.y, max.z},
            {min.x, max.y, max.z}, {max.x, max.y, max.z},
        };
        glm::vec3 newMin(std::numeric_limits<float>::max());
        glm::vec3 newMax(std::numeric_limits<float>::lowest());
        for (auto& c : corners) {
            glm::vec3 tc = glm::vec3(m * glm::vec4(c, 1.0f));
            newMin = glm::min(newMin, tc);
            newMax = glm::max(newMax, tc);
        }
        return {newMin, newMax};
    }
};

// View-frustum with 6 planes extracted from VP matrix (Gribb–Hartmann method)
class Frustum {
public:
    void update(const glm::mat4& vp) {
        // Left
        m_planes[0] = glm::vec4(vp[0][3] + vp[0][0], vp[1][3] + vp[1][0],
                                 vp[2][3] + vp[2][0], vp[3][3] + vp[3][0]);
        // Right
        m_planes[1] = glm::vec4(vp[0][3] - vp[0][0], vp[1][3] - vp[1][0],
                                 vp[2][3] - vp[2][0], vp[3][3] - vp[3][0]);
        // Bottom
        m_planes[2] = glm::vec4(vp[0][3] + vp[0][1], vp[1][3] + vp[1][1],
                                 vp[2][3] + vp[2][1], vp[3][3] + vp[3][1]);
        // Top
        m_planes[3] = glm::vec4(vp[0][3] - vp[0][1], vp[1][3] - vp[1][1],
                                 vp[2][3] - vp[2][1], vp[3][3] - vp[3][1]);
        // Near
        m_planes[4] = glm::vec4(vp[0][3] + vp[0][2], vp[1][3] + vp[1][2],
                                 vp[2][3] + vp[2][2], vp[3][3] + vp[3][2]);
        // Far
        m_planes[5] = glm::vec4(vp[0][3] - vp[0][2], vp[1][3] - vp[1][2],
                                 vp[2][3] - vp[2][2], vp[3][3] - vp[3][2]);

        for (auto& p : m_planes) {
            float len = glm::length(glm::vec3(p));
            if (len > 0.0f) p /= len;
        }
    }

    // Returns true if the AABB is at least partially inside the frustum
    bool isVisible(const AABB& box) const {
        for (const auto& plane : m_planes) {
            glm::vec3 n(plane);
            // Positive-vertex: the AABB corner most in the direction of the normal
            glm::vec3 pv(
                (n.x >= 0.0f) ? box.max.x : box.min.x,
                (n.y >= 0.0f) ? box.max.y : box.min.y,
                (n.z >= 0.0f) ? box.max.z : box.min.z
            );
            if (glm::dot(n, pv) + plane.w < 0.0f)
                return false;
        }
        return true;
    }

private:
    std::array<glm::vec4, 6> m_planes{};
};

} // namespace glory
