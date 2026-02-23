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
// valid() / alive() / contains() / emplace() / erase() / try_get()
// =============================================================================

static void test_valid_alias()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    TEST_ASSERT(reg.valid(e), "valid() true for live entity");
    reg.destroy(e);
    TEST_ASSERT(!reg.valid(e), "valid() false after destroy");
}

static void test_alive_alias()
{
    fatp_ecs::Registry reg;
    TEST_ASSERT(reg.alive() == 0, "alive() 0 on empty registry");
    (void)reg.create(); (void)reg.create();
    TEST_ASSERT(reg.alive() == 2, "alive() returns entity count");
}

static void test_contains_alias()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    TEST_ASSERT(!CHECK(reg.contains<Position>(e)), "contains false before add");
    reg.add<Position>(e, Position{});
    TEST_ASSERT(CHECK(reg.contains<Position>(e)),  "contains true after add");
}

static void test_emplace_alias()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    auto& pos = reg.emplace<Position>(e, Position{3.f, 4.f});
    TEST_ASSERT(pos.x == 3.f, "emplace alias constructs component");
    TEST_ASSERT(CHECK(reg.has<Position>(e)), "entity has component after emplace");
}

static void test_erase_removes_component()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    reg.add<Position>(e, Position{});
    reg.erase<Position>(e);
    TEST_ASSERT(!CHECK(reg.has<Position>(e)), "erase removes component");
}

static void test_try_get_alias()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    TEST_ASSERT(CHECK(reg.try_get<Position>(e)) == nullptr, "try_get null when missing");
    reg.add<Position>(e, Position{7.f, 8.f});
    auto* p = reg.try_get<Position>(e);
    TEST_ASSERT(p != nullptr,  "try_get non-null when present");
    TEST_ASSERT(p->x == 7.f,   "try_get returns correct pointer");
}

// =============================================================================
// clear<T>()
// =============================================================================

static void test_clear_single_type()
{
    fatp_ecs::Registry reg;
    auto e1 = reg.create(); auto e2 = reg.create(); auto e3 = reg.create();
    reg.add<Position>(e1, Position{}); reg.add<Position>(e2, Position{});
    reg.add<Velocity>(e3, Velocity{});

    reg.clear<Position>();

    TEST_ASSERT(!CHECK(reg.has<Position>(e1)), "e1 Position cleared");
    TEST_ASSERT(!CHECK(reg.has<Position>(e2)), "e2 Position cleared");
    TEST_ASSERT(CHECK(reg.has<Velocity>(e3)),  "e3 Velocity unaffected");
    TEST_ASSERT(reg.valid(e1) && reg.valid(e2), "entities still alive after clear<T>");
}

static void test_clear_single_type_fires_events()
{
    fatp_ecs::Registry reg;
    int removedCount = 0;
    auto conn = reg.events().onComponentRemoved<Position>().connect(
        [&](fatp_ecs::Entity) { ++removedCount; });

    auto e1 = reg.create(); auto e2 = reg.create();
    reg.add<Position>(e1, Position{}); reg.add<Position>(e2, Position{});

    reg.clear<Position>();
    TEST_ASSERT(removedCount == 2, "clear<T> fires onComponentRemoved for each entity");
}

static void test_clear_unregistered_type_is_noop()
{
    fatp_ecs::Registry reg;
    (void)reg.create();
    // Position never added — should not crash
    reg.clear<Position>();
    TEST_ASSERT(reg.alive() == 1, "entity survives clear of unregistered type");
}

// =============================================================================
// storage<T>()
// =============================================================================

static void test_storage_null_when_unregistered()
{
    fatp_ecs::Registry reg;
    TEST_ASSERT(reg.storage<Position>() == nullptr, "storage null for unregistered type");
}

static void test_storage_non_null_after_add()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    reg.add<Position>(e, Position{1.f, 2.f});
    auto* s = reg.storage<Position>();
    TEST_ASSERT(s != nullptr,    "storage non-null after add");
    TEST_ASSERT(s->size() == 1,  "storage reflects correct size");
}

static void test_storage_dense_matches_component()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    reg.add<Position>(e, Position{5.f, 6.f});
    auto* s = reg.storage<Position>();
    TEST_ASSERT(s->dense()[0] == e, "storage dense array contains entity");
}

// =============================================================================
// on_construct / on_destroy / on_update
// =============================================================================

