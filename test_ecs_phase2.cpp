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

#define RUN_TEST(func)                                                   \
    do                                                                   \
    {                                                                    \
        std::printf("  Running: %s\n", #func);                          \
        func();                                                          \
        ++gTestsPassed;                                                  \
    } while (false)

// =============================================================================
// EVENT TESTS
// =============================================================================

void test_entity_created_event()
{
    Registry reg;
    int count = 0;
    auto conn = reg.events().onEntityCreated.connect([&](Entity) { ++count; });

    (void)reg.create();
    (void)reg.create();
    (void)reg.create();

    TEST_ASSERT(count == 3, "count == 3");
}

void test_entity_destroyed_event()
{
    Registry reg;
    std::vector<Entity> destroyed;
    auto conn = reg.events().onEntityDestroyed.connect(
        [&](Entity e) { destroyed.push_back(e); });

    Entity e1 = reg.create();
    Entity e2 = reg.create();
    Entity e3 = reg.create();

    reg.destroy(e2);

    TEST_ASSERT(destroyed.size() == 1, "destroyed.size() == 1");
    TEST_ASSERT(destroyed[0] == e2, "destroyed[0] == e2");

    reg.destroy(e1);
    reg.destroy(e3);

    TEST_ASSERT(destroyed.size() == 3, "destroyed.size() == 3");
}

void test_component_added_event()
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

    TEST_ASSERT(addedCount == 1, "addedCount == 1");

    // Adding same component again should NOT fire (idempotent add)
    reg.add<Position>(e, 99.0f, 99.0f);
    TEST_ASSERT(addedCount == 1, "addedCount == 1");
}

void test_component_removed_event()
{
    Registry reg;
    int removedCount = 0;
    auto conn = reg.events().onComponentRemoved<Position>().connect(
        [&](Entity) { ++removedCount; });

    Entity e = reg.create();
    reg.add<Position>(e, 10.0f, 20.0f);
    reg.remove<Position>(e);

    TEST_ASSERT(removedCount == 1, "removedCount == 1");

    // Remove again — should not fire (already removed)
    reg.remove<Position>(e);
    TEST_ASSERT(removedCount == 1, "removedCount == 1");
}

void test_component_event_not_fired_for_wrong_type()
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

    TEST_ASSERT(posAdded == 1, "posAdded == 1");
    TEST_ASSERT(velAdded == 0, "velAdded == 0");

    reg.add<Velocity>(e, 3.0f, 4.0f);
    TEST_ASSERT(posAdded == 1, "posAdded == 1");
    TEST_ASSERT(velAdded == 1, "velAdded == 1");
}

void test_scoped_connection_disconnect()
{
    Registry reg;
    int count = 0;

    {
        auto conn = reg.events().onEntityCreated.connect(
            [&](Entity) { ++count; });
        (void)reg.create();
        TEST_ASSERT(count == 1, "count == 1");
    }
    // conn is out of scope — disconnected

    (void)reg.create();
    TEST_ASSERT(count == 1, "count == 1"); // Should NOT have fired
}

void test_destroy_fires_events_before_removal()
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

    TEST_ASSERT(hadPosition == true, "hadPosition == true");
}

// =============================================================================
// COMPONENT MASK TESTS
// =============================================================================

void test_component_mask_basic()
{
    Registry reg;
    Entity e = reg.create();

    TEST_ASSERT(reg.mask(e).count() == 0, "reg.mask(e).count() == 0");

    reg.add<Position>(e, 1.0f, 2.0f);
    TEST_ASSERT(reg.mask(e).count() == 1, "reg.mask(e).count() == 1");
    TEST_ASSERT(reg.mask(e).test(typeId<Position>()), "reg.mask(e).test(typeId<Position>())");

    reg.add<Velocity>(e, 3.0f, 4.0f);
    TEST_ASSERT(reg.mask(e).count() == 2, "reg.mask(e).count() == 2");
    TEST_ASSERT(reg.mask(e).test(typeId<Velocity>()), "reg.mask(e).test(typeId<Velocity>())");

    reg.remove<Position>(e);
    TEST_ASSERT(reg.mask(e).count() == 1, "reg.mask(e).count() == 1");
    TEST_ASSERT(!reg.mask(e).test(typeId<Position>()), "!reg.mask(e).test(typeId<Position>())");
    TEST_ASSERT(reg.mask(e).test(typeId<Velocity>()), "reg.mask(e).test(typeId<Velocity>())");
}

