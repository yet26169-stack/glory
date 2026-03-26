#pragma once

#include <cstdint>
#include <queue>
#include <unordered_map>
#include <entt.hpp>

namespace glory {

class Descriptors;

/// Manages bone-slot allocation for GPU-skinned entities.
/// Each skinned entity needs a unique slot in the per-frame bone SSBO.
/// The pool tracks entity→slot mappings, recycles slots from destroyed
/// entities via a free queue, and writes bone matrices to the descriptor.
class BoneSlotPool {
public:
    /// Assign bone slots and upload skinning matrices for all skinned entities.
    /// Recycles slots from entities that no longer exist.
    void assignAndUpload(entt::registry& reg, Descriptors& descriptors, uint32_t frameIndex);

    uint32_t slotsUsed() const { return m_slotsUsed; }

    const std::unordered_map<entt::entity, uint32_t>& entitySlotMap() const {
        return m_entityBoneSlot;
    }

private:
    std::queue<uint32_t>                       m_freeBoneSlots;
    std::unordered_map<entt::entity, uint32_t> m_entityBoneSlot;
    uint32_t m_nextSlot  = 0;
    uint32_t m_slotsUsed = 0;
};

} // namespace glory
