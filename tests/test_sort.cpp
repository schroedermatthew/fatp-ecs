/**
 * @file test_sort.cpp
 * @brief Tests for Registry::sort<T>(comparator) and sort<A, B>().
 *
 * Tests cover:
 *
 * sort<T>(comparator):
 * - Ascending and descending sort by component field
 * - sort<T> on a single entity (no-op, no crash)
 * - sort<T> on empty / unregistered store (no-op)
 * - Sorting preserves component data integrity (get<T>(entity) still correct)
 * - Sorting preserves entity→component mapping (all entities still accessible)
 * - view<T>().each() visits in sorted order after sort
 * - Stable relative order of equal elements (std::stable_sort not guaranteed
 *   here, but we can verify strict orderings)
 * - Large-scale sort (1000 entities, random values)
 *
 * sort<A, B>() (match-sort):
 * - Entities in B appear in A's order after sort<A, B>()
 * - Entities in B but absent from A remain at the tail
 * - Entities in A but absent from B are skipped silently
 * - sort<A, B>() is a no-op when B has <= 1 element
 * - sort<A, B>() when A or B unregistered: no-op
 * - view<A, B>().each() visits in A's sorted order after sort<A, B>()
 * - Large-scale match-sort (1000 entities, partial overlap)
 *
 * Interaction:
 * - sort<T> then view<T>: correct entity+component pairs
 * - sort<A, B> then view<A, B>: both components from same entity per call
 */

#include <fatp_ecs/FatpEcs.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <numeric>
#include <random>
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

struct Value  { int v = 0; };
struct Score  { float s = 0.0f; };
struct Marker { uint8_t id = 0; };

// =============================================================================
// sort<T>(comparator)
// =============================================================================

void test_sort_ascending()
{
    Registry reg;
    // Add in reverse order
    for (int i = 9; i >= 0; --i)
    {
        Entity e = reg.create();
        reg.add<Value>(e, i);
    }

    reg.sort<Value>([](const Value& a, const Value& b) { return a.v < b.v; });

    // Collect iteration order
    std::vector<int> order;
    reg.view<Value>().each([&](Entity, const Value& val) {
        order.push_back(val.v);
    });

    TEST_ASSERT(order.size() == 10, "should have 10 elements");
    for (int i = 0; i < 9; ++i)
    {
        TEST_ASSERT(order[i] <= order[i + 1], "should be ascending");
    }
}

void test_sort_descending()
{
    Registry reg;
    for (int i = 0; i < 10; ++i)
    {
        Entity e = reg.create();
        reg.add<Value>(e, i);
    }

    reg.sort<Value>([](const Value& a, const Value& b) { return a.v > b.v; });

    std::vector<int> order;
    reg.view<Value>().each([&](Entity, const Value& val) {
        order.push_back(val.v);
    });

    TEST_ASSERT(order.size() == 10, "should have 10 elements");
    for (int i = 0; i < 9; ++i)
    {
        TEST_ASSERT(order[i] >= order[i + 1], "should be descending");
    }
}

void test_sort_single_entity_noop()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Value>(e, 42);
    reg.sort<Value>([](const Value& a, const Value& b) { return a.v < b.v; });
    TEST_ASSERT(reg.get<Value>(e).v == 42, "single entity unchanged after sort");
}

void test_sort_empty_store_noop()
{
    Registry reg;
    // Add and immediately destroy to leave an empty (but registered) store
    Entity e = reg.create();
    reg.add<Value>(e, 0);
    reg.destroy(e);

    reg.sort<Value>([](const Value& a, const Value& b) { return a.v < b.v; });
    int count = 0;
    reg.view<Value>().each([&](Entity, Value&) { ++count; });
    TEST_ASSERT(count == 0, "empty store: nothing visited");
}

void test_sort_unregistered_type_noop()
{
    Registry reg;
    // Value never registered — sort should silently no-op
    reg.sort<Value>([](const Value& a, const Value& b) { return a.v < b.v; });
    // No crash = pass
}

void test_sort_preserves_entity_component_mapping()
{
    Registry reg;
    std::vector<std::pair<Entity, int>> pairs;

    for (int i = 9; i >= 0; --i)
    {
        Entity e = reg.create();
        reg.add<Value>(e, i);
        pairs.push_back({e, i});
    }

    reg.sort<Value>([](const Value& a, const Value& b) { return a.v < b.v; });

    // For every entity we created, get<Value> should still return its original value
    for (auto [entity, val] : pairs)
    {
        TEST_ASSERT(reg.get<Value>(entity).v == val,
                    "entity→component mapping preserved after sort");
    }
}

