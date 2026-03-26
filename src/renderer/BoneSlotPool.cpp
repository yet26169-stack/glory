#include "renderer/BoneSlotPool.h"
#include "renderer/Descriptors.h"
#include "scene/Components.h"
#include "animation/AnimationPlayer.h"
#include "animation/AnimationClip.h"
#include "animation/Skeleton.h"

#include <unordered_set>

namespace glory {

void BoneSlotPool::assignAndUpload(entt::registry& reg, Descriptors& descriptors,
                                   uint32_t frameIndex) {
    auto view = reg.view<SkeletonComponent, AnimationComponent,
                         GPUSkinnedMeshComponent, TransformComponent>();

    // Collect currently-alive skinned entities
    std::unordered_set<entt::entity> alive;
    for (auto e : view)
        alive.insert(e);

    // Return slots from destroyed / de-skinned entities to the free queue
    for (auto it = m_entityBoneSlot.begin(); it != m_entityBoneSlot.end(); ) {
        if (alive.find(it->first) == alive.end()) {
            m_freeBoneSlots.push(it->second);
            it = m_entityBoneSlot.erase(it);
        } else {
            ++it;
        }
    }

    // Assign a slot to every entity that doesn't already have one
    for (auto e : view) {
        if (m_entityBoneSlot.find(e) == m_entityBoneSlot.end()) {
            uint32_t slot;
            if (!m_freeBoneSlots.empty()) {
                slot = m_freeBoneSlots.front();
                m_freeBoneSlots.pop();
            } else {
                slot = m_nextSlot++;
            }
            m_entityBoneSlot[e] = slot;
        }
    }

    // Upload skinning matrices and tag each GPUSkinnedMeshComponent
    uint32_t maxSlotPlusOne = 0;
    for (auto&& [e, skel, anim, ssm, t] : view.each()) {
        uint32_t slot = m_entityBoneSlot[e];
        const auto& matrices = anim.player.getSkinningMatrices();
        descriptors.writeBoneSlot(frameIndex, slot, matrices);
        ssm.boneSlot = slot;
        maxSlotPlusOne = std::max(maxSlotPlusOne, slot + 1);
    }
    m_slotsUsed = maxSlotPlusOne;

    if (m_slotsUsed > 0)
        descriptors.flushBones(frameIndex);
}

} // namespace glory
