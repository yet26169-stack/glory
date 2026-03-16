#pragma once
/// Linear (bump) allocator that resets every frame.
///
/// Pre-allocates a contiguous block (default 16 MB).  alloc() bumps a pointer
/// forward with alignment padding.  reset() snaps the pointer back to the base
/// in O(1).  No individual free() — the entire arena is reclaimed at once.
///
/// Thread-safety: NOT thread-safe.  Each thread should own its own instance,
/// or callers must synchronise externally.

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <memory>
#include <new>

namespace glory {

class FrameAllocator {
public:
    static constexpr size_t DEFAULT_CAPACITY = 16u * 1024u * 1024u; // 16 MB

    explicit FrameAllocator(size_t capacity = DEFAULT_CAPACITY)
        : m_capacity(capacity)
    {
        m_base = static_cast<uint8_t*>(::operator new(capacity, std::align_val_t{64}));
        m_offset = 0;
    }

    ~FrameAllocator() {
        if (m_base)
            ::operator delete(m_base, std::align_val_t{64});
    }

    // Non-copyable, movable
    FrameAllocator(const FrameAllocator&)            = delete;
    FrameAllocator& operator=(const FrameAllocator&) = delete;
    FrameAllocator(FrameAllocator&& o) noexcept
        : m_base(o.m_base), m_capacity(o.m_capacity), m_offset(o.m_offset),
          m_peakUsage(o.m_peakUsage)
    {
        o.m_base = nullptr;
        o.m_capacity = 0;
        o.m_offset = 0;
    }
    FrameAllocator& operator=(FrameAllocator&& o) noexcept {
        if (this != &o) {
            if (m_base) ::operator delete(m_base, std::align_val_t{64});
            m_base      = o.m_base;
            m_capacity  = o.m_capacity;
            m_offset    = o.m_offset;
            m_peakUsage = o.m_peakUsage;
            o.m_base = nullptr;
            o.m_capacity = 0;
            o.m_offset = 0;
        }
        return *this;
    }

    /// Allocate `size` bytes with `align` alignment.
    /// Returns nullptr if out of space (should never happen with correct sizing).
    void* alloc(size_t size, size_t align = alignof(std::max_align_t)) {
        size_t aligned = alignUp(m_offset, align);
        if (aligned + size > m_capacity) return nullptr;
        void* ptr = m_base + aligned;
        m_offset = aligned + size;
        if (m_offset > m_peakUsage) m_peakUsage = m_offset;
        return ptr;
    }

    /// Typed allocation helper: allocates sizeof(T)*count with alignof(T).
    template<typename T>
    T* alloc(size_t count = 1) {
        return static_cast<T*>(alloc(sizeof(T) * count, alignof(T)));
    }

    /// Reset the allocator.  O(1) — no destructors called.
    /// Call at the beginning of each frame.
    void reset() { m_offset = 0; }

    size_t used()     const { return m_offset; }
    size_t capacity() const { return m_capacity; }
    size_t peak()     const { return m_peakUsage; }

    /// STL-compatible span view over a frame-allocated array.
    template<typename T>
    struct Span {
        T*     data;
        size_t count;
        T*       begin()       { return data; }
        T*       end()         { return data + count; }
        const T* begin() const { return data; }
        const T* end()   const { return data + count; }
        T& operator[](size_t i) { return data[i]; }
        const T& operator[](size_t i) const { return data[i]; }
        size_t size() const { return count; }
        bool empty()  const { return count == 0; }
    };

    /// Allocate and construct `count` default-initialised T objects.
    template<typename T>
    Span<T> allocSpan(size_t count) {
        T* ptr = alloc<T>(count);
        if (!ptr) return { nullptr, 0 };
        for (size_t i = 0; i < count; ++i) new (ptr + i) T{};
        return { ptr, count };
    }

private:
    uint8_t* m_base     = nullptr;
    size_t   m_capacity = 0;
    size_t   m_offset   = 0;
    size_t   m_peakUsage = 0;

    static constexpr size_t alignUp(size_t offset, size_t align) {
        return (offset + align - 1) & ~(align - 1);
    }
};

} // namespace glory