void test_sort_view_entity_component_pairs_correct()
{
    Registry reg;

    Entity e0 = reg.create(); reg.add<Value>(e0, 30);
    Entity e1 = reg.create(); reg.add<Value>(e1, 10);
    Entity e2 = reg.create(); reg.add<Value>(e2, 20);

    reg.sort<Value>([](const Value& a, const Value& b) { return a.v < b.v; });

    std::vector<std::pair<Entity, int>> visited;
    reg.view<Value>().each([&](Entity e, const Value& v) {
        visited.push_back({e, v.v});
    });

    // Should visit in order: (e1,10), (e2,20), (e0,30)
    TEST_ASSERT(visited.size() == 3,       "three entities visited");
    TEST_ASSERT(visited[0] == std::make_pair(e1, 10), "first: e1=10");
    TEST_ASSERT(visited[1] == std::make_pair(e2, 20), "second: e2=20");
    TEST_ASSERT(visited[2] == std::make_pair(e0, 30), "third: e0=30");
}

void test_sort_large_scale()
{
    Registry reg;
    constexpr int kN = 1000;
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 9999);

    for (int i = 0; i < kN; ++i)
    {
        Entity e = reg.create();
        reg.add<Value>(e, dist(rng));
    }

    reg.sort<Value>([](const Value& a, const Value& b) { return a.v < b.v; });

    std::vector<int> order;
    order.reserve(kN);
    reg.view<Value>().each([&](Entity, const Value& v) { order.push_back(v.v); });

    TEST_ASSERT(static_cast<int>(order.size()) == kN, "all entities present");
    TEST_ASSERT(std::is_sorted(order.begin(), order.end()), "sorted ascending");
}

// =============================================================================
// sort<A, B>() — match-sort
// =============================================================================

void test_match_sort_basic()
{
    Registry reg;

    Entity e0 = reg.create(); reg.add<Value>(e0, 30); reg.add<Score>(e0, 3.0f);
    Entity e1 = reg.create(); reg.add<Value>(e1, 10); reg.add<Score>(e1, 1.0f);
    Entity e2 = reg.create(); reg.add<Value>(e2, 20); reg.add<Score>(e2, 2.0f);

    // Sort Value ascending
    reg.sort<Value>([](const Value& a, const Value& b) { return a.v < b.v; });
    // Match Score to Value's order
    reg.sort<Value, Score>();

    // Now view<Value, Score>() should visit in Value's sorted order
    std::vector<std::pair<int, float>> visited;
    reg.view<Value, Score>().each([&](Entity, const Value& v, const Score& s) {
        visited.push_back({v.v, s.s});
    });

    TEST_ASSERT(visited.size() == 3,                       "three entities");
    TEST_ASSERT(visited[0] == std::make_pair(10, 1.0f),    "first: (10, 1.0)");
    TEST_ASSERT(visited[1] == std::make_pair(20, 2.0f),    "second: (20, 2.0)");
    TEST_ASSERT(visited[2] == std::make_pair(30, 3.0f),    "third: (30, 3.0)");
}

void test_match_sort_partial_overlap()
{
    Registry reg;

    // A has 4 entities; B has 3 of them (plus one extra)
    Entity eA = reg.create(); reg.add<Value>(eA, 40);
    Entity eB = reg.create(); reg.add<Value>(eB, 20); reg.add<Score>(eB, 2.0f);
    Entity eC = reg.create(); reg.add<Value>(eC, 10); reg.add<Score>(eC, 1.0f);
    Entity eD = reg.create(); reg.add<Value>(eD, 30); reg.add<Score>(eD, 3.0f);
    Entity eE = reg.create(); /* no Value */            reg.add<Score>(eE, 9.0f);

    // Sort Value ascending: eC(10), eB(20), eD(30), eA(40)
    reg.sort<Value>([](const Value& a, const Value& b) { return a.v < b.v; });
    // Match-sort Score to Value. eA absent from Score → skipped. eE absent from Value → tail.
    reg.sort<Value, Score>();

    // Score order should be: eC, eB, eD (in Value's order), then eE at tail
    std::vector<float> scoreOrder;
    reg.view<Score>().each([&](Entity, const Score& s) {
        scoreOrder.push_back(s.s);
    });

    TEST_ASSERT(scoreOrder.size() == 4, "four Score entities");
    // First three in Value order: 1.0, 2.0, 3.0
    TEST_ASSERT(scoreOrder[0] == 1.0f, "first: eC=1.0");
    TEST_ASSERT(scoreOrder[1] == 2.0f, "second: eB=2.0");
    TEST_ASSERT(scoreOrder[2] == 3.0f, "third: eD=3.0");
    // eE (9.0) lands at tail
    TEST_ASSERT(scoreOrder[3] == 9.0f, "tail: eE=9.0");
}

