#pragma once
/// Fixed-size block pool allocator.
///
/// Pre-allocates N blocks of `BlockSize` bytes.  alloc() pops from a free-list
/// in O(1).  free() pushes back in O(1).  No fragmentation because every block
/// is the same size.
///
/// Use for high-churn, fixed-size objects: particles, collision pairs, VFX
/// event queue overflow entries, etc.
///
/// Thread-safety: NOT thread-safe.  Wrap in a mutex or use per-thread pools.

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <new>
#include <type_traits>

namespace glory {

template<size_t BlockSize, size_t MaxBlocks = 4096>
class PoolAllocator {
    static_assert(BlockSize >= sizeof(void*), "BlockSize must be >= sizeof(void*)");

public:
    PoolAllocator() {
        // Ensure each block is large enough for the free-list pointer and respects
        // max_align_t alignment.
        constexpr size_t actualBlock = alignUp(BlockSize, alignof(std::max_align_t));
        m_blockSize = actualBlock;
        m_storage = static_cast<uint8_t*>(
            ::operator new(actualBlock * MaxBlocks, std::align_val_t{alignof(std::max_align_t)}));

        // Build intrusive free-list (each block's first sizeof(void*) bytes → next free)
        for (size_t i = 0; i < MaxBlocks - 1; ++i) {
            auto* block = m_storage + i * actualBlock;
            *reinterpret_cast<void**>(block) = m_storage + (i + 1) * actualBlock;
        }
        // Last block points to nullptr
        *reinterpret_cast<void**>(m_storage + (MaxBlocks - 1) * actualBlock) = nullptr;

        m_freeHead = m_storage;
        m_freeCount = MaxBlocks;
    }

    ~PoolAllocator() {
        if (m_storage)
            ::operator delete(m_storage, std::align_val_t{alignof(std::max_align_t)});
    }

    // Non-copyable, movable
    PoolAllocator(const PoolAllocator&)            = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;
    PoolAllocator(PoolAllocator&& o) noexcept
        : m_storage(o.m_storage), m_freeHead(o.m_freeHead),
          m_blockSize(o.m_blockSize), m_freeCount(o.m_freeCount)
    {
        o.m_storage = nullptr;
        o.m_freeHead = nullptr;
        o.m_freeCount = 0;
    }
    PoolAllocator& operator=(PoolAllocator&& o) noexcept {
        if (this != &o) {
            if (m_storage)
                ::operator delete(m_storage, std::align_val_t{alignof(std::max_align_t)});
            m_storage   = o.m_storage;
            m_freeHead  = o.m_freeHead;
            m_blockSize = o.m_blockSize;
            m_freeCount = o.m_freeCount;
            o.m_storage = nullptr;
            o.m_freeHead = nullptr;
            o.m_freeCount = 0;
        }
        return *this;
    }

    /// Allocate one block.  Returns nullptr if the pool is exhausted.
    void* alloc() {
        if (!m_freeHead) return nullptr;
        void* block = m_freeHead;
        m_freeHead = *reinterpret_cast<void**>(m_freeHead);
        --m_freeCount;
        return block;
    }

    /// Typed allocation: allocate one T (must fit in BlockSize).
    template<typename T, typename... Args>
    T* construct(Args&&... args) {
        static_assert(sizeof(T) <= BlockSize, "T doesn't fit in this pool's block size");
        void* mem = alloc();
        if (!mem) return nullptr;
        return new (mem) T(std::forward<Args>(args)...);
    }

    /// Return a block to the pool.
    void free(void* block) {
        if (!block) return;
        assert(owns(block) && "Block does not belong to this pool");
        *reinterpret_cast<void**>(block) = m_freeHead;
        m_freeHead = block;
        ++m_freeCount;
    }

    /// Typed deallocation: destruct + free.
    template<typename T>
    void destroy(T* obj) {
        if (!obj) return;
        obj->~T();
        free(obj);
    }

    /// Reset the entire pool (all blocks returned to free-list).  O(N).
    void resetAll() {
        constexpr size_t actualBlock = alignUp(BlockSize, alignof(std::max_align_t));
        for (size_t i = 0; i < MaxBlocks - 1; ++i) {
            auto* block = m_storage + i * actualBlock;
            *reinterpret_cast<void**>(block) = m_storage + (i + 1) * actualBlock;
        }
        *reinterpret_cast<void**>(m_storage + (MaxBlocks - 1) * actualBlock) = nullptr;
        m_freeHead = m_storage;
        m_freeCount = MaxBlocks;
    }

    bool owns(const void* ptr) const {
        auto* p = static_cast<const uint8_t*>(ptr);
        return p >= m_storage && p < m_storage + m_blockSize * MaxBlocks;
    }

    size_t freeCount()     const { return m_freeCount; }
    size_t allocatedCount() const { return MaxBlocks - m_freeCount; }
    size_t maxBlocks()     const { return MaxBlocks; }
    size_t blockSize()     const { return m_blockSize; }

private:
    uint8_t* m_storage   = nullptr;
    void*    m_freeHead  = nullptr;
    size_t   m_blockSize = 0;
    size_t   m_freeCount = 0;

    static constexpr size_t alignUp(size_t v, size_t align) {
        return (v + align - 1) & ~(align - 1);
    }
};

// ── Common pool type aliases ────────────────────────────────────────────────

/// Pool for collision pairs: typically 16 bytes (2 × entity ID + flags)
using CollisionPairPool = PoolAllocator<16, 8192>;

/// Pool for small fixed objects (64 bytes)
using SmallObjectPool   = PoolAllocator<64, 4096>;

/// Pool for medium objects (256 bytes, e.g. per-entity scratch data)
using MediumObjectPool  = PoolAllocator<256, 2048>;

} // namespace glory
