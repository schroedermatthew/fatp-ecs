/**
 * @file test_ecs_phase3.cpp
 * @brief Phase 3 tests: Gameplay Infrastructure
 *
 * Tests: FrameAllocator, EntityNames, EntityTemplate, SystemToggle,
 *        SafeMath, StateMachine integration, full game loop simulation.
 *
 * FAT-P components exercised:
 * - ObjectPool (FrameAllocator)
 * - StringPool + FlatMap (EntityNames)
 * - JsonLite (EntityTemplate)
 * - FeatureManager (SystemToggle)
 * - CheckedArithmetic (SafeMath)
 * - StateMachine (AI state machines)
 * - AlignedVector (SIMD-friendly component storage)
 */

#include <fatp_ecs/FatpEcs.h>

#include <fat_p/AlignedVector.h>
#include <fat_p/StateMachine.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

using namespace fatp_ecs;

// =============================================================================
// Test Harness (same pattern as Phase 1+2)
// =============================================================================

static int sTestsPassed = 0;
static int sTestsFailed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++sTestsFailed; \
            return; \
        } \
    } while (0)

#define RUN_TEST(fn) \
    do { \
        std::printf("  Running: %s\n", #fn); \
        fn(); \
        if (sTestsFailed == 0 || true) { ++sTestsPassed; } \
    } while (0)

// =============================================================================
// Game Component Types
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
    int hp = 100;
    int maxHp = 100;
};

struct Damage
{
    int amount = 10;
};

struct Score
{
    int value = 0;
};

struct AITag {};    // Marker component for entities with AI
struct DeadTag {};  // Marker for entities pending cleanup

struct Name
{
    const char* value = nullptr;
};

// Collision result for FrameAllocator tests
struct CollisionPair
{
    Entity a;
    Entity b;
    float distance;
};

// =============================================================================
// FrameAllocator Tests
// =============================================================================

void test_frame_allocator_basic()
{
    FrameAllocator<CollisionPair> allocator(32);

    auto* c1 = allocator.acquire(Entity{}, Entity{}, 1.0f);
    auto* c2 = allocator.acquire(Entity{}, Entity{}, 2.0f);

    TEST_ASSERT(c1 != nullptr, "First acquire should succeed");
    TEST_ASSERT(c2 != nullptr, "Second acquire should succeed");
    TEST_ASSERT(allocator.activeCount() == 2, "Should have 2 active");
    TEST_ASSERT(c1->distance == 1.0f, "First pair distance should be 1.0");
    TEST_ASSERT(c2->distance == 2.0f, "Second pair distance should be 2.0");

    allocator.releaseAll();
    TEST_ASSERT(allocator.activeCount() == 0, "After releaseAll, active should be 0");
}

void test_frame_allocator_reuse()
{
    FrameAllocator<CollisionPair> allocator(16);

    // Frame 1: acquire some objects
    for (int i = 0; i < 10; ++i)
    {
        (void)allocator.acquire(Entity{}, Entity{}, static_cast<float>(i));
    }
    TEST_ASSERT(allocator.activeCount() == 10, "Should have 10 active in frame 1");
    allocator.releaseAll();

    // Frame 2: acquire again — should reuse pool memory
    std::size_t capacityBefore = allocator.capacity();
    for (int i = 0; i < 10; ++i)
    {
        (void)allocator.acquire(Entity{}, Entity{}, static_cast<float>(i));
    }
    TEST_ASSERT(allocator.capacity() == capacityBefore,
                "Capacity should not grow when reusing released objects");
    allocator.releaseAll();
}

void test_frame_allocator_grow()
{
    FrameAllocator<CollisionPair> allocator(4);

    // Acquire more than one block
    for (int i = 0; i < 20; ++i)
    {
        auto* c = allocator.acquire(Entity{}, Entity{}, static_cast<float>(i));
        TEST_ASSERT(c != nullptr, "Acquire should succeed even past block size");
    }
    TEST_ASSERT(allocator.activeCount() == 20, "Should have 20 active");
    TEST_ASSERT(allocator.capacity() >= 20, "Capacity should accommodate 20 objects");
    allocator.releaseAll();
}

// =============================================================================
// EntityNames Tests
// =============================================================================

void test_entity_names_basic()
{
    Registry registry;
    EntityNames names;

    Entity player = registry.create();
    Entity enemy = registry.create();

    const char* pName = names.setName(player, "player_1");
    const char* eName = names.setName(enemy, "goblin_01");

    TEST_ASSERT(pName != nullptr, "setName should return interned pointer");
    TEST_ASSERT(eName != nullptr, "setName for enemy should succeed");
    TEST_ASSERT(names.size() == 2, "Should have 2 named entities");
}

