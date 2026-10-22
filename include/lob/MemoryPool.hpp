#pragma once
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#if defined(__SANITIZE_ADDRESS__)
#define LOB_HAS_ASAN 1
#include <sanitizer/asan_interface.h>
#define LOB_ASAN_POISON(addr, size) ASAN_POISON_MEMORY_REGION((addr), (size))
#define LOB_ASAN_UNPOISON(addr, size) ASAN_UNPOISON_MEMORY_REGION((addr), (size))
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LOB_HAS_ASAN 1
#include <sanitizer/asan_interface.h>
#define LOB_ASAN_POISON(addr, size) ASAN_POISON_MEMORY_REGION((addr), (size))
#define LOB_ASAN_UNPOISON(addr, size) ASAN_UNPOISON_MEMORY_REGION((addr), (size))
#else
#define LOB_ASAN_POISON(addr, size) ((void)(addr), (void)(size))
#define LOB_ASAN_UNPOISON(addr, size) ((void)(addr), (void)(size))
#endif
#else
#define LOB_ASAN_POISON(addr, size) ((void)(addr), (void)(size))
#define LOB_ASAN_UNPOISON(addr, size) ((void)(addr), (void)(size))
#endif

/// @brief Fixed-capacity intrusive slab memory pool.
/// @note ~pool() frees the backing slab without calling ~T() on remaining live objects.
/// @note If T(...) throws inside my_alloc(), that slot is not returned to the free list.
template <typename T, size_t capacity>
class pool {
private:
    struct Freenode {
        Freenode* next;
    };

    // 1. Compile-time invariants
    static_assert(capacity > 0, "Pool capacity must be greater than 0");
    static_assert(sizeof(T) >= sizeof(void*), "sizeof(T) must be >= sizeof(void*) to store Freenode*");

    // 2. Align slab to the stricter of T's alignment or pointer alignment
    static constexpr size_t Alignment = std::max(alignof(T), alignof(void*));

    std::byte* slab;
    Freenode* head;
    size_t live_count_ = 0;

    // Poison bytes after the first 8 bytes (which hold Freenode::next)
    static void poison_free_slot(void* slot) noexcept {
        if constexpr (sizeof(T) > sizeof(Freenode)) {
            auto* payload = static_cast<unsigned char*>(slot) + sizeof(Freenode);
            constexpr size_t payload_bytes = sizeof(T) - sizeof(Freenode);
#ifndef NDEBUG
            std::memset(payload, 0xEF, payload_bytes);
#endif
            LOB_ASAN_POISON(payload, payload_bytes);
        }
    }

public:
    static constexpr uint32_t INVALID_INDEX = UINT32_MAX;

    explicit pool() {
        slab = new (std::align_val_t{Alignment}) std::byte[capacity * sizeof(T)];
        head = reinterpret_cast<Freenode*>(slab);

        Freenode* current = head;
        for (size_t i = 1; i < capacity; i++) {
            Freenode* next_slot = reinterpret_cast<Freenode*>(slab + (i * sizeof(T)));
            current->next = next_slot;
            poison_free_slot(current);
            current = next_slot;
        }
        current->next = nullptr;
        poison_free_slot(current);
    }

    ~pool() {
        // Unpoison before returning memory to the OS allocator so ASan stays clean
        LOB_ASAN_UNPOISON(slab, capacity * sizeof(T));
        ::operator delete[](slab, std::align_val_t{Alignment});
    }

    // 3. Delete copy and move to prevent double-free of `slab`
    pool(const pool&) = delete;
    pool& operator=(const pool&) = delete;
    pool(pool&&) = delete;
    pool& operator=(pool&&) = delete;

    template <typename... Args>
    T* my_alloc(Args&&... args) {
        if (!head) {
            return nullptr;
        }
        Freenode* node = head;
        head = head->next;
        ++live_count_;

        // Unpoison the full T slot before placement-new writes to it
        LOB_ASAN_UNPOISON(node, sizeof(T));
        return new (node) T(std::forward<Args>(args)...);
    }

    void my_free(T* ptr) noexcept {
        assert(ptr != nullptr && "Cannot free nullptr");
        auto* byte_ptr = reinterpret_cast<std::byte*>(ptr);
        assert(byte_ptr >= slab && byte_ptr < slab + (capacity * sizeof(T)) && "Pointer out of slab bounds");
        assert(static_cast<size_t>(byte_ptr - slab) % sizeof(T) == 0 && "Pointer misaligned to slot boundary");
        (void)byte_ptr;

        if constexpr (!std::is_trivially_destructible_v<T>) {
            ptr->~T();
        }

        Freenode* node = reinterpret_cast<Freenode*>(ptr);
        node->next = head;
        head = node;
        assert(live_count_ > 0 && "Pool live_count underflow");
        --live_count_;

        poison_free_slot(node);
    }

    // 4. Convert a T* pointer into a 32-bit slot index (0 .. capacity - 1)
    uint32_t index_of(const T* ptr) const noexcept {
        assert(ptr != nullptr && "Cannot take index_of nullptr");
        const auto* byte_ptr = reinterpret_cast<const std::byte*>(ptr);
        assert(byte_ptr >= slab && byte_ptr < slab + (capacity * sizeof(T)) && "Pointer out of slab bounds");
        assert(static_cast<size_t>(byte_ptr - slab) % sizeof(T) == 0 && "Pointer misaligned to slot boundary");

        return static_cast<uint32_t>((byte_ptr - slab) / sizeof(T));
    }

    // 5. Convert a 32-bit slot index back into a T* pointer in O(1)
    T* at(uint32_t index) noexcept {
        assert(index < capacity && "Slot index out of bounds");
        return reinterpret_cast<T*>(slab + (static_cast<size_t>(index) * sizeof(T)));
    }

    const T* at(uint32_t index) const noexcept {
        assert(index < capacity && "Slot index out of bounds");
        return reinterpret_cast<const T*>(slab + (static_cast<size_t>(index) * sizeof(T)));
    }

    size_t live_count() const noexcept { return live_count_; }
};