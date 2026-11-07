// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/Common.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Luau
{
// `VecDeque` is a general double-ended implementation designed as a drop-in replacement for the
// standard library `std::deque`. It's backed by a growable ring buffer, rather than using the
// segmented queue design of `std::deque` which can degrade into a linked list in the worst case.
// The motivation for `VecDeque` as a replacement is to maintain the asymptotic complexity of
// `std::deque` while reducing overall allocations and promoting better usage of the cache. Its API
// is intended to be compatible with `std::deque` and `std::vector` as appropriate, and as such
// provides corresponding method definitions and supports the use of custom allocators.
//
// `VecDeque` offers pushing and popping from both ends with an amortized O(1) complexity. It also
// supports `std::vector`-style random-access in O(1). The implementation of buffer resizing uses
// a growth factor of 1.5x to enable better memory reuse after resizing, and reduce overall memory
// fragmentation when using the queue.
//
// Since `VecDeque` is a ring buffer, its elements are not necessarily contiguous in memory. To
// describe this, we refer to the two portions of the buffer as the `head` and the `tail`. The
// `head` is the initial portion of the queue that is on the range `[head, capacity)` and the tail
// is the (optionally) remaining portion on the range `[0, head + size - capacity)` whenever the
// `head + size` exceeds the capacity of the buffer.
//
// `VecDeque` does not currently support iteration since its primary focus is on providing
// double-ended queue functionality specifically, but it can be reasonably expanded to provide
// an iterator if we have a use-case for one in the future.
template<typename T, class Allocator = std::allocator<T>>
class VecDeque : Allocator
{
private:
    static_assert(std::is_nothrow_move_constructible_v<T>);
    static_assert(std::is_nothrow_move_assignable_v<T>);

    T* buffer = nullptr;        // the existing allocation we have backing this queue
    size_t buffer_capacity = 0; // the size of our allocation

    size_t head = 0;       // the index of the head of the queue
    size_t queue_size = 0; // the size of the queue

    void destroyElements() noexcept
    {
        size_t head_size =
            std::min(queue_size, capacity() - head); // how many elements are in the head portion (i.e. from the head to the end of the buffer)
        size_t tail_size = queue_size - head_size;   // how many elements are in the tail portion (i.e. any portion that wrapped to the front)

        // we have to destroy every element in the head portion
        for (size_t index = head; index < head + head_size; index++)
            buffer[index].~T();

        // and any in the tail portion, if one exists
        for (size_t index = 0; index < tail_size; index++)
            buffer[index].~T();
    }

    bool is_full()
    {
        return queue_size == capacity();
    }

    void grow()
    {
        size_t old_capacity = capacity();

        // we use a growth factor of 1.5x (plus a constant) here in order to enable the
        // previous memory to be reused after a certain number of calls to grow.
        // see: https://github.com/facebook/folly/blob/main/folly/docs/FBVector.md#memory-handling
        size_t new_capacity = (old_capacity > 0) ? old_capacity * 3 / 2 + 1 : 4;

        // check that it's a legal allocation
        if (new_capacity > max_size())
            throw std::bad_array_new_length();

        // allocate a new backing buffer
        T* new_buffer = this->allocate(new_capacity);

        // we should not be growing if the capacity is not the current size
        LUAU_ASSERT(old_capacity == queue_size);

        size_t head_size =
            std::min(queue_size, old_capacity - head); // how many elements are in the head portion (i.e. from the head to the end of the buffer)
        size_t tail_size = queue_size - head_size;     // how many elements are in the tail portion (i.e. any portion that wrapped to the front)

        // move the head into the new buffer
        if (head_size != 0)
            std::uninitialized_move(buffer + head, buffer + head + head_size, new_buffer);

        // move the tail into the new buffer immediately after
        if (tail_size != 0)
            std::uninitialized_move(buffer, buffer + tail_size, new_buffer + head_size);

        // destroy the old elements
        destroyElements();
        // deallocate the old buffer
        this->deallocate(buffer, old_capacity);

        // set up the queue to be backed by the new buffer
        buffer = new_buffer;
        buffer_capacity = new_capacity;
        head = 0;
    }

    size_t logicalToPhysical(size_t pos)
    {
        return (head + pos) % capacity();
    }

public:
    VecDeque() = default;

    explicit VecDeque(const Allocator& alloc) noexcept
        : Allocator{alloc}
    {
    }

    VecDeque(const VecDeque& other)
        : buffer(this->allocate(other.buffer_capacity))
        , buffer_capacity(other.buffer_capacity)
        , head(other.head)
        , queue_size(other.queue_size)
    {
        // copy the initialized contents of the other buffer to this one
        size_t head_size = std::min(
            other.queue_size,
            other.buffer_capacity - other.head
        );                                               // how many elements are in the head portion (i.e. from the head to the end of the buffer)
        size_t tail_size = other.queue_size - head_size; // how many elements are in the tail portion (i.e. any portion that wrapped to the front)

        if (head_size != 0)
            std::uninitialized_copy(other.buffer + other.head, other.buffer + other.head + head_size, buffer + head);

        if (tail_size != 0)
            std::uninitialized_copy(other.buffer, other.buffer + tail_size, buffer);
    }

    VecDeque(const VecDeque& other, const Allocator& alloc)
        : Allocator{alloc}
        , buffer(this->allocate(other.buffer_capacity))
        , buffer_capacity(other.buffer_capacity)
        , head(other.head)
        , queue_size(other.queue_size)
    {
        // copy the initialized contents of the other buffer to this one
        size_t head_size = std::min(
            other.queue_size,
            other.buffer_capacity - other.head
        );                                               // how many elements are in the head portion (i.e. from the head to the end of the buffer)
        size_t tail_size = other.queue_size - head_size; // how many elements are in the tail portion (i.e. any portion that wrapped to the front)

        if (head_size != 0)
            std::uninitialized_copy(other.buffer + other.head, other.buffer + other.head + head_size, buffer + head);

        if (tail_size != 0)
            std::uninitialized_copy(other.buffer, other.buffer + tail_size, buffer);
    }

    VecDeque(VecDeque&& other) noexcept
        : buffer(std::exchange(other.buffer, nullptr))
        , buffer_capacity(std::exchange(other.buffer_capacity, 0))
        , head(std::exchange(other.head, 0))
        , queue_size(std::exchange(other.queue_size, 0))
    {
    }

    VecDeque(VecDeque&& other, const Allocator& alloc) noexcept
        : Allocator{alloc}
        , buffer(std::exchange(other.buffer, nullptr))
        , buffer_capacity(std::exchange(other.buffer_capacity, 0))
        , head(std::exchange(other.head, 0))
        , queue_size(std::exchange(other.queue_size, 0))
    {
    }

    VecDeque(std::initializer_list<T> init, const Allocator& alloc = Allocator())
        : Allocator{alloc}
    {
        buffer = this->allocate(init.size());
        buffer_capacity = init.size();
        queue_size = init.size();

        std::uninitialized_copy(init.begin(), init.end(), buffer);
    }

    ~VecDeque() noexcept
    {
        // destroy any elements that exist
        destroyElements();
        // free the allocated buffer
        this->deallocate(buffer, buffer_capacity);
    }

    VecDeque& operator=(const VecDeque& other)
    {
        if (this == &other)
            return *this;

        // destroy all of the existing elements
        destroyElements();

        if (buffer_capacity < other.size())
        {
            // free the current buffer
            this->deallocate(buffer, buffer_capacity);

            buffer = this->allocate(other.buffer_capacity);
            buffer_capacity = other.buffer_capacity;
        }

        size_t head_size = std::min(
            other.queue_size,
            other.buffer_capacity - other.head
        );                                               // how many elements are in the head portion (i.e. from the head to the end of the buffer)
        size_t tail_size = other.queue_size - head_size; // how many elements are in the tail portion (i.e. any portion that wrapped to the front)

        // Assignment doesn't try to match the capacity of 'other' and thus makes the buffer contiguous
        head = 0;
        queue_size = other.queue_size;

        if (head_size != 0)
            std::uninitialized_copy(other.buffer + other.head, other.buffer + other.head + head_size, buffer);

        if (tail_size != 0)
            std::uninitialized_copy(other.buffer, other.buffer + tail_size, buffer + head_size);

        return *this;
    }

    VecDeque& operator=(VecDeque&& other)
    {
        if (this == &other)
            return *this;

        // destroy all of the existing elements
        destroyElements();
        // free the current buffer
        this->deallocate(buffer, buffer_capacity);

        buffer = std::exchange(other.buffer, nullptr);
        buffer_capacity = std::exchange(other.buffer_capacity, 0);
        head = std::exchange(other.head, 0);
        queue_size = std::exchange(other.queue_size, 0);

        return *this;
    }

    Allocator get_allocator() const noexcept
    {
        return this;
    }

    // element access

    T& at(size_t pos)
    {
        if (pos >= queue_size)
            throw std::out_of_range("VecDeque");

        return buffer[logicalToPhysical(pos)];
    }

    const T& at(size_t pos) const
    {
        if (pos >= queue_size)
            throw std::out_of_range("VecDeque");

        return buffer[logicalToPhysical(pos)];
    }

    [[nodiscard]] T& operator[](size_t pos) noexcept
    {
        LUAU_ASSERT(pos < queue_size);

        return buffer[logicalToPhysical(pos)];
    }

    [[nodiscard]] const T& operator[](size_t pos) const noexcept
    {
        LUAU_ASSERT(pos < queue_size);

        return buffer[logicalToPhysical(pos)];
    }

    T& front()
    {
        LUAU_ASSERT(!empty());

        return buffer[head];
    }

    const T& front() const
    {
        LUAU_ASSERT(!empty());

        return buffer[head];
    }

    T& back()
    {
        LUAU_ASSERT(!empty());

        size_t back = logicalToPhysical(queue_size - 1);
        return buffer[back];
    }

    const T& back() const
    {
        LUAU_ASSERT(!empty());

        size_t back = logicalToPhysical(queue_size - 1);
        return buffer[back];
    }

    // capacity

    bool empty() const noexcept
    {
        return queue_size == 0;
    }

    size_t size() const noexcept
    {
        return queue_size;
    }

    size_t max_size() const noexcept
    {
        return std::numeric_limits<size_t>::max() / sizeof(T);
    }

    void reserve(size_t new_capacity)
    {
        // error if this allocation would be illegal
        if (new_capacity > max_size())
            throw std::length_error("too large");

        size_t old_capacity = capacity();

        // do nothing if we're requesting a capacity that would not cause growth
        if (new_capacity <= old_capacity)
            return;

        size_t head_size =
            std::min(queue_size, old_capacity - head); // how many elements are in the head portion (i.e. from the head to the end of the buffer)
        size_t tail_size = queue_size - head_size;     // how many elements are in the tail portion (i.e. any portion that wrapped to the front)

        // allocate a new backing buffer
        T* new_buffer = this->allocate(new_capacity);

        // move the head into the new buffer
        if (head_size != 0)
            std::uninitialized_move(buffer + head, buffer + head + head_size, new_buffer);

        // move the tail into the new buffer immediately after
        if (tail_size != 0)
            std::uninitialized_move(buffer, buffer + tail_size, new_buffer + head_size);

        // destroy all the existing elements before freeing the old buffer
        destroyElements();

        // deallocate the old buffer
        this->deallocate(buffer, old_capacity);

        // set up the queue to be backed by the new buffer
        buffer = new_buffer;
        buffer_capacity = new_capacity;
        head = 0;
    }

    size_t capacity() const noexcept
    {
        return buffer_capacity;
    }

    void shrink_to_fit()
    {
        size_t old_capacity = capacity();
        size_t new_capacity = queue_size;

        if (old_capacity == new_capacity)
            return;

        size_t head_size =
            std::min(queue_size, old_capacity - head); // how many elements are in the head portion (i.e. from the head to the end of the buffer)
        size_t tail_size = queue_size - head_size;     // how many elements are in the tail portion (i.e. any portion that wrapped to the front)

        // allocate a new backing buffer
        T* new_buffer = this->allocate(new_capacity);

        // move the head into the new buffer
        if (head_size != 0)
            std::uninitialized_move(buffer + head, buffer + head + head_size, new_buffer);

        // move the tail into the new buffer immediately after, if we have one
        if (tail_size != 0)
            std::uninitialized_move(buffer, buffer + tail_size, new_buffer + head_size);

        // destroy all the existing elements before freeing the old buffer
        destroyElements();
        // deallocate the old buffer
        this->deallocate(buffer, old_capacity);

        // set up the queue to be backed by the new buffer
        buffer = new_buffer;
        buffer_capacity = new_capacity;
        head = 0;
    }

    [[nodiscard]] bool is_contiguous() const noexcept
    {
        // this is an overflow-safe alternative to writing `head + size <= capacity`.
        return head <= capacity() - queue_size;
    }

    // modifiers

    void clear() noexcept
    {
        destroyElements();

        head = 0;
        queue_size = 0;
    }

    void push_back(const T& value)
    {
        if (is_full())
            grow();

        size_t next_back = logicalToPhysical(queue_size);
        new (buffer + next_back) T(value);
        queue_size++;
    }

    template<typename... Args>
    T& emplace_back(Args&&... args)
    {
        if (is_full())
            grow();

        size_t next_back = logicalToPhysical(queue_size);
        new (buffer + next_back) T(std::forward<Args>(args)...);
        queue_size++;
        return buffer[next_back];
    }


    void pop_back()
    {
        LUAU_ASSERT(!empty());

        queue_size--;
        size_t next_back = logicalToPhysical(queue_size);
        buffer[next_back].~T();
    }

    void push_front(const T& value)
    {
        if (is_full())
            grow();

        head = (head == 0) ? capacity() - 1 : head - 1;
        new (buffer + head) T(value);
        queue_size++;
    }

    template<typename... Args>
    T& emplace_front(Args&&... args)
    {
        if (is_full())
            grow();

        head = (head == 0) ? capacity() - 1 : head - 1;
        new (buffer + head) T(std::forward<Args>(args)...);
        queue_size++;
        return buffer[head];
    }

    void pop_front()
    {
        LUAU_ASSERT(!empty());

        buffer[head].~T();
        head++;
        queue_size--;

        if (head == capacity())
            head = 0;
    }
};

} // namespace Luau
