/**
 * @file test_entity_copy.cpp
 * @brief Tests for registry.copy(src, dst).
 *
 * Tests cover:
 *  1.  copy() returns component count
 *  2.  All components from src appear on dst
 *  3.  Component values are correctly duplicated
 *  4.  Existing components on dst are overwritten
 *  5.  Components not on src are untouched on dst
 *  6.  Sparse coverage: src has subset of components
 *  7.  copy() fires onComponentAdded for newly added components
 *  8.  copy() fires onComponentUpdated for overwritten components
 *  9.  copy(src, src) is a no-op (returns 0)
 * 10.  copy() with dead src returns 0
 * 11.  copy() with dead dst returns 0
 * 12.  copy() with both dead returns 0
 * 13.  Modifying dst after copy does not affect src
 * 14.  Entity with no components: copy returns 0
 * 15.  Non-trivial component type (std::string) round-trips correctly
 */

#include <fatp_ecs/FatpEcs.h>

#include <cassert>
#include <cstdio>
#include <string>

using namespace fatp_ecs;

// =============================================================================
// Test harness
// =============================================================================

static int sTestsPassed = 0;
static int sTestsFailed = 0;

#define TEST_ASSERT(cond, msg)                                              \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            std::printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);  \
            ++sTestsFailed;                                                 \
            return;                                                         \
        }                                                                   \
    } while (0)

#define RUN_TEST(fn)                                    \
    do                                                  \
    {                                                   \
        std::printf("  Running: %s\n", #fn);            \
        fn();                                           \
        ++sTestsPassed;                                 \
    } while (0)

// =============================================================================
// Component types
// =============================================================================

struct Position { float x{0.f}; float y{0.f}; };
struct Health   { uint32_t hp{0}; };
struct Velocity { float dx{0.f}; float dy{0.f}; };
struct Label    { std::string text; };

// =============================================================================
// Tests
// =============================================================================

static void test_copy_returns_count()
{
    Registry reg;
    Entity src = reg.create();
    Entity dst = reg.create();
    reg.add<Position>(src, 1.f, 2.f);
    reg.add<Health>(src, 10u);

    std::size_t n = reg.copy(src, dst);
    TEST_ASSERT(n == 2, "copy returns 2 for two components");
}

static void test_all_components_copied()
{
    Registry reg;
    Entity src = reg.create();
    Entity dst = reg.create();
    reg.add<Position>(src, 3.f, 4.f);
    reg.add<Health>(src, 50u);
    reg.add<Velocity>(src, 1.f, -1.f);

    (void)reg.copy(src, dst);

    TEST_ASSERT(reg.has<Position>(dst), "dst has Position");
    TEST_ASSERT(reg.has<Health>(dst),   "dst has Health");
    TEST_ASSERT(reg.has<Velocity>(dst), "dst has Velocity");
}

static void test_component_values_duplicated()
{
    Registry reg;
    Entity src = reg.create();
    Entity dst = reg.create();
    reg.add<Position>(src, 7.f, 8.f);
    reg.add<Health>(src, 99u);

    (void)reg.copy(src, dst);

    TEST_ASSERT(reg.get<Position>(dst).x == 7.f, "Position.x matches");
    TEST_ASSERT(reg.get<Position>(dst).y == 8.f, "Position.y matches");
    TEST_ASSERT(reg.get<Health>(dst).hp  == 99u, "Health.hp matches");
}

static void test_existing_components_overwritten()
{
    Registry reg;
    Entity src = reg.create();
    Entity dst = reg.create();
    reg.add<Health>(src, 100u);
    reg.add<Health>(dst,   1u);  // dst already has Health with different value

    (void)reg.copy(src, dst);

    TEST_ASSERT(reg.get<Health>(dst).hp == 100u, "Health overwritten with src value");
}

static void test_untouched_components_preserved()
{
    Registry reg;
    Entity src = reg.create();
    Entity dst = reg.create();
    reg.add<Position>(src, 5.f, 5.f);
    reg.add<Velocity>(dst, 9.f, 9.f);  // dst has Velocity; src does not

    (void)reg.copy(src, dst);

    TEST_ASSERT(reg.has<Position>(dst),  "Position added to dst");
    TEST_ASSERT(reg.has<Velocity>(dst),  "Velocity untouched on dst");
    TEST_ASSERT(reg.get<Velocity>(dst).dx == 9.f, "Velocity value unchanged");
}

static void test_sparse_coverage()
{
    Registry reg;
    Entity src = reg.create();
    Entity dst = reg.create();
    // src only has Health
    reg.add<Health>(src, 42u);
    // dst has Position but not Health
    reg.add<Position>(dst, 1.f, 1.f);

    std::size_t n = reg.copy(src, dst);
    TEST_ASSERT(n == 1, "only one component copied");
    TEST_ASSERT(reg.has<Health>(dst),      "dst gained Health");
    TEST_ASSERT(reg.has<Position>(dst),    "dst kept Position");
    TEST_ASSERT(!reg.has<Position>(src),   "src still lacks Position"); // sanity
}

static void test_added_signal_fires()
{
    Registry reg;
    int addCount = 0;
    auto conn1 = reg.events().onComponentAdded<Health>().connect(
        [&](Entity, Health&){ ++addCount; });

    Entity src = reg.create();
    Entity dst = reg.create();
    reg.add<Health>(src, 5u);
    addCount = 0;  // reset after src.add fired

    (void)reg.copy(src, dst);
    TEST_ASSERT(addCount == 1, "onComponentAdded fired once for new dst component");
}

static void test_updated_signal_fires()
{
    Registry reg;
    int updateCount = 0;
    auto conn2 = reg.events().onComponentUpdated<Health>().connect(
        [&](Entity, Health&){ ++updateCount; });

    Entity src = reg.create();
    Entity dst = reg.create();
    reg.add<Health>(src, 10u);
    reg.add<Health>(dst, 20u);

    (void)reg.copy(src, dst);
    TEST_ASSERT(updateCount == 1, "onComponentUpdated fired for overwrite");
}

static void test_copy_self_is_noop()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Health>(e, 77u);

    std::size_t n = reg.copy(e, e);
    TEST_ASSERT(n == 0, "copy(e,e) returns 0");
    TEST_ASSERT(reg.get<Health>(e).hp == 77u, "value unchanged after self-copy");
}

