/**
 * @file test_view_iteration.cpp
 * @brief Tests for ViewImpl::get<T>(entity) and range-for (begin/end) support.
 *
 * These APIs were added to close a gap discovered during the EnTT-Pacman port:
 * EnTT code uses two patterns that fatp-ecs views previously did not support:
 *
 *   1. view.get<T>(entity)   — retrieve a component from an entity inside a loop
 *   2. for (Entity e : view) — entity-only range-for with separate get() calls
 *
 * Tests cover:
 *
 * get<T>(entity):
 *   - Single-component view: get returns correct component
 *   - Multi-component view: get returns correct component for each type
 *   - Mutation via get persists (returns reference, not copy)
 *   - const view: get<T> returns const reference
 *   - get<T> on entity not in view is UB (not tested, documented contract)
 *
 * Range-for (begin / end):
 *   - Single-component view: all matching entities visited exactly once
 *   - Multi-component view: only entities with all components visited
 *   - Empty view: zero iterations
 *   - Component added mid-loop does not appear in current iteration
 *     (entity cache is built at begin() and is stable for the loop)
 *   - Component removed mid-loop does not affect current iteration
 *     (same stability guarantee)
 *   - const view: range-for works on const view
 *   - Nested range-for over two different views in the same scope
 *   - Range-for combined with get<T> inside the loop body (Pacman pattern)
 *   - Exclude filters honoured during range-for
 *   - Large-scale: 1000 entities, correct count
 *
 * Pacman-pattern integration:
 *   - Reproduces the exact coding pattern used in EnTT-Pacman systems:
 *     auto view = reg.view<A, B>(); for (Entity e : view) { view.get<A>(e)... }
 *   - Modifying components (add/remove) on the iterated entity during loop
 *     is safe (matches ghost-mode switching in change_ghost_mode.cpp)
 */

#include <fatp_ecs/FatpEcs.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>

using namespace fatp_ecs;

// =============================================================================
// Test harness
// =============================================================================

static int gPassed = 0;
static int gFailed = 0;

#define TEST_ASSERT(cond, msg)                                              \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            std::printf("  FAIL: %s (line %d)\n", msg, __LINE__);          \
            ++gFailed;                                                      \
            return;                                                         \
        }                                                                   \
    } while (false)

#define RUN_TEST(fn)                                                        \
    do                                                                      \
    {                                                                       \
        std::printf("  %s ... ", #fn);                                      \
        int before = gFailed;                                               \
        fn();                                                               \
        if (gFailed == before) { ++gPassed; std::printf("OK\n"); }         \
    } while (false)

// =============================================================================
// Component types
// =============================================================================

struct Position  { float x = 0.f; float y = 0.f; };
struct Velocity  { float dx = 0.f; float dy = 0.f; };
struct Health    { int hp = 100; };
struct ChaseMode { uint8_t dummy = 0; };
struct ScaredMode { int timer = 60; };
struct Ghost     { uint8_t dummy = 0; };

// =============================================================================
// get<T>(entity) — single-component view
// =============================================================================

static void test_get_single_comp_returns_correct_value()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e, Position{3.f, 4.f});

    auto view = reg.view<Position>();
    view.each([&](Entity ev, Position&)
    {
        Position& p = view.get<Position>(ev);
        TEST_ASSERT(p.x == 3.f, "get<Position>.x should be 3");
        TEST_ASSERT(p.y == 4.f, "get<Position>.y should be 4");
    });
}

static void test_get_single_comp_mutation_persists()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e, Position{0.f, 0.f});

    auto view = reg.view<Position>();
    view.each([&](Entity ev, Position&)
    {
        view.get<Position>(ev).x = 99.f;
    });

    TEST_ASSERT(reg.get<Position>(e).x == 99.f,
                "mutation via view.get<T> should persist");
}

// =============================================================================
// get<T>(entity) — multi-component view
// =============================================================================