void test_match_sort_entity_mapping_correct()
{
    Registry reg;

    Entity e0 = reg.create(); reg.add<Value>(e0, 3); reg.add<Score>(e0, 30.0f);
    Entity e1 = reg.create(); reg.add<Value>(e1, 1); reg.add<Score>(e1, 10.0f);
    Entity e2 = reg.create(); reg.add<Value>(e2, 2); reg.add<Score>(e2, 20.0f);

    reg.sort<Value>([](const Value& a, const Value& b) { return a.v < b.v; });
    reg.sort<Value, Score>();

    // Entity mappings must survive sort
    TEST_ASSERT(reg.get<Value>(e0).v  == 3,    "e0 Value still 3");
    TEST_ASSERT(reg.get<Value>(e1).v  == 1,    "e1 Value still 1");
    TEST_ASSERT(reg.get<Value>(e2).v  == 2,    "e2 Value still 2");
    TEST_ASSERT(reg.get<Score>(e0).s  == 30.0f,"e0 Score still 30");
    TEST_ASSERT(reg.get<Score>(e1).s  == 10.0f,"e1 Score still 10");
    TEST_ASSERT(reg.get<Score>(e2).s  == 20.0f,"e2 Score still 20");
}

void test_match_sort_single_b_noop()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Value>(e, 1);
    reg.add<Score>(e, 1.0f);

    // B has 1 element — should be no-op, no crash
    reg.sort<Value, Score>();
    TEST_ASSERT(reg.get<Score>(e).s == 1.0f, "single-element B unchanged");
}

void test_match_sort_unregistered_noop()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Value>(e, 42);

    // Score never registered — should no-op
    reg.sort<Value, Score>();

    // Value never registered — should no-op
    Registry reg2;
    Entity e2 = reg2.create();
    reg2.add<Score>(e2, 1.0f);
    reg2.sort<Value, Score>();
}

void test_match_sort_view_pairs_correct()
{
    Registry reg;

    // Add 5 entities with both Value and Score
    Entity entities[5];
    for (int i = 0; i < 5; ++i)
    {
        entities[i] = reg.create();
        reg.add<Value>(entities[i], 5 - i);        // 5, 4, 3, 2, 1
        reg.add<Score>(entities[i], float(5 - i)); // 5.0, 4.0, 3.0, 2.0, 1.0
    }

    reg.sort<Value>([](const Value& a, const Value& b) { return a.v < b.v; });
    reg.sort<Value, Score>();

    // view<Value, Score>() should now give matching pairs (v == s)
    bool allMatch = true;
    reg.view<Value, Score>().each([&](Entity, const Value& v, const Score& s) {
        if (static_cast<float>(v.v) != s.s) allMatch = false;
    });

    TEST_ASSERT(allMatch, "each view<Value,Score> pair should have v==s after match-sort");
}

void test_match_sort_large_scale()
{
    Registry reg;
    constexpr int kN = 1000;

    // Create kN entities with Value; half also have Score
    std::vector<Entity> entities;
    entities.reserve(kN);
    for (int i = 0; i < kN; ++i)
    {
        Entity e = reg.create();
        entities.push_back(e);
        reg.add<Value>(e, kN - i); // kN, kN-1, ..., 1
        if (i % 2 == 0) reg.add<Score>(e, float(kN - i));
    }

    // Sort Value ascending
    reg.sort<Value>([](const Value& a, const Value& b) { return a.v < b.v; });
    // Match Score to Value order
    reg.sort<Value, Score>();

    // Verify Score is in ascending order (matching Value's sort)
    std::vector<float> scoreOrder;
    reg.view<Score>().each([&](Entity, const Score& s) { scoreOrder.push_back(s.s); });

    TEST_ASSERT(static_cast<int>(scoreOrder.size()) == kN / 2, "kN/2 Score entities");
    TEST_ASSERT(std::is_sorted(scoreOrder.begin(), scoreOrder.end()),
                "Score should be in ascending order after match-sort");
}

// =============================================================================
// Main
// =============================================================================

int main()
{
    std::printf("=== Sort Tests ===\n\n");

    std::printf("sort<T>(comparator):\n");
    RUN_TEST(test_sort_ascending);
    RUN_TEST(test_sort_descending);
    RUN_TEST(test_sort_single_entity_noop);
    RUN_TEST(test_sort_empty_store_noop);
    RUN_TEST(test_sort_unregistered_type_noop);
    RUN_TEST(test_sort_preserves_entity_component_mapping);
    RUN_TEST(test_sort_view_entity_component_pairs_correct);
    RUN_TEST(test_sort_large_scale);

    std::printf("\nsort<A, B>() match-sort:\n");
    RUN_TEST(test_match_sort_basic);
    RUN_TEST(test_match_sort_partial_overlap);
    RUN_TEST(test_match_sort_entity_mapping_correct);
    RUN_TEST(test_match_sort_single_b_noop);
    RUN_TEST(test_match_sort_unregistered_noop);
    RUN_TEST(test_match_sort_view_pairs_correct);
    RUN_TEST(test_match_sort_large_scale);

    std::printf("\n=== Results: %d passed, %d failed ===\n",
                sTestsPassed, sTestsFailed);
    return sTestsFailed == 0 ? 0 : 1;
}
