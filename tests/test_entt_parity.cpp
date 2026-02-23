/**
 * @file test_entt_parity.cpp
 * @brief Tests for EnTT-parity API additions.
 *
 * Covers:
 *   - get_or_emplace<T>()
 *   - all_of<Ts...>()
 *   - any_of<Ts...>()
 *   - none_of<Ts...>()
 *   - each(func)
 *   - orphans(func)
 */

#include <cassert>
#include <cstdio>
#include <vector>

#include <fatp_ecs/FatpEcs.h>

static int gPassed = 0;
static int gFailed = 0;

#define TEST_ASSERT(cond, msg)                                        \
    do {                                                              \
        if (!(cond)) {                                                \
            std::printf("  FAIL: %s (line %d)\n", msg, __LINE__);    \
            ++gFailed; return;                                        \
        }                                                             \
    } while (false)

// Wrap conditions with template args in parens at call site, or use this alias
#define CHECK(...) (__VA_ARGS__)

#define RUN_TEST(fn)                                    \
    do {                                                \
        std::printf("  %s ... ", #fn);                  \
        int before = gFailed;                           \
        fn();                                           \
        if (gFailed == before) { ++gPassed; std::printf("OK\n"); } \
    } while (false)

struct Position { float x, y; };
struct Velocity { float dx, dy; };
struct Health   { int hp; };
struct Frozen   {};

// =============================================================================
// get_or_emplace
// =============================================================================

static void test_get_or_emplace_creates_when_missing()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    auto& pos = reg.get_or_emplace<Position>(e, 1.f, 2.f);
    TEST_ASSERT(pos.x == 1.f, "x should be 1");
    TEST_ASSERT(pos.y == 2.f, "y should be 2");
    TEST_ASSERT(reg.has<Position>(e), "entity should now have Position");
}

static void test_get_or_emplace_returns_existing()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    reg.add<Position>(e, Position{5.f, 6.f});
    auto& pos = reg.get_or_emplace<Position>(e, 99.f, 99.f); // should NOT overwrite
    TEST_ASSERT(pos.x == 5.f, "existing x should be unchanged");
    TEST_ASSERT(pos.y == 6.f, "existing y should be unchanged");
}

static void test_get_or_emplace_fires_event_only_on_create()
{
    fatp_ecs::Registry reg;
    int addedCount = 0;
    auto conn = reg.events().onComponentAdded<Position>().connect(
        [&](fatp_ecs::Entity, Position&) { ++addedCount; });

    auto e = reg.create();
    reg.get_or_emplace<Position>(e, 1.f, 1.f);  // should fire
    reg.get_or_emplace<Position>(e, 2.f, 2.f);  // should NOT fire again
    TEST_ASSERT(addedCount == 1, "onComponentAdded should fire exactly once");
}

static void test_get_or_emplace_reference_is_live()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    auto& pos = reg.get_or_emplace<Position>(e, 0.f, 0.f);
    pos.x = 42.f;
    TEST_ASSERT(reg.get<Position>(e).x == 42.f, "mutation via reference should persist");
}

// =============================================================================
// all_of / any_of / none_of
// =============================================================================

static void test_all_of_true_when_all_present()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    reg.add<Position>(e, Position{});
    reg.add<Velocity>(e, Velocity{});
    TEST_ASSERT(CHECK(reg.all_of<Position, Velocity>(e)), "all_of should be true");
}

static void test_all_of_false_when_one_missing()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    reg.add<Position>(e, Position{});
    TEST_ASSERT(CHECK(!reg.all_of<Position, Velocity>(e)), "all_of false when Velocity missing");
}

static void test_all_of_single_type()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    TEST_ASSERT(!reg.all_of<Position>(e), "false before add");
    reg.add<Position>(e, Position{});
    TEST_ASSERT(reg.all_of<Position>(e),  "true after add");
}

static void test_any_of_true_when_one_present()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    reg.add<Frozen>(e, Frozen{});
    TEST_ASSERT(CHECK(reg.any_of<Velocity, Frozen>(e)), "any_of true when Frozen present");
}

static void test_any_of_false_when_none_present()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    reg.add<Position>(e, Position{});
    TEST_ASSERT(CHECK(!reg.any_of<Velocity, Frozen>(e)), "any_of false when neither present");
}

static void test_none_of_true_when_none_present()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    reg.add<Position>(e, Position{});
    TEST_ASSERT(CHECK(reg.none_of<Velocity, Frozen>(e)), "none_of true when neither present");
}

static void test_none_of_false_when_one_present()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    reg.add<Frozen>(e, Frozen{});
    TEST_ASSERT(CHECK(!reg.none_of<Velocity, Frozen>(e)), "none_of false when Frozen present");
}

// =============================================================================
// each
// =============================================================================

