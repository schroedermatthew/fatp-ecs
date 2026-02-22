/**
 * @file test_storage_policy.cpp
 * @brief Tests for ComponentStore storage policies.
 *
 * Verifies:
 *   DefaultStoragePolicy  — baseline correctness (regression coverage)
 *   AlignedStoragePolicy  — correct alignment, full functional parity
 *   ConcurrentStoragePolicy — locking wrapper correctness
 *   Registry::useStorage<T, Policy>() — pre-registration API
 *   Registry::useAlignedStorage<T, N>() — convenience shorthand
 *   dataAlignment() introspection
 *   Policy-mismatch assertion (not tested here — would abort; documented)
 */

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fat_p/ConcurrencyPolicies.h>

#include "fatp_ecs/FatpEcs.h"

// =============================================================================
// Test infrastructure
// =============================================================================

static int gPassed = 0;
static int gFailed = 0;

#define TEST_ASSERT(cond, msg)                                 \
    do                                                         \
    {                                                          \
        if (!(cond))                                           \
        {                                                      \
            std::printf("  FAIL [%s]: %s\n", __func__, (msg)); \
            ++gFailed;                                         \
        }                                                      \
        else                                                   \
        {                                                      \
            ++gPassed;                                         \
        }                                                      \
    } while (false)

