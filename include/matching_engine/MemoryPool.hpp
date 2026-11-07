#pragma once

#include <vector>
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <cstdlib>   // for std::malloc / std::free
#include "Types.hpp"

 // MEMORY POOL AND 64-BYTE ALIGNMENT:
 // 1. Every Order has a fixed size (currently 64 bytes), because its fields
 //    have fixed types. alignas(64) ensures sizeof(Order) is a multiple of 64.
 //    If Order grows, sizeof(Order) may become 128, 192, etc.
 //
 // 2. We call malloc() ONCE for the entire pool, not once per Order.
 //    For 100 orders of 64 bytes each, we need 100 * 64 = 6400 bytes.
 //    Extra bytes are reserved for alignment and saving the raw pointer.
 //
 // 3. malloc() can return an address that is not divisible by 64.
 //    We move forward to the next 64-byte-aligned address.
 //
 // 4. IMPORTANT: We first skip sizeof(void*) = 8 bytes, then align.
 //    This guarantees space BEFORE the aligned address to store the
 //    original malloc pointer, even if malloc already returned an
 //    address divisible by 64.
 //
 //    Example A: malloc returns 1003
 //      1003 + 8 = 1011 -> align to 1024
 //      Store original pointer 1003 at addresses 1016-1023.
 //      Order 1 starts at 1024, Order 2 at 1088.
 //
 //    Example B: malloc returns 1024 (already aligned)
 //      1024 + 8 = 1032 -> align to 1088
 //      Store original pointer 1024 at addresses 1080-1087.
 //      Order 1 starts at 1088, Order 2 at 1152.
 //
 // 5. Only ONE original malloc pointer is stored for the ENTIRE pool.
 //    We save it because free() must receive the original malloc address,
 //    not the shifted/aligned address.
 //
 // 6. Each Order's address = alignedStart + index * sizeof(Order).
 //    Since sizeof(Order) is a multiple of 64, every Order starts at
 //    a 64-byte boundary. No orders overlap.
 //
 // 7. A 64-byte-aligned object is NOT necessarily 64 bytes in size.
 //    Alignment controls its starting address; sizeof controls its size.
 //
 // 8. Memory pooling avoids repeated malloc/free calls for each Order.
 //    Alignment can improve CPU cache efficiency (cache lines are often
 //    64 bytes), but it does not guarantee faster execution.

namespace matching_engine {

// Portable, self-contained 64-byte cache-aligned memory allocator
// we're specifically asking for 64-byte alignment.
// addresses are like say 1000, 1001, 1002.. 
// but we want addresses as multiples of 64 like 1024, 1088, 1152, etc.
// That means every Order must start at an address divisible by 64.

// Because CPUs access RAM through a faster small memory called CPU cache.
// A cache line is a block of 
// memory transferred into the CPU cache together. On many modern CPUs, a cache line is 64 bytes.
// so lets say order is 49 bytes, if we start it as a multiple of 64, 
// then it will fit in one cache line. 
// If we start it at say 1001, then it will span across two cache lines, 
// which is inefficient. So we align it to 64 bytes.
// altough the Order struct is 49 bytes, but because of alignas(64),
// sizeof(Order) will be 64 bytes. If it was 69 bytes, then sizeof(Order) would be 128 bytes.
// also it still may not guaranteed that order will be filled in one cache line, 
// because the order may be more than 64 bytes
inline void* allocateAligned64(size_t size) {
    const size_t alignment = 64;
    size_t total = size + alignment + sizeof(void*);
    // raw is the original assigned pointer returned by malloc, w
    // hich we will store just before the aligned address
    // say its 1000 but it will not work as it is so we calculate the 
    // next multiple of 64 (1024) and store the original pointer (1000) just before it (1024-8=1016)
    void* raw = std::malloc(total);
    if (!raw) return NULL;

    // Calculate aligned address after reserving space to store the raw pointer
    uintptr_t rawAddress = reinterpret_cast<uintptr_t>(raw) + sizeof(void*);
    // clearing lower 6 bits to make it a muliple of 64
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
        // as all the datatypes are already fixed in Order so each order will be of 
        // same size and we can allocate a block of memory for all the orders

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
    // allocates 0 1 2...
    // say we allocated 0 1 2 then we deallocate 1, then we allocate again, 
    // it will be allocated at 1 as we updated the freeHead and freeList accordingly
    
    T* allocate(OrderId id, Price price, Quantity qty, Side side, OrderType type, Timestamp ts = 0) {
        if (freeHead_ == 0) {
            return NULL;
        }

        // Take the next free slot
        const size_t index = freeList_[--freeHead_];

        // Calculate the memory address of that slot
        T* ptr = reinterpret_cast<T*>(storage_ + index * sizeof(T));

        // Construct the Order directly at that address (no malloc)
        new (ptr) T(id, price, qty, side, type, ts);

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