void test_entity_names_lookup()
{
    Registry registry;
    EntityNames names;

    Entity e = registry.create();
    names.setName(e, "hero");

    Entity found = names.findByName("hero");
    TEST_ASSERT(found == e, "findByName should return the correct entity");

    Entity notFound = names.findByName("villain");
    TEST_ASSERT(notFound == NullEntity, "findByName for nonexistent name should return NullEntity");
}

void test_entity_names_reverse_lookup()
{
    Registry registry;
    EntityNames names;

    Entity e = registry.create();
    names.setName(e, "wizard");

    const char* name = names.getName(e);
    TEST_ASSERT(name != nullptr, "getName should return name");
    TEST_ASSERT(std::string(name) == "wizard", "getName should return 'wizard'");

    Entity unnamed = registry.create();
    TEST_ASSERT(names.getName(unnamed) == nullptr, "Unnamed entity should return nullptr");
}

void test_entity_names_duplicate_name_rejected()
{
    Registry registry;
    EntityNames names;

    Entity e1 = registry.create();
    Entity e2 = registry.create();

    names.setName(e1, "unique_name");
    const char* result = names.setName(e2, "unique_name");

    TEST_ASSERT(result == nullptr, "Duplicate name for different entity should be rejected");
    TEST_ASSERT(names.findByName("unique_name") == e1, "Original mapping should be preserved");
}

void test_entity_names_rename()
{
    Registry registry;
    EntityNames names;

    Entity e = registry.create();
    names.setName(e, "old_name");
    names.setName(e, "new_name");

    TEST_ASSERT(names.findByName("new_name") == e, "Should find by new name");
    TEST_ASSERT(names.findByName("old_name") == NullEntity, "Old name should be cleared");
    TEST_ASSERT(names.size() == 1, "Size should still be 1 after rename");
}

void test_entity_names_remove()
{
    Registry registry;
    EntityNames names;

    Entity e = registry.create();
    names.setName(e, "temp");
    TEST_ASSERT(names.removeName(e) == true, "removeName should return true");
    TEST_ASSERT(names.findByName("temp") == NullEntity, "Name should be cleared");
    TEST_ASSERT(names.getName(e) == nullptr, "Entity should be unnamed");
    TEST_ASSERT(names.size() == 0, "Size should be 0");
}

// =============================================================================
// EntityTemplate Tests
// =============================================================================

void test_entity_template_register_and_spawn()
{
    Registry registry;
    TemplateRegistry templates;

    // Register component factories
    templates.registerComponent("Position", [](Registry& reg, Entity e, const fat_p::JsonValue& data)
    {
        auto& obj = std::get<fat_p::JsonObject>(data);
        float x = static_cast<float>(std::get<double>(obj.at("x")));
        float y = static_cast<float>(std::get<double>(obj.at("y")));
        reg.add<Position>(e, x, y);
    });

    templates.registerComponent("Health", [](Registry& reg, Entity e, const fat_p::JsonValue& data)
    {
        auto& obj = std::get<fat_p::JsonObject>(data);
        int hp = static_cast<int>(std::get<int64_t>(obj.at("current")));
        int maxHp = static_cast<int>(std::get<int64_t>(obj.at("max")));
        reg.add<Health>(e, hp, maxHp);
    });

    // Register template from JSON
    bool added = templates.addTemplate("goblin", R"({
        "components": {
            "Position": { "x": 0.0, "y": 0.0 },
            "Health": { "current": 50, "max": 50 }
        }
    })");
    TEST_ASSERT(added, "addTemplate should succeed");
    TEST_ASSERT(templates.hasTemplate("goblin"), "Template should exist");

    // Spawn from template
    Entity goblin = templates.spawn(registry, "goblin");
    TEST_ASSERT(goblin != NullEntity, "spawn should return a valid entity");
    TEST_ASSERT(registry.has<Position>(goblin), "Goblin should have Position");
    TEST_ASSERT(registry.has<Health>(goblin), "Goblin should have Health");
    TEST_ASSERT(registry.get<Health>(goblin).hp == 50, "Goblin hp should be 50");
    TEST_ASSERT(registry.get<Health>(goblin).maxHp == 50, "Goblin maxHp should be 50");
}

void test_entity_template_missing_template()
{
    Registry registry;
    TemplateRegistry templates;

    Entity result = templates.spawn(registry, "nonexistent");
    TEST_ASSERT(result == NullEntity, "Spawning nonexistent template should return NullEntity");
}