static void test_each_visits_all_entities()
{
    fatp_ecs::Registry reg;
    auto e1 = reg.create();
    auto e2 = reg.create();
    auto e3 = reg.create();

    std::vector<fatp_ecs::Entity> visited;
    reg.each([&](fatp_ecs::Entity e) { visited.push_back(e); });

    TEST_ASSERT(visited.size() == 3, "each should visit 3 entities");
    auto contains = [&](fatp_ecs::Entity e) {
        return std::find(visited.begin(), visited.end(), e) != visited.end();
    };
    TEST_ASSERT(contains(e1), "e1 should be visited");
    TEST_ASSERT(contains(e2), "e2 should be visited");
    TEST_ASSERT(contains(e3), "e3 should be visited");
}

static void test_each_empty_registry()
{
    fatp_ecs::Registry reg;
    int count = 0;
    reg.each([&](fatp_ecs::Entity) { ++count; });
    TEST_ASSERT(count == 0, "each on empty registry visits 0 entities");
}

static void test_each_skips_destroyed_entities()
{
    fatp_ecs::Registry reg;
    auto e1 = reg.create();
    auto e2 = reg.create();
    auto e3 = reg.create();
    reg.destroy(e2);

    int count = 0;
    std::vector<fatp_ecs::Entity> visited;
    reg.each([&](fatp_ecs::Entity e) { ++count; visited.push_back(e); });

    TEST_ASSERT(count == 2, "each skips destroyed entity");
    auto contains = [&](fatp_ecs::Entity e) {
        return std::find(visited.begin(), visited.end(), e) != visited.end();
    };
    TEST_ASSERT(contains(e1),  "e1 visited");
    TEST_ASSERT(!contains(e2), "destroyed e2 NOT visited");
    TEST_ASSERT(contains(e3),  "e3 visited");
}

// =============================================================================
// orphans
// =============================================================================

static void test_orphans_finds_componentless_entities()
{
    fatp_ecs::Registry reg;
    auto e1 = reg.create();  // orphan
    auto e2 = reg.create();
    auto e3 = reg.create();  // orphan

    reg.add<Position>(e2, Position{});

    std::vector<fatp_ecs::Entity> orphanList;
    reg.orphans([&](fatp_ecs::Entity e) { orphanList.push_back(e); });

    TEST_ASSERT(orphanList.size() == 2, "should find 2 orphans");
    auto contains = [&](fatp_ecs::Entity e) {
        return std::find(orphanList.begin(), orphanList.end(), e) != orphanList.end();
    };
    TEST_ASSERT(contains(e1),  "e1 is an orphan");
    TEST_ASSERT(!contains(e2), "e2 has Position, not an orphan");
    TEST_ASSERT(contains(e3),  "e3 is an orphan");
}

static void test_orphans_empty_when_all_have_components()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    reg.add<Position>(e, Position{});

    int count = 0;
    reg.orphans([&](fatp_ecs::Entity) { ++count; });
    TEST_ASSERT(count == 0, "no orphans when all entities have components");
}

static void test_orphans_all_when_no_components_registered()
{
    fatp_ecs::Registry reg;
    (void)reg.create();
    (void)reg.create();

    int count = 0;
    reg.orphans([&](fatp_ecs::Entity) { ++count; });
    TEST_ASSERT(count == 2, "all entities are orphans when no components registered");
}

// =============================================================================
// Main
// =============================================================================

int main()
{
    std::printf("=== EnTT Parity API Tests ===\n\n");

    std::printf("[get_or_emplace]\n");
    RUN_TEST(test_get_or_emplace_creates_when_missing);
    RUN_TEST(test_get_or_emplace_returns_existing);
    RUN_TEST(test_get_or_emplace_fires_event_only_on_create);
    RUN_TEST(test_get_or_emplace_reference_is_live);

    std::printf("\n[all_of / any_of / none_of]\n");
    RUN_TEST(test_all_of_true_when_all_present);
    RUN_TEST(test_all_of_false_when_one_missing);
    RUN_TEST(test_all_of_single_type);
    RUN_TEST(test_any_of_true_when_one_present);
    RUN_TEST(test_any_of_false_when_none_present);
    RUN_TEST(test_none_of_true_when_none_present);
    RUN_TEST(test_none_of_false_when_one_present);

    std::printf("\n[each]\n");
    RUN_TEST(test_each_visits_all_entities);
    RUN_TEST(test_each_empty_registry);
    RUN_TEST(test_each_skips_destroyed_entities);

    std::printf("\n[orphans]\n");
    RUN_TEST(test_orphans_finds_componentless_entities);
    RUN_TEST(test_orphans_empty_when_all_have_components);
    RUN_TEST(test_orphans_all_when_no_components_registered);

    std::printf("\n=== Results: %d passed, %d failed ===\n", gPassed, gFailed);
    return gFailed > 0 ? 1 : 0;
}
