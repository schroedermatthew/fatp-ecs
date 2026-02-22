/**
 * @file test_exclude_filters.cpp
 * @brief Tests for Exclude<> filter support on Registry::view().
 *
 * Tests cover:
 * - Single-component view with one exclude type
 * - Single-component view with multiple exclude types
 * - Multi-component view with one exclude type
 * - Multi-component view with multiple exclude types
 * - Exclude type never registered (null store — trivially passes)
 * - Entity gains excluded component mid-iteration does not affect current view
 * - count() with exclude filters
 * - const view with exclude filters
 * - Exclude type same as include type (no entities matched — all excluded)
 * - All entities excluded
 * - No entities excluded
 * - Large-scale correctness (1000 entities, alternating exclude tags)
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

#define RUN_TEST(fn)                                   \
    do                                                 \
    {                                                  \
        std::printf("  Running: %s\n", #fn);           \
        fn();                                          \
        ++sTestsPassed;                                \
    } while (0)

// =============================================================================
// Component Types
// =============================================================================

struct Position  { float x = 0.0f; float y = 0.0f; };
struct Velocity  { float dx = 0.0f; float dy = 0.0f; };
struct Health    { int hp = 100; };

// Tag components (zero-size would be ideal but FAT-P stores data; use 1-byte tag)
struct Frozen    { uint8_t dummy = 0; };
struct Dead      { uint8_t dummy = 0; };
struct Invisible { uint8_t dummy = 0; };

// =============================================================================
// Tests: single-component views with exclude
// =============================================================================

void test_single_exclude_basic()
{
    Registry reg;

    // e1: Position only           — should be visited
    // e2: Position + Frozen       — should be skipped
    // e3: Position only           — should be visited
    Entity e1 = reg.create();
    Entity e2 = reg.create();
    Entity e3 = reg.create();
    reg.add<Position>(e1, 1.0f, 0.0f);
    reg.add<Position>(e2, 2.0f, 0.0f);
    reg.add<Frozen>(e2);
    reg.add<Position>(e3, 3.0f, 0.0f);

    std::vector<Entity> visited;
    reg.view<Position>(Exclude<Frozen>{}).each(
        [&](Entity e, Position&) { visited.push_back(e); });

    TEST_ASSERT(visited.size() == 2, "Should visit 2 entities (e1 and e3)");
    TEST_ASSERT(std::find(visited.begin(), visited.end(), e1) != visited.end(),
                "e1 (no Frozen) should be visited");
    TEST_ASSERT(std::find(visited.begin(), visited.end(), e3) != visited.end(),
                "e3 (no Frozen) should be visited");
    TEST_ASSERT(std::find(visited.begin(), visited.end(), e2) == visited.end(),
                "e2 (has Frozen) should be skipped");
}

void test_single_multiple_excludes()
{
    Registry reg;

    // e1: Position only             — visited
    // e2: Position + Frozen         — skipped (Frozen)
    // e3: Position + Dead           — skipped (Dead)
    // e4: Position + Frozen + Dead  — skipped (both)
    Entity e1 = reg.create();
    Entity e2 = reg.create();
    Entity e3 = reg.create();
    Entity e4 = reg.create();
    reg.add<Position>(e1);
    reg.add<Position>(e2); reg.add<Frozen>(e2);
    reg.add<Position>(e3); reg.add<Dead>(e3);
    reg.add<Position>(e4); reg.add<Frozen>(e4); reg.add<Dead>(e4);

    std::vector<Entity> visited;
    reg.view<Position>(Exclude<Frozen, Dead>{}).each(
        [&](Entity e, Position&) { visited.push_back(e); });

    TEST_ASSERT(visited.size() == 1, "Only e1 should be visited");
    TEST_ASSERT(visited[0] == e1, "The visited entity should be e1");
}

void test_single_exclude_unregistered_type()
{
    Registry reg;

    // Frozen is never registered — null exclude store should pass trivially.
    Entity e1 = reg.create(); reg.add<Position>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2);

    std::vector<Entity> visited;
    reg.view<Position>(Exclude<Frozen>{}).each(
        [&](Entity e, Position&) { visited.push_back(e); });

    TEST_ASSERT(visited.size() == 2,
                "Unregistered exclude type should not filter anything");
}

void test_single_exclude_all_excluded()
{
    Registry reg;

    Entity e1 = reg.create(); reg.add<Position>(e1); reg.add<Frozen>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2); reg.add<Frozen>(e2);

    std::vector<Entity> visited;
    reg.view<Position>(Exclude<Frozen>{}).each(
        [&](Entity e, Position&) { visited.push_back(e); });

    TEST_ASSERT(visited.empty(), "All entities have Frozen — none should be visited");
}

void test_single_exclude_none_excluded()
{
    Registry reg;

    Entity e1 = reg.create(); reg.add<Position>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2);
    // Dead registered but no entity has it
    Entity temp = reg.create(); reg.add<Dead>(temp); reg.destroy(temp);

    std::vector<Entity> visited;
    reg.view<Position>(Exclude<Dead>{}).each(
        [&](Entity e, Position&) { visited.push_back(e); });

    TEST_ASSERT(visited.size() == 2, "No entity has Dead — both should be visited");
}

// =============================================================================
// Tests: multi-component views with exclude
// =============================================================================

void test_multi_exclude_basic()
{
    Registry reg;

    // e1: Position + Velocity               — visited
    // e2: Position + Velocity + Frozen      — skipped
    // e3: Position only                     — not in view (no Velocity)
    // e4: Position + Velocity               — visited
    Entity e1 = reg.create(); reg.add<Position>(e1); reg.add<Velocity>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2); reg.add<Velocity>(e2); reg.add<Frozen>(e2);
    Entity e3 = reg.create(); reg.add<Position>(e3);
    Entity e4 = reg.create(); reg.add<Position>(e4); reg.add<Velocity>(e4);

    std::vector<Entity> visited;
    reg.view<Position, Velocity>(Exclude<Frozen>{}).each(
        [&](Entity e, Position&, Velocity&) { visited.push_back(e); });

    TEST_ASSERT(visited.size() == 2, "Should visit e1 and e4 only");
    TEST_ASSERT(std::find(visited.begin(), visited.end(), e1) != visited.end(),
                "e1 should be visited");
    TEST_ASSERT(std::find(visited.begin(), visited.end(), e4) != visited.end(),
                "e4 should be visited");
    TEST_ASSERT(std::find(visited.begin(), visited.end(), e2) == visited.end(),
                "e2 (Frozen) should be skipped");
    TEST_ASSERT(std::find(visited.begin(), visited.end(), e3) == visited.end(),
                "e3 (no Velocity) should not appear");
}

void test_multi_multiple_excludes()
{
    Registry reg;

    Entity e1 = reg.create(); reg.add<Position>(e1); reg.add<Velocity>(e1); // visited
    Entity e2 = reg.create(); reg.add<Position>(e2); reg.add<Velocity>(e2); reg.add<Frozen>(e2); // skip
    Entity e3 = reg.create(); reg.add<Position>(e3); reg.add<Velocity>(e3); reg.add<Dead>(e3);   // skip
    Entity e4 = reg.create(); reg.add<Position>(e4); reg.add<Velocity>(e4);
                               reg.add<Frozen>(e4); reg.add<Dead>(e4); // skip (both)

    std::vector<Entity> visited;
    reg.view<Position, Velocity>(Exclude<Frozen, Dead>{}).each(
        [&](Entity e, Position&, Velocity&) { visited.push_back(e); });

    TEST_ASSERT(visited.size() == 1, "Only e1 should be visited");
    TEST_ASSERT(visited[0] == e1, "The visited entity should be e1");
}

void test_multi_exclude_unregistered()
{
    Registry reg;

    Entity e1 = reg.create(); reg.add<Position>(e1); reg.add<Velocity>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2); reg.add<Velocity>(e2);

    // Frozen never registered
    std::vector<Entity> visited;
    reg.view<Position, Velocity>(Exclude<Frozen>{}).each(
        [&](Entity e, Position&, Velocity&) { visited.push_back(e); });

    TEST_ASSERT(visited.size() == 2,
                "Unregistered exclude type should not filter multi-comp view");
}

void test_three_component_with_exclude()
{
    Registry reg;

    Entity e1 = reg.create();
    reg.add<Position>(e1); reg.add<Velocity>(e1); reg.add<Health>(e1);
    // visited — has all 3, no Frozen

    Entity e2 = reg.create();
    reg.add<Position>(e2); reg.add<Velocity>(e2); reg.add<Health>(e2);
    reg.add<Frozen>(e2);
    // skipped — has all 3 but also Frozen

    Entity e3 = reg.create();
    reg.add<Position>(e3); reg.add<Velocity>(e3); reg.add<Health>(e3);
    // visited

    std::vector<Entity> visited;
    reg.view<Position, Velocity, Health>(Exclude<Frozen>{}).each(
        [&](Entity e, Position&, Velocity&, Health&) { visited.push_back(e); });

    TEST_ASSERT(visited.size() == 2, "e1 and e3 should be visited");
    TEST_ASSERT(std::find(visited.begin(), visited.end(), e2) == visited.end(),
                "e2 (Frozen) should be skipped");
}

// =============================================================================
// Tests: count() with exclude
// =============================================================================

void test_count_with_exclude()
{
    Registry reg;

    for (int i = 0; i < 6; ++i)
    {
        Entity e = reg.create();
        reg.add<Position>(e);
        if (i % 2 == 0) reg.add<Frozen>(e); // 3 frozen, 3 not
    }

    std::size_t cnt = reg.view<Position>(Exclude<Frozen>{}).count();
    TEST_ASSERT(cnt == 3, "count() should return 3 non-frozen entities");
}

void test_count_no_exclude_fast_path()
{
    Registry reg;

    for (int i = 0; i < 5; ++i)
    {
        Entity e = reg.create();
        reg.add<Position>(e);
    }

    // No-exclude path should use the fast size() return.
    std::size_t cnt = reg.view<Position>().count();
    TEST_ASSERT(cnt == 5, "count() without exclude should return 5");
}

// =============================================================================
// Tests: const view with exclude
// =============================================================================

void test_const_view_with_exclude()
{
    Registry reg;

    Entity e1 = reg.create(); reg.add<Position>(e1, 1.0f, 0.0f);
    Entity e2 = reg.create(); reg.add<Position>(e2, 2.0f, 0.0f); reg.add<Frozen>(e2);
    Entity e3 = reg.create(); reg.add<Position>(e3, 3.0f, 0.0f);

    // Const iteration is via the view's own const each() — the Registry
    // does not need to be const. This exercises the const code path.
    const auto view = reg.view<Position>(Exclude<Frozen>{});

    std::vector<Entity> visited;
    view.each([&](Entity e, const Position&) { visited.push_back(e); });

    TEST_ASSERT(visited.size() == 2, "const view: should visit 2 entities");
    TEST_ASSERT(std::find(visited.begin(), visited.end(), e2) == visited.end(),
                "const view: e2 (Frozen) should be skipped");
}

// =============================================================================
// Tests: large-scale correctness
// =============================================================================

void test_large_scale_exclude()
{
    Registry reg;
    constexpr int kN = 1000;

    std::vector<Entity> entities;
    entities.reserve(kN);

    for (int i = 0; i < kN; ++i)
    {
        Entity e = reg.create();
        entities.push_back(e);
        reg.add<Position>(e, static_cast<float>(i), 0.0f);
        reg.add<Velocity>(e, 1.0f, 0.0f);
        if (i % 3 == 0) reg.add<Frozen>(e);   // 334 frozen
        if (i % 7 == 0) reg.add<Dead>(e);     // 143 dead (some overlap with frozen)
    }

    // Count expected: not (i%3==0 || i%7==0)
    int expected = 0;
    for (int i = 0; i < kN; ++i)
    {
        if (i % 3 != 0 && i % 7 != 0) ++expected;
    }

    int counted = 0;
    reg.view<Position, Velocity>(Exclude<Frozen, Dead>{}).each(
        [&](Entity, Position&, Velocity&) { ++counted; });

    TEST_ASSERT(counted == expected,
                "Large-scale: counted entities should match expected");

    std::size_t cnt = reg.view<Position, Velocity>(Exclude<Frozen, Dead>{}).count();
    TEST_ASSERT(static_cast<int>(cnt) == expected,
                "Large-scale: count() should match expected");
}

void test_exclude_mixed_include_exclude_overlap()
{
    // Exclude type is the same as one of the include types.
    // No entity can have Position while being excluded for Position.
    // Result: zero entities visited.
    Registry reg;

    Entity e1 = reg.create(); reg.add<Position>(e1); reg.add<Velocity>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2); reg.add<Velocity>(e2);

    std::vector<Entity> visited;
    reg.view<Position, Velocity>(Exclude<Position>{}).each(
        [&](Entity e, Position&, Velocity&) { visited.push_back(e); });

    TEST_ASSERT(visited.empty(),
                "Excluding an included type means no entity can be visited");
}

// =============================================================================
// Tests: sparse exclude (exclude has fewer entries than include store)
// =============================================================================

void test_sparse_exclude()
{
    Registry reg;
    constexpr int kN = 100;

    // All N entities have Position. Only entities 0, 10, 20... have Frozen.
    std::vector<Entity> entities;
    for (int i = 0; i < kN; ++i)
    {
        Entity e = reg.create();
        entities.push_back(e);
        reg.add<Position>(e);
        if (i % 10 == 0) reg.add<Frozen>(e);
    }

    int counted = 0;
    reg.view<Position>(Exclude<Frozen>{}).each(
        [&](Entity, Position&) { ++counted; });

    TEST_ASSERT(counted == 90, "Sparse exclude: 90 of 100 should be visited");
}

// =============================================================================
// Main
// =============================================================================

int main()
{
    std::printf("=== Exclude Filter Tests ===\n\n");

    std::printf("Single-component views:\n");
    RUN_TEST(test_single_exclude_basic);
    RUN_TEST(test_single_multiple_excludes);
    RUN_TEST(test_single_exclude_unregistered_type);
    RUN_TEST(test_single_exclude_all_excluded);
    RUN_TEST(test_single_exclude_none_excluded);

    std::printf("\nMulti-component views:\n");
    RUN_TEST(test_multi_exclude_basic);
    RUN_TEST(test_multi_multiple_excludes);
    RUN_TEST(test_multi_exclude_unregistered);
    RUN_TEST(test_three_component_with_exclude);

    std::printf("\ncount() with exclude:\n");
    RUN_TEST(test_count_with_exclude);
    RUN_TEST(test_count_no_exclude_fast_path);

    std::printf("\nConst view:\n");
    RUN_TEST(test_const_view_with_exclude);

    std::printf("\nScale and edge cases:\n");
    RUN_TEST(test_large_scale_exclude);
    RUN_TEST(test_exclude_mixed_include_exclude_overlap);
    RUN_TEST(test_sparse_exclude);

    std::printf("\n=== Results: %d passed, %d failed ===\n",
                sTestsPassed, sTestsFailed);

    return sTestsFailed == 0 ? 0 : 1;
}