void test_entity_template_multiple_spawns()
{
    Registry registry;
    TemplateRegistry templates;

    templates.registerComponent("Position", [](Registry& reg, Entity e, const fat_p::JsonValue& data)
    {
        auto& obj = std::get<fat_p::JsonObject>(data);
        float x = static_cast<float>(std::get<double>(obj.at("x")));
        float y = static_cast<float>(std::get<double>(obj.at("y")));
        reg.add<Position>(e, x, y);
    });

    templates.addTemplate("bullet", R"({
        "components": {
            "Position": { "x": 5.0, "y": 10.0 }
        }
    })");

    std::vector<Entity> bullets;
    for (int i = 0; i < 100; ++i)
    {
        bullets.push_back(templates.spawn(registry, "bullet"));
    }

    TEST_ASSERT(registry.entityCount() == 100, "Should have 100 entities");
    for (auto b : bullets)
    {
        TEST_ASSERT(registry.has<Position>(b), "Each bullet should have Position");
        TEST_ASSERT(registry.get<Position>(b).x == 5.0f, "Each bullet x should be 5.0");
    }
}

// =============================================================================
// SystemToggle Tests
// =============================================================================

void test_system_toggle_basic()
{
    SystemToggle toggle;

    TEST_ASSERT(toggle.registerSystem("physics", true), "Register physics enabled");
    TEST_ASSERT(toggle.registerSystem("rendering", true), "Register rendering enabled");
    TEST_ASSERT(toggle.registerSystem("debug_overlay", false), "Register debug disabled");

    TEST_ASSERT(toggle.isEnabled("physics"), "Physics should be enabled");
    TEST_ASSERT(toggle.isEnabled("rendering"), "Rendering should be enabled");
    TEST_ASSERT(!toggle.isEnabled("debug_overlay"), "Debug should be disabled");
}

void test_system_toggle_enable_disable()
{
    SystemToggle toggle;
    toggle.registerSystem("ai", true);

    TEST_ASSERT(toggle.isEnabled("ai"), "AI should start enabled");

    toggle.disable("ai");
    TEST_ASSERT(!toggle.isEnabled("ai"), "AI should be disabled");

    toggle.enable("ai");
    TEST_ASSERT(toggle.isEnabled("ai"), "AI should be re-enabled");
}

void test_system_toggle_unregistered()
{
    SystemToggle toggle;
    TEST_ASSERT(!toggle.isEnabled("nonexistent"), "Unregistered system should not be enabled");
}

// =============================================================================
// SafeMath Tests
// =============================================================================

void test_safe_math_clamped_add()
{
    // Normal case
    TEST_ASSERT(clampedAdd(10, 20) == 30, "10 + 20 = 30");

    // Overflow saturation
    int maxInt = std::numeric_limits<int>::max();
    TEST_ASSERT(clampedAdd(maxInt, 1) == maxInt, "MAX + 1 should saturate at MAX");
    TEST_ASSERT(clampedAdd(maxInt, maxInt) == maxInt, "MAX + MAX should saturate");

    // Underflow saturation
    int minInt = std::numeric_limits<int>::min();
    TEST_ASSERT(clampedAdd(minInt, -1) == minInt, "MIN + (-1) should saturate at MIN");
}

void test_safe_math_clamped_sub()
{
    TEST_ASSERT(clampedSub(100, 30) == 70, "100 - 30 = 70");

    int minInt = std::numeric_limits<int>::min();
    TEST_ASSERT(clampedSub(minInt, 1) == minInt, "MIN - 1 should saturate at MIN");

    int maxInt = std::numeric_limits<int>::max();
    TEST_ASSERT(clampedSub(maxInt, -1) == maxInt, "MAX - (-1) should saturate at MAX");
}

void test_safe_math_clamped_mul()
{
    TEST_ASSERT(clampedMul(10, 20) == 200, "10 * 20 = 200");

    int maxInt = std::numeric_limits<int>::max();
    TEST_ASSERT(clampedMul(maxInt, 2) == maxInt, "MAX * 2 should saturate");
    TEST_ASSERT(clampedMul(maxInt, -2) == std::numeric_limits<int>::min(),
                "MAX * -2 should saturate at MIN");
}

void test_safe_math_apply_damage()
{
    TEST_ASSERT(applyDamage(100, 30, 100) == 70, "100 hp - 30 dmg = 70");
    TEST_ASSERT(applyDamage(100, 150, 100) == 0, "Overkill clamps to 0");
    TEST_ASSERT(applyDamage(50, -20, 100) == 70, "Negative damage heals (clamped at maxHp)");
    TEST_ASSERT(applyDamage(90, -50, 100) == 100, "Heal past max clamps to maxHp");
}

void test_safe_math_apply_healing()
{
    TEST_ASSERT(applyHealing(50, 30, 100) == 80, "50 + 30 heal = 80");
    TEST_ASSERT(applyHealing(90, 30, 100) == 100, "Overheal clamps at maxHp");
    TEST_ASSERT(applyHealing(0, 100, 100) == 100, "Full heal from 0");
}

