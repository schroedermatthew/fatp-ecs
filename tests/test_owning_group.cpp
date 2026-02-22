/**
 * @file test_owning_group.cpp
 * @brief Tests for OwningGroup — zero-probe simultaneous multi-component iteration.
 *
 * Tests cover:
 * - Basic two-type group: entities with both components are in the group
 * - Three-type group
 * - Entities missing one type are not in the group
 * - Adding the final missing component admits entity to group
 * - Removing any owned component evicts entity from group
 * - Group size tracks membership correctly
 * - contains() correctly identifies group members
 * - each() receives correct entity and component references
 * - each() modifications are reflected back in the stores
 * - Seeding: entities present before group() call are included
 * - Registry::group() returns same group on repeated calls (idempotent)
 * - Group and plain View over same types produce same entity set
 * - Destroy entity: removed from group (via onComponentRemoved signals)
 * - Large-scale: 1000 entities, alternating membership
 * - Interleaved add/remove preserves invariant
 * - empty() and size() edge cases
 * - const each()
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
struct Velocity { float dx = 1.0f; float dy = 0.0f; };
struct Health   { int hp = 100; };
struct Tag      { uint8_t id = 0; };

// =============================================================================
// Helpers
// =============================================================================

static std::vector<Entity> collectGroup(OwningGroup<Position, Velocity>& grp)
{
    std::vector<Entity> result;
    grp.each([&](Entity e, Position&, Velocity&) { result.push_back(e); });
    return result;
}

static bool contains(const std::vector<Entity>& v, Entity e)
{
    return std::find(v.begin(), v.end(), e) != v.end();
}

// =============================================================================
// Basic membership
// =============================================================================

void test_basic_two_type_membership()
{
    Registry reg;
    auto& grp = reg.group<Position, Velocity>();

    Entity e1 = reg.create(); reg.add<Position>(e1); reg.add<Velocity>(e1); // in group
    Entity e2 = reg.create(); reg.add<Position>(e2);                        // missing Velocity
    Entity e3 = reg.create(); reg.add<Velocity>(e3);                        // missing Position
    Entity e4 = reg.create(); reg.add<Position>(e4); reg.add<Velocity>(e4); // in group

    TEST_ASSERT(grp.size() == 2,     "two entities should be in group");
    TEST_ASSERT(grp.contains(e1),    "e1 should be in group");
    TEST_ASSERT(grp.contains(e4),    "e4 should be in group");
    TEST_ASSERT(!grp.contains(e2),   "e2 (no Velocity) not in group");
    TEST_ASSERT(!grp.contains(e3),   "e3 (no Position) not in group");
}

void test_three_type_group()
{
    Registry reg;
    auto& grp = reg.group<Position, Velocity, Health>();

    Entity e1 = reg.create();
    reg.add<Position>(e1); reg.add<Velocity>(e1); reg.add<Health>(e1); // in group

    Entity e2 = reg.create();
    reg.add<Position>(e2); reg.add<Velocity>(e2); // missing Health

    Entity e3 = reg.create();
    reg.add<Position>(e3); reg.add<Velocity>(e3); reg.add<Health>(e3); // in group

    TEST_ASSERT(grp.size() == 2,  "two entities with all three components");
    TEST_ASSERT(grp.contains(e1), "e1 should be in group");
    TEST_ASSERT(grp.contains(e3), "e3 should be in group");
    TEST_ASSERT(!grp.contains(e2),"e2 (no Health) not in group");
}

// =============================================================================
// Dynamic membership
// =============================================================================

void test_adding_last_component_admits_to_group()
{
    Registry reg;
    auto& grp = reg.group<Position, Velocity>();

    Entity e = reg.create();
    reg.add<Position>(e);
    TEST_ASSERT(grp.size() == 0,  "not in group yet (missing Velocity)");
    TEST_ASSERT(!grp.contains(e), "not in group yet");

    reg.add<Velocity>(e); // completes the set
    TEST_ASSERT(grp.size() == 1, "should now be in group");
    TEST_ASSERT(grp.contains(e), "should be in group");
}

void test_removing_component_evicts_from_group()
{
    Registry reg;
    auto& grp = reg.group<Position, Velocity>();

    Entity e = reg.create();
    reg.add<Position>(e); reg.add<Velocity>(e);
    TEST_ASSERT(grp.size() == 1, "in group");

    reg.remove<Velocity>(e);
    TEST_ASSERT(grp.size() == 0,  "evicted from group");
    TEST_ASSERT(!grp.contains(e), "not in group after remove");
}

void test_removing_either_component_evicts()
{
    Registry reg;
    auto& grp = reg.group<Position, Velocity>();

    Entity e1 = reg.create(); reg.add<Position>(e1); reg.add<Velocity>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2); reg.add<Velocity>(e2);

    reg.remove<Position>(e1); // evicted via Position removal
    TEST_ASSERT(!grp.contains(e1), "e1 evicted via Position remove");
    TEST_ASSERT(grp.contains(e2),  "e2 still in group");
    TEST_ASSERT(grp.size() == 1,   "one entity in group");
}

// =============================================================================
// each() correctness
// =============================================================================

void test_each_visits_all_group_members()
{
    Registry reg;
    auto& grp = reg.group<Position, Velocity>();

    Entity e1 = reg.create(); reg.add<Position>(e1); reg.add<Velocity>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2);                       // not in group
    Entity e3 = reg.create(); reg.add<Position>(e3); reg.add<Velocity>(e3);

    auto visited = collectGroup(grp);
    TEST_ASSERT(visited.size() == 2,   "each should visit 2 group entities");
    TEST_ASSERT(contains(visited, e1), "e1 should be visited");
    TEST_ASSERT(contains(visited, e3), "e3 should be visited");
    TEST_ASSERT(!contains(visited, e2),"e2 should not be visited");
}

void test_each_provides_correct_component_refs()
{
    Registry reg;
    auto& grp = reg.group<Position, Velocity>();

    Entity e = reg.create();
    reg.add<Position>(e, 3.0f, 4.0f);
    reg.add<Velocity>(e, 1.5f, 2.5f);

    float sumX = 0.0f, sumDx = 0.0f;
    grp.each([&](Entity, Position& p, Velocity& v) {
        sumX  = p.x;
        sumDx = v.dx;
    });

    TEST_ASSERT(sumX  == 3.0f, "Position.x should be 3.0");
    TEST_ASSERT(sumDx == 1.5f, "Velocity.dx should be 1.5");
}

void test_each_modifications_persist()
{
    Registry reg;
    auto& grp = reg.group<Position, Velocity>();

    Entity e = reg.create();
    reg.add<Position>(e, 0.0f, 0.0f);
    reg.add<Velocity>(e, 2.0f, 3.0f);

    // Integrate position
    grp.each([](Entity, Position& p, Velocity& v) {
        p.x += v.dx;
        p.y += v.dy;
    });

    TEST_ASSERT(reg.get<Position>(e).x == 2.0f, "x should be 2.0 after integration");
    TEST_ASSERT(reg.get<Position>(e).y == 3.0f, "y should be 3.0 after integration");
}

void test_const_each()
{
    Registry reg;
    auto& grp = reg.group<Position, Velocity>();
    Entity e = reg.create();
    reg.add<Position>(e, 5.0f, 0.0f);
    reg.add<Velocity>(e, 0.0f, 0.0f);

    const auto& cgrp = grp;
    float seenX = -1.0f;
    cgrp.each([&](Entity, const Position& p, const Velocity&) {
        seenX = p.x;
    });
    TEST_ASSERT(seenX == 5.0f, "const each should see Position.x = 5.0");
}

// =============================================================================
// Seeding (entities existing before group() call)
// =============================================================================

void test_seeding_includes_existing_entities()
{
    Registry reg;

    // Add entities BEFORE creating the group
    Entity e1 = reg.create(); reg.add<Position>(e1); reg.add<Velocity>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2);
    Entity e3 = reg.create(); reg.add<Position>(e3); reg.add<Velocity>(e3);

    // Now create the group — it must seed from existing stores
    auto& grp = reg.group<Position, Velocity>();

    TEST_ASSERT(grp.size() == 2,    "seeded group should have 2 entities");
    TEST_ASSERT(grp.contains(e1),   "e1 should be seeded");
    TEST_ASSERT(grp.contains(e3),   "e3 should be seeded");
    TEST_ASSERT(!grp.contains(e2),  "e2 (no Velocity) should not be seeded");
}

// =============================================================================
// Idempotency
// =============================================================================

void test_group_is_idempotent()
{
    Registry reg;
    auto& grp1 = reg.group<Position, Velocity>();
    auto& grp2 = reg.group<Position, Velocity>();

    TEST_ASSERT(&grp1 == &grp2, "group() should return the same object");

    Entity e = reg.create();
    reg.add<Position>(e); reg.add<Velocity>(e);
    TEST_ASSERT(grp1.size() == 1, "size should be 1");
    TEST_ASSERT(grp2.size() == 1, "same group, same size");
}

// =============================================================================
// Group vs plain View agreement
// =============================================================================

void test_group_matches_view()
{
    Registry reg;
    auto& grp = reg.group<Position, Velocity>();

    for (int i = 0; i < 20; ++i)
    {
        Entity e = reg.create();
        reg.add<Position>(e);
        if (i % 2 == 0) reg.add<Velocity>(e);
    }

    // Collect via group
    std::vector<Entity> groupResult;
    grp.each([&](Entity e, Position&, Velocity&) { groupResult.push_back(e); });
    std::sort(groupResult.begin(), groupResult.end());

    // Collect via compile-time view
    std::vector<Entity> viewResult;
    reg.view<Position, Velocity>().each(
        [&](Entity e, Position&, Velocity&) { viewResult.push_back(e); });
    std::sort(viewResult.begin(), viewResult.end());

    TEST_ASSERT(groupResult == viewResult,
                "group and view should visit the same entities");
}

// =============================================================================
// Entity destruction
// =============================================================================

void test_destroy_entity_removes_from_group()
{
    Registry reg;
    auto& grp = reg.group<Position, Velocity>();

    Entity e1 = reg.create(); reg.add<Position>(e1); reg.add<Velocity>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2); reg.add<Velocity>(e2);

    TEST_ASSERT(grp.size() == 2, "two in group before destroy");

    reg.destroy(e1);

    TEST_ASSERT(grp.size() == 1,  "one in group after destroy");
    TEST_ASSERT(grp.contains(e2), "e2 should remain");
}

// =============================================================================
// Interleaved add/remove stress
// =============================================================================

void test_interleaved_add_remove_invariant()
{
    Registry reg;
    auto& grp = reg.group<Position, Velocity>();

    Entity e = reg.create();
    reg.add<Position>(e);

    for (int i = 0; i < 10; ++i)
    {
        reg.add<Velocity>(e);
        TEST_ASSERT(grp.size() == 1,  "in group after add");
        TEST_ASSERT(grp.contains(e),  "e in group");

        reg.remove<Velocity>(e);
        TEST_ASSERT(grp.size() == 0,  "not in group after remove");
        TEST_ASSERT(!grp.contains(e), "e not in group");
    }
}

// =============================================================================
// empty() edge cases
// =============================================================================

void test_empty_group()
{
    Registry reg;
    auto& grp = reg.group<Position, Velocity>();

    TEST_ASSERT(grp.empty(), "fresh group should be empty");
    TEST_ASSERT(grp.size() == 0, "fresh group size is 0");

    Entity e = reg.create();
    reg.add<Position>(e);
    TEST_ASSERT(grp.empty(), "still empty (no Velocity)");

    reg.add<Velocity>(e);
    TEST_ASSERT(!grp.empty(), "not empty after Velocity added");
    TEST_ASSERT(grp.size() == 1, "size is 1");
}

// =============================================================================
// Large scale
// =============================================================================

void test_large_scale()
{
    Registry reg;
    auto& grp = reg.group<Position, Velocity>();

    constexpr int kN = 1000;
    int expectedInGroup = 0;
    std::vector<Entity> entities;
    entities.reserve(kN);

    for (int i = 0; i < kN; ++i)
    {
        Entity e = reg.create();
        entities.push_back(e);
        reg.add<Position>(e);
        if (i % 2 == 0)
        {
            reg.add<Velocity>(e);
            ++expectedInGroup;
        }
    }

    TEST_ASSERT(grp.size() == static_cast<std::size_t>(expectedInGroup),
                "group size should equal entities with both components");

    // Verify each() reaches the right count
    int eachCount = 0;
    grp.each([&](Entity, Position&, Velocity&) { ++eachCount; });
    TEST_ASSERT(static_cast<std::size_t>(eachCount) == grp.size(),
                "each() should visit exactly size() entities");

    // Verify integration result: sum all positions
    grp.each([](Entity, Position& p, Velocity& v) {
        p.x += v.dx;
        p.y += v.dy;
    });

    // All Velocity.dx defaults to 1.0, so x should be 1.0 for group members
    for (int i = 0; i < kN; i += 2)
    {
        TEST_ASSERT(reg.get<Position>(entities[i]).x == 1.0f,
                    "integrated position x should be 1.0");
    }
}

// =============================================================================
// Main
// =============================================================================

int main()
{
    std::printf("=== OwningGroup Tests ===\n\n");

    std::printf("Basic membership:\n");
    RUN_TEST(test_basic_two_type_membership);
    RUN_TEST(test_three_type_group);

    std::printf("\nDynamic membership:\n");
    RUN_TEST(test_adding_last_component_admits_to_group);
    RUN_TEST(test_removing_component_evicts_from_group);
    RUN_TEST(test_removing_either_component_evicts);

    std::printf("\neach() correctness:\n");
    RUN_TEST(test_each_visits_all_group_members);
    RUN_TEST(test_each_provides_correct_component_refs);
    RUN_TEST(test_each_modifications_persist);
    RUN_TEST(test_const_each);

    std::printf("\nSeeding and idempotency:\n");
    RUN_TEST(test_seeding_includes_existing_entities);
    RUN_TEST(test_group_is_idempotent);

    std::printf("\nGroup vs View agreement:\n");
    RUN_TEST(test_group_matches_view);

    std::printf("\nEntity destruction:\n");
    RUN_TEST(test_destroy_entity_removes_from_group);

    std::printf("\nStress and scale:\n");
    RUN_TEST(test_interleaved_add_remove_invariant);
    RUN_TEST(test_empty_group);
    RUN_TEST(test_large_scale);

    std::printf("\n=== Results: %d passed, %d failed ===\n",
                sTestsPassed, sTestsFailed);

    return sTestsFailed == 0 ? 0 : 1;
}
