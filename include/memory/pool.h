#pragma once
#include <cassert>
#include <cstddef>
#include <memory>
#include <new>

namespace marketlib::memory {

// Fixed-size object pool. O(1) alloc/free, zero heap allocation after init.
// Not thread-safe — give each thread its own pool.
template<typename T, std::size_t N>
class PoolAllocator {
    static_assert(sizeof(T) >= sizeof(void*),
        "T must be at least pointer-sized for the free-list node");

    union Slot {
        alignas(T) std::byte storage[sizeof(T)];
        Slot*                next;
    };

    Slot        pool_[N];
    Slot*       free_head_{nullptr};
    std::size_t available_{N};

public:
    PoolAllocator() noexcept {
        for (std::size_t i = 0; i < N - 1; ++i)
            pool_[i].next = &pool_[i + 1];
        pool_[N - 1].next = nullptr;
        free_head_        = &pool_[0];
    }

    PoolAllocator(const PoolAllocator&)            = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    [[nodiscard]] T* allocate() noexcept {
        if (!free_head_) [[unlikely]] return nullptr;
        Slot* slot = free_head_;
        free_head_ = slot->next;
        --available_;
        return reinterpret_cast<T*>(slot->storage);
    }

    void deallocate(T* ptr) noexcept {
        std::destroy_at(ptr);
        auto* slot = reinterpret_cast<Slot*>(ptr);
        slot->next = free_head_;
        free_head_ = slot;
        ++available_;
    }

    template<typename... Args>
    [[nodiscard]] T* construct(Args&&... args)
        noexcept(std::is_nothrow_constructible_v<T, Args...>)
    {
        T* ptr = allocate();
        if (!ptr) [[unlikely]] return nullptr;
        return std::construct_at(ptr, std::forward<Args>(args)...);
    }

    [[nodiscard]] std::size_t    available() const noexcept { return available_; }
    [[nodiscard]] bool           full()      const noexcept { return available_ == 0; }
    static constexpr std::size_t capacity()        noexcept { return N; }
};

// Bump-pointer arena. Allocate-only; reset at tick/message boundary.
template<std::size_t Capacity>
class ArenaAllocator {
    alignas(std::max_align_t) std::byte buf_[Capacity];
    std::size_t offset_{0};

public:
    [[nodiscard]] void* allocate(std::size_t size,
                                 std::size_t align = alignof(std::max_align_t)) noexcept
    {
        const std::size_t aligned = (offset_ + align - 1) & ~(align - 1);
        if (aligned + size > Capacity) [[unlikely]] return nullptr;
        offset_ = aligned + size;
        return buf_ + aligned;
    }

    template<typename T, typename... Args>
    [[nodiscard]] T* construct(Args&&... args)
        noexcept(std::is_nothrow_constructible_v<T, Args...>)
    {
        void* p = allocate(sizeof(T), alignof(T));
        if (!p) [[unlikely]] return nullptr;
        return std::construct_at(static_cast<T*>(p), std::forward<Args>(args)...);
    }

    void reset() noexcept { offset_ = 0; }

    [[nodiscard]] std::size_t    used()      const noexcept { return offset_; }
    [[nodiscard]] std::size_t    remaining() const noexcept { return Capacity - offset_; }
    static constexpr std::size_t capacity()        noexcept { return Capacity; }
};

} // namespace marketlib::memory