static void test_get_multi_comp_each_type()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e, Position{1.f, 2.f});
    reg.add<Velocity>(e, Velocity{3.f, 4.f});

    auto view = reg.view<Position, Velocity>();
    view.each([&](Entity ev, Position&, Velocity&)
    {
        TEST_ASSERT(view.get<Position>(ev).x == 1.f, "Position.x via get");
        TEST_ASSERT(view.get<Velocity>(ev).dx == 3.f, "Velocity.dx via get");
    });
}

static void test_get_multi_comp_mutation_persists()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e, Position{0.f, 0.f});
    reg.add<Velocity>(e, Velocity{0.f, 0.f});

    auto view = reg.view<Position, Velocity>();
    view.each([&](Entity ev, Position&, Velocity&)
    {
        view.get<Position>(ev).x  = 10.f;
        view.get<Velocity>(ev).dx = 20.f;
    });

    TEST_ASSERT(reg.get<Position>(e).x  == 10.f, "Position.x mutated");
    TEST_ASSERT(reg.get<Velocity>(e).dx == 20.f, "Velocity.dx mutated");
}

static void test_get_three_comp_view()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e, Position{5.f, 6.f});
    reg.add<Velocity>(e, Velocity{7.f, 8.f});
    reg.add<Health>(e,   Health{42});

    auto view = reg.view<Position, Velocity, Health>();
    int visited = 0;
    view.each([&](Entity ev, Position&, Velocity&, Health&)
    {
        TEST_ASSERT(view.get<Position>(ev).x  == 5.f,  "Position.x");
        TEST_ASSERT(view.get<Velocity>(ev).dx == 7.f,  "Velocity.dx");
        TEST_ASSERT(view.get<Health>(ev).hp   == 42,   "Health.hp");
        ++visited;
    });
    TEST_ASSERT(visited == 1, "exactly one entity visited");
}

// =============================================================================
// get<T>(entity) — const view
// =============================================================================

static void test_get_const_view_returns_const_ref()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e, Position{11.f, 22.f});

    const auto view = reg.view<Position>();
    view.each([&](Entity ev, const Position&)
    {
        const Position& p = view.get<Position>(ev);
        TEST_ASSERT(p.x == 11.f, "const get<Position>.x");
        TEST_ASSERT(p.y == 22.f, "const get<Position>.y");
    });
}

// =============================================================================
// Range-for: begin / end
// =============================================================================

static void test_range_for_single_comp_visits_all()
{
    Registry reg;
    Entity e1 = reg.create(); reg.add<Position>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2);
    Entity e3 = reg.create(); reg.add<Position>(e3);

    std::vector<Entity> visited;
    auto view = reg.view<Position>();
    for (Entity e : view)
        visited.push_back(e);

    TEST_ASSERT(visited.size() == 3, "should visit 3 entities");
    auto has = [&](Entity x) {
        return std::find(visited.begin(), visited.end(), x) != visited.end();
    };
    TEST_ASSERT(has(e1), "e1 visited");
    TEST_ASSERT(has(e2), "e2 visited");
    TEST_ASSERT(has(e3), "e3 visited");
}

static void test_range_for_multi_comp_only_matching()
{
    Registry reg;
    Entity e1 = reg.create(); reg.add<Position>(e1); reg.add<Velocity>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2); // no Velocity — excluded
    Entity e3 = reg.create(); reg.add<Position>(e3); reg.add<Velocity>(e3);

    std::vector<Entity> visited;
    auto view = reg.view<Position, Velocity>();
    for (Entity e : view)
        visited.push_back(e);

    TEST_ASSERT(visited.size() == 2, "should visit e1 and e3 only");
    TEST_ASSERT(std::find(visited.begin(), visited.end(), e2) == visited.end(),
                "e2 (no Velocity) must not appear");
}

static void test_range_for_empty_view_zero_iterations()
{
    Registry reg;
    // No entities, no components registered.
    auto view = reg.view<Position>();
    int count = 0;
    for (Entity e : view)
    {
        (void)e;
        ++count;
    }
    TEST_ASSERT(count == 0, "empty view: zero iterations");
}