void test_make_component_mask()
{
    // Ensure typeIds are initialized
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e);
    reg.add<Velocity>(e);

    auto mask = makeComponentMask<Position, Velocity>();
    TEST_ASSERT(mask.test(typeId<Position>()), "mask.test(typeId<Position>())");
    TEST_ASSERT(mask.test(typeId<Velocity>()), "mask.test(typeId<Velocity>())");
    TEST_ASSERT(!mask.test(typeId<Health>()), "!mask.test(typeId<Health>())");
}

void test_component_mask_dead_entity()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e, 1.0f, 2.0f);
    reg.destroy(e);

    auto mask = reg.mask(e);
    TEST_ASSERT(mask.count() == 0, "mask.count() == 0"); // Dead entity returns empty mask
}

void test_component_mask_archetype_matching()
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
    TEST_ASSERT(posVelMask.isSubsetOf(reg.mask(e1)), "posVelMask.isSubsetOf(reg.mask(e1))");

    // e2 does NOT have Velocity — posVelMask is NOT subset
    TEST_ASSERT(!posVelMask.isSubsetOf(reg.mask(e2)), "!posVelMask.isSubsetOf(reg.mask(e2))");
}

// =============================================================================
// COMMAND BUFFER TESTS
// =============================================================================

void test_command_buffer_deferred_create()
{
    Registry reg;
    CommandBuffer cmd;

    std::size_t beforeCount = reg.entityCount();

    cmd.create([](Registry& r, Entity e) {
        r.add<Position>(e, 42.0f, 84.0f);
    });

    // Not flushed yet
    TEST_ASSERT(reg.entityCount() == beforeCount, "reg.entityCount() == beforeCount");

    cmd.flush(reg);

    TEST_ASSERT(reg.entityCount() == beforeCount + 1, "reg.entityCount() == beforeCount + 1");
}

void test_command_buffer_deferred_destroy()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e, 1.0f, 2.0f);

    CommandBuffer cmd;
    cmd.destroy(e);

    TEST_ASSERT(reg.isAlive(e), "reg.isAlive(e)"); // Not yet destroyed

    cmd.flush(reg);

    TEST_ASSERT(!reg.isAlive(e), "!reg.isAlive(e)");
}

void test_command_buffer_deferred_add_remove()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Position>(e, 1.0f, 2.0f);

    CommandBuffer cmd;
    cmd.add<Velocity>(e, 3.0f, 4.0f);
    cmd.remove<Position>(e);

    TEST_ASSERT(reg.has<Position>(e), "reg.has<Position>(e)");
    TEST_ASSERT(!reg.has<Velocity>(e), "!reg.has<Velocity>(e)");

    cmd.flush(reg);

    TEST_ASSERT(!reg.has<Position>(e), "!reg.has<Position>(e)");
    TEST_ASSERT(reg.has<Velocity>(e), "reg.has<Velocity>(e)");

    auto* vel = reg.tryGet<Velocity>(e);
    TEST_ASSERT(vel != nullptr, "vel != nullptr");
    TEST_ASSERT(vel->dx == 3.0f, "vel->dx == 3.0f");
    TEST_ASSERT(vel->dy == 4.0f, "vel->dy == 4.0f");
}

void test_command_buffer_clear()
{
    Registry reg;
    Entity e = reg.create();

    CommandBuffer cmd;
    cmd.destroy(e);
    TEST_ASSERT(cmd.size() == 1, "cmd.size() == 1");

    cmd.clear();
    TEST_ASSERT(cmd.empty(), "cmd.empty()");

    cmd.flush(reg);
    TEST_ASSERT(reg.isAlive(e), "reg.isAlive(e)"); // Should NOT have been destroyed
}

void test_command_buffer_ordering()
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

    TEST_ASSERT(order.size() == 2, "order.size() == 2");
    TEST_ASSERT(order[0] == 1, "order[0] == 1");
    TEST_ASSERT(order[1] == 2, "order[1] == 2");
    TEST_ASSERT(reg.entityCount() == 2, "reg.entityCount() == 2");
}

void test_command_buffer_with_iteration()
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

    TEST_ASSERT(reg.entityCount() == 10, "reg.entityCount() == 10"); // Not yet destroyed

    cmd.flush(reg);

    // Entities with health 0, 10, 20, 30 should be destroyed (4 entities)
    TEST_ASSERT(reg.entityCount() == 6, "reg.entityCount() == 6");
}

// =============================================================================
// PARALLEL COMMAND BUFFER TESTS
// =============================================================================

void test_parallel_command_buffer_thread_safe()
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
    TEST_ASSERT(tagCount == 100, "tagCount == 100");
}

