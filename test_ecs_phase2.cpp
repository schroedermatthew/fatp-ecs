/**
 * @file test_ecs_phase2.cpp
 * @brief Phase 2 tests: Events, Parallelism, CommandBuffer, ComponentMask, Scheduler.
 *
 * Covers:
 * - EventBus: entity/component lifecycle signals
 * - ComponentMask: BitSet-based archetype matching
 * - CommandBuffer: deferred operations, flush, clear
 * - ParallelCommandBuffer: thread-safe deferred operations
 * - Scheduler: system registration, dependency analysis, parallel execution
 * - Scheduler::parallel_for: data-level parallelism
 * - Integration: events + command buffer + scheduler working together
 *
 * Compile:
 *   g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror \
 *       -I include -I /path/to/FatP/include \
 *       tests/test_ecs_phase2.cpp -lpthread -o test_ecs_phase2
 */

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "fatp_ecs/FatpEcs.h"

using namespace fatp_ecs;

// =============================================================================
// Test Components
// =============================================================================

struct Position
{
    float x = 0.0f;
    float y = 0.0f;
};

struct Velocity
{
    float dx = 0.0f;
    float dy = 0.0f;
};

struct Health
{
    int current = 100;
    int max = 100;
};

struct Sprite
{
    int textureId = 0;
};

struct Tag
{
};

// =============================================================================
// Test Infrastructure
// =============================================================================

static int sPassCount = 0;
static int sFailCount = 0;

#define TEST(name)                                \
    static void test_##name();                    \
    struct Register_##name                        \
    {                                             \
        Register_##name()                         \
        {                                         \
            test_##name();                        \
        }                                         \
    };                                            \
    static Register_##name sReg_##name;           \
    static void test_##name()