static void test_range_for_no_matching_entities()
{
    Registry reg;
    // Entities exist but none have Position.
    Entity e1 = reg.create(); reg.add<Velocity>(e1);
    Entity e2 = reg.create(); reg.add<Velocity>(e2);

    auto view = reg.view<Position>();
    int count = 0;
    for (Entity e : view) { (void)e; ++count; }
    TEST_ASSERT(count == 0, "no matching entities: zero iterations");
}

static void test_range_for_entity_cache_stable_during_add()
{
    // Adding a component to a new entity mid-loop must not cause the new
    // entity to appear in the current iteration (cache built at begin()).
    Registry reg;
    Entity e1 = reg.create(); reg.add<Position>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2);

    Entity sneaky = reg.create(); // no Position yet

    std::vector<Entity> visited;
    auto view = reg.view<Position>();
    for (Entity e : view)
    {
        visited.push_back(e);
        if (e == e1)
            reg.add<Position>(sneaky); // add mid-loop
    }

    TEST_ASSERT(visited.size() == 2, "sneaky entity must not appear in current loop");
    TEST_ASSERT(std::find(visited.begin(), visited.end(), sneaky) == visited.end(),
                "sneaky must not be visited");
}

static void test_range_for_entity_cache_stable_during_remove()
{
    // Removing a component from a later entity mid-loop must not skip it —
    // the entity cache was already built at begin().
    Registry reg;
    Entity e1 = reg.create(); reg.add<Position>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2);
    Entity e3 = reg.create(); reg.add<Position>(e3);

    std::vector<Entity> visited;
    auto view = reg.view<Position>();
    for (Entity e : view)
    {
        visited.push_back(e);
        // When we visit e1, remove Position from e3.
        // e3 should still appear in visited (cache was built before loop body ran).
        if (e == e1)
            reg.remove<Position>(e3);
    }

    TEST_ASSERT(visited.size() == 3,
                "all 3 entities must appear; cache is stable");
}

static void test_range_for_const_view()
{
    Registry reg;
    Entity e1 = reg.create(); reg.add<Position>(e1, Position{1.f, 0.f});
    Entity e2 = reg.create(); reg.add<Position>(e2, Position{2.f, 0.f});

    const auto view = reg.view<Position>();
    std::vector<Entity> visited;
    for (Entity e : view)
        visited.push_back(e);

    TEST_ASSERT(visited.size() == 2, "const view range-for visits 2 entities");
}

static void test_range_for_nested_views()
{
    // Two different views iterated in nested loops — each has its own cache.
    Registry reg;
    Entity e1 = reg.create(); reg.add<Position>(e1); reg.add<Health>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2);
    Entity e3 = reg.create(); reg.add<Health>(e3);

    auto posView    = reg.view<Position>();
    auto healthView = reg.view<Health>();

    int pairs = 0;
    for (Entity ep : posView)
        for (Entity eh : healthView)
            if (ep == eh) ++pairs; // entities that have both

    // Only e1 has both Position and Health.
    TEST_ASSERT(pairs == 1, "exactly one entity appears in both views");
}

static void test_range_for_with_get_inside_loop()
{
    // Reproduces the exact Pacman pattern:
    //   auto view = reg.view<A, B>();
    //   for (Entity e : view) { view.get<A>(e).field = ...; }
    Registry reg;
    Entity e1 = reg.create(); reg.add<Position>(e1, Position{1.f, 0.f}); reg.add<Velocity>(e1, Velocity{10.f, 0.f});
    Entity e2 = reg.create(); reg.add<Position>(e2, Position{2.f, 0.f}); reg.add<Velocity>(e2, Velocity{20.f, 0.f});

    auto view = reg.view<Position, Velocity>();
    for (Entity e : view)
    {
        Position& p = view.get<Position>(e);
        Velocity& v = view.get<Velocity>(e);
        p.x += v.dx;
    }

    TEST_ASSERT(reg.get<Position>(e1).x == 11.f, "e1 Position.x updated");
    TEST_ASSERT(reg.get<Position>(e2).x == 22.f, "e2 Position.x updated");
}

