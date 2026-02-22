/**
 * @file test_runtime_view.cpp
 * @brief Tests for RuntimeView — type-erased dynamic component iteration.
 *
 * Tests cover:
 * - Single-type include: visits correct entities
 * - Multi-type include: intersection (all types required)
 * - Exclude filter: entities with excluded type are skipped
 * - Multi-type exclude
 * - Unregistered include type: each() is a no-op (null store)
 * - Unregistered exclude type: no entities filtered (null store skipped)
 * - Empty include list: each() is a no-op
 * - All entities excluded: visits none
 * - No entities excluded: visits all matching
 * - count() matches each() count
 * - empty() correct when matching / not matching
 * - includeCount() / excludeCount() accessors
 * - Span-based runtimeView overload
 * - TypeId round-trip: runtimeView matches compile-time view
 * - Large-scale: 1000 entities, alternating component presence
 * - Three-type include with exclude
 */

#include <fatp_ecs/FatpEcs.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>

using namespace fatp_ecs;

// =============================================================================
// Test Harness
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
// Component Types
// =============================================================================

struct Position  { float x = 0.0f; float y = 0.0f; };
struct Velocity  { float dx = 0.0f; float dy = 0.0f; };
struct Health    { int hp = 100; };
struct Frozen    { uint8_t dummy = 0; };
struct Dead      { uint8_t dummy = 0; };

// =============================================================================
// Helpers
// =============================================================================

static std::vector<Entity> collectEntities(RuntimeView& view)
{
    std::vector<Entity> result;
    view.each([&](Entity e) { result.push_back(e); });
    return result;
}

static bool contains(const std::vector<Entity>& v, Entity e)
{
    return std::find(v.begin(), v.end(), e) != v.end();
}

// =============================================================================
// Single-type include
// =============================================================================

void test_single_include()
{
    Registry reg;
    Entity e1 = reg.create(); reg.add<Position>(e1);
    Entity e2 = reg.create(); reg.add<Velocity>(e2); // no Position
    Entity e3 = reg.create(); reg.add<Position>(e3);

    auto view = reg.runtimeView({typeId<Position>()});
    auto visited = collectEntities(view);

    TEST_ASSERT(visited.size() == 2,     "should visit 2 entities with Position");
    TEST_ASSERT(contains(visited, e1),   "e1 should be visited");
    TEST_ASSERT(contains(visited, e3),   "e3 should be visited");
    TEST_ASSERT(!contains(visited, e2),  "e2 (no Position) should not appear");
}

// =============================================================================
// Multi-type include
// =============================================================================

void test_multi_include()
{
    Registry reg;
    Entity e1 = reg.create(); reg.add<Position>(e1); reg.add<Velocity>(e1); // included
    Entity e2 = reg.create(); reg.add<Position>(e2);                        // missing Velocity
    Entity e3 = reg.create(); reg.add<Velocity>(e3);                        // missing Position
    Entity e4 = reg.create(); reg.add<Position>(e4); reg.add<Velocity>(e4); // included

    auto view = reg.runtimeView({typeId<Position>(), typeId<Velocity>()});
    auto visited = collectEntities(view);

    TEST_ASSERT(visited.size() == 2,    "only e1 and e4 have both components");
    TEST_ASSERT(contains(visited, e1),  "e1 should be visited");
    TEST_ASSERT(contains(visited, e4),  "e4 should be visited");
    TEST_ASSERT(!contains(visited, e2), "e2 missing Velocity");
    TEST_ASSERT(!contains(visited, e3), "e3 missing Position");
}

void test_three_type_include()
{
    Registry reg;
    Entity e1 = reg.create();
    reg.add<Position>(e1); reg.add<Velocity>(e1); reg.add<Health>(e1); // included

    Entity e2 = reg.create();
    reg.add<Position>(e2); reg.add<Velocity>(e2); // missing Health

    Entity e3 = reg.create();
    reg.add<Position>(e3); reg.add<Velocity>(e3); reg.add<Health>(e3); // included

    auto view = reg.runtimeView(
        {typeId<Position>(), typeId<Velocity>(), typeId<Health>()});
    auto visited = collectEntities(view);

    TEST_ASSERT(visited.size() == 2,    "only e1 and e3 have all three");
    TEST_ASSERT(contains(visited, e1),  "e1 should be visited");
    TEST_ASSERT(contains(visited, e3),  "e3 should be visited");
    TEST_ASSERT(!contains(visited, e2), "e2 missing Health");
}

// =============================================================================
// Exclude filter
// =============================================================================

void test_single_exclude()
{
    Registry reg;
    Entity e1 = reg.create(); reg.add<Position>(e1);                        // visited
    Entity e2 = reg.create(); reg.add<Position>(e2); reg.add<Frozen>(e2);   // excluded
    Entity e3 = reg.create(); reg.add<Position>(e3);                        // visited

    auto view = reg.runtimeView({typeId<Position>()}, {typeId<Frozen>()});
    auto visited = collectEntities(view);

    TEST_ASSERT(visited.size() == 2,    "e1 and e3 should be visited");
    TEST_ASSERT(!contains(visited, e2), "e2 (Frozen) should be excluded");
}