void test_safe_math_add_score()
{
    TEST_ASSERT(addScore(1000, 500) == 1500, "1000 + 500 = 1500");
    TEST_ASSERT(addScore(std::numeric_limits<int>::max(), 1) == std::numeric_limits<int>::max(),
                "Score overflow saturates at MAX");
}

// =============================================================================
// AlignedVector Component Storage Test
// =============================================================================

void test_aligned_vector_simd_storage()
{
    // Demonstrate AlignedVector for SIMD-friendly component storage
    constexpr std::size_t kAlignment = 32; // AVX-friendly
    fat_p::AlignedVector<float, kAlignment> xPositions;
    fat_p::AlignedVector<float, kAlignment> yPositions;

    const int N = 1024;
    for (int i = 0; i < N; ++i)
    {
        xPositions.push_back(static_cast<float>(i));
        yPositions.push_back(static_cast<float>(i * 2));
    }

    TEST_ASSERT(xPositions.size() == static_cast<std::size_t>(N), "Should have 1024 x positions");
    TEST_ASSERT(yPositions.size() == static_cast<std::size_t>(N), "Should have 1024 y positions");

    // Verify alignment
    auto xAddr = reinterpret_cast<std::uintptr_t>(xPositions.data());
    auto yAddr = reinterpret_cast<std::uintptr_t>(yPositions.data());
    TEST_ASSERT(xAddr % kAlignment == 0, "X positions should be 32-byte aligned");
    TEST_ASSERT(yAddr % kAlignment == 0, "Y positions should be 32-byte aligned");

    // Simulate a SIMD-style batch update (scalar loop, but data is aligned)
    for (int i = 0; i < N; ++i)
    {
        xPositions[static_cast<std::size_t>(i)] += 1.0f;
        yPositions[static_cast<std::size_t>(i)] += 0.5f;
    }

    TEST_ASSERT(xPositions[0] == 1.0f, "x[0] should be 1.0 after update");
    TEST_ASSERT(yPositions[0] == 0.5f, "y[0] should be 0.5 after update");
    TEST_ASSERT(xPositions[512] == 513.0f, "x[512] should be 513.0 after update");
}

// =============================================================================
// StateMachine AI Integration Tests
// =============================================================================

// AI states
struct IdleState
{
    void on_entry(struct AIContext& ctx);
    void on_exit(struct AIContext& ctx);
};

struct ChaseState
{
    void on_entry(AIContext& ctx);
    void on_exit(AIContext& ctx);
};

struct AttackState
{
    void on_entry(AIContext& ctx);
    void on_exit(AIContext& ctx);
};

struct DeadState
{
    void on_entry(AIContext& ctx);
    void on_exit(AIContext& ctx);
};

// AI context — the data the state machine operates on
struct AIContext
{
    Entity self;
    float distanceToTarget = 100.0f;
    int hp = 100;
    int maxHp = 100;
    std::string lastTransition;
    int stateChanges = 0;
};

// State machine type alias
using AIStateMachine = fat_p::StateMachine<
    AIContext,
    std::tuple<>,                               // No transition list needed
    fat_p::AnyToAnyTransitionPolicy,            // Allow all transitions
    fat_p::ThrowingActionPolicy,                // Throw on errors
    0,                                          // Start in IdleState
    IdleState, ChaseState, AttackState, DeadState
>;

// State implementations
void IdleState::on_entry(AIContext& ctx)
{
    ctx.lastTransition = "enter_idle";
    ++ctx.stateChanges;
}
void IdleState::on_exit(AIContext& ctx)
{
    (void)ctx;
}

void ChaseState::on_entry(AIContext& ctx)
{
    ctx.lastTransition = "enter_chase";
    ++ctx.stateChanges;
}
void ChaseState::on_exit(AIContext& ctx)
{
    (void)ctx;
}

void AttackState::on_entry(AIContext& ctx)
{
    ctx.lastTransition = "enter_attack";
    ++ctx.stateChanges;
}
void AttackState::on_exit(AIContext& ctx)
{
    (void)ctx;
}

void DeadState::on_entry(AIContext& ctx)
{
    ctx.lastTransition = "enter_dead";
    ++ctx.stateChanges;
}
void DeadState::on_exit(AIContext& ctx)
{
    (void)ctx;
}

// AI component that wraps a state machine
struct AIComponent
{
    AIContext context;
    std::unique_ptr<AIStateMachine> stateMachine;

    AIComponent(Entity self, int hp, int maxHp)
        : context{self, 100.0f, hp, maxHp, "", 0}
        , stateMachine(std::make_unique<AIStateMachine>(context))
    {
    }