static void test_range_for_with_exclude_filter()
{
    Registry reg;
    Entity e1 = reg.create(); reg.add<Position>(e1);
    Entity e2 = reg.create(); reg.add<Position>(e2); reg.add<ChaseMode>(e2); // excluded
    Entity e3 = reg.create(); reg.add<Position>(e3);

    std::vector<Entity> visited;
    auto view = reg.view<Position>(Exclude<ChaseMode>{});
    for (Entity e : view)
        visited.push_back(e);

    TEST_ASSERT(visited.size() == 2, "exclude filter: 2 of 3 entities visited");
    TEST_ASSERT(std::find(visited.begin(), visited.end(), e2) == visited.end(),
                "e2 (ChaseMode) must be excluded");
}

static void test_range_for_large_scale()
{
    Registry reg;
    constexpr int kN = 1000;

    std::vector<Entity> expected;
    for (int i = 0; i < kN; ++i)
    {
        Entity e = reg.create();
        reg.add<Position>(e);
        if (i % 2 == 0)
        {
            reg.add<Velocity>(e);
            expected.push_back(e);
        }
    }

    std::vector<Entity> visited;
    auto view = reg.view<Position, Velocity>();
    for (Entity e : view)
        visited.push_back(e);

    TEST_ASSERT(visited.size() == expected.size(),
                "large-scale: correct entity count");

    // Every expected entity must appear exactly once.
    for (Entity e : expected)
    {
        int count = static_cast<int>(
            std::count(visited.begin(), visited.end(), e));
        TEST_ASSERT(count == 1, "large-scale: each expected entity visited once");
    }
}

// =============================================================================
// Pacman-pattern integration tests
// (Reproduce the actual system patterns from EnTT-Pacman)
// =============================================================================

static void test_pacman_ghost_mode_switch()
{
    // Reproduces change_ghost_mode.cpp: ghostScatter()
    //   auto view = reg.view<Ghost, ChaseMode>();
    //   for (Entity e : view) {
    //     reg.remove<ChaseMode>(e);
    //     reg.emplace<ScaredMode>(e);
    //   }
    Registry reg;

    // Set up 4 ghosts in ChaseMode.
    std::vector<Entity> ghosts;
    for (int i = 0; i < 4; ++i)
    {
        Entity e = reg.create();
        reg.add<Ghost>(e);
        reg.add<ChaseMode>(e);
        ghosts.push_back(e);
    }
    // One ghost without ChaseMode — should be unaffected.
    Entity outsider = reg.create();
    reg.add<Ghost>(outsider);

    // Run the system.
    auto view = reg.view<Ghost, ChaseMode>();
    for (Entity e : view)
    {
        reg.remove<ChaseMode>(e);
        reg.add<ScaredMode>(e);
    }

    // All 4 ghosts should now have ScaredMode and no ChaseMode.
    for (Entity e : ghosts)
    {
        TEST_ASSERT(!reg.has<ChaseMode>(e),  "ghost should not have ChaseMode");
        TEST_ASSERT(reg.has<ScaredMode>(e),  "ghost should have ScaredMode");
    }
    // Outsider untouched.
    TEST_ASSERT(!reg.has<ChaseMode>(outsider),  "outsider never had ChaseMode");
    TEST_ASSERT(!reg.has<ScaredMode>(outsider), "outsider should not have ScaredMode");
}

static void test_pacman_scared_timeout()
{
    // Reproduces ghostScaredTimeout():
    //   auto view = reg.view<Ghost, ScaredMode>();
    //   for (Entity e : view) {
    //     ScaredMode& scared = view.get<ScaredMode>(e);
    //     --scared.timer;
    //     if (scared.timer <= 0) {
    //       reg.remove<ScaredMode>(e);
    //       reg.emplace<ChaseMode>(e);
    //     }
    //   }
    Registry reg;

    Entity e1 = reg.create(); reg.add<Ghost>(e1); reg.add<ScaredMode>(e1, ScaredMode{1});  // will time out
    Entity e2 = reg.create(); reg.add<Ghost>(e2); reg.add<ScaredMode>(e2, ScaredMode{5});  // will not
    Entity e3 = reg.create(); reg.add<Ghost>(e3); reg.add<ScaredMode>(e3, ScaredMode{1});  // will time out

    auto view = reg.view<Ghost, ScaredMode>();
    for (Entity e : view)
    {
        ScaredMode& scared = view.get<ScaredMode>(e);
        --scared.timer;
        if (scared.timer <= 0)
        {
            reg.remove<ScaredMode>(e);
            reg.add<ChaseMode>(e);
        }
    }

    TEST_ASSERT(!reg.has<ScaredMode>(e1) && reg.has<ChaseMode>(e1), "e1 timed out → ChaseMode");
    TEST_ASSERT(reg.has<ScaredMode>(e2)  && reg.get<ScaredMode>(e2).timer == 4, "e2 decremented, still scared");
    TEST_ASSERT(!reg.has<ScaredMode>(e3) && reg.has<ChaseMode>(e3), "e3 timed out → ChaseMode");
}