static void test_on_construct_alias()
{
    fatp_ecs::Registry reg;
    int count = 0;
    auto conn = reg.on_construct<Position>().connect(
        [&](fatp_ecs::Entity, Position&) { ++count; });
    auto e = reg.create();
    reg.add<Position>(e, Position{});
    TEST_ASSERT(count == 1, "on_construct fires on add");
}

static void test_on_destroy_alias()
{
    fatp_ecs::Registry reg;
    int count = 0;
    auto conn = reg.on_destroy<Position>().connect(
        [&](fatp_ecs::Entity) { ++count; });
    auto e = reg.create();
    reg.add<Position>(e, Position{});
    reg.remove<Position>(e);
    TEST_ASSERT(count == 1, "on_destroy fires on remove");
}

static void test_on_update_alias()
{
    fatp_ecs::Registry reg;
    int count = 0;
    auto conn = reg.on_update<Position>().connect(
        [&](fatp_ecs::Entity, Position&) { ++count; });
    auto e = reg.create();
    reg.add<Position>(e, Position{});
    reg.patch<Position>(e, [](Position& p) { p.x = 1.f; });
    TEST_ASSERT(count == 1, "on_update fires on patch");
}

// =============================================================================
// group_if_exists / non_owning_group_if_exists
// =============================================================================

static void test_group_if_exists_null_before_create()
{
    fatp_ecs::Registry reg;
    auto* grp = reg.group_if_exists<Position, Velocity>();
    TEST_ASSERT(grp == nullptr, "group_if_exists null before group() called");
}

static void test_group_if_exists_non_null_after_create()
{
    fatp_ecs::Registry reg;
(void)reg.group<Position, Velocity>();
    auto* grp = reg.group_if_exists<Position, Velocity>();
    TEST_ASSERT(grp != nullptr, "group_if_exists non-null after group() called");
}

static void test_group_if_exists_is_same_instance()
{
    fatp_ecs::Registry reg;
    auto& grp1 = reg.group<Position, Velocity>();
    auto* grp2 = reg.group_if_exists<Position, Velocity>();
    TEST_ASSERT(&grp1 == grp2, "group_if_exists returns same instance as group()");
}

static void test_group_if_exists_wrong_types_null()
{
    fatp_ecs::Registry reg;
(void)reg.group<Position, Velocity>();
    // Health was never grouped
    auto* grp = reg.group_if_exists<Position, Health>();
    TEST_ASSERT(grp == nullptr, "group_if_exists null for different type set");
}

static void test_non_owning_group_if_exists_null_before_create()
{
    fatp_ecs::Registry reg;
    auto* grp = reg.non_owning_group_if_exists<Position, Velocity>();
    TEST_ASSERT(grp == nullptr, "non_owning_group_if_exists null before creation");
}

static void test_non_owning_group_if_exists_non_null_after_create()
{
    fatp_ecs::Registry reg;
(void)reg.non_owning_group<Position, Velocity>();
    auto* grp = reg.non_owning_group_if_exists<Position, Velocity>();
    TEST_ASSERT(grp != nullptr, "non_owning_group_if_exists non-null after creation");
}

static void test_group_if_exists_does_not_create()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    reg.add<Position>(e, Position{1.f, 2.f});
    reg.add<Velocity>(e, Velocity{});

    // Speculative lookup — must not create or assert
    auto* grp = reg.group_if_exists<Position, Velocity>();
    TEST_ASSERT(grp == nullptr, "group_if_exists does not create group");

    // Now create — ownership must still be available
    auto& created = reg.group<Position, Velocity>();
    TEST_ASSERT(created.size() == 1, "group created normally after if_exists miss");
}

// =============================================================================
// create(hint)
// =============================================================================

static void test_create_hint_honours_free_slot()
{
    fatp_ecs::Registry reg;
    auto original = reg.create();
    reg.destroy(original);

    auto restored = reg.create(original);
    // Same slot index
    TEST_ASSERT(fatp_ecs::EntityTraits::index(restored) ==
                fatp_ecs::EntityTraits::index(original),
                "hint slot index should match");
}

static void test_create_hint_different_generation()
{
    fatp_ecs::Registry reg;
    auto original = reg.create();
    reg.destroy(original);

    auto restored = reg.create(original);
    // Different generation — old handle must be dead
    TEST_ASSERT(restored != original, "restored entity is a new handle");
    TEST_ASSERT(!reg.valid(original), "original handle still invalid");
    TEST_ASSERT(reg.valid(restored),  "restored handle is valid");
}