#define RUN_TEST(fn)                         \
    do                                       \
    {                                        \
        std::printf("  Running: %s\n", #fn); \
        fn();                                \
    } while (false)

using namespace fatp_ecs;

// =============================================================================
// Helper component types
// =============================================================================

struct Position
{
    float x{}, y{}, z{};
};
struct Velocity
{
    float vx{}, vy{};
};
struct Health
{
    uint32_t hp{};
};

// Aligned struct that benefits from 32-byte alignment.
// MSVC C4324: structure padded due to alignment specifier — expected and intentional.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct alignas(32) SimdVec4
{
    float v[4]{};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// =============================================================================
// DefaultStoragePolicy — baseline correctness
// =============================================================================

static void test_default_policy_add_remove()
{
    ComponentStore<Position> store;

    Entity e0{0};
    Entity e1{1};

    TEST_ASSERT(store.empty(), "initially empty");
    store.emplace(e0, Position{1.f, 2.f, 3.f});
    store.emplace(e1, Position{4.f, 5.f, 6.f});
    TEST_ASSERT(store.size() == 2, "size after two adds");
    TEST_ASSERT(store.has(e0), "e0 present");
    TEST_ASSERT(store.has(e1), "e1 present");

    store.remove(e0);
    TEST_ASSERT(!store.has(e0), "e0 removed");
    TEST_ASSERT(store.size() == 1, "size after remove");
}

static void test_default_policy_get_values()
{
    ComponentStore<Position> store;
    Entity e{42};
    store.emplace(e, Position{7.f, 8.f, 9.f});

    const Position& p = store.get(e);
    TEST_ASSERT(p.x == 7.f && p.y == 8.f && p.z == 9.f, "get returns correct values");

    Position* pp = store.tryGet(e);
    TEST_ASSERT(pp != nullptr, "tryGet returns non-null");
    pp->x = 99.f;
    TEST_ASSERT(store.get(e).x == 99.f, "mutation via tryGet persists");
}

static void test_default_policy_swap_with_back()
{
    ComponentStore<Health> store;
    Entity e0{0}, e1{1}, e2{2};

    store.emplace(e0, Health{10});
    store.emplace(e1, Health{20});
    store.emplace(e2, Health{30});

    // Remove middle — triggers swap-with-back
    store.remove(e1);
    TEST_ASSERT(store.size() == 2, "size after mid-remove");
    TEST_ASSERT(!store.has(e1), "e1 gone");
    TEST_ASSERT(store.has(e0), "e0 still present");
    TEST_ASSERT(store.has(e2), "e2 still present");
    TEST_ASSERT(store.get(e0).hp == 10, "e0 value intact");
    TEST_ASSERT(store.get(e2).hp == 30, "e2 value intact");
}

static void test_default_policy_clear()
{
    ComponentStore<Position> store;
    for (uint32_t i = 0; i < 8; ++i)
    {
        store.emplace(Entity{i}, Position{float(i), 0.f, 0.f});
    }

    store.clear();
    TEST_ASSERT(store.empty(), "empty after clear");
    TEST_ASSERT(store.size() == 0, "size == 0 after clear");
}

static void test_default_alignment()
{
    TEST_ASSERT(ComponentStore<Position>::dataAlignment() == alignof(Position),
                "default policy alignment == alignof(T)");
}

// =============================================================================
// AlignedStoragePolicy
// =============================================================================

static void test_aligned_policy_basic_ops()
{
    ComponentStore<Position, AlignedStoragePolicy<64>::Policy> store;

    Entity e0{0}, e1{1};
    store.emplace(e0, Position{1.f, 2.f, 3.f});
    store.emplace(e1, Position{4.f, 5.f, 6.f});

    TEST_ASSERT(store.size() == 2, "size after two adds (aligned)");
    TEST_ASSERT(store.has(e0), "e0 present (aligned)");
    TEST_ASSERT(store.get(e0).x == 1.f, "e0.x correct (aligned)");
    TEST_ASSERT(store.get(e1).z == 6.f, "e1.z correct (aligned)");

    store.remove(e0);
    TEST_ASSERT(!store.has(e0), "e0 removed (aligned)");
    TEST_ASSERT(store.has(e1), "e1 still present (aligned)");
}

static void test_aligned_policy_data_pointer_alignment()
{
    ComponentStore<SimdVec4, AlignedStoragePolicy<32>::Policy> store;

    // Add enough entities to force AlignedVector to allocate
    for (uint32_t i = 0; i < 16; ++i)
    {
        store.emplace(Entity{i}, SimdVec4{});
    }

    const SimdVec4* ptr = store.componentDataPtr();
    TEST_ASSERT(ptr != nullptr, "data pointer non-null");

    const auto addr = reinterpret_cast<uintptr_t>(ptr);
    TEST_ASSERT((addr % 32) == 0, "data pointer 32-byte aligned");
}

static void test_aligned_policy_alignment_introspection()
{
    using Store64 = ComponentStore<Position, AlignedStoragePolicy<64>::Policy>;
    using Store32 = ComponentStore<Position, AlignedStoragePolicy<32>::Policy>;

    TEST_ASSERT(Store64::dataAlignment() == 64, "64-byte aligned store reports 64");
    TEST_ASSERT(Store32::dataAlignment() == 32, "32-byte aligned store reports 32");
}

static void test_aligned_policy_swap_with_back()
{
    ComponentStore<Health, AlignedStoragePolicy<64>::Policy> store;
    Entity e0{0}, e1{1}, e2{2};

    store.emplace(e0, Health{1});
    store.emplace(e1, Health{2});
    store.emplace(e2, Health{3});

    store.remove(e1);
    TEST_ASSERT(store.size() == 2, "size after remove (aligned)");
    TEST_ASSERT(store.get(e0).hp == 1, "e0 value intact (aligned)");
    TEST_ASSERT(store.get(e2).hp == 3, "e2 value intact (aligned)");
}

static void test_aligned_policy_clear()
{
    ComponentStore<Position, AlignedStoragePolicy<64>::Policy> store;
    for (uint32_t i = 0; i < 10; ++i)
    {
        store.emplace(Entity{i}, Position{});
    }
    store.clear();
    TEST_ASSERT(store.empty(), "empty after clear (aligned)");
}

// =============================================================================
// ConcurrentStoragePolicy
// =============================================================================

static void test_concurrent_policy_basic_ops()
{
    ComponentStore<Health, ConcurrentStoragePolicy<fat_p::SingleThreadedPolicy>::Policy> store;

    Entity e0{0}, e1{1};
    store.emplace(e0, Health{100});
    store.emplace(e1, Health{200});

    TEST_ASSERT(store.size() == 2, "size after two adds (concurrent)");
    TEST_ASSERT(store.has(e0), "e0 present (concurrent)");
    TEST_ASSERT(store.get(e0).hp == 100, "e0 value (concurrent)");
    TEST_ASSERT(store.get(e1).hp == 200, "e1 value (concurrent)");

    store.remove(e1);
    TEST_ASSERT(!store.has(e1), "e1 removed (concurrent)");
    TEST_ASSERT(store.size() == 1, "size after remove (concurrent)");
}

static void test_concurrent_policy_swap_with_back()
{
    ComponentStore<Position, ConcurrentStoragePolicy<fat_p::SingleThreadedPolicy>::Policy> store;
    Entity e0{0}, e1{1}, e2{2};

    store.emplace(e0, Position{1.f, 0.f, 0.f});
    store.emplace(e1, Position{2.f, 0.f, 0.f});
    store.emplace(e2, Position{3.f, 0.f, 0.f});

    store.remove(e1);
    TEST_ASSERT(store.get(e0).x == 1.f, "e0.x correct after mid-remove (concurrent)");
    TEST_ASSERT(store.get(e2).x == 3.f, "e2.x correct after mid-remove (concurrent)");
}

static void test_concurrent_policy_multithreaded_reads()
{
    // Multiple threads read component data concurrently.
    // SharedMutexPolicy allows concurrent reads.
    ComponentStore<Health, ConcurrentStoragePolicy<fat_p::SharedMutexPolicy>::Policy> store;

    for (uint32_t i = 0; i < 64; ++i)
    {
        store.emplace(Entity{i}, Health{i * 10});
    }

    std::vector<std::thread> readers;
    std::vector<bool> results(4, true);

    for (int t = 0; t < 4; ++t)
    {
        readers.emplace_back([&store, &results, t]() {
            for (uint32_t i = 0; i < 64; ++i)
            {
                const Health* h = store.tryGet(Entity{i});
                if (!h || h->hp != i * 10)
                {
                    results[t] = false;
                    return;
                }
            }
        });
    }

    for (auto& th : readers)
    {
        th.join();
    }

    for (int t = 0; t < 4; ++t)
    {
        TEST_ASSERT(results[t], "reader thread saw correct values");
    }
}

// =============================================================================
// Registry::useStorage / useAlignedStorage integration
// =============================================================================

static void test_registry_use_aligned_storage()
{
    Registry reg;
    reg.useAlignedStorage<Position, 64>();

    Entity e0 = reg.create();
    Entity e1 = reg.create();

    reg.add<Position>(e0, Position{10.f, 20.f, 30.f});
    reg.add<Position>(e1, Position{40.f, 50.f, 60.f});

    TEST_ASSERT(reg.has<Position>(e0), "e0 has Position (aligned registry)");
    TEST_ASSERT(reg.get<Position>(e0).x == 10.f, "e0.x correct (aligned registry)");
    TEST_ASSERT(reg.get<Position>(e1).z == 60.f, "e1.z correct (aligned registry)");

    reg.remove<Position>(e0);
    TEST_ASSERT(!reg.has<Position>(e0), "e0 removed (aligned registry)");
}

static void test_registry_use_aligned_storage_data_alignment()
{
    Registry reg;
    reg.useAlignedStorage<SimdVec4, 32>();

    for (int i = 0; i < 16; ++i)
    {
        reg.add<SimdVec4>(reg.create());
    }

    const auto* store = reg.tryGetStore<SimdVec4>();
    TEST_ASSERT(store != nullptr, "store present after useAlignedStorage");

    const SimdVec4* ptr = store->componentDataPtr();
    TEST_ASSERT(ptr != nullptr, "data pointer non-null");
    TEST_ASSERT((reinterpret_cast<uintptr_t>(ptr) % 32) == 0, "data pointer 32-byte aligned via registry");
}

static void test_registry_use_concurrent_storage()
{
    Registry reg;
    reg.useStorage<Health, ConcurrentStoragePolicy<fat_p::SharedMutexPolicy>::Policy>();

    Entity e = reg.create();
    reg.add<Health>(e, Health{999});
    TEST_ASSERT(reg.get<Health>(e).hp == 999, "concurrent storage via registry");
}

static void test_registry_default_policy_unchanged()
{
    // Entities added without useStorage() still work — DefaultStoragePolicy
    Registry reg;
    Entity e = reg.create();
    reg.add<Velocity>(e, Velocity{1.f, 2.f});
    TEST_ASSERT(reg.get<Velocity>(e).vx == 1.f, "default policy unaffected");
}

static void test_registry_view_works_with_aligned_storage()
{
    Registry reg;
    reg.useAlignedStorage<Position, 64>();
    reg.useAlignedStorage<Velocity, 64>();

    for (int i = 0; i < 8; ++i)
    {
        Entity e = reg.create();
        reg.add<Position>(e, Position{float(i), 0.f, 0.f});
        reg.add<Velocity>(e, Velocity{float(i) * 2.f, 0.f});
    }

    int count = 0;
    reg.view<Position, Velocity>().each([&](Entity, Position& p, Velocity& v) {
        p.x += v.vx;
        ++count;
    });

    TEST_ASSERT(count == 8, "view iterated all 8 entities with aligned storage");

    reg.view<Position>().each([&](Entity, Position& p) {
        TEST_ASSERT(p.x >= 0.f, "position x non-negative after update");
    });
}

static void test_registry_entity_copy_with_aligned_storage()
{
    Registry reg;
    reg.useAlignedStorage<Position, 64>();

    Entity src = reg.create();
    Entity dst = reg.create();
    reg.add<Position>(src, Position{1.f, 2.f, 3.f});

    std::size_t n = reg.copy(src, dst);
    TEST_ASSERT(n == 1, "copy count correct");
    TEST_ASSERT(reg.has<Position>(dst), "dst has Position after copy");
    TEST_ASSERT(reg.get<Position>(dst).x == 1.f, "copied value correct");
}

// =============================================================================
// main
// =============================================================================

int main()
{
    std::printf("=== test_storage_policy ===\n");

    // DefaultStoragePolicy
    RUN_TEST(test_default_policy_add_remove);
    RUN_TEST(test_default_policy_get_values);
    RUN_TEST(test_default_policy_swap_with_back);
    RUN_TEST(test_default_policy_clear);
    RUN_TEST(test_default_alignment);

    // AlignedStoragePolicy
    RUN_TEST(test_aligned_policy_basic_ops);
    RUN_TEST(test_aligned_policy_data_pointer_alignment);
    RUN_TEST(test_aligned_policy_alignment_introspection);
    RUN_TEST(test_aligned_policy_swap_with_back);
    RUN_TEST(test_aligned_policy_clear);

    // ConcurrentStoragePolicy
    RUN_TEST(test_concurrent_policy_basic_ops);
    RUN_TEST(test_concurrent_policy_swap_with_back);
    RUN_TEST(test_concurrent_policy_multithreaded_reads);

    // Registry integration
    RUN_TEST(test_registry_use_aligned_storage);
    RUN_TEST(test_registry_use_aligned_storage_data_alignment);
    RUN_TEST(test_registry_use_concurrent_storage);
    RUN_TEST(test_registry_default_policy_unchanged);
    RUN_TEST(test_registry_view_works_with_aligned_storage);
    RUN_TEST(test_registry_entity_copy_with_aligned_storage);

    std::printf("\n%d/%d tests passed\n", gPassed, gPassed + gFailed);
    return gFailed == 0 ? 0 : 1;
}