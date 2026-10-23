#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace matching_engine {

// Allocate memory on a 64-byte boundary (or a stricter boundary requested by T).
// The original pointer is saved immediately before the aligned address so it can
// be passed back to std::free later.
inline void* allocateAlignedMemory(size_t size, size_t alignment) {
    if (size == 0) {
        return nullptr;
    }

    const size_t pointerSize = sizeof(void*);
    if (alignment < pointerSize) {
        alignment = pointerSize;
    }

    if (size > std::numeric_limits<size_t>::max() - alignment - pointerSize) {
        return nullptr;
    }

    const size_t totalSize = size + alignment - 1 + pointerSize;
    void* rawMemory = std::malloc(totalSize);
    if (rawMemory == nullptr) {
        return nullptr;
    }

    const std::uintptr_t rawAddress =
        reinterpret_cast<std::uintptr_t>(rawMemory) + pointerSize;
    const std::uintptr_t alignedAddress =
        (rawAddress + alignment - 1) & ~static_cast<std::uintptr_t>(alignment - 1);

    std::memcpy(reinterpret_cast<void*>(alignedAddress - pointerSize),
                &rawMemory, sizeof(rawMemory));
    return reinterpret_cast<void*>(alignedAddress);
}

inline void freeAlignedMemory(void* alignedMemory) {
    if (alignedMemory == nullptr) {
        return;
    }

    void* rawMemory = nullptr;
    std::memcpy(&rawMemory,
                reinterpret_cast<const char*>(alignedMemory) - sizeof(void*),
                sizeof(rawMemory));
    std::free(rawMemory);
}

/**
 * A fixed-capacity object pool.
 *
 * All object storage and free-slot bookkeeping is allocated when the pool is
 * constructed. allocate() and deallocate() then reuse slots in constant time.
 */
template <typename T>
class MemoryPool {
public:
    explicit MemoryPool(size_t capacity)
        : storage_(nullptr),
          capacity_(capacity),
          freeHead_(capacity),
          freeSlots_(capacity),
          slotInUse_(capacity, 0) {
        if (capacity_ > std::numeric_limits<size_t>::max() / sizeof(T)) {
            throw std::bad_alloc();
        }

        if (capacity_ == 0) {
            return;
        }

        const size_t alignment = alignof(T) > 64 ? alignof(T) : 64;
        storage_ = static_cast<char*>(
            allocateAlignedMemory(capacity_ * sizeof(T), alignment));
        if (storage_ == nullptr) {
            throw std::bad_alloc();
        }

        for (size_t i = 0; i < capacity_; ++i) {
            freeSlots_[i] = capacity_ - 1 - i;
        }
    }

    ~MemoryPool() {
        reset();
        freeAlignedMemory(storage_);
    }

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;

    template <typename... Args>
    T* allocate(Args&&... args) {
        if (freeHead_ == 0) {
            return nullptr;
        }

        const size_t index = freeSlots_[--freeHead_];
        T* object = reinterpret_cast<T*>(storage_ + index * sizeof(T));
        try {
            new (object) T(std::forward<Args>(args)...);
        } catch (...) {
            freeSlots_[freeHead_++] = index;
            throw;
        }
        slotInUse_[index] = 1;
        return object;
    }

    void deallocate(T* object) {
        if (object == nullptr) {
            return;
        }

        const size_t index = getSlotIndex(object);
        if (index >= capacity_) {
            assert(false && "Pointer does not belong to this MemoryPool");
            return;
        }
        if (slotInUse_[index] == 0) {
            assert(false && "Object was already returned to this MemoryPool");
            return;
        }

        object->~T();
        slotInUse_[index] = 0;
        freeSlots_[freeHead_++] = index;
    }

    bool owns(const T* object) const {
        if (object == nullptr || storage_ == nullptr) {
            return false;
        }

        const std::uintptr_t baseAddress = reinterpret_cast<std::uintptr_t>(storage_);
        const std::uintptr_t objectAddress = reinterpret_cast<std::uintptr_t>(object);
        const std::uintptr_t poolEnd = baseAddress + capacity_ * sizeof(T);
        return objectAddress >= baseAddress && objectAddress < poolEnd &&
               (objectAddress - baseAddress) % sizeof(T) == 0;
    }

    size_t available() const { return freeHead_; }
    size_t capacity() const { return capacity_; }
    size_t allocatedCount() const { return capacity_ - freeHead_; }

    // Destroy any live objects and make every slot available again.
    void reset() {
        if (storage_ != nullptr) {
            for (size_t i = 0; i < capacity_; ++i) {
                if (slotInUse_[i] != 0) {
                    T* object = reinterpret_cast<T*>(storage_ + i * sizeof(T));
                    object->~T();
                    slotInUse_[i] = 0;
                }
            }
        }

        for (size_t i = 0; i < capacity_; ++i) {
            freeSlots_[i] = capacity_ - 1 - i;
        }
        freeHead_ = capacity_;
    }

private:
    size_t getSlotIndex(const T* object) const {
        if (!owns(object)) {
            return capacity_;
        }

        const std::uintptr_t baseAddress = reinterpret_cast<std::uintptr_t>(storage_);
        const std::uintptr_t objectAddress = reinterpret_cast<std::uintptr_t>(object);
        return static_cast<size_t>((objectAddress - baseAddress) / sizeof(T));
    }

    char* storage_;
    size_t capacity_;
    size_t freeHead_;
    std::vector<size_t> freeSlots_;
    std::vector<unsigned char> slotInUse_;
};

} // namespace matching_engine