static void test_create_hint_fallback_when_occupied()
{
    fatp_ecs::Registry reg;
    auto occupant = reg.create();   // slot 0 occupied
    auto hint     = occupant;       // hint points at that same slot

    auto result = reg.create(hint); // must NOT evict occupant
    TEST_ASSERT(reg.valid(occupant), "occupant still alive after create(hint)");
    TEST_ASSERT(reg.valid(result),   "result is valid after fallback");
    TEST_ASSERT(result != occupant,  "result is a different entity");
}

static void test_create_hint_out_of_range_falls_back()
{
    fatp_ecs::Registry reg;
    // Manufacture a hint with a very large index that doesn't exist yet
    fatp_ecs::Entity big_hint = fatp_ecs::EntityTraits::make(9999, 1);
    auto result = reg.create(big_hint);
    TEST_ASSERT(reg.valid(result), "entity created even with large hint index");
}

static void test_create_hint_snapshot_round_trip()
{
    // Simulate: create 3 entities, save their IDs, destroy them,
    // restore with create(hint) — all should recover their original indices.
    fatp_ecs::Registry reg;
    auto e0 = reg.create();
    auto e1 = reg.create();
    auto e2 = reg.create();

    reg.add<Position>(e0, Position{1.f, 0.f});
    reg.add<Position>(e1, Position{2.f, 0.f});
    reg.add<Position>(e2, Position{3.f, 0.f});

    // Save
    fatp_ecs::Entity saved[3] = {e0, e1, e2};

    // Destroy all
    reg.destroy(e0); reg.destroy(e1); reg.destroy(e2);

    // Restore — hint in order
    auto r0 = reg.create(saved[0]);
    auto r1 = reg.create(saved[1]);
    auto r2 = reg.create(saved[2]);

    TEST_ASSERT(fatp_ecs::EntityTraits::index(r0) == fatp_ecs::EntityTraits::index(saved[0]),
                "r0 index matches saved[0]");
    TEST_ASSERT(fatp_ecs::EntityTraits::index(r1) == fatp_ecs::EntityTraits::index(saved[1]),
                "r1 index matches saved[1]");
    TEST_ASSERT(fatp_ecs::EntityTraits::index(r2) == fatp_ecs::EntityTraits::index(saved[2]),
                "r2 index matches saved[2]");
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

    std::printf("\n[valid / alive / contains / emplace / erase / try_get]\n");
    RUN_TEST(test_valid_alias);
    RUN_TEST(test_alive_alias);
    RUN_TEST(test_contains_alias);
    RUN_TEST(test_emplace_alias);
    RUN_TEST(test_erase_removes_component);
    RUN_TEST(test_try_get_alias);

    std::printf("\n[clear<T>]\n");
    RUN_TEST(test_clear_single_type);
    RUN_TEST(test_clear_single_type_fires_events);
    RUN_TEST(test_clear_unregistered_type_is_noop);

    std::printf("\n[storage<T>]\n");
    RUN_TEST(test_storage_null_when_unregistered);
    RUN_TEST(test_storage_non_null_after_add);
    RUN_TEST(test_storage_dense_matches_component);

    std::printf("\n[on_construct / on_destroy / on_update]\n");
    RUN_TEST(test_on_construct_alias);
    RUN_TEST(test_on_destroy_alias);
    RUN_TEST(test_on_update_alias);

    std::printf("\n[group_if_exists / non_owning_group_if_exists]\n");
    RUN_TEST(test_group_if_exists_null_before_create);
    RUN_TEST(test_group_if_exists_non_null_after_create);
    RUN_TEST(test_group_if_exists_is_same_instance);
    RUN_TEST(test_group_if_exists_wrong_types_null);
    RUN_TEST(test_non_owning_group_if_exists_null_before_create);
    RUN_TEST(test_non_owning_group_if_exists_non_null_after_create);
    RUN_TEST(test_group_if_exists_does_not_create);

    std::printf("\n[create(hint)]\n");
    RUN_TEST(test_create_hint_honours_free_slot);
    RUN_TEST(test_create_hint_different_generation);
    RUN_TEST(test_create_hint_fallback_when_occupied);
    RUN_TEST(test_create_hint_out_of_range_falls_back);
    RUN_TEST(test_create_hint_snapshot_round_trip);

    std::printf("\n=== Results: %d passed, %d failed ===\n", gPassed, gFailed);
    return gFailed > 0 ? 1 : 0;
}