    // Move constructor needed for storage in ComponentStore
    AIComponent(AIComponent&& other) noexcept
        : context(std::move(other.context))
        , stateMachine(std::move(other.stateMachine))
    {
        // Rebind the state machine's context reference after move
        // The state machine references `context` which has moved.
        // Since we used AnyToAny policy with no internal state storage,
        // we need to recreate to rebind.
        if (stateMachine)
        {
            int savedChanges = context.stateChanges;
            stateMachine.reset();
            stateMachine = std::make_unique<AIStateMachine>(context);
            context.stateChanges = savedChanges;
        }
    }

    AIComponent& operator=(AIComponent&&) = default;
};

void test_state_machine_basic_transitions()
{
    AIContext ctx{Entity{}, 100.0f, 100, 100, "", 0};
    AIStateMachine sm(ctx);

    // Should start in IdleState (index 0)
    TEST_ASSERT(sm.currentStateIndex() == 0, "Should start in Idle");
    TEST_ASSERT(ctx.lastTransition == "enter_idle", "Should have entered Idle");

    // Chase when target is near
    ctx.distanceToTarget = 15.0f;
    sm.transition<ChaseState>();
    TEST_ASSERT(sm.currentStateIndex() == 1, "Should be in Chase");
    TEST_ASSERT(ctx.lastTransition == "enter_chase", "Should have entered Chase");

    // Attack when in range
    ctx.distanceToTarget = 3.0f;
    sm.transition<AttackState>();
    TEST_ASSERT(sm.currentStateIndex() == 2, "Should be in Attack");
    TEST_ASSERT(ctx.lastTransition == "enter_attack", "Should have entered Attack");

    // Die
    ctx.hp = 0;
    sm.transition<DeadState>();
    TEST_ASSERT(sm.currentStateIndex() == 3, "Should be in Dead");
    TEST_ASSERT(ctx.lastTransition == "enter_dead", "Should have entered Dead");
    TEST_ASSERT(ctx.stateChanges == 4, "Should have 4 state changes (Idle + Chase + Attack + Dead)");
}

void test_state_machine_self_transition_noop()
{
    AIContext ctx{Entity{}, 100.0f, 100, 100, "", 0};
    AIStateMachine sm(ctx);

    int changesBefore = ctx.stateChanges;
    sm.transition<IdleState>(); // Self-transition = no-op
    TEST_ASSERT(ctx.stateChanges == changesBefore, "Self-transition should not fire hooks");
}

// =============================================================================
// Full Game Loop Simulation
// =============================================================================

// This is the Phase 3 deliverable test: spawn waves, run AI, detect collisions,
// apply damage, destroy dead entities, accumulate score.

