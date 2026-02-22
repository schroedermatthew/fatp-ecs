/**
 * @file test_observer.cpp
 * @brief Tests for Observer — reactive dirty-entity accumulation.
 *
 * Tests cover:
 * - OnAdded<T>: entity dirtied when T is added
 * - OnRemoved<T>: entity dirtied when T is removed
 * - OnUpdated<T>: entity dirtied when T is patched
 * - Mixed triggers on same observer (OnAdded + OnUpdated)
 * - Multiple component types observed
 * - Dirty set deduplication (entity patched N times appears once)
 * - clear() resets the dirty set
 * - each() iterates dirty entities correctly
 * - count() and empty() accessors
 * - Entity destroyed before processing: removed from dirty set
 * - Entity destroyed after dirtying: not visible after destroy
 * - Observer destroyed: connections disconnected (no dangling callbacks)
 * - No cross-contamination between two independent observers
 * - Observer with no triggers: always empty
 * - Large-scale: 1000 entities, verify dirty set matches expected
 * - Sequence: add → patch → remove → clear → verify each step
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

struct Position { float x = 0.0f; float y = 0.0f; };
struct Velocity { float dx = 0.0f; float dy = 0.0f; };
struct Health   { int hp = 100; };

// =============================================================================
// Helpers
// =============================================================================

static std::vector<Entity> collectDirty(Observer& obs)
{
    std::vector<Entity> result;
    obs.each([&](Entity e) { result.push_back(e); });
    return result;
}

static bool contains(const std::vector<Entity>& v, Entity e)
{
    return std::find(v.begin(), v.end(), e) != v.end();
}

// =============================================================================
// OnAdded<T>
// =============================================================================

void test_onadded_basic()
{
    Registry reg;
    auto obs = reg.observe(OnAdded<Position>{});

    Entity e1 = reg.create(); reg.add<Position>(e1);
    Entity e2 = reg.create(); reg.add<Velocity>(e2); // no Position
    Entity e3 = reg.create(); reg.add<Position>(e3);

    auto dirty = collectDirty(obs);
    TEST_ASSERT(dirty.size() == 2,    "two entities with Position added");
    TEST_ASSERT(contains(dirty, e1),  "e1 should be dirty");
    TEST_ASSERT(contains(dirty, e3),  "e3 should be dirty");
    TEST_ASSERT(!contains(dirty, e2), "e2 (no Position) should not be dirty");
}

void test_onadded_does_not_fire_on_patch()
{
    Registry reg;
    auto obs = reg.observe(OnAdded<Position>{});
    Entity e = reg.create(); reg.add<Position>(e);
    obs.clear();

    reg.patch<Position>(e, [](Position& p) { p.x = 1.0f; });

    TEST_ASSERT(obs.empty(), "patch should not trigger OnAdded observer");
}

void test_onadded_does_not_fire_on_remove()
{
    Registry reg;
    auto obs = reg.observe(OnAdded<Position>{});
    Entity e = reg.create(); reg.add<Position>(e);
    obs.clear();

    reg.remove<Position>(e);

    TEST_ASSERT(obs.empty(), "remove should not trigger OnAdded observer");
}

// =============================================================================
// OnRemoved<T>
// =============================================================================

void test_onremoved_basic()
{
    Registry reg;
    auto obs = reg.observe(OnRemoved<Position>{});

    Entity e1 = reg.create(); reg.add<Position>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2);
    Entity e3 = reg.create(); reg.add<Position>(e3);

    TEST_ASSERT(obs.empty(), "no removes yet");

    reg.remove<Position>(e2);

    auto dirty = collectDirty(obs);
    TEST_ASSERT(dirty.size() == 1,   "only e2 was removed");
    TEST_ASSERT(contains(dirty, e2), "e2 should be dirty");
}

void test_onremoved_does_not_fire_on_add()
{
    Registry reg;
    auto obs = reg.observe(OnRemoved<Position>{});
    [[maybe_unused]] auto unused = reg.create(); // make some activity

    Entity e = reg.create(); reg.add<Position>(e);

    TEST_ASSERT(obs.empty(), "add should not trigger OnRemoved observer");
}

// =============================================================================
// OnUpdated<T>
// =============================================================================

void test_onupdated_basic()
{
    Registry reg;
    auto obs = reg.observe(OnUpdated<Position>{});

    Entity e1 = reg.create(); reg.add<Position>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2);

    TEST_ASSERT(obs.empty(), "add should not trigger OnUpdated");

    reg.patch<Position>(e1, [](Position& p) { p.x = 5.0f; });

    auto dirty = collectDirty(obs);
    TEST_ASSERT(dirty.size() == 1,   "only e1 was patched");
    TEST_ASSERT(contains(dirty, e1), "e1 should be dirty");
}

void test_onupdated_signal_only_patch()
{
    Registry reg;
    auto obs = reg.observe(OnUpdated<Position>{});
    Entity e = reg.create(); reg.add<Position>(e);
    obs.clear();

    // Signal-only patch (no functor)
    reg.patch<Position>(e);

    TEST_ASSERT(obs.count() == 1,   "signal-only patch should dirty e");
}

// =============================================================================
// Deduplication
// =============================================================================

void test_deduplication_multiple_patches()
{
    Registry reg;
    auto obs = reg.observe(OnUpdated<Position>{});
    Entity e = reg.create(); reg.add<Position>(e);
    obs.clear();

    // Patch the same entity multiple times
    reg.patch<Position>(e, [](Position& p) { p.x = 1.0f; });
    reg.patch<Position>(e, [](Position& p) { p.x = 2.0f; });
    reg.patch<Position>(e, [](Position& p) { p.x = 3.0f; });

    TEST_ASSERT(obs.count() == 1, "entity patched 3x should appear once in dirty set");
}

void test_deduplication_add_and_update()
{
    Registry reg;
    auto obs = reg.observe(OnAdded<Position>{}, OnUpdated<Position>{});

    Entity e = reg.create();
    reg.add<Position>(e);          // dirty via OnAdded
    reg.patch<Position>(e);        // would dirty again via OnUpdated — deduplicated

    TEST_ASSERT(obs.count() == 1, "add + patch of same entity should appear once");
}

// =============================================================================
// Mixed triggers
// =============================================================================

void test_mixed_triggers_added_and_updated()
{
    Registry reg;
    auto obs = reg.observe(OnAdded<Position>{}, OnUpdated<Position>{});

    Entity e1 = reg.create(); reg.add<Position>(e1);    // dirty via OnAdded
    Entity e2 = reg.create(); reg.add<Position>(e2);    // dirty via OnAdded
    obs.clear();

    reg.patch<Position>(e1, [](Position& p) { p.x = 1.0f; }); // dirty via OnUpdated

    auto dirty = collectDirty(obs);
    TEST_ASSERT(dirty.size() == 1,   "only e1 was patched after clear");
    TEST_ASSERT(contains(dirty, e1), "e1 should be dirty");
}

void test_mixed_triggers_added_removed_updated()
{
    Registry reg;
    auto obs = reg.observe(OnAdded<Position>{}, OnRemoved<Position>{}, OnUpdated<Position>{});

    Entity e1 = reg.create(); reg.add<Position>(e1);       // dirty (added)
    Entity e2 = reg.create(); reg.add<Position>(e2);       // dirty (added)
    reg.remove<Position>(e1);                              // dirty again (removed) — dedup
    reg.patch<Position>(e2);                               // dirty again (updated) — dedup

    TEST_ASSERT(obs.count() == 2, "two distinct entities dirtied");
}

// =============================================================================
// Multiple component types
// =============================================================================

void test_multiple_component_types()
{
    Registry reg;
    auto obs = reg.observe(OnUpdated<Position>{}, OnUpdated<Velocity>{});

    Entity e1 = reg.create(); reg.add<Position>(e1); reg.add<Velocity>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2);
    obs.clear();

    reg.patch<Position>(e1);   // dirty e1
    reg.patch<Velocity>(e1);   // dirty e1 again — dedup
    reg.patch<Position>(e2);   // dirty e2

    TEST_ASSERT(obs.count() == 2, "e1 and e2 should be dirty");
}

// =============================================================================
// clear()
// =============================================================================

void test_clear_resets_dirty_set()
{
    Registry reg;
    auto obs = reg.observe(OnAdded<Position>{});

    Entity e = reg.create(); reg.add<Position>(e);
    TEST_ASSERT(obs.count() == 1, "one dirty entity before clear");

    obs.clear();
    TEST_ASSERT(obs.empty(), "dirty set should be empty after clear");

    // New events should accumulate fresh
    Entity e2 = reg.create(); reg.add<Position>(e2);
    TEST_ASSERT(obs.count() == 1,   "one new dirty entity after clear");
    TEST_ASSERT(!contains(collectDirty(obs), e), "old entity should not reappear");
}

// =============================================================================
// count() and empty()
// =============================================================================

void test_count_and_empty()
{
    Registry reg;
    auto obs = reg.observe(OnAdded<Position>{});

    TEST_ASSERT(obs.empty(),      "fresh observer should be empty");
    TEST_ASSERT(obs.count() == 0, "fresh observer count should be 0");

    Entity e1 = reg.create(); reg.add<Position>(e1);
    TEST_ASSERT(!obs.empty(),     "after add: not empty");
    TEST_ASSERT(obs.count() == 1, "after add: count is 1");

    Entity e2 = reg.create(); reg.add<Position>(e2);
    TEST_ASSERT(obs.count() == 2, "after second add: count is 2");
}

// =============================================================================
// Entity destruction
// =============================================================================

void test_destroyed_entity_removed_from_dirty_set()
{
    Registry reg;
    auto obs = reg.observe(OnAdded<Position>{});

    Entity e1 = reg.create(); reg.add<Position>(e1); // dirty
    Entity e2 = reg.create(); reg.add<Position>(e2); // dirty

    // Destroy e1 before we process the observer
    reg.destroy(e1);

    auto dirty = collectDirty(obs);
    TEST_ASSERT(dirty.size() == 1,    "e1 should have been removed from dirty set");
    TEST_ASSERT(contains(dirty, e2),  "e2 should still be dirty");
    TEST_ASSERT(!contains(dirty, e1), "destroyed e1 should not appear");
}

void test_entity_dirtied_then_destroyed_then_cleared()
{
    Registry reg;
    auto obs = reg.observe(OnAdded<Position>{});

    Entity e = reg.create(); reg.add<Position>(e);
    reg.destroy(e);

    TEST_ASSERT(obs.empty(), "dirty set should be empty after entity destroyed");
    obs.clear(); // should be safe even when already empty
    TEST_ASSERT(obs.empty(), "still empty after clear");
}

// =============================================================================
// Observer lifetime / disconnection
// =============================================================================

void test_observer_destruction_disconnects()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e, 1.0f, 2.0f);

    {
        auto obs = reg.observe(OnUpdated<Position>{});
        reg.patch<Position>(e);
        TEST_ASSERT(obs.count() == 1, "should be dirty while observer alive");
    }
    // obs destroyed here — connections disconnected

    // This patch should not cause a dangling callback
    reg.patch<Position>(e);
    // If we reach here without crash, the test passes.
}

// =============================================================================
// No cross-contamination between observers
// =============================================================================

void test_independent_observers()
{
    Registry reg;
    auto obs1 = reg.observe(OnAdded<Position>{});
    auto obs2 = reg.observe(OnUpdated<Position>{});

    Entity e = reg.create(); reg.add<Position>(e);

    TEST_ASSERT(obs1.count() == 1, "obs1 (OnAdded) should see the add");
    TEST_ASSERT(obs2.empty(),      "obs2 (OnUpdated) should not see the add");

    obs1.clear();
    reg.patch<Position>(e);

    TEST_ASSERT(obs1.empty(),      "obs1 (OnAdded) should not see the patch");
    TEST_ASSERT(obs2.count() == 1, "obs2 (OnUpdated) should see the patch");
}

// =============================================================================
// Observer with no triggers
// =============================================================================

void test_empty_observer()
{
    Registry reg;
    auto obs = reg.observe(); // no triggers

    Entity e = reg.create();
    reg.add<Position>(e);
    reg.patch<Position>(e);
    reg.remove<Position>(e);

    TEST_ASSERT(obs.empty(), "observer with no triggers should always be empty");
}

// =============================================================================
// Large scale
// =============================================================================

void test_large_scale_observer()
{
    Registry reg;
    auto obs = reg.observe(OnAdded<Position>{}, OnUpdated<Position>{});

    constexpr int kN = 1000;
    std::vector<Entity> entities;
    entities.reserve(kN);

    for (int i = 0; i < kN; ++i)
    {
        Entity e = reg.create();
        entities.push_back(e);
        reg.add<Position>(e); // all kN dirtied via OnAdded
    }

    TEST_ASSERT(obs.count() == kN, "all kN entities should be dirty after add");
    obs.clear();

    // Patch every other entity
    for (int i = 0; i < kN; i += 2)
    {
        reg.patch<Position>(entities[i], [i](Position& p) { p.x = static_cast<float>(i); });
    }

    TEST_ASSERT(obs.count() == kN / 2, "kN/2 entities should be dirty after patches");
}

// =============================================================================
// Sequence test: add → clear → patch → clear → remove → clear
// =============================================================================

void test_sequence()
{
    Registry reg;
    auto obs = reg.observe(OnAdded<Health>{}, OnUpdated<Health>{}, OnRemoved<Health>{});

    Entity e = reg.create();

    // Step 1: add
    reg.add<Health>(e, 100);
    TEST_ASSERT(obs.count() == 1, "dirty after add");
    obs.clear();
    TEST_ASSERT(obs.empty(), "clear after add");

    // Step 2: patch
    reg.patch<Health>(e, [](Health& h) { h.hp -= 20; });
    TEST_ASSERT(obs.count() == 1, "dirty after patch");
    obs.clear();

    // Step 3: remove
    reg.remove<Health>(e);
    TEST_ASSERT(obs.count() == 1, "dirty after remove");
    obs.clear();
    TEST_ASSERT(obs.empty(), "clean after final clear");
}

// =============================================================================
// Main
// =============================================================================

int main()
{
    std::printf("=== Observer Tests ===\n\n");

    std::printf("OnAdded<T>:\n");
    RUN_TEST(test_onadded_basic);
    RUN_TEST(test_onadded_does_not_fire_on_patch);
    RUN_TEST(test_onadded_does_not_fire_on_remove);

    std::printf("\nOnRemoved<T>:\n");
    RUN_TEST(test_onremoved_basic);
    RUN_TEST(test_onremoved_does_not_fire_on_add);

    std::printf("\nOnUpdated<T>:\n");
    RUN_TEST(test_onupdated_basic);
    RUN_TEST(test_onupdated_signal_only_patch);

    std::printf("\nDeduplication:\n");
    RUN_TEST(test_deduplication_multiple_patches);
    RUN_TEST(test_deduplication_add_and_update);

    std::printf("\nMixed triggers:\n");
    RUN_TEST(test_mixed_triggers_added_and_updated);
    RUN_TEST(test_mixed_triggers_added_removed_updated);
    RUN_TEST(test_multiple_component_types);

    std::printf("\nclear(), count(), empty():\n");
    RUN_TEST(test_clear_resets_dirty_set);
    RUN_TEST(test_count_and_empty);

    std::printf("\nEntity destruction:\n");
    RUN_TEST(test_destroyed_entity_removed_from_dirty_set);
    RUN_TEST(test_entity_dirtied_then_destroyed_then_cleared);

    std::printf("\nObserver lifetime:\n");
    RUN_TEST(test_observer_destruction_disconnects);

    std::printf("\nIsolation and edge cases:\n");
    RUN_TEST(test_independent_observers);
    RUN_TEST(test_empty_observer);

    std::printf("\nScale and sequence:\n");
    RUN_TEST(test_large_scale_observer);
    RUN_TEST(test_sequence);

    std::printf("\n=== Results: %d passed, %d failed ===\n",
                sTestsPassed, sTestsFailed);

    return sTestsFailed == 0 ? 0 : 1;
}