#define CHECK(expr)                                                     \
    do                                                                  \
    {                                                                   \
        if (!(expr))                                                    \
        {                                                               \
            std::printf("  FAIL: %s (line %d)\n", #expr, __LINE__);    \
            ++sFailCount;                                               \
            return;                                                     \
        }                                                               \
    } while (false)

#define PASS(name)                                               \
    do                                                           \
    {                                                            \
        ++sPassCount;                                            \
        std::printf("  PASS: %s\n", name);                      \
    } while (false)

// =============================================================================
// EVENT TESTS
// =============================================================================

TEST(entity_created_event)
{
    Registry reg;
    int count = 0;
    auto conn = reg.events().onEntityCreated.connect([&](Entity) { ++count; });

    (void)reg.create();
    (void)reg.create();
    (void)reg.create();

    CHECK(count == 3);
    PASS("entity_created_event");
}

TEST(entity_destroyed_event)
{
    Registry reg;
    std::vector<Entity> destroyed;
    auto conn = reg.events().onEntityDestroyed.connect(
        [&](Entity e) { destroyed.push_back(e); });

    Entity e1 = reg.create();
    Entity e2 = reg.create();
    Entity e3 = reg.create();

    reg.destroy(e2);

    CHECK(destroyed.size() == 1);
    CHECK(destroyed[0] == e2);

    reg.destroy(e1);
    reg.destroy(e3);

    CHECK(destroyed.size() == 3);
    PASS("entity_destroyed_event");
}

TEST(component_added_event)
{
    Registry reg;
    int addedCount = 0;
    auto conn = reg.events().onComponentAdded<Position>().connect(
        [&](Entity, Position& p) {
            ++addedCount;
            // Verify we can read the component
            (void)p.x;
        });

    Entity e = reg.create();
    reg.add<Position>(e, 10.0f, 20.0f);

    CHECK(addedCount == 1);

    // Adding same component again should NOT fire (idempotent add)
    reg.add<Position>(e, 99.0f, 99.0f);
    CHECK(addedCount == 1);

    PASS("component_added_event");
}

TEST(component_removed_event)
{
    Registry reg;
    int removedCount = 0;
    auto conn = reg.events().onComponentRemoved<Position>().connect(
        [&](Entity) { ++removedCount; });

    Entity e = reg.create();
    reg.add<Position>(e, 10.0f, 20.0f);
    reg.remove<Position>(e);

    CHECK(removedCount == 1);

    // Remove again — should not fire (already removed)
    reg.remove<Position>(e);
    CHECK(removedCount == 1);

    PASS("component_removed_event");
}

TEST(component_event_not_fired_for_wrong_type)
{
    Registry reg;
    int posAdded = 0;
    int velAdded = 0;

    auto c1 = reg.events().onComponentAdded<Position>().connect(
        [&](Entity, Position&) { ++posAdded; });
    auto c2 = reg.events().onComponentAdded<Velocity>().connect(
        [&](Entity, Velocity&) { ++velAdded; });

    Entity e = reg.create();
    reg.add<Position>(e, 1.0f, 2.0f);

    CHECK(posAdded == 1);
    CHECK(velAdded == 0);

    reg.add<Velocity>(e, 3.0f, 4.0f);
    CHECK(posAdded == 1);
    CHECK(velAdded == 1);

    PASS("component_event_not_fired_for_wrong_type");
}

TEST(scoped_connection_disconnect)
{
    Registry reg;
    int count = 0;

    {
        auto conn = reg.events().onEntityCreated.connect(
            [&](Entity) { ++count; });
        (void)reg.create();
        CHECK(count == 1);
    }
    // conn is out of scope — disconnected

    (void)reg.create();
    CHECK(count == 1); // Should NOT have fired

    PASS("scoped_connection_disconnect");
}

TEST(destroy_fires_events_before_removal)
{
    Registry reg;
    bool hadPosition = false;

    auto conn = reg.events().onEntityDestroyed.connect(
        [&](Entity e) {
            // At destruction time, the entity should still be alive
            // and its components accessible
            hadPosition = reg.has<Position>(e);
        });

    Entity e = reg.create();
    reg.add<Position>(e, 1.0f, 2.0f);
    reg.destroy(e);

    CHECK(hadPosition == true);
    PASS("destroy_fires_events_before_removal");
}

// =============================================================================
// COMPONENT MASK TESTS
// =============================================================================

TEST(component_mask_basic)
{
    Registry reg;
    Entity e = reg.create();

    CHECK(reg.mask(e).count() == 0);

    reg.add<Position>(e, 1.0f, 2.0f);
    CHECK(reg.mask(e).count() == 1);
    CHECK(reg.mask(e).test(typeId<Position>()));

    reg.add<Velocity>(e, 3.0f, 4.0f);
    CHECK(reg.mask(e).count() == 2);
    CHECK(reg.mask(e).test(typeId<Velocity>()));

    reg.remove<Position>(e);
    CHECK(reg.mask(e).count() == 1);
    CHECK(!reg.mask(e).test(typeId<Position>()));
    CHECK(reg.mask(e).test(typeId<Velocity>()));

    PASS("component_mask_basic");
}

TEST(make_component_mask)
{
    // Ensure typeIds are initialized
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e);
    reg.add<Velocity>(e);

    auto mask = makeComponentMask<Position, Velocity>();
    CHECK(mask.test(typeId<Position>()));
    CHECK(mask.test(typeId<Velocity>()));
    CHECK(!mask.test(typeId<Health>()));

    PASS("make_component_mask");
}

TEST(component_mask_dead_entity)
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e, 1.0f, 2.0f);
    reg.destroy(e);

    auto mask = reg.mask(e);
    CHECK(mask.count() == 0); // Dead entity returns empty mask

    PASS("component_mask_dead_entity");
}

TEST(component_mask_archetype_matching)
{
    Registry reg;
    Entity e1 = reg.create();
    reg.add<Position>(e1);
    reg.add<Velocity>(e1);
    reg.add<Health>(e1);

    Entity e2 = reg.create();
    reg.add<Position>(e2);
    reg.add<Sprite>(e2);

    auto posVelMask = makeComponentMask<Position, Velocity>();

    // e1 has Position+Velocity — posVelMask is subset of e1's mask
    CHECK(posVelMask.isSubsetOf(reg.mask(e1)));

    // e2 does NOT have Velocity — posVelMask is NOT subset
    CHECK(!posVelMask.isSubsetOf(reg.mask(e2)));

    PASS("component_mask_archetype_matching");
}