void test_full_game_loop_simulation()
{
    Registry registry;
    TemplateRegistry templates;
    EntityNames names;
    FrameAllocator<CollisionPair> collisionAllocator(128);
    SystemToggle systemToggle;

    // Register systems as toggleable
    systemToggle.registerSystem("movement", true);
    systemToggle.registerSystem("ai", true);
    systemToggle.registerSystem("collision", true);
    systemToggle.registerSystem("damage", true);
    systemToggle.registerSystem("cleanup", true);

    // Register component factories
    templates.registerComponent("Position", [](Registry& reg, Entity e, const fat_p::JsonValue& data)
    {
        auto& obj = std::get<fat_p::JsonObject>(data);
        float x = static_cast<float>(std::get<double>(obj.at("x")));
        float y = static_cast<float>(std::get<double>(obj.at("y")));
        reg.add<Position>(e, x, y);
    });

    templates.registerComponent("Velocity", [](Registry& reg, Entity e, const fat_p::JsonValue& data)
    {
        auto& obj = std::get<fat_p::JsonObject>(data);
        float dx = static_cast<float>(std::get<double>(obj.at("dx")));
        float dy = static_cast<float>(std::get<double>(obj.at("dy")));
        reg.add<Velocity>(e, dx, dy);
    });

    templates.registerComponent("Health", [](Registry& reg, Entity e, const fat_p::JsonValue& data)
    {
        auto& obj = std::get<fat_p::JsonObject>(data);
        int hp = static_cast<int>(std::get<int64_t>(obj.at("current")));
        int maxHp = static_cast<int>(std::get<int64_t>(obj.at("max")));
        reg.add<Health>(e, hp, maxHp);
    });

    templates.registerComponent("Damage", [](Registry& reg, Entity e, const fat_p::JsonValue& data)
    {
        auto& obj = std::get<fat_p::JsonObject>(data);
        int amount = static_cast<int>(std::get<int64_t>(obj.at("amount")));
        reg.add<Damage>(e, amount);
    });

    // Register entity templates
    templates.addTemplate("enemy", R"({
        "components": {
            "Position": { "x": 100.0, "y": 0.0 },
            "Velocity": { "dx": -2.0, "dy": 0.0 },
            "Health":   { "current": 30, "max": 30 },
            "Damage":   { "amount": 5 }
        }
    })");

    templates.addTemplate("player_bullet", R"({
        "components": {
            "Position": { "x": 0.0, "y": 0.0 },
            "Velocity": { "dx": 10.0, "dy": 0.0 },
            "Damage":   { "amount": 15 }
        }
    })");

    // Create player
    Entity player = registry.create();
    registry.add<Position>(player, 0.0f, 0.0f);
    registry.add<Health>(player, 100, 100);
    registry.add<Score>(player, 0);
    names.setName(player, "player");

    // Spawn a wave of enemies
    const int kWaveSize = 10;
    std::vector<Entity> enemies;
    for (int i = 0; i < kWaveSize; ++i)
    {
        Entity e = templates.spawn(registry, "enemy");
        // Offset each enemy's position
        registry.get<Position>(e).y = static_cast<float>(i * 5);
        enemies.push_back(e);
    }

    // Spawn some bullets
    const int kBulletCount = 5;
    std::vector<Entity> bullets;
    for (int i = 0; i < kBulletCount; ++i)
    {
        Entity b = templates.spawn(registry, "player_bullet");
        registry.get<Position>(b).y = static_cast<float>(i * 10);
        bullets.push_back(b);
    }

    TEST_ASSERT(registry.entityCount() == static_cast<std::size_t>(1 + kWaveSize + kBulletCount),
                "Should have player + enemies + bullets");

    // === SIMULATE 5 FRAMES ===
    int totalKills = 0;
    const float kCollisionRadius = 12.0f;

    for (int frame = 0; frame < 5; ++frame)
    {
        CommandBuffer commandBuffer;

        // --- Movement System ---
        if (systemToggle.isEnabled("movement"))
        {
            auto view = registry.view<Position, Velocity>();
            view.each([]([[maybe_unused]] Entity entity, Position& pos, Velocity& vel)
            {
                pos.x += vel.dx;
                pos.y += vel.dy;
            });
        }

        // --- Collision Detection ---
        if (systemToggle.isEnabled("collision"))
        {
            collisionAllocator.releaseAll();

            // Brute-force O(n^2) — fine for 15 entities
            auto damagers = registry.view<Position, Damage>();
            auto targets = registry.view<Position, Health>();

            damagers.each([&]([[maybe_unused]] Entity de, Position& dp,
                              [[maybe_unused]] Damage& dd)
            {
                targets.each([&]([[maybe_unused]] Entity te, Position& tp,
                                 [[maybe_unused]] Health& th)
                {
                    if (de == te)
                    {
                        return;
                    }
                    float dx = dp.x - tp.x;
                    float dy = dp.y - tp.y;
                    float dist = std::sqrt(dx * dx + dy * dy);
                    if (dist < kCollisionRadius)
                    {
                        (void)collisionAllocator.acquire(de, te, dist);
                    }
                });
            });
        }

        // --- Damage System ---
        if (systemToggle.isEnabled("damage"))
        {
            // Process collision pairs from frame allocator
            // (In a real system we'd iterate the allocator; here we re-detect)
            auto damagers = registry.view<Position, Damage>();
            auto targets = registry.view<Position, Health>();

            damagers.each([&]([[maybe_unused]] Entity de, Position& dp, Damage& dd)
            {
                targets.each([&]([[maybe_unused]] Entity te, Position& tp, Health& th)
                {
                    if (de == te)
                    {
                        return;
                    }
                    float dx = dp.x - tp.x;
                    float dy = dp.y - tp.y;
                    float dist = std::sqrt(dx * dx + dy * dy);
                    if (dist < kCollisionRadius)
                    {
                        th.hp = applyDamage(th.hp, dd.amount, th.maxHp);
                    }
                });
            });
        }

        // --- Cleanup System ---
        if (systemToggle.isEnabled("cleanup"))
        {
            auto healthView = registry.view<Health>();
            healthView.each([&](Entity entity, Health& hp)
            {
                if (hp.hp <= 0 && entity != player)
                {
                    commandBuffer.destroy(entity);
                    ++totalKills;
                }
            });
        }

        commandBuffer.flush(registry);
    }

    // --- Verify game state ---
    TEST_ASSERT(registry.isAlive(player), "Player should survive");
    TEST_ASSERT(registry.get<Health>(player).hp > 0, "Player should have health remaining");

    // Some enemies should be dead (bullets should have killed some)
    std::size_t surviving = registry.entityCount();
    TEST_ASSERT(surviving < static_cast<std::size_t>(1 + kWaveSize + kBulletCount),
                "Some entities should have been destroyed");

    // Score system would have accumulated kills
    TEST_ASSERT(totalKills > 0, "Should have at least one kill");

    // Verify system toggle works: disable movement, verify positions don't change
    systemToggle.disable("movement");
    TEST_ASSERT(!systemToggle.isEnabled("movement"), "Movement should be disabled");

    // Capture positions
    float playerX = registry.get<Position>(player).x;

    // "Run" movement (but it's disabled)
    if (systemToggle.isEnabled("movement"))
    {
        auto view = registry.view<Position, Velocity>();
        view.each([]([[maybe_unused]] Entity entity, Position& pos, Velocity& vel)
        {
            pos.x += vel.dx;
        });
    }

    TEST_ASSERT(registry.get<Position>(player).x == playerX,
                "Position should not change when movement is disabled");

    // Verify named entity survived
    TEST_ASSERT(names.findByName("player") == player,
                "Named player entity should still be findable");
}

