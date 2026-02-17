/**
 * @file test_ecs.cpp
 * @brief Phase 1 test suite for the FAT-P ECS framework.
 *
 * Tests cover:
 * - Entity creation, destruction, and validity checking
 * - Component add/remove/get with type safety
 * - Single-component and multi-component View iteration
 * - Generational safety (stale entity detection)
 * - Edge cases: empty views, missing components, double-destroy
 * - Scale: 10K entity stress test
 *
 * Uses a minimal assert-based test harness (no external test framework
 * dependency, consistent with FAT-P's zero-dependency philosophy).
 */

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include <fatp_ecs/FatpEcs.h>

// =============================================================================
// Test Harness
// =============================================================================

static int gTestsPassed = 0;
static int gTestsFailed = 0;

#define TEST_ASSERT(condition, msg)                                      \
    do                                                                   \
    {                                                                    \
        if (!(condition))                                                \
        {                                                                \
            std::printf("  FAIL: %s (line %d)\n", msg, __LINE__);       \
            ++gTestsFailed;                                              \
            return;                                                      \
        }                                                                \
    } while (false)

#define TEST_ASSERT_THROWS(expr, exception_type, msg)                    \
    do                                                                   \
    {                                                                    \
        bool caught = false;                                             \
        try                                                              \
        {                                                                \
            expr;                                                        \
        }                                                                \
        catch (const exception_type&)                                    \
        {                                                                \
            caught = true;                                               \
        }                                                                \
        if (!caught)                                                     \
        {                                                                \
            std::printf("  FAIL: %s (line %d)\n", msg, __LINE__);       \
            ++gTestsFailed;                                              \
            return;                                                      \
        }                                                                \
    } while (false)