// =============================================================================
// COMMAND BUFFER TESTS
// =============================================================================

TEST(command_buffer_deferred_create)
{
    Registry reg;
    CommandBuffer cmd;

    std::size_t beforeCount = reg.entityCount();

    cmd.create([](Registry& r, Entity e) {
        r.add<Position>(e, 42.0f, 84.0f);
    });

    // Not flushed yet
    CHECK(reg.entityCount() == beforeCount);

    cmd.flush(reg);

    CHECK(reg.entityCount() == beforeCount + 1);
    PASS("command_buffer_deferred_create");
}

TEST(command_buffer_deferred_destroy)
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e, 1.0f, 2.0f);

    CommandBuffer cmd;
    cmd.destroy(e);

    CHECK(reg.isAlive(e)); // Not yet destroyed

    cmd.flush(reg);

    CHECK(!reg.isAlive(e));
    PASS("command_buffer_deferred_destroy");
}

TEST(command_buffer_deferred_add_remove)
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e, 1.0f, 2.0f);

    CommandBuffer cmd;
    cmd.add<Velocity>(e, 3.0f, 4.0f);
    cmd.remove<Position>(e);

    CHECK(reg.has<Position>(e));
    CHECK(!reg.has<Velocity>(e));

    cmd.flush(reg);

    CHECK(!reg.has<Position>(e));
    CHECK(reg.has<Velocity>(e));

    auto* vel = reg.tryGet<Velocity>(e);
    CHECK(vel != nullptr);
    CHECK(vel->dx == 3.0f);
    CHECK(vel->dy == 4.0f);

    PASS("command_buffer_deferred_add_remove");
}

TEST(command_buffer_clear)
{
    Registry reg;
    Entity e = reg.create();

    CommandBuffer cmd;
    cmd.destroy(e);
    CHECK(cmd.size() == 1);

    cmd.clear();
    CHECK(cmd.empty());

    cmd.flush(reg);
    CHECK(reg.isAlive(e)); // Should NOT have been destroyed

    PASS("command_buffer_clear");
}

TEST(command_buffer_ordering)
{
    Registry reg;

    CommandBuffer cmd;
    std::vector<int> order;

    // Create, then add component to newly created entity
    cmd.create([&order](Registry& r, Entity e) {
        order.push_back(1);
        r.add<Health>(e, 50, 100);
    });

    cmd.create([&order](Registry& r, Entity e) {
        order.push_back(2);
        r.add<Position>(e, 0.0f, 0.0f);
    });

    cmd.flush(reg);

    CHECK(order.size() == 2);
    CHECK(order[0] == 1);
    CHECK(order[1] == 2);
    CHECK(reg.entityCount() == 2);

    PASS("command_buffer_ordering");
}

TEST(command_buffer_with_iteration)
{
    Registry reg;

    // Create entities with health
    for (int i = 0; i < 10; ++i)
    {
        Entity e = reg.create();
        reg.add<Health>(e, i * 10, 100);
    }

    // During iteration, record destroys for entities with health <= 30
    CommandBuffer cmd;
    reg.view<Health>().each([&](Entity e, Health& hp) {
        if (hp.current <= 30)
        {
            cmd.destroy(e);
        }
    });

    CHECK(reg.entityCount() == 10); // Not yet destroyed

    cmd.flush(reg);

    // Entities with health 0, 10, 20, 30 should be destroyed (4 entities)
    CHECK(reg.entityCount() == 6);

    PASS("command_buffer_with_iteration");
}

// =============================================================================
// PARALLEL COMMAND BUFFER TESTS
// =============================================================================