// =============================================================================
// Phase 1+2 Regression Tests
// =============================================================================

void test_regression_phase12_with_phase3_components()
{
    // Verify Phase 1+2 features still work with Phase 3 component types
    Registry registry;

    Entity e1 = registry.create();
    Entity e2 = registry.create();

    registry.add<Position>(e1, 1.0f, 2.0f);
    registry.add<Health>(e1, 100, 100);
    registry.add<Score>(e1, 0);

    registry.add<Position>(e2, 3.0f, 4.0f);
    registry.add<Damage>(e2, 25);

    // View iteration
    int posCount = 0;
    auto posView = registry.view<Position>();
    posView.each([&]([[maybe_unused]] Entity entity,
                     [[maybe_unused]] Position& pos) { ++posCount; });
    TEST_ASSERT(posCount == 2, "Should iterate 2 Position entities");

    // Command buffer with Phase 3 components
    CommandBuffer cmd;
    cmd.create([](Registry& reg, Entity spawned)
    {
        reg.add<Score>(spawned, 9999);
    });
    cmd.flush(registry);
    TEST_ASSERT(registry.entityCount() == 3, "Command buffer should have created entity");

    // Events
    int destroyCount = 0;
    auto conn = registry.events().onEntityDestroyed.connect([&]([[maybe_unused]] Entity e)
    {
        ++destroyCount;
    });
    registry.destroy(e2);
    TEST_ASSERT(destroyCount == 1, "Destroy event should fire");
}

void test_regression_generational_safety_phase3()
{
    Registry registry;

    Entity e = registry.create();
    registry.add<Health>(e, 50, 100);

    registry.destroy(e);
    TEST_ASSERT(!registry.isAlive(e), "Destroyed entity should not be alive");

    // Reuse slot
    Entity e2 = registry.create();
    registry.add<Health>(e2, 75, 100);

    // isAlive correctly distinguishes generations
    TEST_ASSERT(!registry.isAlive(e), "Old generation entity should not be alive");
    TEST_ASSERT(registry.isAlive(e2), "New generation entity should be alive");
    TEST_ASSERT(registry.get<Health>(e2).hp == 75, "New entity should have correct data");

    // Correct usage pattern: always check isAlive() before has()/get()
    // NOTE: has()/get() use slot index only (by design, for iteration speed).
    // User code must guard with isAlive() for stale handle safety.
    if (registry.isAlive(e))
    {
        // This branch should NOT execute
        TEST_ASSERT(false, "Should not reach here — e is dead");
    }
}

// =============================================================================
// Scale Test
// =============================================================================