#define RUN_TEST(func)                                                   \
    do                                                                   \
    {                                                                    \
        std::printf("  Running: %s\n", #func);                          \
        func();                                                          \
        ++gTestsPassed;                                                  \
    } while (false)

// =============================================================================
// Test Component Types
// =============================================================================

struct Position
{
    float x = 0.0f;
    float y = 0.0f;

    Position() = default;
    Position(float px, float py)
        : x(px)
        , y(py)
    {
    }
};

struct Velocity
{
    float dx = 0.0f;
    float dy = 0.0f;

    Velocity() = default;
    Velocity(float vx, float vy)
        : dx(vx)
        , dy(vy)
    {
    }
};

struct Health
{
    int hp = 100;
    int maxHp = 100;

    Health() = default;
    Health(int h, int m)
        : hp(h)
        , maxHp(m)
    {
    }
};

struct Tag
{
    std::string name;

    Tag() = default;
    explicit Tag(std::string n)
        : name(std::move(n))
    {
    }
};

// Empty component (marker)
struct Renderable
{
};

// =============================================================================
// Entity Tests
// =============================================================================

void test_entity_type_safety()
{
    using namespace fatp_ecs;

    Entity e1(42);
    Entity e2(42);
    Entity e3(99);

    TEST_ASSERT(e1 == e2, "Same-value entities should be equal");
    TEST_ASSERT(e1 != e3, "Different-value entities should not be equal");
    TEST_ASSERT(e1.get() == 42, "Entity::get() should return underlying value");
}

void test_null_entity()
{
    using namespace fatp_ecs;

    TEST_ASSERT(NullEntity == Entity::invalid(), "NullEntity should equal Entity::invalid()");
    TEST_ASSERT(!NullEntity.isValid(), "NullEntity should not be valid");

    Entity e(0);
    TEST_ASSERT(e.isValid(), "Entity(0) should be valid (not the sentinel)");
    TEST_ASSERT(e != NullEntity, "Entity(0) should not equal NullEntity");
}

// =============================================================================
// Registry: Entity Lifecycle Tests
// =============================================================================

void test_create_entity()
{
    using namespace fatp_ecs;

    Registry registry;

    Entity e1 = registry.create();
    Entity e2 = registry.create();

    TEST_ASSERT(e1 != e2, "Created entities should have distinct IDs");
    TEST_ASSERT(registry.isAlive(e1), "Created entity should be alive");
    TEST_ASSERT(registry.isAlive(e2), "Created entity should be alive");
    TEST_ASSERT(registry.entityCount() == 2, "Entity count should be 2");
}

void test_destroy_entity()
{
    using namespace fatp_ecs;

    Registry registry;

    Entity e = registry.create();
    TEST_ASSERT(registry.isAlive(e), "Entity should be alive after creation");

    bool destroyed = registry.destroy(e);
    TEST_ASSERT(destroyed, "destroy() should return true for alive entity");
    TEST_ASSERT(!registry.isAlive(e), "Entity should be dead after destroy");
    TEST_ASSERT(registry.entityCount() == 0, "Entity count should be 0 after destroy");
}

void test_double_destroy()
{
    using namespace fatp_ecs;

    Registry registry;

    Entity e = registry.create();
    registry.destroy(e);

    bool destroyed_again = registry.destroy(e);
    TEST_ASSERT(!destroyed_again, "Double destroy should return false");
}

void test_destroy_removes_components()
{
    using namespace fatp_ecs;

    Registry registry;

    Entity e = registry.create();
    registry.add<Position>(e, 10.0f, 20.0f);
    registry.add<Velocity>(e, 1.0f, 2.0f);

    TEST_ASSERT(registry.has<Position>(e), "Entity should have Position before destroy");

    registry.destroy(e);

    // After destroy, entity is dead. Component queries should return false/null.
    TEST_ASSERT(!registry.has<Position>(e), "Destroyed entity should not have Position");
    TEST_ASSERT(!registry.has<Velocity>(e), "Destroyed entity should not have Velocity");
}

void test_null_entity_operations()
{
    using namespace fatp_ecs;

    Registry registry;

    TEST_ASSERT(!registry.isAlive(NullEntity), "NullEntity should not be alive");
    TEST_ASSERT(!registry.destroy(NullEntity), "Destroying NullEntity should return false");
    TEST_ASSERT(!registry.has<Position>(NullEntity), "NullEntity should not have components");
    TEST_ASSERT(registry.tryGet<Position>(NullEntity) == nullptr,
                "tryGet on NullEntity should return nullptr");
}

void test_entity_reuse()
{
    using namespace fatp_ecs;

    Registry registry;

    // Create and destroy, creating a free slot
    Entity e1 = registry.create();
    auto originalIndex [[maybe_unused]] = e1.get();
    registry.destroy(e1);

    // Create again — the SlotMap may reuse the slot
    Entity e2 = registry.create();

    // e1 should still be dead even if slot is reused (generational safety)
    TEST_ASSERT(!registry.isAlive(e1), "Original entity should be dead after reuse");
    TEST_ASSERT(registry.isAlive(e2), "New entity should be alive");
}

// =============================================================================
// Registry: Component Tests
// =============================================================================

void test_add_and_get_component()
{
    using namespace fatp_ecs;

    Registry registry;

    Entity e = registry.create();
    Position& pos = registry.add<Position>(e, 10.0f, 20.0f);

    TEST_ASSERT(pos.x == 10.0f, "Position.x should be 10");
    TEST_ASSERT(pos.y == 20.0f, "Position.y should be 20");

    TEST_ASSERT(registry.has<Position>(e), "Entity should have Position");

    Position& retrieved = registry.get<Position>(e);
    TEST_ASSERT(retrieved.x == 10.0f, "get() should return same Position");
    TEST_ASSERT(&pos == &retrieved, "add() and get() should return same reference");
}

void test_add_idempotent()
{
    using namespace fatp_ecs;

    Registry registry;

    Entity e = registry.create();
    Position& first = registry.add<Position>(e, 10.0f, 20.0f);
    Position& second = registry.add<Position>(e, 99.0f, 99.0f);

    // Second add should return existing, NOT overwrite
    TEST_ASSERT(&first == &second, "Duplicate add should return existing component");
    TEST_ASSERT(second.x == 10.0f, "Duplicate add should not modify existing component");
}

void test_remove_component()
{
    using namespace fatp_ecs;

    Registry registry;

    Entity e = registry.create();
    registry.add<Position>(e, 1.0f, 2.0f);

    bool removed = registry.remove<Position>(e);
    TEST_ASSERT(removed, "remove() should return true for existing component");
    TEST_ASSERT(!registry.has<Position>(e), "Entity should not have Position after remove");

    bool removed_again = registry.remove<Position>(e);
    TEST_ASSERT(!removed_again, "Double remove should return false");
}

void test_tryGet_component()
{
    using namespace fatp_ecs;

    Registry registry;

    Entity e = registry.create();

    Position* before = registry.tryGet<Position>(e);
    TEST_ASSERT(before == nullptr, "tryGet should return nullptr before add");

    registry.add<Position>(e, 5.0f, 6.0f);

    Position* after = registry.tryGet<Position>(e);
    TEST_ASSERT(after != nullptr, "tryGet should return non-null after add");
    TEST_ASSERT(after->x == 5.0f, "tryGet should return correct data");
}

void test_get_throws_on_missing()
{
    using namespace fatp_ecs;

    Registry registry;

    Entity e = registry.create();

    TEST_ASSERT_THROWS((void)registry.get<Position>(e), std::out_of_range,
                       "get() on missing component should throw");
}

void test_multiple_component_types()
{
    using namespace fatp_ecs;

    Registry registry;

    Entity e = registry.create();
    registry.add<Position>(e, 1.0f, 2.0f);
    registry.add<Velocity>(e, 3.0f, 4.0f);
    registry.add<Health>(e, 50, 100);

    TEST_ASSERT(registry.has<Position>(e), "Entity should have Position");
    TEST_ASSERT(registry.has<Velocity>(e), "Entity should have Velocity");
    TEST_ASSERT(registry.has<Health>(e), "Entity should have Health");

    TEST_ASSERT(registry.get<Position>(e).x == 1.0f, "Position.x correct");
    TEST_ASSERT(registry.get<Velocity>(e).dx == 3.0f, "Velocity.dx correct");
    TEST_ASSERT(registry.get<Health>(e).hp == 50, "Health.hp correct");

    registry.remove<Velocity>(e);

    TEST_ASSERT(registry.has<Position>(e), "Position should remain after removing Velocity");
    TEST_ASSERT(!registry.has<Velocity>(e), "Velocity should be gone");
    TEST_ASSERT(registry.has<Health>(e), "Health should remain");
}

void test_string_component()
{
    using namespace fatp_ecs;

    Registry registry;

    Entity e = registry.create();
    registry.add<Tag>(e, std::string("Player"));

    TEST_ASSERT(registry.get<Tag>(e).name == "Player", "String component should work");
}

void test_empty_marker_component()
{
    using namespace fatp_ecs;

    Registry registry;

    Entity e = registry.create();
    registry.add<Renderable>(e);

    TEST_ASSERT(registry.has<Renderable>(e), "Empty marker component should be trackable");
    registry.remove<Renderable>(e);
    TEST_ASSERT(!registry.has<Renderable>(e), "Marker component should be removable");
}

// =============================================================================
// View Tests
// =============================================================================

void test_single_component_view()
{
    using namespace fatp_ecs;

    Registry registry;

    Entity e1 = registry.create();
    Entity e2 = registry.create();
    [[maybe_unused]] Entity e3 = registry.create();

    registry.add<Position>(e1, 1.0f, 0.0f);
    registry.add<Position>(e2, 2.0f, 0.0f);
    // e3 has no Position

    int count = 0;
    float sum = 0.0f;

    auto view = registry.view<Position>();
    view.each([&]([[maybe_unused]] Entity entity, Position& pos) {
        ++count;
        sum += pos.x;
    });

    TEST_ASSERT(count == 2, "Single-component view should visit 2 entities");
    TEST_ASSERT(sum == 3.0f, "Position.x sum should be 3.0");
}

void test_multi_component_view()
{
    using namespace fatp_ecs;

    Registry registry;

    Entity e1 = registry.create();
    Entity e2 = registry.create();
    Entity e3 = registry.create();

    // e1: Position + Velocity
    registry.add<Position>(e1, 0.0f, 0.0f);
    registry.add<Velocity>(e1, 1.0f, 2.0f);

    // e2: Position only
    registry.add<Position>(e2, 10.0f, 10.0f);

    // e3: Position + Velocity
    registry.add<Position>(e3, 5.0f, 5.0f);
    registry.add<Velocity>(e3, -1.0f, 0.0f);

    int count = 0;
    auto view = registry.view<Position, Velocity>();
    view.each([&]([[maybe_unused]] Entity entity, Position& pos, Velocity& vel) {
        pos.x += vel.dx;
        pos.y += vel.dy;
        ++count;
    });

    TEST_ASSERT(count == 2, "Multi-component view should visit 2 entities (e1, e3)");

    // Check e1 was updated
    TEST_ASSERT(registry.get<Position>(e1).x == 1.0f, "e1.pos.x should be updated");
    TEST_ASSERT(registry.get<Position>(e1).y == 2.0f, "e1.pos.y should be updated");

    // Check e2 was NOT updated (no Velocity)
    TEST_ASSERT(registry.get<Position>(e2).x == 10.0f, "e2.pos.x should be unchanged");

    // Check e3 was updated
    TEST_ASSERT(registry.get<Position>(e3).x == 4.0f, "e3.pos.x should be updated");
}

void test_three_component_view()
{
    using namespace fatp_ecs;

    Registry registry;

    Entity e1 = registry.create();
    Entity e2 = registry.create();

    // e1 has all three
    registry.add<Position>(e1, 0.0f, 0.0f);
    registry.add<Velocity>(e1, 1.0f, 1.0f);
    registry.add<Health>(e1, 100, 100);

    // e2 has only two
    registry.add<Position>(e2, 5.0f, 5.0f);
    registry.add<Velocity>(e2, 2.0f, 2.0f);

    int count = 0;
    auto view = registry.view<Position, Velocity, Health>();
    view.each([&]([[maybe_unused]] Entity entity,
               [[maybe_unused]] Position& pos,
               [[maybe_unused]] Velocity& vel,
               Health& hp) {
        ++count;
        TEST_ASSERT(hp.hp == 100, "Health should be 100");
    });

    TEST_ASSERT(count == 1, "Three-component view should visit only e1");
}

void test_empty_view()
{
    using namespace fatp_ecs;

    Registry registry;

    // No entities at all
    int count = 0;
    auto view = registry.view<Position>();
    view.each([&]([[maybe_unused]] Entity entity,
               [[maybe_unused]] Position& pos) { ++count; });

    TEST_ASSERT(count == 0, "Empty view should visit no entities");
}

void test_view_no_matching_entities()
{
    using namespace fatp_ecs;

    Registry registry;

    Entity e = registry.create();
    registry.add<Position>(e, 1.0f, 2.0f);

    // View requires Velocity, which no entity has
    int count = 0;
    auto view = registry.view<Velocity>();
    view.each([&]([[maybe_unused]] Entity entity,
               [[maybe_unused]] Velocity& vel) { ++count; });

    TEST_ASSERT(count == 0, "View with unregistered component should visit no entities");
}

void test_view_count()
{
    using namespace fatp_ecs;

    Registry registry;

    Entity e1 = registry.create();
    Entity e2 = registry.create();
    Entity e3 = registry.create();

    registry.add<Position>(e1, 0.0f, 0.0f);
    registry.add<Position>(e2, 0.0f, 0.0f);
    registry.add<Position>(e3, 0.0f, 0.0f);
    registry.add<Velocity>(e1, 0.0f, 0.0f);
    registry.add<Velocity>(e3, 0.0f, 0.0f);

    auto singleView = registry.view<Position>();
    TEST_ASSERT(singleView.count() == 3, "Single view count should be 3");

    auto multiView = registry.view<Position, Velocity>();
    TEST_ASSERT(multiView.count() == 2, "Multi view count should be 2");
}

// =============================================================================
// Generational Safety Tests
// =============================================================================

void test_generational_safety()
{
    using namespace fatp_ecs;

    Registry registry;

    Entity e1 = registry.create();
    registry.add<Position>(e1, 1.0f, 2.0f);

    // Destroy e1
    registry.destroy(e1);

    // Create a new entity (may reuse the same slot)
    Entity e2 = registry.create();
    registry.add<Position>(e2, 99.0f, 99.0f);

    // e1 should still be dead — even if slot index matches
    TEST_ASSERT(!registry.isAlive(e1), "Stale entity should be detected as dead");
    TEST_ASSERT(registry.isAlive(e2), "New entity should be alive");
}

// =============================================================================
// Clear Tests
// =============================================================================

void test_registry_clear()
{
    using namespace fatp_ecs;

    Registry registry;

    for (int i = 0; i < 100; ++i)
    {
        Entity e = registry.create();
        registry.add<Position>(e, static_cast<float>(i), 0.0f);
    }

    TEST_ASSERT(registry.entityCount() == 100, "Should have 100 entities before clear");

    registry.clear();

    TEST_ASSERT(registry.entityCount() == 0, "Should have 0 entities after clear");

    // Views should be empty
    int count = 0;
    auto view = registry.view<Position>();
    view.each([&]([[maybe_unused]] Entity entity,
               [[maybe_unused]] Position& pos) { ++count; });
    TEST_ASSERT(count == 0, "View should be empty after clear");

    // Should be able to create new entities after clear
    Entity e = registry.create();
    TEST_ASSERT(registry.isAlive(e), "New entity after clear should be alive");
}

// =============================================================================
// Scale Test
// =============================================================================

void test_10k_entities()
{
    using namespace fatp_ecs;

    Registry registry;

    constexpr int N = 10000;
    std::vector<Entity> entities;
    entities.reserve(N);

    // Create 10K entities with Position and Velocity
    for (int i = 0; i < N; ++i)
    {
        Entity e = registry.create();
        entities.push_back(e);
        registry.add<Position>(e, static_cast<float>(i), 0.0f);
        registry.add<Velocity>(e, 1.0f, 0.5f);
    }

    TEST_ASSERT(registry.entityCount() == N, "Should have 10K entities");

    // Iterate and update positions
    auto view = registry.view<Position, Velocity>();
    view.each([]([[maybe_unused]] Entity entity, Position& pos, Velocity& vel) {
        pos.x += vel.dx;
        pos.y += vel.dy;
    });

    // Verify first entity was updated
    TEST_ASSERT(registry.get<Position>(entities[0]).x == 1.0f,
                "Entity 0 position should be updated");
    TEST_ASSERT(registry.get<Position>(entities[0]).y == 0.5f,
                "Entity 0 position.y should be updated");

    // Destroy every other entity
    for (int i = 0; i < N; i += 2)
    {
        registry.destroy(entities[static_cast<std::size_t>(i)]);
    }

    TEST_ASSERT(registry.entityCount() == N / 2, "Should have 5K entities after culling");

    // View should only iterate remaining entities
    int count = 0;
    auto view2 = registry.view<Position, Velocity>();
    view2.each([&]([[maybe_unused]] Entity entity,
                [[maybe_unused]] Position& pos,
                [[maybe_unused]] Velocity& vel) { ++count; });

    TEST_ASSERT(count == N / 2, "View should visit 5K entities after culling");

    // Verify destroyed entities are dead
    TEST_ASSERT(!registry.isAlive(entities[0]), "Destroyed entity should be dead");
    TEST_ASSERT(registry.isAlive(entities[1]), "Surviving entity should be alive");
}

// =============================================================================
// Component Modification During Iteration Tests
// =============================================================================

void test_modify_components_during_iteration()
{
    using namespace fatp_ecs;

    Registry registry;

    Entity e1 = registry.create();
    Entity e2 = registry.create();

    registry.add<Health>(e1, 50, 100);
    registry.add<Health>(e2, 10, 100);

    // Modify component values during iteration (safe operation)
    auto view = registry.view<Health>();
    view.each([]([[maybe_unused]] Entity entity, Health& hp) {
        hp.hp = hp.maxHp; // Heal to full
    });

    TEST_ASSERT(registry.get<Health>(e1).hp == 100, "e1 should be healed");
    TEST_ASSERT(registry.get<Health>(e2).hp == 100, "e2 should be healed");
}

// =============================================================================
// allEntities Test
// =============================================================================

void test_all_entities()
{
    using namespace fatp_ecs;

    Registry registry;

    [[maybe_unused]] Entity e1 = registry.create();
    Entity e2 = registry.create();
    [[maybe_unused]] Entity e3 = registry.create();
    registry.destroy(e2);

    auto all = registry.allEntities();
    TEST_ASSERT(all.size() == 2, "allEntities should return 2 alive entities");
}

// =============================================================================
// Main
// =============================================================================

int main()
{
    std::printf("=== FAT-P ECS Phase 1 Tests ===\n\n");

    std::printf("[Entity]\n");
    RUN_TEST(test_entity_type_safety);
    RUN_TEST(test_null_entity);

    std::printf("\n[Registry: Entity Lifecycle]\n");
    RUN_TEST(test_create_entity);
    RUN_TEST(test_destroy_entity);
    RUN_TEST(test_double_destroy);
    RUN_TEST(test_destroy_removes_components);
    RUN_TEST(test_null_entity_operations);
    RUN_TEST(test_entity_reuse);

    std::printf("\n[Registry: Components]\n");
    RUN_TEST(test_add_and_get_component);
    RUN_TEST(test_add_idempotent);
    RUN_TEST(test_remove_component);
    RUN_TEST(test_tryGet_component);
    RUN_TEST(test_get_throws_on_missing);
    RUN_TEST(test_multiple_component_types);
    RUN_TEST(test_string_component);
    RUN_TEST(test_empty_marker_component);

    std::printf("\n[Views]\n");
    RUN_TEST(test_single_component_view);
    RUN_TEST(test_multi_component_view);
    RUN_TEST(test_three_component_view);
    RUN_TEST(test_empty_view);
    RUN_TEST(test_view_no_matching_entities);
    RUN_TEST(test_view_count);

    std::printf("\n[Generational Safety]\n");
    RUN_TEST(test_generational_safety);

    std::printf("\n[Clear]\n");
    RUN_TEST(test_registry_clear);

    std::printf("\n[Scale]\n");
    RUN_TEST(test_10k_entities);

    std::printf("\n[Iteration Safety]\n");
    RUN_TEST(test_modify_components_during_iteration);

    std::printf("\n[Bulk Queries]\n");
    RUN_TEST(test_all_entities);

    std::printf("\n=== Results: %d passed, %d failed ===\n",
                gTestsPassed, gTestsFailed);

    return gTestsFailed > 0 ? 1 : 0;
}