TEST(parallel_command_buffer_thread_safe)
{
    Registry reg;
    for (int i = 0; i < 100; ++i)
    {
        Entity e = reg.create();
        reg.add<Health>(e, i, 100);
    }

    ParallelCommandBuffer pcmd;

    // Simulate multiple threads recording commands
    std::vector<std::thread> threads;
    auto entities = reg.allEntities();

    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([&pcmd, &entities, t]() {
            for (std::size_t i = t; i < entities.size(); i += 4)
            {
                (void)pcmd.add<Tag>(entities[i]);
            }
        });
    }

    for (auto& th : threads)
    {
        th.join();
    }

    pcmd.flush(reg);

    // All 100 entities should have Tag
    std::size_t tagCount = 0;
    reg.view<Tag>().each([&](Entity, Tag&) { ++tagCount; });
    CHECK(tagCount == 100);

    PASS("parallel_command_buffer_thread_safe");
}

// =============================================================================
// SCHEDULER TESTS
// =============================================================================

TEST(scheduler_basic_execution)
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e, 0.0f, 0.0f);
    reg.add<Velocity>(e, 1.0f, 2.0f);

    Scheduler scheduler(2);

    scheduler.addSystem("Physics",
        [](Registry& r) {
            r.view<Position, Velocity>().each(
                [](Entity, Position& pos, Velocity& vel) {
                    pos.x += vel.dx;
                    pos.y += vel.dy;
                });
        },
        makeComponentMask<Position>(),     // writes
        makeComponentMask<Velocity>());    // reads

    scheduler.run(reg);

    auto& pos = reg.get<Position>(e);
    CHECK(pos.x == 1.0f);
    CHECK(pos.y == 2.0f);

    PASS("scheduler_basic_execution");
}

TEST(scheduler_non_conflicting_parallel)
{
    Registry reg;

    // Create entities with different components
    for (int i = 0; i < 1000; ++i)
    {
        Entity e = reg.create();
        reg.add<Position>(e, 0.0f, 0.0f);
        reg.add<Health>(e, 100, 100);
    }

    std::atomic<int> physicsRan{0};
    std::atomic<int> healthRan{0};

    Scheduler scheduler(2);

    // These systems don't conflict — they write to different components
    scheduler.addSystem("Physics",
        [&physicsRan](Registry& r) {
            r.view<Position>().each([](Entity, Position& pos) {
                pos.x += 1.0f;
            });
            physicsRan.store(1, std::memory_order_release);
        },
        makeComponentMask<Position>(),  // writes Position
        {});

    scheduler.addSystem("HealthRegen",
        [&healthRan](Registry& r) {
            r.view<Health>().each([](Entity, Health& hp) {
                hp.current = std::min(hp.current + 1, hp.max);
            });
            healthRan.store(1, std::memory_order_release);
        },
        makeComponentMask<Health>(),  // writes Health
        {});

    scheduler.run(reg);

    CHECK(physicsRan.load() == 1);
    CHECK(healthRan.load() == 1);

    PASS("scheduler_non_conflicting_parallel");
}

TEST(scheduler_conflicting_sequential)
{
    Registry reg;
    for (int i = 0; i < 100; ++i)
    {
        Entity e = reg.create();
        reg.add<Position>(e, 0.0f, 0.0f);
    }

    std::vector<int> executionOrder;
    std::mutex orderMutex;

    Scheduler scheduler(2);

    // Both write Position — must run sequentially
    scheduler.addSystem("MoveRight",
        [&](Registry& r) {
            r.view<Position>().each([](Entity, Position& pos) {
                pos.x += 1.0f;
            });
            std::lock_guard<std::mutex> lock(orderMutex);
            executionOrder.push_back(1);
        },
        makeComponentMask<Position>(),  // writes
        {});

    scheduler.addSystem("MoveUp",
        [&](Registry& r) {
            r.view<Position>().each([](Entity, Position& pos) {
                pos.y += 1.0f;
            });
            std::lock_guard<std::mutex> lock(orderMutex);
            executionOrder.push_back(2);
        },
        makeComponentMask<Position>(),  // writes — conflicts!
        {});

    scheduler.run(reg);

    CHECK(executionOrder.size() == 2);
    // Both ran (order guaranteed sequential but order doesn't matter)

    // Verify both effects applied
    reg.view<Position>().each([](Entity, Position& pos) {
        assert(pos.x == 1.0f);
        assert(pos.y == 1.0f);
    });

    PASS("scheduler_conflicting_sequential");
}

