#include <gtest/gtest.h>
#include <lob/MemoryPool.hpp>
#include <cstdint>
#include <vector>

namespace {

struct TestSlot {
    uint64_t a;
    uint64_t b;
    uint64_t c;
};

struct alignas(64) CacheAlignedSlot {
    uint64_t x;
    uint64_t y;
};

struct TrackedNonTrivial {
    int* dtor_count;
    uint64_t payload;
    TrackedNonTrivial(int* counter, uint64_t v) : dtor_count(counter), payload(v) {}
    ~TrackedNonTrivial() { ++(*dtor_count); }
};

} // namespace

// 1. LIFO reuse and 32-bit handle round-trip (index_of / at)
TEST(MemoryPoolTest, LifoReuseAndHandles) {
    MemoryPool<TestSlot, 4> p;

    TestSlot* s0 = p.allocate(1ULL, 2ULL, 3ULL);
    TestSlot* s1 = p.allocate(4ULL, 5ULL, 6ULL);
    TestSlot* s2 = p.allocate(7ULL, 8ULL, 9ULL);

    ASSERT_NE(s0, nullptr);
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(p.live_count(), 3u);

    uint32_t idx0 = p.index_of(s0);
    uint32_t idx1 = p.index_of(s1);
    EXPECT_EQ(p.at(idx0), s0);
    EXPECT_EQ(p.at(idx1), s1);

    // Free s1 then s0; LIFO free list must return s0 on the next alloc, then s1
    p.free(s1);
    p.free(s0);
    EXPECT_EQ(p.live_count(), 1u);

    TestSlot* r0 = p.allocate(10ULL, 20ULL, 30ULL);
    TestSlot* r1 = p.allocate(40ULL, 50ULL, 60ULL);
    EXPECT_EQ(r0, s0);
    EXPECT_EQ(r1, s1);
    EXPECT_EQ(p.index_of(r0), idx0);
    EXPECT_EQ(p.index_of(r1), idx1);
}

// 2. Exhaustion returns nullptr and recovers after free
TEST(MemoryPoolTest, ExhaustionReturnsNullptr) {
    MemoryPool<TestSlot, 3> p;

    TestSlot* a = p.allocate(1ULL, 1ULL, 1ULL);
    TestSlot* b = p.allocate(2ULL, 2ULL, 2ULL);
    TestSlot* c = p.allocate(3ULL, 3ULL, 3ULL);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(p.live_count(), 3u);

    // Pool is full
    EXPECT_EQ(p.allocate(4ULL, 4ULL, 4ULL), nullptr);
    EXPECT_EQ(p.live_count(), 3u);

    // Free one slot and verify allocate succeeds again
    p.free(b);
    EXPECT_EQ(p.live_count(), 2u);
    TestSlot* b2 = p.allocate(5ULL, 5ULL, 5ULL);
    EXPECT_EQ(b2, b);
    EXPECT_EQ(b2->a, 5ULL);
}

// 3. Over-aligned types receive properly aligned addresses
TEST(MemoryPoolTest, AlignmentRespected) {
    std::vector<std::unique_ptr<char[]>> jitter;
    std::vector<std::unique_ptr<MemoryPool<CacheAlignedSlot,4>>> pools;
    
    for (size_t round = 0; round < 32; ++round) {
        jitter.push_back(std::make_unique<char[]>((round *13) % 53 + 1));
        auto pool_ptr = std::make_unique<MemoryPool<CacheAlignedSlot,4>>();
        for (size_t i = 0; i < 4; i++)
        {
            
            CacheAlignedSlot* slot = pool_ptr->allocate(i,round);
            ASSERT_NE(slot, nullptr);
            EXPECT_EQ(reinterpret_cast<uintptr_t>(slot) % alignof(CacheAlignedSlot), 0u);
        }
        pools.push_back(std::move(pool_ptr));
    }
}

// 4. Address stability of live objects across interleaved frees and allocations
TEST(MemoryPoolTest, AddressStability) {
    MemoryPool<TestSlot, 8> p;
    std::vector<TestSlot*> ptrs;
    for (uint64_t i = 0; i < 8; ++i) {
        ptrs.push_back(p.allocate(i, i + 100, i + 200));
        ASSERT_NE(ptrs.back(), nullptr);
    }

    TestSlot* anchor1 = ptrs[1];
    TestSlot* anchor6 = ptrs[6];
    uint32_t h1 = p.index_of(anchor1);
    uint32_t h6 = p.index_of(anchor6);

    p.free(ptrs[0]);
    p.free(ptrs[2]);
    p.free(ptrs[7]);
    (void)p.allocate(99ULL, 99ULL, 99ULL);
    (void)p.allocate(88ULL, 88ULL, 88ULL);

    EXPECT_EQ(p.at(h1), anchor1);
    EXPECT_EQ(p.at(h6), anchor6);
    EXPECT_EQ(anchor1->a, 1ULL);
    EXPECT_EQ(anchor1->b, 101ULL);
    EXPECT_EQ(anchor6->a, 6ULL);
    EXPECT_EQ(anchor6->b, 106ULL);
}

// 5. Poison-on-free (first 8 bytes unpoisoned for Freenode::next; payload poisoned)
TEST(MemoryPoolTest, UseAfterFreePoisoning) {
    MemoryPool<TestSlot, 4> p;
    TestSlot* s = p.allocate(111ULL, 222ULL, 333ULL);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->a, 111ULL);
    EXPECT_EQ(s->b, 222ULL);
    EXPECT_EQ(s->c, 333ULL);

    p.free(s);

#ifdef LOB_HAS_ASAN
    // First 8 bytes hold Freenode::next and must remain unpoisoned
    EXPECT_EQ(__asan_address_is_poisoned(s), 0);
    // Remaining payload bytes must be poisoned in ASan shadow memory
    EXPECT_NE(__asan_region_is_poisoned(
                  reinterpret_cast<char*>(s) + sizeof(void*),
                  sizeof(TestSlot) - sizeof(void*)),
              nullptr);
    // Reading poisoned payload after free must trigger ASan abort
    EXPECT_DEATH(
        {
            volatile uint64_t bad_read = s->b;
            (void)bad_read;
        },
        "");
#elif !defined(NDEBUG)
    const auto* raw = reinterpret_cast<const unsigned char*>(s);
    for (size_t i = sizeof(void*); i < sizeof(TestSlot); ++i) {
        EXPECT_EQ(raw[i], 0xEF);
    }
#endif

    // Re-allocating unpoisons the slot so subsequent writes/reads succeed
    TestSlot* s2 = p.allocate(444ULL, 555ULL, 666ULL);
    ASSERT_EQ(s2, s);
#ifdef LOB_HAS_ASAN
    EXPECT_EQ(__asan_region_is_poisoned(s2, sizeof(TestSlot)), nullptr);
#endif
    EXPECT_EQ(s2->a, 444ULL);
    EXPECT_EQ(s2->b, 555ULL);
    EXPECT_EQ(s2->c, 666ULL);
}
// 6. Non-trivial destructor runs on free()
TEST(MemoryPoolTest, DestructorCalledOnFree) {
    MemoryPool<TrackedNonTrivial, 4> p;
    int destroyed = 0;
    TrackedNonTrivial* obj = p.allocate(&destroyed, 42ULL);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(destroyed, 0);
    p.free(obj);
    EXPECT_EQ(destroyed, 1);
}
