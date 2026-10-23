#pragma once

#include <cstddef>
#include <cassert>

namespace matching_engine {

/**
 * @brief Intrusive doubly linked list node hook.
 *
 * Types to be stored in an IntrusiveList must inherit from IntrusiveNode<T>
 * or contain an IntrusiveNode hook. By embedding prev/next pointers directly
 * within the item, memory allocation is completely avoided when inserting or
 * removing nodes from queues, and cache locality is maximized.
 */
template <typename T>
struct IntrusiveNode {
    T* prev;
    T* next;

    IntrusiveNode() : prev(nullptr), next(nullptr) {}
};

/**
 * @brief Zero-allocation, cache-friendly intrusive doubly-linked list.
 *
 * Designed for price-time priority queues where orders need:
 *  - O(1) append to tail (new incoming limit order)
 *  - O(1) removal from head (order filled by matching)
 *  - O(1) removal of arbitrary node (order cancellation/modification)
 *  - Zero dynamic heap allocation on insertion or deletion.
 */
template <typename T>
class IntrusiveList {
public:
    IntrusiveList() : head_(nullptr), tail_(nullptr), size_(0) {}

    ~IntrusiveList() {
        head_ = nullptr;
        tail_ = nullptr;
        size_ = 0;
    }

    // Non-copyable for safety
    IntrusiveList(const IntrusiveList&) = delete;
    IntrusiveList& operator=(const IntrusiveList&) = delete;

    // Movable
    IntrusiveList(IntrusiveList&& other)
        : head_(other.head_), tail_(other.tail_), size_(other.size_) {
        other.head_ = nullptr;
        other.tail_ = nullptr;
        other.size_ = 0;
    }

    IntrusiveList& operator=(IntrusiveList&& other) {
        if (this != &other) {
            clear();
            head_ = other.head_;
            tail_ = other.tail_;
            size_ = other.size_;
            other.head_ = nullptr;
            other.tail_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    bool empty() const {
        return head_ == nullptr;
    }

    size_t size() const {
        return size_;
    }

    T* front() {
        return head_;
    }

    const T* front() const {
        return head_;
    }

    T* back() {
        return tail_;
    }

    const T* back() const {
        return tail_;
    }

    /**
     * @brief Append a node to the tail of the list in O(1).
     */
    void push_back(T* node) {
        assert(node != nullptr);
        assert(node->prev == nullptr && node->next == nullptr);

        node->prev = tail_;
        node->next = nullptr;

        if (tail_ != nullptr) {
            tail_->next = node;
        } else {
            head_ = node;
        }
        tail_ = node;
        ++size_;
    }

    /**
     * @brief Remove an arbitrary node from the list in O(1).
     */
    void remove(T* node) {
        assert(node != nullptr);

        if (node->prev != nullptr) {
            node->prev->next = node->next;
        } else if (head_ == node) {
            head_ = node->next;
        }

        if (node->next != nullptr) {
            node->next->prev = node->prev;
        } else if (tail_ == node) {
            tail_ = node->prev;
        }

        node->prev = nullptr;
        node->next = nullptr;
        --size_;
    }

    /**
     * @brief Remove and return the front node in O(1).
     */
    T* pop_front() {
        if (empty()) {
            return nullptr;
        }
        T* node = head_;
        remove(node);
        return node;
    }

    /**
     * @brief Reset list pointers without freeing nodes (nodes belong to pool).
     */
    void clear() {
        T* curr = head_;
        while (curr != nullptr) {
            T* next = curr->next;
            curr->prev = nullptr;
            curr->next = nullptr;
            curr = next;
        }
        head_ = nullptr;
        tail_ = nullptr;
        size_ = 0;
    }

    // Iterator support for range-based for loops
    class Iterator {
    public:
        explicit Iterator(T* ptr) : current_(ptr) {}

        T& operator*() const { return *current_; }
        T* operator->() const { return current_; }

        Iterator& operator++() {
            if (current_) current_ = current_->next;
            return *this;
        }

        Iterator operator++(int) {
            Iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        bool operator==(const Iterator& other) const { return current_ == other.current_; }
        bool operator!=(const Iterator& other) const { return current_ != other.current_; }

    private:
        T* current_;
    };

    Iterator begin() { return Iterator(head_); }
    Iterator end()   { return Iterator(nullptr); }

private:
    T*     head_;
    T*     tail_;
    size_t size_;
};

} // namespace matching_engine
