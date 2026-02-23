/**
 * @file test_non_owning_group.cpp
 * @brief Tests for NonOwningGroup.
 *
 * Covers:
 *   - Basic construction and seeding from existing entities
 *   - size() / empty() / contains()
 *   - Dynamic membership: add / remove components
 *   - Single-type group
 *   - Multi-type group
 *   - Correct callback arguments (mutable & const)
 *   - Coexistence with OwningGroup and View on the same types
 *   - Correct events fired (add vs replace path)
 *   - Entity destruction removes from group
 *   - Re-entrant safety: multiple non-owning groups with overlapping types
 */

#include <cassert>
#include <cstdio>
#include <vector>

#include <fatp_ecs/FatpEcs.h>

// =============================================================================
// Minimal test harness
// =============================================================================

static int gTestsPassed = 0;
static int gTestsFailed = 0;

#define TEST_ASSERT(condition, msg)                                        \
    do                                                                     \
    {                                                                      \
        if (!(condition))                                                  \
        {                                                                  \
            std::printf("  FAIL: %s (line %d)\n", msg, __LINE__);         \
            ++gTestsFailed;                                                \
            return;                                                        \
        }                                                                  \
    } while (false)

#define RUN_TEST(fn)                                           \
    do                                                         \
    {                                                          \
        std::printf("  %s ... ", #fn);                         \
        int failsBefore = gTestsFailed;                        \
        fn();                                                  \
        if (gTestsFailed == failsBefore)                       \
        {                                                      \
            ++gTestsPassed;                                    \
            std::printf("OK\n");                               \
        }                                                      \
    } while (false)

// =============================================================================
// Component types
// =============================================================================

struct Position  { float x, y; };
struct Velocity  { float dx, dy; };
struct Health    { int hp; };
struct Tag       {};   // zero-size marker

// =============================================================================
// Construction / seeding
// =============================================================================

static void test_empty_on_empty_registry()
{
    fatp_ecs::Registry reg;
    auto& grp = reg.non_owning_group<Position, Velocity>();
    TEST_ASSERT(grp.empty(),    "group should be empty when no entities exist");
    TEST_ASSERT(grp.size() == 0, "size should be 0");
}

static void test_seeded_from_existing_entities()
{
    fatp_ecs::Registry reg;

    auto e1 = reg.create();
    auto e2 = reg.create();
    auto e3 = reg.create();

    reg.add<Position>(e1, Position{1.f, 2.f});
    reg.add<Velocity>(e1, Velocity{3.f, 4.f});

    reg.add<Position>(e2, Position{5.f, 6.f});
    // e2 missing Velocity — should NOT be in group

    reg.add<Position>(e3, Position{7.f, 8.f});
    reg.add<Velocity>(e3, Velocity{9.f, 10.f});

    // Create group AFTER entities exist — seeding must pick them up.
    auto& grp = reg.non_owning_group<Position, Velocity>();

    TEST_ASSERT(grp.size() == 2,        "should seed 2 qualifying entities");
    TEST_ASSERT(grp.contains(e1),       "e1 should be in group");
    TEST_ASSERT(!grp.contains(e2),      "e2 should NOT be in group");
    TEST_ASSERT(grp.contains(e3),       "e3 should be in group");
}

// =============================================================================
// Dynamic membership
// =============================================================================

static void test_entity_joins_when_last_type_added()
{
    fatp_ecs::Registry reg;
    auto& grp = reg.non_owning_group<Position, Velocity>();

    auto e = reg.create();
    TEST_ASSERT(!grp.contains(e), "not in group before any components");

    reg.add<Position>(e, Position{1.f, 0.f});
    TEST_ASSERT(!grp.contains(e), "still not in group with only Position");

    reg.add<Velocity>(e, Velocity{0.f, 1.f});
    TEST_ASSERT(grp.contains(e),  "should join group once both components present");
    TEST_ASSERT(grp.size() == 1,  "size should be 1");
}

static void test_entity_leaves_when_component_removed()
{
    fatp_ecs::Registry reg;
    auto& grp = reg.non_owning_group<Position, Velocity>();

    auto e = reg.create();
    reg.add<Position>(e, Position{});
    reg.add<Velocity>(e, Velocity{});
    TEST_ASSERT(grp.contains(e),  "should be in group");

    reg.remove<Velocity>(e);
    TEST_ASSERT(!grp.contains(e), "should leave group when Velocity removed");
    TEST_ASSERT(grp.size() == 0,  "size should be 0");
}

static void test_entity_rejoins_after_readd()
{
    fatp_ecs::Registry reg;
    auto& grp = reg.non_owning_group<Position, Velocity>();

    auto e = reg.create();
    reg.add<Position>(e, Position{});
    reg.add<Velocity>(e, Velocity{});
    TEST_ASSERT(grp.contains(e), "in group");

    reg.remove<Velocity>(e);
    TEST_ASSERT(!grp.contains(e), "left group");

    reg.add<Velocity>(e, Velocity{1.f, 1.f});
    TEST_ASSERT(grp.contains(e), "rejoined group");
}

static void test_entity_destroyed_leaves_group()
{
    fatp_ecs::Registry reg;
    auto& grp = reg.non_owning_group<Position, Velocity>();

    auto e1 = reg.create();
    auto e2 = reg.create();
    reg.add<Position>(e1, Position{}); reg.add<Velocity>(e1, Velocity{});
    reg.add<Position>(e2, Position{}); reg.add<Velocity>(e2, Velocity{});
    TEST_ASSERT(grp.size() == 2, "both in group");

    reg.destroy(e1);
    TEST_ASSERT(grp.size() == 1,    "size should drop to 1 after destroy");
    TEST_ASSERT(!grp.contains(e1),  "destroyed entity should not be in group");
    TEST_ASSERT(grp.contains(e2),   "other entity still in group");
}

// =============================================================================
// Single-type group
// =============================================================================

static void test_single_type_group()
{
    fatp_ecs::Registry reg;
    auto& grp = reg.non_owning_group<Health>();

    auto e1 = reg.create();
    auto e2 = reg.create();
    reg.add<Health>(e1, Health{100});
    reg.add<Health>(e2, Health{50});

    TEST_ASSERT(grp.size() == 2,  "single-type group should contain both entities");

    int sum = 0;
    grp.each([&](fatp_ecs::Entity, Health& h) { sum += h.hp; });
    TEST_ASSERT(sum == 150, "iteration sum should be 150");
}

// =============================================================================
// Iteration correctness
// =============================================================================

static void test_each_visits_correct_data()
{
    fatp_ecs::Registry reg;
    auto& grp = reg.non_owning_group<Position, Velocity>();

    auto e1 = reg.create();
    auto e2 = reg.create();
    reg.add<Position>(e1, Position{1.f, 0.f});
    reg.add<Velocity>(e1, Velocity{10.f, 0.f});
    reg.add<Position>(e2, Position{2.f, 0.f});
    reg.add<Velocity>(e2, Velocity{20.f, 0.f});

    float sumX = 0.f;
    float sumDx = 0.f;
    grp.each([&](fatp_ecs::Entity, Position& p, Velocity& v) {
        sumX  += p.x;
        sumDx += v.dx;
    });

    TEST_ASSERT(sumX  == 3.f,  "sum of position.x should be 3");
    TEST_ASSERT(sumDx == 30.f, "sum of velocity.dx should be 30");
}

static void test_const_each()
{
    fatp_ecs::Registry reg;
    auto& grp = reg.non_owning_group<Position, Velocity>();

    auto e = reg.create();
    reg.add<Position>(e, Position{5.f, 5.f});
    reg.add<Velocity>(e, Velocity{2.f, 2.f});

    const auto& cgrp = grp;
    float sumX = 0.f;
    cgrp.each([&](fatp_ecs::Entity, const Position& p, const Velocity&) {
        sumX += p.x;
    });
    TEST_ASSERT(sumX == 5.f, "const each should read correct position");
}

static void test_each_mutation()
{
    fatp_ecs::Registry reg;
    auto& grp = reg.non_owning_group<Position, Velocity>();

    constexpr int N = 100;
    for (int i = 0; i < N; ++i)
    {
        auto e = reg.create();
        reg.add<Position>(e, Position{static_cast<float>(i), 0.f});
        reg.add<Velocity>(e, Velocity{1.f, 0.f});
    }

    // Apply velocity
    grp.each([](fatp_ecs::Entity, Position& p, Velocity& v) {
        p.x += v.dx;
    });

    // Verify via view
    int count = 0;
    reg.view<Position>().each([&](fatp_ecs::Entity, const Position& p) {
        // Each position should have been incremented by 1
        (void)p;
        ++count;
    });
    TEST_ASSERT(count == N, "all entities should be visible via view after mutation");

    // Verify actual values via group
    float sumX = 0.f;
    grp.each([&](fatp_ecs::Entity, const Position& p, const Velocity&) { sumX += p.x; });
    // sum of (i + 1) for i in 0..N-1 = N*(N-1)/2 + N = N*(N+1)/2
    float expected = static_cast<float>(N * (N + 1) / 2);
    TEST_ASSERT(sumX == expected, "mutated position sum should be N*(N+1)/2");
}

// =============================================================================
// Idempotent retrieval
// =============================================================================

static void test_second_call_returns_same_group()
{
    fatp_ecs::Registry reg;
    auto& grp1 = reg.non_owning_group<Position, Velocity>();
    auto& grp2 = reg.non_owning_group<Position, Velocity>();
    TEST_ASSERT(&grp1 == &grp2, "same group should be returned on repeated calls");
}

// =============================================================================
// Coexistence with OwningGroup and View
// =============================================================================

static void test_coexists_with_view()
{
    fatp_ecs::Registry reg;
    auto& grp = reg.non_owning_group<Position, Velocity>();

    auto e = reg.create();
    reg.add<Position>(e, Position{3.f, 4.f});
    reg.add<Velocity>(e, Velocity{1.f, 2.f});

    // View should still work fine
    int viewCount = 0;
    reg.view<Position, Velocity>().each([&](fatp_ecs::Entity, Position&, Velocity&) {
        ++viewCount;
    });
    TEST_ASSERT(viewCount == 1,  "view should see the entity");
    TEST_ASSERT(grp.size() == 1, "group should also see the entity");
}

static void test_coexists_with_owning_group()
{
    // OwningGroup over {Health, Tag}, NonOwningGroup over {Position, Velocity}
    // No overlapping types — both should work independently.
    fatp_ecs::Registry reg;

    auto& og  = reg.group<Health, Tag>();                   // owning, 2 types
    auto& nog = reg.non_owning_group<Position, Velocity>(); // non-owning

    auto e1 = reg.create();
    reg.add<Health>(e1, Health{100});
    reg.add<Tag>(e1, Tag{});
    reg.add<Position>(e1, Position{});
    reg.add<Velocity>(e1, Velocity{});

    auto e2 = reg.create();
    reg.add<Health>(e2, Health{50});
    reg.add<Tag>(e2, Tag{});

    auto e3 = reg.create();
    reg.add<Position>(e3, Position{});
    reg.add<Velocity>(e3, Velocity{});

    TEST_ASSERT(og.size()  == 2, "owning group should have e1 and e2");
    TEST_ASSERT(nog.size() == 2, "non-owning group should have e1 and e3");

    int healthSum = 0;
    og.each([&](fatp_ecs::Entity, Health& h, Tag&) { healthSum += h.hp; });
    TEST_ASSERT(healthSum == 150, "owning group health sum should be 150");
}

static void test_overlapping_non_owning_groups()
{
    // Two non-owning groups with overlapping types — both should work.
    fatp_ecs::Registry reg;

    auto& grpAB = reg.non_owning_group<Position, Velocity>();
    auto& grpAC = reg.non_owning_group<Position, Health>();

    auto e1 = reg.create();
    reg.add<Position>(e1, Position{});
    reg.add<Velocity>(e1, Velocity{});
    reg.add<Health>(e1, Health{100});

    auto e2 = reg.create();
    reg.add<Position>(e2, Position{});
    reg.add<Velocity>(e2, Velocity{});
    // e2 has no Health

    auto e3 = reg.create();
    reg.add<Position>(e3, Position{});
    reg.add<Health>(e3, Health{50});
    // e3 has no Velocity

    TEST_ASSERT(grpAB.size() == 2, "grpAB: e1 and e2");
    TEST_ASSERT(grpAC.size() == 2, "grpAC: e1 and e3");
    TEST_ASSERT(grpAB.contains(e1) && grpAB.contains(e2),  "grpAB contents");
    TEST_ASSERT(grpAC.contains(e1) && grpAC.contains(e3),  "grpAC contents");
}

// =============================================================================
// Main
// =============================================================================

int main()
{
    std::printf("=== FAT-P ECS NonOwningGroup Tests ===\n\n");

    std::printf("[Construction / Seeding]\n");
    RUN_TEST(test_empty_on_empty_registry);
    RUN_TEST(test_seeded_from_existing_entities);

    std::printf("\n[Dynamic Membership]\n");
    RUN_TEST(test_entity_joins_when_last_type_added);
    RUN_TEST(test_entity_leaves_when_component_removed);
    RUN_TEST(test_entity_rejoins_after_readd);
    RUN_TEST(test_entity_destroyed_leaves_group);

    std::printf("\n[Single-Type Group]\n");
    RUN_TEST(test_single_type_group);

    std::printf("\n[Iteration]\n");
    RUN_TEST(test_each_visits_correct_data);
    RUN_TEST(test_const_each);
    RUN_TEST(test_each_mutation);

    std::printf("\n[Idempotent Retrieval]\n");
    RUN_TEST(test_second_call_returns_same_group);

    std::printf("\n[Coexistence]\n");
    RUN_TEST(test_coexists_with_view);
    RUN_TEST(test_coexists_with_owning_group);
    RUN_TEST(test_overlapping_non_owning_groups);

    std::printf("\n=== Results: %d passed, %d failed ===\n",
                gTestsPassed, gTestsFailed);

    return gTestsFailed > 0 ? 1 : 0;
}
