#pragma once

/**
 * @file Simulation.h
 * @brief Shared simulation logic for the FAT-P ECS space battle demo.
 *
 * Contains components, AI state machine, configuration, and the core
 * simulation class used by both the terminal and visual demos.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// FAT-P direct includes
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
    int fireCooldown = 3;
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

// State implementations (inline for header-only)
inline void IdleState::on_entry(AIContext& ctx) { ++ctx.stateChanges; }
inline void IdleState::on_exit(AIContext&) {}
inline void ChaseState::on_entry(AIContext& ctx) { ++ctx.stateChanges; }
inline void ChaseState::on_exit(AIContext&) {}
inline void AttackState::on_entry(AIContext& ctx) { ++ctx.stateChanges; }
inline void AttackState::on_exit(AIContext&) {}
inline void DeadState::on_entry(AIContext& ctx) { ++ctx.stateChanges; }
inline void DeadState::on_exit(AIContext&) {}

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
// SpaceBattleSim — Core Simulation (no I/O)
// =============================================================================

class SpaceBattleSim
{
public:
    explicit SpaceBattleSim(const SimConfig& config)
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

    // Advance the simulation by one frame.
    void tick()
    {
        // --- Pre-frame: spawning ---
        if (mStats.frame % mConfig.spawnInterval == 0)
        {
            spawnWave(mStats.frame);
        }

        // --- System execution (parallel via Scheduler) ---
        mScheduler.run(mRegistry);

        // --- Post-frame: flush deferred commands ---
        mCommandBuffer.flush(mRegistry);

        // --- Stats ---
        mStats.frame++;
        mStats.entityCount = mRegistry.entityCount();
        if (mStats.entityCount > mStats.peakEntities)
        {
            mStats.peakEntities = mStats.entityCount;
        }

        // --- Frame allocator reset ---
        mCollisionAllocator.releaseAll();
    }

    // Record a frame time measurement.
    void recordFrameTime(double ms)
    {
        (void)mFrameTimes.push(ms);
        if (ms > mStats.peakFrameTimeMs)
        {
            mStats.peakFrameTimeMs = ms;
        }
    }

    void updateAvgFrameTime()
    {
        double sum = 0.0;
        int count = 0;
        double val = 0.0;
        std::vector<double> times;
        times.reserve(mFrameTimes.size());
        while (mFrameTimes.pop(val))
        {
            times.push_back(val);
        }
        for (double t : times)
        {
            sum += t;
            ++count;
        }
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

    int getCurrentScore()
    {
        Entity scoreEntity = mNames.findByName("score");
        if (scoreEntity != NullEntity)
        {
            const Score* s = mRegistry.tryGet<Score>(scoreEntity);
            if (s != nullptr)
            {
                return s->value;
            }
        }
        return 0;
    }

    int countSurvivingEnemies()
    {
        int count = 0;
        mRegistry.view<EnemyTag>().each(
            [&count]([[maybe_unused]] Entity e, [[maybe_unused]] EnemyTag&)
            { ++count; });
        return count;
    }

    int countSurvivingTurrets()
    {
        int count = 0;
        mRegistry.view<TurretTag>().each(
            [&count]([[maybe_unused]] Entity e, [[maybe_unused]] TurretTag&)
            { ++count; });
        return count;
    }

    // --- Accessors ---

    const SimConfig& config() const noexcept { return mConfig; }
    const SimStats& stats() const noexcept { return mStats; }
    Registry& registry() noexcept { return mRegistry; }
    const Registry& registry() const noexcept { return mRegistry; }
    Scheduler& scheduler() noexcept { return mScheduler; }
    const EntityNames& names() const noexcept { return mNames; }

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
        mSystemToggle.registerSystem("ai", true);
        mSystemToggle.registerSystem("movement", true);
        mSystemToggle.registerSystem("turret", true);
        mSystemToggle.registerSystem("collision", true);
        mSystemToggle.registerSystem("damage", true);
        mSystemToggle.registerSystem("cleanup", true);

        // --- AI System ---
        mScheduler.addSystem("AI",
            [this](Registry& reg)
            {
                if (!mSystemToggle.isEnabled("ai")) { return; }
                reg.view<AIComponent, Position, Health>().each(
                    [](Entity, AIComponent& ai, Position& pos, Health& hp)
                    {
                        ai.context.hp = hp.hp;
                        ai.context.maxHp = hp.maxHp;
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

        // --- Movement System ---
        mScheduler.addSystem("Movement",
            [this](Registry& reg)
            {
                if (!mSystemToggle.isEnabled("movement")) { return; }
                reg.view<Position, Velocity>().each(
                    []([[maybe_unused]] Entity e, Position& pos, Velocity& vel)
                    {
                        pos.x += vel.dx;
                        pos.y += vel.dy;
                    });
            },
            makeComponentMask<Position>(),
            makeComponentMask<Velocity>());

        // --- Turret System ---
        mScheduler.addSystem("Turret",
            [this](Registry& reg)
            {
                if (!mSystemToggle.isEnabled("turret")) { return; }
                reg.view<TurretTag, Position>().each(
                    [this, &reg](Entity, TurretTag& turret, Position& turretPos)
                    {
                        if (turret.cooldown > 0)
                        {
                            --turret.cooldown;
                            return;
                        }
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

        // --- Collision System ---
        mScheduler.addSystem("Collision",
            [this](Registry& reg)
            {
                if (!mSystemToggle.isEnabled("collision")) { return; }
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

        // --- Damage System ---
        mScheduler.addSystem("Damage",
            [this](Registry& reg)
            {
                if (!mSystemToggle.isEnabled("damage")) { return; }
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

        // --- Cleanup System ---
        mScheduler.addSystem("Cleanup",
            [this](Registry& reg)
            {
                if (!mSystemToggle.isEnabled("cleanup")) { return; }
                reg.view<EnemyTag, Health>().each(
                    [this]([[maybe_unused]] Entity e,
                           [[maybe_unused]] EnemyTag&, Health& hp)
                    {
                        if (hp.hp <= 0)
                        {
                            mCommandBuffer.destroy(e);
                            ++mStats.totalKilled;
                        }
                    });
                reg.view<BulletTag, Position>().each(
                    [this]([[maybe_unused]] Entity e,
                           [[maybe_unused]] BulletTag&, Position& pos)
                    {
                        if (pos.x < -50.0f || pos.x > mConfig.arenaWidth + 50.0f ||
                            pos.y < -50.0f || pos.y > mConfig.arenaHeight + 50.0f)
                        {
                            mCommandBuffer.destroy(e);
                        }
                    });
                reg.view<EnemyTag, Position>().each(
                    [this]([[maybe_unused]] Entity e,
                           [[maybe_unused]] EnemyTag&, Position& pos)
                    {
                        if (pos.x < 0.0f)
                        {
                            mCommandBuffer.destroy(e);
                        }
                    });
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
        mDestroyConn = mRegistry.events().onEntityDestroyed.connect(
            []([[maybe_unused]] Entity e) {});
    }

    void spawnTurrets()
    {
        fat_p::AlignedVector<float> turretXPositions;
        fat_p::AlignedVector<float> turretYPositions;
        float spacing = mConfig.arenaHeight /
                        static_cast<float>(mConfig.numTurrets + 1);
        for (int i = 0; i < mConfig.numTurrets; ++i)
        {
            Entity turret = mRegistry.create();
            float ty = spacing * static_cast<float>(i + 1);
            float tx = 50.0f;
            mRegistry.add<Position>(turret, tx, ty);
            mRegistry.add<TurretTag>(turret,
                mConfig.turretRange, mConfig.turretDamage, 0, 2);
            mRegistry.add<Health>(turret, 500, 500);
            turretXPositions.push_back(tx);
            turretYPositions.push_back(ty);
            char name[32];
            std::snprintf(name, sizeof(name), "turret_%d", i);
            mNames.setName(turret, name);
        }
        if (!turretXPositions.empty())
        {
            auto addr = reinterpret_cast<std::uintptr_t>(turretXPositions.data());
            (void)addr;
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
            if (enemy == NullEntity) { continue; }
            float yPos = spacing * static_cast<float>(i + 1);
            mRegistry.get<Position>(enemy).y = yPos;
            mRegistry.add<EnemyTag>(enemy);
            mRegistry.add<AIComponent>(enemy, enemy, 50, 50);
            ++mStats.totalSpawned;
        }
        char waveName[32];
        std::snprintf(waveName, sizeof(waveName), "wave_%d", waveNum);
        (void)waveName;
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
    fat_p::CircularBuffer<double, 512> mFrameTimes;
    fat_p::ScopedConnection mDestroyConn;
};
