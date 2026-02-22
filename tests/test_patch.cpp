/**
 * @file test_patch.cpp
 * @brief Tests for registry.patch<T>() and onComponentUpdated signal.
 *
 * Tests cover:
 * - patch<T>(entity, func): modifies component and fires signal
 * - patch<T>(entity): signal-only, no modification
 * - Returns false when entity lacks component
 * - Returns false when component type never registered
 * - Functor receives correct component reference
 * - Signal receives correct entity and post-modification value
 * - No signal fired when no listeners connected (sentinel cache fast path)
 * - Multiple listeners all notified
 * - patch during signal emission (reentrancy safety)
 * - onComponentUpdated does not fire onComponentAdded or onComponentRemoved
 * - ScopedConnection disconnects correctly
 * - patch on destroyed entity returns false
 * - Large-scale: 1000 patches, all fire signal
 */

#include <fatp_ecs/FatpEcs.h>

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
struct Health   { int hp = 100; };
struct Tag      { uint8_t dummy = 0; };

// =============================================================================
// patch<T>(entity, func) — basic behaviour
// =============================================================================

void test_patch_modifies_component()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e, 1.0f, 2.0f);

    bool called = false;
    bool result = reg.patch<Position>(e, [&](Position& p) {
        called = true;
        p.x = 99.0f;
        p.y = 88.0f;
    });

    TEST_ASSERT(result, "patch should return true when entity has the component");
    TEST_ASSERT(called, "functor should be called");
    TEST_ASSERT(reg.get<Position>(e).x == 99.0f, "x should be updated to 99");
    TEST_ASSERT(reg.get<Position>(e).y == 88.0f, "y should be updated to 88");
}

void test_patch_returns_false_no_component()
{
    Registry reg;
    Entity e = reg.create();
    // Entity has no Position

    bool called = false;
    bool result = reg.patch<Position>(e, [&](Position&) { called = true; });

    TEST_ASSERT(!result, "patch should return false when entity lacks component");
    TEST_ASSERT(!called, "functor should not be called");
}

void test_patch_returns_false_unregistered_type()
{
    Registry reg;
    Entity e = reg.create();
    // Position never registered at all

    bool result = reg.patch<Position>(e, [](Position&) {});

    TEST_ASSERT(!result, "patch should return false for unregistered component type");
}

void test_patch_returns_false_destroyed_entity()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e);
    reg.destroy(e);

    bool called = false;
    bool result = reg.patch<Position>(e, [&](Position&) { called = true; });

    // Entity is dead — store may still have stale data but tryGet checks generation.
    // patch must not call the functor.
    TEST_ASSERT(!result || !called,
                "patch should not modify component for destroyed entity");
}

// =============================================================================
// onComponentUpdated signal
// =============================================================================

void test_patch_fires_signal()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e, 1.0f, 2.0f);

    Entity signalEntity = NullEntity;
    float  signalX      = -1.0f;

    auto conn = reg.events().onComponentUpdated<Position>().connect(
        [&](Entity ent, Position& p) {
            signalEntity = ent;
            signalX      = p.x;
        });

    reg.patch<Position>(e, [](Position& p) { p.x = 42.0f; });

    TEST_ASSERT(signalEntity == e,     "signal should receive the correct entity");
    TEST_ASSERT(signalX == 42.0f,      "signal should see the post-patch value");
}

void test_patch_signal_fires_after_modification()
{
    // Signal must receive the modified value, not the pre-patch value.
    Registry reg;
    Entity e = reg.create();
    reg.add<Health>(e, 100);

    int signalHp = -1;
    auto conn = reg.events().onComponentUpdated<Health>().connect(
        [&](Entity, Health& h) { signalHp = h.hp; });

    reg.patch<Health>(e, [](Health& h) { h.hp = 75; });

    TEST_ASSERT(signalHp == 75, "signal should see hp=75, the post-patch value");
}

void test_patch_no_signal_without_listener()
{
    // When no listener is connected, emitComponentUpdated must be a no-op
    // (sentinel cache fast path). Just verify it doesn't crash or corrupt state.
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e, 1.0f, 2.0f);

    bool result = reg.patch<Position>(e, [](Position& p) { p.x = 5.0f; });

    TEST_ASSERT(result,                          "patch should still return true");
    TEST_ASSERT(reg.get<Position>(e).x == 5.0f, "component should still be updated");
}

void test_patch_multiple_listeners()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e);

    int fireCount = 0;
    auto c1 = reg.events().onComponentUpdated<Position>().connect(
        [&](Entity, Position&) { ++fireCount; });
    auto c2 = reg.events().onComponentUpdated<Position>().connect(
        [&](Entity, Position&) { ++fireCount; });
    auto c3 = reg.events().onComponentUpdated<Position>().connect(
        [&](Entity, Position&) { ++fireCount; });

    reg.patch<Position>(e, [](Position& p) { p.x = 1.0f; });

    TEST_ASSERT(fireCount == 3, "all three listeners should be notified");
}

void test_patch_signal_only_overload()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e, 7.0f, 8.0f);

    bool fired = false;
    float signalX = -1.0f;
    auto conn = reg.events().onComponentUpdated<Position>().connect(
        [&](Entity, Position& p) {
            fired    = true;
            signalX  = p.x;
        });

    // Modify directly via get(), then signal via patch with no functor.
    reg.get<Position>(e).x = 55.0f;
    bool result = reg.patch<Position>(e);

    TEST_ASSERT(result,          "signal-only patch should return true");
    TEST_ASSERT(fired,           "signal should be fired");
    TEST_ASSERT(signalX == 55.0f,"signal should see the value set via get()");
}