static void test_dead_src_returns_zero()
{
    Registry reg;
    Entity src = reg.create();
    Entity dst = reg.create();
    reg.add<Health>(src, 1u);
    reg.destroy(src);

    std::size_t n = reg.copy(src, dst);
    TEST_ASSERT(n == 0, "dead src returns 0");
}

static void test_dead_dst_returns_zero()
{
    Registry reg;
    Entity src = reg.create();
    Entity dst = reg.create();
    reg.add<Health>(src, 1u);
    reg.destroy(dst);

    std::size_t n = reg.copy(src, dst);
    TEST_ASSERT(n == 0, "dead dst returns 0");
}

static void test_both_dead_returns_zero()
{
    Registry reg;
    Entity src = reg.create();
    Entity dst = reg.create();
    reg.destroy(src);
    reg.destroy(dst);

    std::size_t n = reg.copy(src, dst);
    TEST_ASSERT(n == 0, "both dead returns 0");
}

static void test_modify_dst_does_not_affect_src()
{
    Registry reg;
    Entity src = reg.create();
    Entity dst = reg.create();
    reg.add<Health>(src, 100u);

    (void)reg.copy(src, dst);
    reg.get<Health>(dst).hp = 1u;

    TEST_ASSERT(reg.get<Health>(src).hp == 100u, "src unchanged after dst mutation");
    TEST_ASSERT(reg.get<Health>(dst).hp ==   1u, "dst has modified value");
}

static void test_no_components_returns_zero()
{
    Registry reg;
    Entity src = reg.create();
    Entity dst = reg.create();
    // src has no components at all

    std::size_t n = reg.copy(src, dst);
    TEST_ASSERT(n == 0, "no components: returns 0");
}

static void test_nontrivial_component()
{
    Registry reg;
    Entity src = reg.create();
    Entity dst = reg.create();
    reg.add<Label>(src, std::string("hello"));

    (void)reg.copy(src, dst);

    TEST_ASSERT(reg.has<Label>(dst), "dst has Label");
    TEST_ASSERT(reg.get<Label>(dst).text == "hello", "string value preserved");

    // Mutate dst; src should be unaffected
    reg.get<Label>(dst).text = "world";
    TEST_ASSERT(reg.get<Label>(src).text == "hello", "src string unchanged");
}

static void test_noncopyable_component_skipped()
{
    // AIComponent-style: has deleted copy assignment (e.g. holds unique_ptr).
    // registry.copy() must compile and return 0 for the non-copyable component
    // rather than failing to compile or crashing.
    struct NonCopyable
    {
        NonCopyable() = default;
        NonCopyable(const NonCopyable&) = delete;
        NonCopyable& operator=(const NonCopyable&) = delete;
        NonCopyable(NonCopyable&&) = default;
        NonCopyable& operator=(NonCopyable&&) = default;
    };

    Registry reg;
    Entity src = reg.create();
    Entity dst = reg.create();

    reg.add<NonCopyable>(src);
    reg.add<Health>(src, 99u);

    std::size_t n = reg.copy(src, dst);

    TEST_ASSERT(n == 1,                    "only copyable components counted");
    TEST_ASSERT(reg.has<Health>(dst),      "Health was copied");
    TEST_ASSERT(!reg.has<NonCopyable>(dst),"NonCopyable was skipped");
}


// =============================================================================
// main
// =============================================================================

int main()
{
    std::printf("=== test_entity_copy ===\n");

    RUN_TEST(test_copy_returns_count);
    RUN_TEST(test_all_components_copied);
    RUN_TEST(test_component_values_duplicated);
    RUN_TEST(test_existing_components_overwritten);
    RUN_TEST(test_untouched_components_preserved);
    RUN_TEST(test_sparse_coverage);
    RUN_TEST(test_added_signal_fires);
    RUN_TEST(test_updated_signal_fires);
    RUN_TEST(test_copy_self_is_noop);
    RUN_TEST(test_dead_src_returns_zero);
    RUN_TEST(test_dead_dst_returns_zero);
    RUN_TEST(test_both_dead_returns_zero);
    RUN_TEST(test_modify_dst_does_not_affect_src);
    RUN_TEST(test_no_components_returns_zero);
    RUN_TEST(test_nontrivial_component);
    RUN_TEST(test_noncopyable_component_skipped);

    std::printf("\n%d/%d tests passed\n", sTestsPassed, sTestsPassed + sTestsFailed);
    return sTestsFailed == 0 ? 0 : 1;
}
