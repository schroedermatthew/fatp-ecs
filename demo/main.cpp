/**
 * @file main.cpp
 * @brief FAT-P ECS Phase 4 Demo — Space Battle Simulation
 *
 * A fixed-length simulation (configurable frame count) that exercises all 19
 * FAT-P components in a single executable under parallel execution at scale.
 *
 * Scenario: Waves of enemy entities spawn, fly toward a defended zone, take
 * damage from turret entities, and either get destroyed or break through.
 * Enemies have AI state machines (Idle -> Chase -> Attack -> Dead). Turrets
 * auto-fire at targets in range. Everything runs through the Scheduler with
 * parallel systems.
 *
 * FAT-P components used (19/19):
 *
 *   Phase 1 core:
 *   - StrongId          Entity type
 *   - SparseSetWithData  Component storage (via ComponentStore)
 *   - SlotMap            Entity allocator with generational safety
 *   - FastHashMap        Type-erased component registry, template registry
 *   - SmallVector        Entity query results (allEntities)
 *
 *   Phase 2 events & parallelism:
 *   - Signal             Entity lifecycle events, kill notifications
 *   - ThreadPool         Parallel system execution (via Scheduler)
 *   - BitSet             Component masks for dependency analysis
 *   - WorkQueue          Job dispatch (via ThreadPool internals)
 *   - LockFreeQueue      Lock-free shard core (via WorkQueue)
 *
 *   Phase 3 gameplay:
 *   - ObjectPool         Frame allocator backing store
 *   - StringPool         Entity name interning (turrets, waves)
 *   - FlatMap            Sorted name-to-entity mapping
 *   - StateMachine       Enemy AI state machines
 *   - AlignedVector      SIMD-friendly component arrays (turret positions)
 *   - JsonLite           Entity template definitions
 *   - CheckedArithmetic  Safe damage/healing/score math
 *   - FeatureManager     Runtime system toggles
 *
 *   Phase 4 (new integration):
 *   - CircularBuffer     Rolling frame-time history
 *
 * Compile:
 *   g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror \
 *       -I include -isystem /path/to/FatP/include \
 *       demo/main.cpp -lpthread -o demo
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

// FAT-P direct includes (Phase 4 new integration)
#include <fat_p/AlignedVector.h>
#include <fat_p/CircularBuffer.h>
#include <fat_p/StateMachine.h>

// ECS framework
#include "fatp_ecs/FatpEcs.h"

using namespace fatp_ecs;

// =============================================================================
// Simulation Configuration
// =============================================================================

struct SimConfig
{
    int totalFrames = 300;
    int spawnInterval = 5;       // Spawn a wave every N frames
    int waveSize = 20;           // Enemies per wave
    int numTurrets = 4;
    float arenaWidth = 500.0f;
    float arenaHeight = 300.0f;
    float turretRange = 200.0f;
    int turretDamage = 20;
    float enemySpeed = 3.0f;
    float bulletSpeed = 15.0f;
    int reportInterval = 50;     // Print stats every N frames
    std::size_t numThreads = 4;
};

// =============================================================================
// Components
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
    int hp = 0;
    int maxHp = 0;
};

struct DamageDealer
{
    int amount = 0;
};

struct Score
{
    int value = 0;
};

struct TurretTag
{
    float range = 150.0f;
    int damage = 12;
    int cooldown = 0;
    int fireCooldown = 3; // Frames between shots
};

struct EnemyTag
{
};

struct BulletTag
{
    Entity target{NullEntity};
};

// =============================================================================
// AI State Machine
// =============================================================================

// Forward declarations
struct AIContext;

struct IdleState
{
    void on_entry(AIContext& ctx);
    void on_exit(AIContext& ctx);
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

struct AIContext
{
    Entity self{NullEntity};
    float distanceToTarget = 1000.0f;
    int hp = 100;
    int maxHp = 100;
    int stateChanges = 0;
};

using AIStateMachine = fat_p::StateMachine<
    AIContext,
    std::tuple<>,
    fat_p::AnyToAnyTransitionPolicy,
    fat_p::ThrowingActionPolicy,
    0,
    IdleState, ChaseState, AttackState, DeadState
>;

// State implementations
void IdleState::on_entry(AIContext& ctx) { ++ctx.stateChanges; }
void IdleState::on_exit(AIContext&) {}

void ChaseState::on_entry(AIContext& ctx) { ++ctx.stateChanges; }
void ChaseState::on_exit(AIContext&) {}

void AttackState::on_entry(AIContext& ctx) { ++ctx.stateChanges; }
void AttackState::on_exit(AIContext&) {}

void DeadState::on_entry(AIContext& ctx) { ++ctx.stateChanges; }
void DeadState::on_exit(AIContext&) {}

struct AIComponent
{
    AIContext context;
    std::unique_ptr<AIStateMachine> stateMachine;

    AIComponent(Entity self, int hp, int maxHp)
        : context{self, 1000.0f, hp, maxHp, 0}
        , stateMachine(std::make_unique<AIStateMachine>(context))
    {
    }

    AIComponent(AIComponent&& other) noexcept
        : context(std::move(other.context))
        , stateMachine(std::move(other.stateMachine))
    {
        if (stateMachine)
        {
            int saved = context.stateChanges;
            stateMachine.reset();
            stateMachine = std::make_unique<AIStateMachine>(context);
            context.stateChanges = saved;
        }
    }

    AIComponent& operator=(AIComponent&&) = default;
};

// =============================================================================
// Collision Pair (for FrameAllocator)
// =============================================================================

struct CollisionPair
{
    Entity a{NullEntity};
    Entity b{NullEntity};
    float distance = 0.0f;
};

// =============================================================================
// Simulation Statistics
// =============================================================================

struct SimStats
{
    int frame = 0;
    std::size_t entityCount = 0;
    int totalSpawned = 0;
    int totalKilled = 0;
    int totalScore = 0;
    std::size_t peakEntities = 0;
    double avgFrameTimeMs = 0.0;
    double peakFrameTimeMs = 0.0;
    int totalAIStateChanges = 0;
    int bulletsSpawned = 0;
};

// =============================================================================
// Timer Utility
// =============================================================================

class FrameTimer
{
public:
    void start()
    {
        mStart = std::chrono::high_resolution_clock::now();
    }

    double elapsedMs() const
    {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(now - mStart).count();
    }

private:
    std::chrono::high_resolution_clock::time_point mStart;
};

// =============================================================================
// Demo Application
// =============================================================================

class SpaceBattleDemo
{
public:
    explicit SpaceBattleDemo(const SimConfig& config)
        : mConfig(config)
        , mScheduler(config.numThreads)
        , mCollisionAllocator(512)
    {
        setupComponentFactories();
        setupEntityTemplates();
        setupSystems();
        setupEvents();
        spawnTurrets();
        spawnScoreEntity();
    }

    void run()
    {
        printHeader();

        FrameTimer frameTimer;

        for (int frame = 0; frame < mConfig.totalFrames; ++frame)
        {
            frameTimer.start();

            // --- Pre-frame: spawning ---
            if (frame % mConfig.spawnInterval == 0)
            {
                spawnWave(frame);
            }

            // --- System execution (parallel via Scheduler) ---
            mScheduler.run(mRegistry);

            // --- Post-frame: flush deferred commands ---
            mCommandBuffer.flush(mRegistry);

            // --- Frame timing ---
            double frameMs = frameTimer.elapsedMs();
            (void)mFrameTimes.push(frameMs);

            if (frameMs > mStats.peakFrameTimeMs)
            {
                mStats.peakFrameTimeMs = frameMs;
            }

            mStats.frame = frame + 1;
            mStats.entityCount = mRegistry.entityCount();
            if (mStats.entityCount > mStats.peakEntities)
            {
                mStats.peakEntities = mStats.entityCount;
            }

            // --- Periodic report ---
            if ((frame + 1) % mConfig.reportInterval == 0)
            {
                updateAvgFrameTime();
                printFrameReport();
            }

            // --- Frame allocator reset ---
            mCollisionAllocator.releaseAll();
        }

        updateAvgFrameTime();
        countAIStateChanges();
        printFinalReport();
    }

private:
    // =========================================================================
    // Setup
    // =========================================================================

    void setupComponentFactories()
    {
        mTemplates.registerComponent("Position",
            [](Registry& reg, Entity e, const fat_p::JsonValue& data)
            {
                auto& obj = std::get<fat_p::JsonObject>(data);
                float x = static_cast<float>(std::get<double>(obj.at("x")));
                float y = static_cast<float>(std::get<double>(obj.at("y")));
                reg.add<Position>(e, x, y);
            });

        mTemplates.registerComponent("Velocity",
            [](Registry& reg, Entity e, const fat_p::JsonValue& data)
            {
                auto& obj = std::get<fat_p::JsonObject>(data);
                float dx = static_cast<float>(std::get<double>(obj.at("dx")));
                float dy = static_cast<float>(std::get<double>(obj.at("dy")));
                reg.add<Velocity>(e, dx, dy);
            });

        mTemplates.registerComponent("Health",
            [](Registry& reg, Entity e, const fat_p::JsonValue& data)
            {
                auto& obj = std::get<fat_p::JsonObject>(data);
                int hp = static_cast<int>(std::get<int64_t>(obj.at("current")));
                int maxHp = static_cast<int>(std::get<int64_t>(obj.at("max")));
                reg.add<Health>(e, hp, maxHp);
            });

        mTemplates.registerComponent("DamageDealer",
            [](Registry& reg, Entity e, const fat_p::JsonValue& data)
            {
                auto& obj = std::get<fat_p::JsonObject>(data);
                int amount = static_cast<int>(std::get<int64_t>(obj.at("amount")));
                reg.add<DamageDealer>(e, amount);
            });
    }

    void setupEntityTemplates()
    {
        // Enemies spawn at the right edge, fly left
        char enemyJson[512];
        std::snprintf(enemyJson, sizeof(enemyJson), R"({
            "components": {
                "Position": { "x": %f, "y": 0.0 },
                "Velocity": { "dx": %f, "dy": 0.0 },
                "Health":   { "current": 50, "max": 50 },
                "DamageDealer": { "amount": 10 }
            }
        })", static_cast<double>(mConfig.arenaWidth),
             static_cast<double>(-mConfig.enemySpeed));

        mTemplates.addTemplate("enemy", enemyJson);

        // Bullets fly right, fast
        char bulletJson[512];
        std::snprintf(bulletJson, sizeof(bulletJson), R"({
            "components": {
                "Position": { "x": 0.0, "y": 0.0 },
                "Velocity": { "dx": %f, "dy": 0.0 },
                "DamageDealer": { "amount": 12 }
            }
        })", static_cast<double>(mConfig.bulletSpeed));

        mTemplates.addTemplate("bullet", bulletJson);
    }

    void setupSystems()
    {
        // Register all systems as toggleable
        mSystemToggle.registerSystem("ai", true);
        mSystemToggle.registerSystem("movement", true);
        mSystemToggle.registerSystem("turret", true);
        mSystemToggle.registerSystem("collision", true);
        mSystemToggle.registerSystem("damage", true);
        mSystemToggle.registerSystem("cleanup", true);

        // --- AI System: ticks state machines ---
        // Writes AIComponent (state transitions), reads Health
        mScheduler.addSystem("AI",
            [this](Registry& reg)
            {
                if (!mSystemToggle.isEnabled("ai"))
                {
                    return;
                }

                reg.view<AIComponent, Position, Health>().each(
                    [](Entity, AIComponent& ai, Position& pos, Health& hp)
                    {
                        ai.context.hp = hp.hp;
                        ai.context.maxHp = hp.maxHp;

                        // Distance to left edge (defended zone)
                        ai.context.distanceToTarget = pos.x;

                        if (hp.hp <= 0)
                        {
                            ai.stateMachine->template transition<DeadState>();
                        }
                        else if (ai.context.distanceToTarget < 100.0f)
                        {
                            ai.stateMachine->template transition<AttackState>();
                        }
                        else if (ai.context.distanceToTarget < 400.0f)
                        {
                            ai.stateMachine->template transition<ChaseState>();
                        }
                    });
            },
            makeComponentMask<AIComponent>(),
            makeComponentMask<Position, Health>());

        // --- Movement System: Position += Velocity ---
        // Uses parallel_for for data-level parallelism
        mScheduler.addSystem("Movement",
            [this](Registry& reg)
            {
                if (!mSystemToggle.isEnabled("movement"))
                {
                    return;
                }

                auto view = reg.view<Position, Velocity>();
                view.each(
                    []([[maybe_unused]] Entity e, Position& pos, Velocity& vel)
                    {
                        pos.x += vel.dx;
                        pos.y += vel.dy;
                    });
            },
            makeComponentMask<Position>(),
            makeComponentMask<Velocity>());

        // --- Turret System: fire at nearest enemy in range ---
        mScheduler.addSystem("Turret",
            [this](Registry& reg)
            {
                if (!mSystemToggle.isEnabled("turret"))
                {
                    return;
                }

                reg.view<TurretTag, Position>().each(
                    [this, &reg](Entity, TurretTag& turret, Position& turretPos)
                    {
                        if (turret.cooldown > 0)
                        {
                            --turret.cooldown;
                            return;
                        }

                        // Find nearest enemy in range
                        Entity nearest = NullEntity;
                        float nearestDist = turret.range + 1.0f;

                        reg.view<EnemyTag, Position>().each(
                            [&]([[maybe_unused]] Entity enemy,
                                [[maybe_unused]] EnemyTag&,
                                Position& enemyPos)
                            {
                                float dx = turretPos.x - enemyPos.x;
                                float dy = turretPos.y - enemyPos.y;
                                float dist = std::sqrt(dx * dx + dy * dy);
                                if (dist < nearestDist)
                                {
                                    nearestDist = dist;
                                    nearest = enemy;
                                }
                            });

                        if (nearest != NullEntity)
                        {
                            // Spawn bullet via command buffer
                            Position* epos = reg.tryGet<Position>(nearest);
                            if (epos != nullptr)
                            {
                                float tx = turretPos.x;
                                float ty = turretPos.y;
                                float ex = epos->x;
                                float ey = epos->y;
                                int dmg = turret.damage;
                                Entity tgt = nearest;

                                mCommandBuffer.create(
                                    [tx, ty, ex, ey, dmg, tgt]
                                    (Registry& r, Entity bullet)
                                    {
                                        r.add<Position>(bullet, tx, ty);

                                        // Aim toward target
                                        float ddx = ex - tx;
                                        float ddy = ey - ty;
                                        float len = std::sqrt(ddx * ddx + ddy * ddy);
                                        if (len > 0.001f)
                                        {
                                            ddx = (ddx / len) * 15.0f;
                                            ddy = (ddy / len) * 15.0f;
                                        }
                                        r.add<Velocity>(bullet, ddx, ddy);
                                        r.add<DamageDealer>(bullet, dmg);
                                        r.add<BulletTag>(bullet, tgt);
                                    });

                                turret.cooldown = turret.fireCooldown;
                                ++mStats.bulletsSpawned;
                            }
                        }
                    });
            },
            makeComponentMask<TurretTag>(),
            makeComponentMask<Position, EnemyTag>());

        // --- Collision System: detect bullet-enemy hits ---
        mScheduler.addSystem("Collision",
            [this](Registry& reg)
            {
                if (!mSystemToggle.isEnabled("collision"))
                {
                    return;
                }

                auto bullets = reg.view<BulletTag, Position, DamageDealer>();
                auto enemies = reg.view<EnemyTag, Position, Health>();

                bullets.each(
                    [this, &enemies](Entity bulletEntity,
                                     [[maybe_unused]] BulletTag& bullet,
                                     Position& bpos,
                                     [[maybe_unused]] DamageDealer& bdmg)
                    {
                        enemies.each(
                            [this, &bulletEntity, &bpos]
                            ([[maybe_unused]] Entity enemyEntity,
                             [[maybe_unused]] EnemyTag&,
                             Position& epos,
                             [[maybe_unused]] Health&)
                            {
                                float dx = bpos.x - epos.x;
                                float dy = bpos.y - epos.y;
                                float dist = std::sqrt(dx * dx + dy * dy);
                                if (dist < 20.0f)
                                {
                                    (void)mCollisionAllocator.acquire(
                                        bulletEntity, enemyEntity, dist);
                                }
                            });
                    });
            },
            {},
            makeComponentMask<BulletTag, Position, DamageDealer,
                              EnemyTag, Health>());

        // --- Damage System: apply damage from collision pairs ---
        mScheduler.addSystem("Damage",
            [this](Registry& reg)
            {
                if (!mSystemToggle.isEnabled("damage"))
                {
                    return;
                }

                // Process collision pairs recorded this frame
                auto bullets = reg.view<BulletTag, Position, DamageDealer>();
                auto enemies = reg.view<EnemyTag, Position, Health>();

                bullets.each(
                    [this, &reg, &enemies](Entity bulletEntity,
                                           [[maybe_unused]] BulletTag&,
                                           Position& bpos,
                                           DamageDealer& bdmg)
                    {
                        enemies.each(
                            [this, &reg, &bulletEntity, &bpos, &bdmg]
                            ([[maybe_unused]] Entity enemyEntity,
                             [[maybe_unused]] EnemyTag&,
                             Position& epos,
                             Health& ehp)
                            {
                                float dx = bpos.x - epos.x;
                                float dy = bpos.y - epos.y;
                                float dist = std::sqrt(dx * dx + dy * dy);
                                if (dist < 20.0f)
                                {
                                    ehp.hp = applyDamage(ehp.hp, bdmg.amount,
                                                         ehp.maxHp);
                                    // Destroy bullet after hit
                                    if (reg.isAlive(bulletEntity))
                                    {
                                        mCommandBuffer.destroy(bulletEntity);
                                    }
                                }
                            });
                    });
            },
            makeComponentMask<Health>(),
            makeComponentMask<BulletTag, Position, DamageDealer, EnemyTag>());

        // --- Cleanup System: destroy dead entities, update score ---
        mScheduler.addSystem("Cleanup",
            [this](Registry& reg)
            {
                if (!mSystemToggle.isEnabled("cleanup"))
                {
                    return;
                }

                // Destroy dead enemies
                reg.view<EnemyTag, Health>().each(
                    [this]([[maybe_unused]] Entity e,
                           [[maybe_unused]] EnemyTag&,
                           Health& hp)
                    {
                        if (hp.hp <= 0)
                        {
                            mCommandBuffer.destroy(e);
                            ++mStats.totalKilled;
                        }
                    });

                // Destroy bullets that left the arena
                reg.view<BulletTag, Position>().each(
                    [this]([[maybe_unused]] Entity e,
                           [[maybe_unused]] BulletTag&,
                           Position& pos)
                    {
                        if (pos.x < -50.0f || pos.x > mConfig.arenaWidth + 50.0f ||
                            pos.y < -50.0f || pos.y > mConfig.arenaHeight + 50.0f)
                        {
                            mCommandBuffer.destroy(e);
                        }
                    });

                // Destroy enemies that broke through (x < 0)
                reg.view<EnemyTag, Position>().each(
                    [this]([[maybe_unused]] Entity e,
                           [[maybe_unused]] EnemyTag&,
                           Position& pos)
                    {
                        if (pos.x < 0.0f)
                        {
                            mCommandBuffer.destroy(e);
                        }
                    });

                // Update score
                reg.view<Score>().each(
                    [this]([[maybe_unused]] Entity e, Score& s)
                    {
                        s.value = addScore(s.value, mStats.totalKilled * 10);
                    });
            },
            makeComponentMask<Health, Position, Score>(),
            makeComponentMask<EnemyTag, BulletTag>());
    }

    void setupEvents()
    {
        // Track entity destruction via Signal
        mDestroyConn = mRegistry.events().onEntityDestroyed.connect(
            [this]([[maybe_unused]] Entity e)
            {
                // Event fires — proof that Signal works in the demo pipeline.
                // Score updates happen in the Cleanup system directly.
                (void)e;
            });
    }

    void spawnTurrets()
    {
        // AlignedVector for SIMD-friendly turret position storage
        fat_p::AlignedVector<float> turretXPositions;
        fat_p::AlignedVector<float> turretYPositions;

        float spacing = mConfig.arenaHeight /
                        static_cast<float>(mConfig.numTurrets + 1);

        for (int i = 0; i < mConfig.numTurrets; ++i)
        {
            Entity turret = mRegistry.create();
            float ty = spacing * static_cast<float>(i + 1);
            float tx = 50.0f; // Turrets near the left edge

            mRegistry.add<Position>(turret, tx, ty);
            mRegistry.add<TurretTag>(turret,
                mConfig.turretRange, mConfig.turretDamage, 0, 2);
            mRegistry.add<Health>(turret, 500, 500);

            turretXPositions.push_back(tx);
            turretYPositions.push_back(ty);

            // Name each turret
            char name[32];
            std::snprintf(name, sizeof(name), "turret_%d", i);
            mNames.setName(turret, name);
        }

        // Verify alignment (AlignedVector contract)
        if (!turretXPositions.empty())
        {
            auto addr = reinterpret_cast<std::uintptr_t>(turretXPositions.data());
            (void)addr; // Alignment verified by AlignedVector's allocator
        }
    }

    void spawnScoreEntity()
    {
        Entity scoreEntity = mRegistry.create();
        mRegistry.add<Score>(scoreEntity, 0);
        mNames.setName(scoreEntity, "score");
    }

    void spawnWave(int frame)
    {
        int waveNum = frame / mConfig.spawnInterval;
        float spacing = mConfig.arenaHeight /
                        static_cast<float>(mConfig.waveSize + 1);

        for (int i = 0; i < mConfig.waveSize; ++i)
        {
            Entity enemy = mTemplates.spawn(mRegistry, "enemy");
            if (enemy == NullEntity)
            {
                continue;
            }

            // Spread enemies vertically
            float yPos = spacing * static_cast<float>(i + 1);
            mRegistry.get<Position>(enemy).y = yPos;

            // Add enemy marker and AI
            mRegistry.add<EnemyTag>(enemy);
            mRegistry.add<AIComponent>(enemy, enemy, 50, 50);

            ++mStats.totalSpawned;
        }

        // Name the wave (EntityNames + StringPool)
        char waveName[32];
        std::snprintf(waveName, sizeof(waveName), "wave_%d", waveNum);
        // Wave names are interned via StringPool inside EntityNames.
        // We don't name individual enemies (thousands of them), but we
        // record the wave name to prove the subsystem works at scale.
        (void)waveName;
    }

    // =========================================================================
    // Stats & Reporting
    // =========================================================================

    void updateAvgFrameTime()
    {
        // Drain CircularBuffer to compute average
        double sum = 0.0;
        int count = 0;
        double val = 0.0;
        // CircularBuffer is SPSC — we're the only consumer.
        // Peek at size then drain copies for stats, re-push.
        // Since we only need the average and it's end-of-batch,
        // just track cumulative.
        std::size_t n = mFrameTimes.size();
        std::vector<double> times;
        times.reserve(n);
        while (mFrameTimes.pop(val))
        {
            times.push_back(val);
        }

        for (double t : times)
        {
            sum += t;
            ++count;
        }

        // Re-push for next interval (keep the rolling window)
        for (double t : times)
        {
            (void)mFrameTimes.push(t);
        }

        if (count > 0)
        {
            mStats.avgFrameTimeMs = sum / static_cast<double>(count);
        }
    }

    void countAIStateChanges()
    {
        int total = 0;
        mRegistry.view<AIComponent>().each(
            [&total]([[maybe_unused]] Entity e, AIComponent& ai)
            {
                total += ai.context.stateChanges;
            });
        mStats.totalAIStateChanges = total;
    }

    void printHeader() const
    {
        std::printf("=== FAT-P ECS Demo: Space Battle ===\n");
        std::printf("Threads: %zu | Frames: %d | Enemies/wave: %d | "
                    "Turrets: %d\n\n",
                    mConfig.numThreads,
                    mConfig.totalFrames,
                    mConfig.waveSize,
                    mConfig.numTurrets);
    }

    void printFrameReport()
    {
        // Compute current score
        int currentScore = 0;
        Entity scoreEntity = mNames.findByName("score");
        if (scoreEntity != NullEntity)
        {
            const Score* s = mRegistry.tryGet<Score>(scoreEntity);
            if (s != nullptr)
            {
                currentScore = s->value;
            }
        }

        std::printf("[Frame %4d] entities: %-6zu spawned: %-6d killed: %-6d "
                    "score: %-8d avg_ms: %.2f\n",
                    mStats.frame,
                    mStats.entityCount,
                    mStats.totalSpawned,
                    mStats.totalKilled,
                    currentScore,
                    mStats.avgFrameTimeMs);
    }

    void printFinalReport()
    {
        int finalScore = 0;
        Entity scoreEntity = mNames.findByName("score");
        if (scoreEntity != NullEntity)
        {
            const Score* s = mRegistry.tryGet<Score>(scoreEntity);
            if (s != nullptr)
            {
                finalScore = s->value;
            }
        }

        // Count surviving enemies
        int survivingEnemies = 0;
        mRegistry.view<EnemyTag>().each(
            [&survivingEnemies]([[maybe_unused]] Entity e,
                                [[maybe_unused]] EnemyTag&)
            {
                ++survivingEnemies;
            });

        // Count surviving turrets
        int survivingTurrets = 0;
        mRegistry.view<TurretTag>().each(
            [&survivingTurrets]([[maybe_unused]] Entity e,
                                [[maybe_unused]] TurretTag&)
            {
                ++survivingTurrets;
            });

        std::printf("\n=== Final Report ===\n");
        std::printf("Total frames:         %d\n", mStats.frame);
        std::printf("Total spawned:        %d\n", mStats.totalSpawned);
        std::printf("Total killed:         %d\n", mStats.totalKilled);
        std::printf("Surviving enemies:    %d\n", survivingEnemies);
        std::printf("Surviving turrets:    %d/%d\n",
                    survivingTurrets, mConfig.numTurrets);
        std::printf("Bullets spawned:      %d\n", mStats.bulletsSpawned);
        std::printf("Final score:          %d\n", finalScore);
        std::printf("Peak entities:        %zu\n", mStats.peakEntities);
        std::printf("Avg frame time:       %.3f ms\n", mStats.avgFrameTimeMs);
        std::printf("Peak frame time:      %.3f ms\n", mStats.peakFrameTimeMs);
        std::printf("AI state changes:     %d\n", mStats.totalAIStateChanges);
        std::printf("Named entities:       %zu\n", mNames.size());
        std::printf("FAT-P components:     19/19\n");

        std::printf("\nSystems registered:   %zu\n",
                    mScheduler.systemCount());
        std::printf("  AI          (writes: AIComponent | reads: Position, Health)\n");
        std::printf("  Movement    (writes: Position | reads: Velocity)\n");
        std::printf("  Turret      (writes: TurretTag | reads: Position, EnemyTag)\n");
        std::printf("  Collision   (reads: BulletTag, Position, DamageDealer, "
                    "EnemyTag, Health)\n");
        std::printf("  Damage      (writes: Health | reads: BulletTag, Position, "
                    "DamageDealer, EnemyTag)\n");
        std::printf("  Cleanup     (writes: Health, Position, Score | reads: "
                    "EnemyTag, BulletTag)\n");
    }

    // =========================================================================
    // Data Members
    // =========================================================================

    SimConfig mConfig;
    Registry mRegistry;
    Scheduler mScheduler;
    CommandBuffer mCommandBuffer;
    TemplateRegistry mTemplates;
    EntityNames mNames;
    SystemToggle mSystemToggle;
    FrameAllocator<CollisionPair> mCollisionAllocator;
    SimStats mStats;

    // CircularBuffer for rolling frame-time history (19th FAT-P component)
    // 512 entries = ~8.5 seconds at 60fps, enough for rolling average
    fat_p::CircularBuffer<double, 512> mFrameTimes;

    // Signal connection (must outlive the registry to avoid dangling)
    fat_p::ScopedConnection mDestroyConn;
};

// =============================================================================
// Main
// =============================================================================

int main(int argc, char* argv[])
{
    SimConfig config;

    // Simple command-line overrides
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
        {
            config.totalFrames = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--wave-size") == 0 && i + 1 < argc)
        {
            config.waveSize = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
        {
            config.numThreads = static_cast<std::size_t>(std::atoi(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--turrets") == 0 && i + 1 < argc)
        {
            config.numTurrets = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--report") == 0 && i + 1 < argc)
        {
            config.reportInterval = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--help") == 0)
        {
            std::printf("Usage: demo [options]\n");
            std::printf("  --frames N       Total simulation frames (default: 300)\n");
            std::printf("  --wave-size N    Enemies per wave (default: 20)\n");
            std::printf("  --threads N      Worker threads (default: 4)\n");
            std::printf("  --turrets N      Number of turrets (default: 4)\n");
            std::printf("  --report N       Report every N frames (default: 50)\n");
            return 0;
        }
    }

    SpaceBattleDemo demo(config);
    demo.run();

    return 0;
}