static void test_pacman_collision_check_two_views()
{
    // Reproduces playerGhostCollide() — iterates player view and ghost view
    // in a nested loop, uses reg.has<ScaredMode>(ghost) inside.
    Registry reg;

    struct Player { uint8_t d = 0; };

    Entity player = reg.create();
    reg.add<Player>(player);
    reg.add<Position>(player, Position{5.f, 5.f});

    Entity ghost1 = reg.create();
    reg.add<Ghost>(ghost1);
    reg.add<Position>(ghost1, Position{5.f, 5.f}); // same pos — collision!
    reg.add<ScaredMode>(ghost1);

    Entity ghost2 = reg.create();
    reg.add<Ghost>(ghost2);
    reg.add<Position>(ghost2, Position{1.f, 1.f}); // far away — no collision

    auto players = reg.view<Player, Position>();
    auto ghosts  = reg.view<Ghost, Position>();

    Entity eaten = NullEntity;
    for (Entity p : players)
    {
        const Position& pPos = players.get<Position>(p);
        for (Entity g : ghosts)
        {
            const Position& gPos = ghosts.get<Position>(g);
            if (pPos.x == gPos.x && pPos.y == gPos.y && reg.has<ScaredMode>(g))
            {
                eaten = g;
            }
        }
    }

    TEST_ASSERT(eaten == ghost1, "ghost1 at same position should be 'eaten'");
}

// =============================================================================
// Main
// =============================================================================

int main()
{
    std::printf("=== View Iteration Tests (get<T> + range-for) ===\n\n");

    std::printf("[get<T>(entity) — single-component]\n");
    RUN_TEST(test_get_single_comp_returns_correct_value);
    RUN_TEST(test_get_single_comp_mutation_persists);

    std::printf("\n[get<T>(entity) — multi-component]\n");
    RUN_TEST(test_get_multi_comp_each_type);
    RUN_TEST(test_get_multi_comp_mutation_persists);
    RUN_TEST(test_get_three_comp_view);

    std::printf("\n[get<T>(entity) — const view]\n");
    RUN_TEST(test_get_const_view_returns_const_ref);

    std::printf("\n[range-for: begin/end]\n");
    RUN_TEST(test_range_for_single_comp_visits_all);
    RUN_TEST(test_range_for_multi_comp_only_matching);
    RUN_TEST(test_range_for_empty_view_zero_iterations);
    RUN_TEST(test_range_for_no_matching_entities);
    RUN_TEST(test_range_for_entity_cache_stable_during_add);
    RUN_TEST(test_range_for_entity_cache_stable_during_remove);
    RUN_TEST(test_range_for_const_view);
    RUN_TEST(test_range_for_nested_views);
    RUN_TEST(test_range_for_with_get_inside_loop);
    RUN_TEST(test_range_for_with_exclude_filter);
    RUN_TEST(test_range_for_large_scale);

    std::printf("\n[Pacman-pattern integration]\n");
    RUN_TEST(test_pacman_ghost_mode_switch);
    RUN_TEST(test_pacman_scared_timeout);
    RUN_TEST(test_pacman_collision_check_two_views);

    std::printf("\n=== Results: %d passed, %d failed ===\n", gPassed, gFailed);
    return gFailed > 0 ? 1 : 0;
}