void test_scale_1k_entities_full_pipeline()
{
    Registry registry;
    TemplateRegistry templates;
    FrameAllocator<CollisionPair> collisionAlloc(512);

    templates.registerComponent("Position", [](Registry& reg, Entity e, const fat_p::JsonValue& data)
    {
        auto& obj = std::get<fat_p::JsonObject>(data);
        float x = static_cast<float>(std::get<double>(obj.at("x")));
        float y = static_cast<float>(std::get<double>(obj.at("y")));
        reg.add<Position>(e, x, y);
    });

    templates.registerComponent("Velocity", [](Registry& reg, Entity e, const fat_p::JsonValue& data)
    {
        auto& obj = std::get<fat_p::JsonObject>(data);
        float dx = static_cast<float>(std::get<double>(obj.at("dx")));
        float dy = static_cast<float>(std::get<double>(obj.at("dy")));
        reg.add<Velocity>(e, dx, dy);
    });

    templates.registerComponent("Health", [](Registry& reg, Entity e, const fat_p::JsonValue& data)
    {
        auto& obj = std::get<fat_p::JsonObject>(data);
        int hp = static_cast<int>(std::get<int64_t>(obj.at("current")));
        int maxHp = static_cast<int>(std::get<int64_t>(obj.at("max")));
        reg.add<Health>(e, hp, maxHp);
    });

    templates.addTemplate("unit", R"({
        "components": {
            "Position": { "x": 0.0, "y": 0.0 },
            "Velocity": { "dx": 1.0, "dy": 0.5 },
            "Health":   { "current": 100, "max": 100 }
        }
    })");

    // Spawn 1000 entities from template
    const int N = 1000;
    for (int i = 0; i < N; ++i)
    {
        Entity e = templates.spawn(registry, "unit");
        registry.get<Position>(e).x = static_cast<float>(i);
        registry.get<Position>(e).y = static_cast<float>(i % 100);
    }

    TEST_ASSERT(registry.entityCount() == static_cast<std::size_t>(N), "Should have 1000 entities");

    // Run 10 frames of movement + damage
    for (int frame = 0; frame < 10; ++frame)
    {
        // Movement
        auto moveView = registry.view<Position, Velocity>();
        moveView.each([]([[maybe_unused]] Entity entity, Position& pos, Velocity& vel)
        {
            pos.x += vel.dx;
            pos.y += vel.dy;
        });

        // Apply damage — every 3rd entity takes 40 damage per frame
        // After 3 frames of hits (40 × 3 = 120 > 100), those entities die
        auto healthView = registry.view<Health>();
        int idx = 0;
        healthView.each([&]([[maybe_unused]] Entity entity, Health& hp)
        {
            if (idx % 3 == 0)
            {
                hp.hp = applyDamage(hp.hp, 40, hp.maxHp);
            }
            ++idx;
        });

        // Cleanup dead entities
        CommandBuffer cmd;
        healthView.each([&](Entity entity, Health& hp)
        {
            if (hp.hp <= 0)
            {
                cmd.destroy(entity);
            }
        });
        cmd.flush(registry);

        collisionAlloc.releaseAll();
    }

    // Verify state
    TEST_ASSERT(registry.entityCount() < static_cast<std::size_t>(N),
                "Some entities should have been destroyed");
    TEST_ASSERT(registry.entityCount() > 0, "Not all entities should be dead");

    // Verify remaining entities are healthy
    auto finalView = registry.view<Health>();
    finalView.each([]([[maybe_unused]] Entity entity,
                     [[maybe_unused]] Health& hp)
    {
        assert(hp.hp > 0 && "Surviving entities should have positive health");
    });
}

// =============================================================================
// Main
// =============================================================================

int main()
{
    std::printf("=== FAT-P ECS Phase 3 Tests ===\n\n");

    std::printf("[FrameAllocator]\n");
    RUN_TEST(test_frame_allocator_basic);
    RUN_TEST(test_frame_allocator_reuse);
    RUN_TEST(test_frame_allocator_grow);

    std::printf("\n[EntityNames]\n");
    RUN_TEST(test_entity_names_basic);
    RUN_TEST(test_entity_names_lookup);
    RUN_TEST(test_entity_names_reverse_lookup);
    RUN_TEST(test_entity_names_duplicate_name_rejected);
    RUN_TEST(test_entity_names_rename);
    RUN_TEST(test_entity_names_remove);

    std::printf("\n[EntityTemplate]\n");
    RUN_TEST(test_entity_template_register_and_spawn);
    RUN_TEST(test_entity_template_missing_template);
    RUN_TEST(test_entity_template_multiple_spawns);

    std::printf("\n[SystemToggle]\n");
    RUN_TEST(test_system_toggle_basic);
    RUN_TEST(test_system_toggle_enable_disable);
    RUN_TEST(test_system_toggle_unregistered);

    std::printf("\n[SafeMath]\n");
    RUN_TEST(test_safe_math_clamped_add);
    RUN_TEST(test_safe_math_clamped_sub);
    RUN_TEST(test_safe_math_clamped_mul);
    RUN_TEST(test_safe_math_apply_damage);
    RUN_TEST(test_safe_math_apply_healing);
    RUN_TEST(test_safe_math_add_score);

    std::printf("\n[AlignedVector]\n");
    RUN_TEST(test_aligned_vector_simd_storage);

    std::printf("\n[StateMachine AI]\n");
    RUN_TEST(test_state_machine_basic_transitions);
    RUN_TEST(test_state_machine_self_transition_noop);

    std::printf("\n[Full Game Loop]\n");
    RUN_TEST(test_full_game_loop_simulation);

    std::printf("\n[Phase 1+2 Regression]\n");
    RUN_TEST(test_regression_phase12_with_phase3_components);
    RUN_TEST(test_regression_generational_safety_phase3);

    std::printf("\n[Scale]\n");
    RUN_TEST(test_scale_1k_entities_full_pipeline);

    std::printf("\n=== Results: %d passed, %d failed ===\n", sTestsPassed, sTestsFailed);
    return sTestsFailed > 0 ? 1 : 0;
}