void test_multi_exclude()
{
    Registry reg;
    Entity e1 = reg.create(); reg.add<Position>(e1);                                      // visited
    Entity e2 = reg.create(); reg.add<Position>(e2); reg.add<Frozen>(e2);                 // excluded
    Entity e3 = reg.create(); reg.add<Position>(e3); reg.add<Dead>(e3);                   // excluded
    Entity e4 = reg.create(); reg.add<Position>(e4); reg.add<Frozen>(e4); reg.add<Dead>(e4); // excluded

    auto view = reg.runtimeView({typeId<Position>()}, {typeId<Frozen>(), typeId<Dead>()});
    auto visited = collectEntities(view);

    TEST_ASSERT(visited.size() == 1,   "only e1 should be visited");
    TEST_ASSERT(contains(visited, e1), "e1 should be visited");
}

void test_include_and_exclude_combined()
{
    Registry reg;
    // e1: Position + Velocity, no Frozen  → visited
    // e2: Position + Velocity + Frozen    → excluded
    // e3: Position only                   → not in include (missing Velocity)
    // e4: Position + Velocity, no Frozen  → visited
    Entity e1 = reg.create(); reg.add<Position>(e1); reg.add<Velocity>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2); reg.add<Velocity>(e2); reg.add<Frozen>(e2);
    Entity e3 = reg.create(); reg.add<Position>(e3);
    Entity e4 = reg.create(); reg.add<Position>(e4); reg.add<Velocity>(e4);

    auto view = reg.runtimeView(
        {typeId<Position>(), typeId<Velocity>()},
        {typeId<Frozen>()});
    auto visited = collectEntities(view);

    TEST_ASSERT(visited.size() == 2,    "e1 and e4 should be visited");
    TEST_ASSERT(contains(visited, e1),  "e1 should be visited");
    TEST_ASSERT(contains(visited, e4),  "e4 should be visited");
    TEST_ASSERT(!contains(visited, e2), "e2 (Frozen) should be excluded");
    TEST_ASSERT(!contains(visited, e3), "e3 (no Velocity) should not appear");
}

// =============================================================================
// Null store handling
// =============================================================================

void test_unregistered_include_type_noop()
{
    Registry reg;
    Entity e1 = reg.create(); reg.add<Position>(e1);

    // Velocity never registered — include store is null → each() is no-op
    auto view = reg.runtimeView({typeId<Position>(), typeId<Velocity>()});
    auto visited = collectEntities(view);

    TEST_ASSERT(visited.empty(), "unregistered include type: no entities visited");
}

void test_unregistered_exclude_type_no_filter()
{
    Registry reg;
    Entity e1 = reg.create(); reg.add<Position>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2);

    // Frozen never registered — exclude store is null → treated as absent → no filtering
    auto view = reg.runtimeView({typeId<Position>()}, {typeId<Frozen>()});
    auto visited = collectEntities(view);

    TEST_ASSERT(visited.size() == 2,
                "unregistered exclude type: both entities should be visited");
}

void test_empty_include_list_noop()
{
    Registry reg;
    [[maybe_unused]] auto dummy = reg.create();

    auto view = reg.runtimeView({});
    auto visited = collectEntities(view);

    TEST_ASSERT(visited.empty(), "empty include list: each() should be no-op");
}

// =============================================================================
// count(), empty(), includeCount(), excludeCount()
// =============================================================================

void test_count_matches_each()
{
    Registry reg;
    for (int i = 0; i < 10; ++i)
    {
        Entity e = reg.create(); reg.add<Position>(e);
        if (i % 3 == 0) reg.add<Frozen>(e);
    }

    auto view = reg.runtimeView({typeId<Position>()}, {typeId<Frozen>()});

    std::size_t eachCount = 0;
    view.each([&](Entity) { ++eachCount; });

    TEST_ASSERT(view.count() == eachCount,
                "count() should match the number of each() callbacks");
}

void test_empty_false_when_matches_exist()
{
    Registry reg;
    Entity e = reg.create(); reg.add<Position>(e);

    auto view = reg.runtimeView({typeId<Position>()});
    TEST_ASSERT(!view.empty(), "empty() should be false when matches exist");
}

void test_empty_true_when_no_matches()
{
    Registry reg;
    Entity e = reg.create(); reg.add<Position>(e); reg.add<Frozen>(e);

    // All Position entities are Frozen
    auto view = reg.runtimeView({typeId<Position>()}, {typeId<Frozen>()});
    TEST_ASSERT(view.empty(), "empty() should be true when all are excluded");
}