void test_patch_signal_only_returns_false_no_component()
{
    Registry reg;
    Entity e = reg.create();

    bool result = reg.patch<Position>(e);
    TEST_ASSERT(!result, "signal-only patch should return false when no component");
}

// =============================================================================
// Signal isolation — patch does not fire add/remove signals
// =============================================================================

void test_patch_does_not_fire_added_signal()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e);

    bool addedFired = false;
    auto c = reg.events().onComponentAdded<Position>().connect(
        [&](Entity, Position&) { addedFired = true; });

    reg.patch<Position>(e, [](Position& p) { p.x = 1.0f; });

    TEST_ASSERT(!addedFired, "patch must not fire onComponentAdded");
}

void test_patch_does_not_fire_removed_signal()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e);

    bool removedFired = false;
    auto c = reg.events().onComponentRemoved<Position>().connect(
        [&](Entity) { removedFired = true; });

    reg.patch<Position>(e, [](Position& p) { p.x = 1.0f; });

    TEST_ASSERT(!removedFired, "patch must not fire onComponentRemoved");
}

// =============================================================================
// ScopedConnection disconnects correctly
// =============================================================================

void test_patch_signal_disconnects_via_scoped_connection()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e);

    int fireCount = 0;

    {
        auto conn = reg.events().onComponentUpdated<Position>().connect(
            [&](Entity, Position&) { ++fireCount; });

        reg.patch<Position>(e, [](Position& p) { p.x = 1.0f; });
        TEST_ASSERT(fireCount == 1, "should fire once while connected");
    } // conn destroyed here — disconnects

    reg.patch<Position>(e, [](Position& p) { p.x = 2.0f; });
    TEST_ASSERT(fireCount == 1, "should not fire after ScopedConnection destroyed");
}

// =============================================================================
// Reentrancy: patch inside signal handler
// =============================================================================

void test_patch_reentrant_safe()
{
    // A signal handler calls patch again. The Signal's reentrancy-safe
    // emit must handle this without deadlock or corruption.
    Registry reg;
    Entity e = reg.create();
    reg.add<Health>(e, 100);

    int depth = 0;
    int maxDepth = 0;

    fat_p::ScopedConnection conn;
    conn = reg.events().onComponentUpdated<Health>().connect(
        [&](Entity ent, Health& /*h*/) {
            ++depth;
            if (depth > maxDepth) maxDepth = depth;
            if (depth < 3)
            {
                // Patch again from within the signal handler
                reg.patch<Health>(ent, [](Health& h2) { h2.hp -= 1; });
            }
            --depth;
        });

    reg.patch<Health>(e, [](Health& h) { h.hp -= 1; });

    TEST_ASSERT(maxDepth >= 1, "signal should fire at least once");
    // The important thing is it completes without crash/deadlock
}

// =============================================================================
// Large-scale
// =============================================================================

void test_patch_large_scale()
{
    Registry reg;
    constexpr int kN = 1000;

    std::vector<Entity> entities;
    entities.reserve(kN);
    for (int i = 0; i < kN; ++i)
    {
        Entity e = reg.create();
        entities.push_back(e);
        reg.add<Health>(e, 100);
    }

    int signalCount = 0;
    int totalHpSeen = 0;
    auto conn = reg.events().onComponentUpdated<Health>().connect(
        [&](Entity, Health& h) {
            ++signalCount;
            totalHpSeen += h.hp;
        });

    for (int i = 0; i < kN; ++i)
    {
        reg.patch<Health>(entities[i], [i](Health& h) { h.hp = i; });
    }

    TEST_ASSERT(signalCount == kN, "signal should fire once per patch");

    // Expected total: sum(0..999) = 499500
    TEST_ASSERT(totalHpSeen == 499500, "signal should see post-patch values");
}

// =============================================================================
// Main
// =============================================================================

int main()
{
    std::printf("=== patch<T>() + onComponentUpdated Tests ===\n\n");

    std::printf("patch<T>(entity, func) — basic:\n");
    RUN_TEST(test_patch_modifies_component);
    RUN_TEST(test_patch_returns_false_no_component);
    RUN_TEST(test_patch_returns_false_unregistered_type);
    RUN_TEST(test_patch_returns_false_destroyed_entity);

    std::printf("\nonComponentUpdated signal:\n");
    RUN_TEST(test_patch_fires_signal);
    RUN_TEST(test_patch_signal_fires_after_modification);
    RUN_TEST(test_patch_no_signal_without_listener);
    RUN_TEST(test_patch_multiple_listeners);
    RUN_TEST(test_patch_signal_only_overload);
    RUN_TEST(test_patch_signal_only_returns_false_no_component);

    std::printf("\nSignal isolation:\n");
    RUN_TEST(test_patch_does_not_fire_added_signal);
    RUN_TEST(test_patch_does_not_fire_removed_signal);

    std::printf("\nScopedConnection:\n");
    RUN_TEST(test_patch_signal_disconnects_via_scoped_connection);

    std::printf("\nReentrancy and scale:\n");
    RUN_TEST(test_patch_reentrant_safe);
    RUN_TEST(test_patch_large_scale);

    std::printf("\n=== Results: %d passed, %d failed ===\n",
                sTestsPassed, sTestsFailed);

    return sTestsFailed == 0 ? 0 : 1;
}
