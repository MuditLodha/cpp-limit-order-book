#pragma once

#include <vector>
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <cstdlib>   // for std::malloc / std::free
#include <new>       // for placement new
#include <utility>   // for std::forward

namespace matching_engine {

// Portable, self-contained 64-byte cache-aligned memory allocator
// Requires no compiler-specific headers or C++17 dependencies
inline void* allocateAligned64(size_t size) {
    const size_t alignment = 64;
    size_t total = size + alignment + sizeof(void*);
    void* raw = std::malloc(total);
    if (!raw) return NULL;

    // Calculate aligned address after reserving space to store the raw pointer
    uintptr_t rawAddress = reinterpret_cast<uintptr_t>(raw) + sizeof(void*);
    uintptr_t alignedAddress = (rawAddress + (alignment - 1)) & ~(static_cast<uintptr_t>(alignment - 1));

    // Store the original unaligned pointer right before the aligned address
    *(reinterpret_cast<void**>(alignedAddress) - 1) = raw;
    return reinterpret_cast<void*>(alignedAddress);
}

inline void freeAligned64(void* ptr) {
    if (!ptr) return;
    // Retrieve original raw pointer stored before aligned address
    void* raw = *(reinterpret_cast<void**>(ptr) - 1);
    std::free(raw);
}

/**
 * @brief High-performance, fixed-capacity object pool allocator.
 *
 * Pre-allocates a contiguous memory buffer for objects of type T.
 * Provides O(1) allocation and deallocation without invoking system malloc
 * or free on the hot path, eliminating heap fragmentation, OS locks,
 * and syscall overhead.
 *
 * @tparam T Type of object managed by the pool.
 * @tparam Capacity Maximum number of active objects in the pool.
 */
template <typename T, size_t Capacity>
class MemoryPool {
public:
    MemoryPool() : storage_(NULL), freeHead_(0) {
        // Allocate a contiguous, cache-line (64-byte) aligned block of raw memory
        storage_ = static_cast<char*>(allocateAligned64(Capacity * sizeof(T)));
        assert(storage_ != NULL && "MemoryPool: aligned allocation failed!");

        freeList_.resize(Capacity);
        for (size_t i = 0; i < Capacity; ++i) {
            freeList_[i] = Capacity - 1 - i; // LIFO order for cache warmth
        }
        freeHead_ = Capacity;
    }

    ~MemoryPool() {
        freeAligned64(storage_);
    }

    // Non-copyable, non-movable for safety
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;

    /**
     * @brief Allocate and construct an object in-place in O(1).
     * @return Pointer to constructed object, or NULL if pool exhausted.
     */
    template <typename... Args>
    T* allocate(Args&&... args) {
        if (freeHead_ == 0) {
            return NULL; // Pool exhausted
        }

        const size_t index = freeList_[--freeHead_];
        T* ptr = reinterpret_cast<T*>(storage_ + index * sizeof(T));
        new (ptr) T(std::forward<Args>(args)...);  // Placement new: construct in-place
        return ptr;
    }

    /**
     * @brief Destroy object and return its slot to the free list in O(1).
     */
    void deallocate(T* ptr) {
        if (ptr == NULL) return;

        assert(owns(ptr) && "Pointer does not belong to this MemoryPool!");

        ptr->~T();  // Explicitly call destructor
        const size_t index = (reinterpret_cast<const char*>(ptr) - storage_) / sizeof(T);
        freeList_[freeHead_++] = index;
    }

    /**
     * @brief Check whether a pointer belongs to this pool's memory region.
     */
    bool owns(const T* ptr) const {
        const char* bytePtr = reinterpret_cast<const char*>(ptr);
        const char* end     = storage_ + Capacity * sizeof(T);
        return (bytePtr >= storage_ && bytePtr < end);
    }

    size_t available() const { return freeHead_; }
    size_t capacity()  const { return Capacity; }
    size_t allocatedCount() const { return Capacity - freeHead_; }

    /**
     * @brief Reset the entire pool, marking all slots as free.
     */
    void reset() {
        freeHead_ = Capacity;
        for (size_t i = 0; i < Capacity; ++i) {
            freeList_[i] = Capacity - 1 - i;
        }
    }

private:
    char*               storage_;   // Raw 64-byte-aligned memory block
    std::vector<size_t> freeList_;  // Stack of free slot indices
    size_t              freeHead_;  // Top of free-slot stack (number of free slots)
};

} // namespace matching_engine