void test_include_exclude_count_accessors()
{
    Registry reg;
    [[maybe_unused]] auto unused = reg.create(); // ensure stores are not null
    Entity e = reg.create();
    reg.add<Position>(e);
    reg.add<Velocity>(e);
    reg.add<Frozen>(e);
    reg.add<Dead>(e);

    auto view = reg.runtimeView(
        {typeId<Position>(), typeId<Velocity>()},
        {typeId<Frozen>(), typeId<Dead>()});

    TEST_ASSERT(view.includeCount() == 2, "includeCount should be 2");
    TEST_ASSERT(view.excludeCount() == 2, "excludeCount should be 2");
}

// =============================================================================
// Span-based overload
// =============================================================================

void test_span_overload()
{
    Registry reg;
    Entity e1 = reg.create(); reg.add<Position>(e1); reg.add<Velocity>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2);

    TypeId inc[] = {typeId<Position>(), typeId<Velocity>()};
    TypeId exc[] = {typeId<Frozen>()};

    auto view = reg.runtimeView(inc, 2, exc, 1);
    auto visited = collectEntities(view);

    TEST_ASSERT(visited.size() == 1,   "span overload: only e1 matches");
    TEST_ASSERT(contains(visited, e1), "span overload: e1 should be visited");
}

void test_span_overload_no_exclude()
{
    Registry reg;
    Entity e1 = reg.create(); reg.add<Position>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2);

    TypeId inc[] = {typeId<Position>()};
    auto view = reg.runtimeView(inc, 1);
    auto visited = collectEntities(view);

    TEST_ASSERT(visited.size() == 2, "span overload no exclude: both visited");
}

// =============================================================================
// TypeId round-trip: runtimeView matches compile-time view
// =============================================================================

void test_matches_compiletime_view()
{
    Registry reg;
    for (int i = 0; i < 20; ++i)
    {
        Entity e = reg.create();
        reg.add<Position>(e);
        if (i % 2 == 0) reg.add<Velocity>(e);
        if (i % 5 == 0) reg.add<Frozen>(e);
    }

    // Collect entities via compile-time view
    std::vector<Entity> ctResult;
    reg.view<Position, Velocity>(Exclude<Frozen>{}).each(
        [&](Entity e, Position&, Velocity&) { ctResult.push_back(e); });
    std::sort(ctResult.begin(), ctResult.end());

    // Collect entities via runtime view
    std::vector<Entity> rtResult;
    reg.runtimeView(
        {typeId<Position>(), typeId<Velocity>()},
        {typeId<Frozen>()}).each(
        [&](Entity e) { rtResult.push_back(e); });
    std::sort(rtResult.begin(), rtResult.end());

    TEST_ASSERT(ctResult == rtResult,
                "runtimeView must visit exactly the same entities as compile-time view");
}

// =============================================================================
// Large scale
// =============================================================================

void test_large_scale()
{
    Registry reg;
    constexpr int kN = 1000;

    std::vector<Entity> entities;
    entities.reserve(kN);
    for (int i = 0; i < kN; ++i)
    {
        Entity e = reg.create();
        entities.push_back(e);
        reg.add<Position>(e);
        reg.add<Velocity>(e);
        if (i % 3 == 0) reg.add<Frozen>(e);
        if (i % 7 == 0) reg.add<Dead>(e);
    }

    int expected = 0;
    for (int i = 0; i < kN; ++i)
    {
        if (i % 3 != 0 && i % 7 != 0) ++expected;
    }

    int counted = 0;
    reg.runtimeView(
        {typeId<Position>(), typeId<Velocity>()},
        {typeId<Frozen>(), typeId<Dead>()}).each(
        [&](Entity) { ++counted; });

    TEST_ASSERT(counted == expected,
                "large scale: counted should match expected non-excluded entities");
}

// =============================================================================
// Main
// =============================================================================

int main()
{
    std::printf("=== RuntimeView Tests ===\n\n");

    std::printf("Single and multi-type include:\n");
    RUN_TEST(test_single_include);
    RUN_TEST(test_multi_include);
    RUN_TEST(test_three_type_include);

    std::printf("\nExclude filters:\n");
    RUN_TEST(test_single_exclude);
    RUN_TEST(test_multi_exclude);
    RUN_TEST(test_include_and_exclude_combined);

    std::printf("\nNull store handling:\n");
    RUN_TEST(test_unregistered_include_type_noop);
    RUN_TEST(test_unregistered_exclude_type_no_filter);
    RUN_TEST(test_empty_include_list_noop);

    std::printf("\ncount(), empty(), accessors:\n");
    RUN_TEST(test_count_matches_each);
    RUN_TEST(test_empty_false_when_matches_exist);
    RUN_TEST(test_empty_true_when_no_matches);
    RUN_TEST(test_include_exclude_count_accessors);

    std::printf("\nSpan-based overload:\n");
    RUN_TEST(test_span_overload);
    RUN_TEST(test_span_overload_no_exclude);

    std::printf("\nRound-trip and scale:\n");
    RUN_TEST(test_matches_compiletime_view);
    RUN_TEST(test_large_scale);

    std::printf("\n=== Results: %d passed, %d failed ===\n",
                sTestsPassed, sTestsFailed);

    return sTestsFailed == 0 ? 0 : 1;
}