// =============================================================================
// SCHEDULER TESTS
// =============================================================================

void test_scheduler_basic_execution()
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
    TEST_ASSERT(pos.x == 1.0f, "pos.x == 1.0f");
    TEST_ASSERT(pos.y == 2.0f, "pos.y == 2.0f");
}

void test_scheduler_non_conflicting_parallel()
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

    TEST_ASSERT(physicsRan.load() == 1, "physicsRan.load() == 1");
    TEST_ASSERT(healthRan.load() == 1, "healthRan.load() == 1");
}

void test_scheduler_conflicting_sequential()
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

    TEST_ASSERT(executionOrder.size() == 2, "executionOrder.size() == 2");
    // Both ran (order guaranteed sequential but order doesn't matter)

    // Verify both effects applied
    reg.view<Position>().each([](Entity, Position& pos) {
        assert(pos.x == 1.0f);
        assert(pos.y == 1.0f);
    });
}

void test_scheduler_system_count()
{
    Scheduler scheduler(1);
    TEST_ASSERT(scheduler.systemCount() == 0, "scheduler.systemCount() == 0");

    scheduler.addSystem("A", [](Registry&) {});
    scheduler.addSystem("B", [](Registry&) {});
    TEST_ASSERT(scheduler.systemCount() == 2, "scheduler.systemCount() == 2");

    scheduler.clearSystems();
    TEST_ASSERT(scheduler.systemCount() == 0, "scheduler.systemCount() == 0");
}

void test_scheduler_empty_run()
{
    Registry reg;
    Scheduler scheduler(1);
    scheduler.run(reg); // Should not crash
}

// =============================================================================
// PARALLEL_FOR TESTS
// =============================================================================

void test_parallel_for_basic()
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
        TEST_ASSERT(data[i] == static_cast<int>(i), "data[i] == static_cast<int>(i)");
    }
}

void test_parallel_for_small_array()
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
        TEST_ASSERT(v == 42, "v == 42");
    }
}

void test_parallel_for_empty()
{
    Scheduler scheduler(4);
    scheduler.parallel_for(0, [](std::size_t, std::size_t) {
        assert(false && "Should not be called");
    });
}

void test_parallel_for_accumulation()
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
    TEST_ASSERT(sum.load() == expected, "sum.load() == expected");
}

// =============================================================================
// INTEGRATION TESTS
// =============================================================================

void test_events_and_command_buffer_integration()
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
    TEST_ASSERT(createdEntities.size() == 2, "createdEntities.size() == 2");
    TEST_ASSERT(reg.entityCount() == 2, "reg.entityCount() == 2");
}

void test_scheduler_with_command_buffer()
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
    TEST_ASSERT(reg.entityCount() == 40, "reg.entityCount() == 40");
}

void test_full_game_loop_simulation()
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
    TEST_ASSERT(destroyedCount > 0, "destroyedCount > 0");
    TEST_ASSERT(reg.entityCount() < 1000, "reg.entityCount() < 1000");

    // Verify physics was applied for survivors (they had 3 full frames of movement)
    reg.view<Position>().each([](Entity, Position& pos) {
        // Each frame adds 1.0 to x and 0.5 to y
        // Survivors were alive for all 3 frames
        assert(pos.x >= 1.0f); // At least one frame
    });
}

void test_event_during_iteration_safety()
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

    TEST_ASSERT(addedDuringIteration == 4, "addedDuringIteration == 4"); // Entities with x=6,7,8,9
}

void test_scale_10k_entities_with_events()
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

    TEST_ASSERT(created == 10000, "created == 10000");

    // Destroy half
    for (int i = 0; i < 5000; ++i)
    {
        reg.destroy(entities[i]);
    }

    TEST_ASSERT(destroyed == 5000, "destroyed == 5000");
    TEST_ASSERT(reg.entityCount() == 5000, "reg.entityCount() == 5000");
}

void test_parallel_for_ecs_iteration()
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

    TEST_ASSERT(processed.load() == viewCount, "processed.load() == viewCount");
}

// =============================================================================
// Phase 1 Regression Tests (ensure Phase 2 doesn't break Phase 1)
// =============================================================================

void test_regression_entity_lifecycle()
{
    Registry reg;
    Entity e = reg.create();
    TEST_ASSERT(reg.isAlive(e), "reg.isAlive(e)");

    reg.add<Position>(e, 1.0f, 2.0f);
    TEST_ASSERT(reg.has<Position>(e), "reg.has<Position>(e)");

    reg.destroy(e);
    TEST_ASSERT(!reg.isAlive(e), "!reg.isAlive(e)");
}