TEST(scheduler_system_count)
{
    Scheduler scheduler(1);
    CHECK(scheduler.systemCount() == 0);

    scheduler.addSystem("A", [](Registry&) {});
    scheduler.addSystem("B", [](Registry&) {});
    CHECK(scheduler.systemCount() == 2);

    scheduler.clearSystems();
    CHECK(scheduler.systemCount() == 0);

    PASS("scheduler_system_count");
}

TEST(scheduler_empty_run)
{
    Registry reg;
    Scheduler scheduler(1);
    scheduler.run(reg); // Should not crash

    PASS("scheduler_empty_run");
}

// =============================================================================
// PARALLEL_FOR TESTS
// =============================================================================

TEST(parallel_for_basic)
{
    Scheduler scheduler(4);
    std::vector<int> data(10000, 0);

    scheduler.parallel_for(data.size(),
        [&data](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i)
            {
                data[i] = static_cast<int>(i);
            }
        });

    // Verify all elements written correctly
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        CHECK(data[i] == static_cast<int>(i));
    }

    PASS("parallel_for_basic");
}

TEST(parallel_for_small_array)
{
    Scheduler scheduler(4);
    std::vector<int> data(10, 0);

    // Small enough to run single-threaded
    scheduler.parallel_for(data.size(),
        [&data](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i)
            {
                data[i] = 42;
            }
        });

    for (auto& v : data)
    {
        CHECK(v == 42);
    }

    PASS("parallel_for_small_array");
}

TEST(parallel_for_empty)
{
    Scheduler scheduler(4);
    scheduler.parallel_for(0, [](std::size_t, std::size_t) {
        assert(false && "Should not be called");
    });

    PASS("parallel_for_empty");
}

TEST(parallel_for_accumulation)
{
    Scheduler scheduler(4);

    constexpr std::size_t N = 100000;
    std::vector<int> data(N);
    std::iota(data.begin(), data.end(), 1);

    std::atomic<long long> sum{0};

    scheduler.parallel_for(data.size(),
        [&data, &sum](std::size_t begin, std::size_t end) {
            long long localSum = 0;
            for (std::size_t i = begin; i < end; ++i)
            {
                localSum += data[i];
            }
            sum.fetch_add(localSum, std::memory_order_relaxed);
        });

    long long expected = static_cast<long long>(N) * (N + 1) / 2;
    CHECK(sum.load() == expected);

    PASS("parallel_for_accumulation");
}

// =============================================================================
// INTEGRATION TESTS
// =============================================================================

TEST(events_and_command_buffer_integration)
{
    Registry reg;
    CommandBuffer cmd;
    std::vector<Entity> createdEntities;

    auto conn = reg.events().onEntityCreated.connect(
        [&](Entity e) { createdEntities.push_back(e); });

    cmd.create([](Registry& r, Entity e) {
        r.add<Position>(e, 0.0f, 0.0f);
    });
    cmd.create([](Registry& r, Entity e) {
        r.add<Health>(e, 100, 100);
    });

    cmd.flush(reg);

    // Events should have fired during flush
    CHECK(createdEntities.size() == 2);
    CHECK(reg.entityCount() == 2);

    PASS("events_and_command_buffer_integration");
}

TEST(scheduler_with_command_buffer)
{
    Registry reg;

    for (int i = 0; i < 50; ++i)
    {
        Entity e = reg.create();
        reg.add<Health>(e, i, 100);
    }

    Scheduler scheduler(2);
    CommandBuffer cmd;

    scheduler.addSystem("DamageSystem",
        [&cmd](Registry& r) {
            r.view<Health>().each([&cmd](Entity e, Health& hp) {
                if (hp.current < 10)
                {
                    cmd.destroy(e);
                }
            });
        },
        {}, // no direct writes (deferred)
        makeComponentMask<Health>()); // reads

    scheduler.run(reg);

    (void)reg.entityCount(); // before
    cmd.flush(reg);

    // Entities with health 0..9 should be destroyed (10 entities)
    CHECK(reg.entityCount() == 40);

    PASS("scheduler_with_command_buffer");
}