void test_regression_generational_safety()
{
    Registry reg;
    Entity e1 = reg.create();
    reg.add<Position>(e1, 1.0f, 2.0f);
    reg.destroy(e1);

    Entity e2 = reg.create();

    TEST_ASSERT(!reg.isAlive(e1), "!reg.isAlive(e1)");
    TEST_ASSERT(reg.isAlive(e2), "reg.isAlive(e2)");
    TEST_ASSERT(!reg.has<Position>(e1), "!reg.has<Position>(e1)");
}

void test_regression_view_iteration()
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

    TEST_ASSERT(count == 50, "count == 50");
}

// =============================================================================
// DEPENDENCY ANALYSIS TESTS
// =============================================================================

void test_system_descriptor_conflict_detection()
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
    TEST_ASSERT(sysA.conflictsWith(sysB), "sysA.conflictsWith(sysB)");
    TEST_ASSERT(sysB.conflictsWith(sysA), "sysB.conflictsWith(sysA)");

    SystemDescriptor sysC;
    sysC.name = "C";
    sysC.writeMask = makeComponentMask<Health>();
    sysC.readMask = {};

    // A writes Position, C writes Health — no conflict
    TEST_ASSERT(!sysA.conflictsWith(sysC), "!sysA.conflictsWith(sysC)");

    // Two readers of Position — no conflict
    SystemDescriptor sysD;
    sysD.name = "D";
    sysD.writeMask = {};
    sysD.readMask = makeComponentMask<Position>();

    TEST_ASSERT(!sysB.conflictsWith(sysD), "!sysB.conflictsWith(sysD)");
}

// =============================================================================
// Main
// =============================================================================

int main()
{
    std::printf("=== FAT-P ECS Phase 2 Tests ===\n\n");

    std::printf("[Events]\n");
    RUN_TEST(test_entity_created_event);
    RUN_TEST(test_entity_destroyed_event);
    RUN_TEST(test_component_added_event);
    RUN_TEST(test_component_removed_event);
    RUN_TEST(test_component_event_not_fired_for_wrong_type);
    RUN_TEST(test_scoped_connection_disconnect);
    RUN_TEST(test_destroy_fires_events_before_removal);

    std::printf("\n[Component Masks]\n");
    RUN_TEST(test_component_mask_basic);
    RUN_TEST(test_make_component_mask);
    RUN_TEST(test_component_mask_dead_entity);
    RUN_TEST(test_component_mask_archetype_matching);

    std::printf("\n[Command Buffer]\n");
    RUN_TEST(test_command_buffer_deferred_create);
    RUN_TEST(test_command_buffer_deferred_destroy);
    RUN_TEST(test_command_buffer_deferred_add_remove);
    RUN_TEST(test_command_buffer_clear);
    RUN_TEST(test_command_buffer_ordering);
    RUN_TEST(test_command_buffer_with_iteration);

    std::printf("\n[Parallel Command Buffer]\n");
    RUN_TEST(test_parallel_command_buffer_thread_safe);

    std::printf("\n[Scheduler]\n");
    RUN_TEST(test_scheduler_basic_execution);
    RUN_TEST(test_scheduler_non_conflicting_parallel);
    RUN_TEST(test_scheduler_conflicting_sequential);
    RUN_TEST(test_scheduler_system_count);
    RUN_TEST(test_scheduler_empty_run);

    std::printf("\n[Parallel For]\n");
    RUN_TEST(test_parallel_for_basic);
    RUN_TEST(test_parallel_for_small_array);
    RUN_TEST(test_parallel_for_empty);
    RUN_TEST(test_parallel_for_accumulation);

    std::printf("\n[Integration]\n");
    RUN_TEST(test_events_and_command_buffer_integration);
    RUN_TEST(test_scheduler_with_command_buffer);
    RUN_TEST(test_full_game_loop_simulation);
    RUN_TEST(test_event_during_iteration_safety);
    RUN_TEST(test_scale_10k_entities_with_events);
    RUN_TEST(test_parallel_for_ecs_iteration);

    std::printf("\n[Phase 1 Regression]\n");
    RUN_TEST(test_regression_entity_lifecycle);
    RUN_TEST(test_regression_generational_safety);
    RUN_TEST(test_regression_view_iteration);

    std::printf("\n[Dependency Analysis]\n");
    RUN_TEST(test_system_descriptor_conflict_detection);

    std::printf("\n=== Results: %d passed, %d failed ===\n",
                gTestsPassed, gTestsFailed);

    return gTestsFailed > 0 ? 1 : 0;
}