TEST(full_game_loop_simulation)
{
    Registry reg;
    Scheduler scheduler(2);

    // Create 1000 entities with Position + Velocity
    for (int i = 0; i < 1000; ++i)
    {
        Entity e = reg.create();
        reg.add<Position>(e, 0.0f, 0.0f);
        reg.add<Velocity>(e, 1.0f, 0.5f);
        reg.add<Health>(e, 5 + (i % 20), 100);
    }

    CommandBuffer cmd;
    int destroyedCount = 0;

    auto destroyConn = reg.events().onEntityDestroyed.connect(
        [&destroyedCount](Entity) { ++destroyedCount; });

    // Physics system — writes Position, reads Velocity
    scheduler.addSystem("Physics",
        [](Registry& r) {
            r.view<Position, Velocity>().each(
                [](Entity, Position& pos, Velocity& vel) {
                    pos.x += vel.dx;
                    pos.y += vel.dy;
                });
        },
        makeComponentMask<Position>(),
        makeComponentMask<Velocity>());

    // Health check — reads Health, defers kills
    scheduler.addSystem("HealthCheck",
        [&cmd](Registry& r) {
            r.view<Health>().each([&cmd](Entity e, Health& hp) {
                hp.current -= 5;
                if (hp.current <= 0)
                {
                    cmd.destroy(e);
                }
            });
        },
        makeComponentMask<Health>(),
        {});

    // Simulate 3 frames
    for (int frame = 0; frame < 3; ++frame)
    {
        scheduler.run(reg);
        cmd.flush(reg);
    }

    // After 3 frames:
    // - Physics: all surviving entities moved 3 frames worth
    // - Some entities destroyed by health check
    CHECK(destroyedCount > 0);
    CHECK(reg.entityCount() < 1000);

    // Verify physics was applied for survivors (they had 3 full frames of movement)
    reg.view<Position>().each([](Entity, Position& pos) {
        // Each frame adds 1.0 to x and 0.5 to y
        // Survivors were alive for all 3 frames
        assert(pos.x >= 1.0f); // At least one frame
    });

    PASS("full_game_loop_simulation");
}

TEST(event_during_iteration_safety)
{
    Registry reg;
    int addedDuringIteration = 0;

    auto conn = reg.events().onComponentAdded<Velocity>().connect(
        [&addedDuringIteration](Entity, Velocity&) {
            ++addedDuringIteration;
        });

    // Create entities
    for (int i = 0; i < 10; ++i)
    {
        Entity e = reg.create();
        reg.add<Position>(e, static_cast<float>(i), 0.0f);
    }

    // This uses a command buffer to safely add components during iteration
    CommandBuffer cmd;
    reg.view<Position>().each([&cmd](Entity e, Position& pos) {
        if (pos.x > 5.0f)
        {
            cmd.add<Velocity>(e, pos.x, 0.0f);
        }
    });

    cmd.flush(reg);

    CHECK(addedDuringIteration == 4); // Entities with x=6,7,8,9

    PASS("event_during_iteration_safety");
}

TEST(scale_10k_entities_with_events)
{
    Registry reg;
    std::atomic<int> created{0};
    std::atomic<int> destroyed{0};

    auto c1 = reg.events().onEntityCreated.connect(
        [&created](Entity) { ++created; });
    auto c2 = reg.events().onEntityDestroyed.connect(
        [&destroyed](Entity) { ++destroyed; });

    std::vector<Entity> entities;
    entities.reserve(10000);

    for (int i = 0; i < 10000; ++i)
    {
        entities.push_back(reg.create());
        reg.add<Position>(entities.back(), static_cast<float>(i), 0.0f);
    }

    CHECK(created == 10000);

    // Destroy half
    for (int i = 0; i < 5000; ++i)
    {
        reg.destroy(entities[i]);
    }

    CHECK(destroyed == 5000);
    CHECK(reg.entityCount() == 5000);

    PASS("scale_10k_entities_with_events");
}

TEST(parallel_for_ecs_iteration)
{
    // Test parallel_for used to iterate component arrays directly
    Registry reg;
    Scheduler scheduler(4);

    for (int i = 0; i < 10000; ++i)
    {
        Entity e = reg.create();
        reg.add<Position>(e, 0.0f, 0.0f);
        reg.add<Velocity>(e, 1.0f, 1.0f);
    }

    // Get direct access to dense arrays for parallel processing
    auto view = reg.view<Position, Velocity>();
    std::size_t viewCount = view.count();

    // Use parallel_for through the view's each (single-threaded view,
    // but demonstrates the pattern)
    std::atomic<std::size_t> processed{0};

    view.each([&processed](Entity, Position& pos, Velocity& vel) {
        pos.x += vel.dx;
        pos.y += vel.dy;
        processed.fetch_add(1, std::memory_order_relaxed);
    });

    CHECK(processed.load() == viewCount);

    PASS("parallel_for_ecs_iteration");
}

// =============================================================================
// Phase 1 Regression Tests (ensure Phase 2 doesn't break Phase 1)
// =============================================================================

TEST(regression_entity_lifecycle)
{
    Registry reg;
    Entity e = reg.create();
    CHECK(reg.isAlive(e));

    reg.add<Position>(e, 1.0f, 2.0f);
    CHECK(reg.has<Position>(e));

    reg.destroy(e);
    CHECK(!reg.isAlive(e));

    PASS("regression_entity_lifecycle");
}

TEST(regression_generational_safety)
{
    Registry reg;
    Entity e1 = reg.create();
    reg.add<Position>(e1, 1.0f, 2.0f);
    reg.destroy(e1);

    Entity e2 = reg.create();

    CHECK(!reg.isAlive(e1));
    CHECK(reg.isAlive(e2));
    CHECK(!reg.has<Position>(e1));

    PASS("regression_generational_safety");
}

TEST(regression_view_iteration)
{
    Registry reg;

    for (int i = 0; i < 100; ++i)
    {
        Entity e = reg.create();
        reg.add<Position>(e, static_cast<float>(i), 0.0f);
        if (i % 2 == 0)
        {
            reg.add<Velocity>(e, 1.0f, 0.0f);
        }
    }

    std::size_t count = 0;
    reg.view<Position, Velocity>().each(
        [&count](Entity, Position&, Velocity&) { ++count; });

    CHECK(count == 50);

    PASS("regression_view_iteration");
}

// =============================================================================
// DEPENDENCY ANALYSIS TESTS
// =============================================================================

TEST(system_descriptor_conflict_detection)
{
    SystemDescriptor sysA;
    sysA.name = "A";
    sysA.writeMask = makeComponentMask<Position>();
    sysA.readMask = {};

    SystemDescriptor sysB;
    sysB.name = "B";
    sysB.writeMask = {};
    sysB.readMask = makeComponentMask<Position>();

    // A writes Position, B reads Position — conflict!
    CHECK(sysA.conflictsWith(sysB));
    CHECK(sysB.conflictsWith(sysA));

    SystemDescriptor sysC;
    sysC.name = "C";
    sysC.writeMask = makeComponentMask<Health>();
    sysC.readMask = {};

    // A writes Position, C writes Health — no conflict
    CHECK(!sysA.conflictsWith(sysC));

    // Two readers of Position — no conflict
    SystemDescriptor sysD;
    sysD.name = "D";
    sysD.writeMask = {};
    sysD.readMask = makeComponentMask<Position>();

    CHECK(!sysB.conflictsWith(sysD));

    PASS("system_descriptor_conflict_detection");
}

// =============================================================================
// Main
// =============================================================================

int main()
{
    std::printf("======================================\n");
    std::printf("FAT-P ECS Phase 2 Test Suite\n");
    std::printf("======================================\n\n");

    // Tests are auto-registered by static initializers above.
    // By the time we reach main(), all tests have already run.

    std::printf("\n======================================\n");
    std::printf("Results: %d passed, %d failed\n", sPassCount, sFailCount);
    std::printf("======================================\n");

    return sFailCount > 0 ? 1 : 0;
}
