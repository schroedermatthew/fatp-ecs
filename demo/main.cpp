/**
 * @file main.cpp
 * @brief FAT-P ECS Phase 4 Demo — Terminal Space Battle Simulation
 *
 * Runs the space battle simulation headless and prints periodic stats.
 * Uses all 19 FAT-P components. See Simulation.h for the shared core.
 */

#include <cstdio>
#include <cstring>

#include "Simulation.h"

int main(int argc, char* argv[])
{
    SimConfig config;

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

    SpaceBattleSim sim(config);

    std::printf("=== FAT-P ECS Demo: Space Battle ===\n");
    std::printf("Threads: %zu | Frames: %d | Enemies/wave: %d | Turrets: %d\n\n",
                config.numThreads, config.totalFrames,
                config.waveSize, config.numTurrets);

    FrameTimer frameTimer;

    for (int frame = 0; frame < config.totalFrames; ++frame)
    {
        frameTimer.start();
        sim.tick();
        double frameMs = frameTimer.elapsedMs();
        sim.recordFrameTime(frameMs);

        if ((frame + 1) % config.reportInterval == 0)
        {
            sim.updateAvgFrameTime();
            const auto& s = sim.stats();
            std::printf("[Frame %4d] entities: %-6zu spawned: %-6d killed: %-6d "
                        "score: %-8d avg_ms: %.2f\n",
                        s.frame, s.entityCount, s.totalSpawned, s.totalKilled,
                        sim.getCurrentScore(), s.avgFrameTimeMs);
        }
    }

    sim.updateAvgFrameTime();
    sim.countAIStateChanges();
    const auto& s = sim.stats();

    std::printf("\n=== Final Report ===\n");
    std::printf("Total frames:         %d\n", s.frame);
    std::printf("Total spawned:        %d\n", s.totalSpawned);
    std::printf("Total killed:         %d\n", s.totalKilled);
    std::printf("Surviving enemies:    %d\n", sim.countSurvivingEnemies());
    std::printf("Surviving turrets:    %d/%d\n",
                sim.countSurvivingTurrets(), config.numTurrets);
    std::printf("Bullets spawned:      %d\n", s.bulletsSpawned);
    std::printf("Final score:          %d\n", sim.getCurrentScore());
    std::printf("Peak entities:        %zu\n", s.peakEntities);
    std::printf("Avg frame time:       %.3f ms\n", s.avgFrameTimeMs);
    std::printf("Peak frame time:      %.3f ms\n", s.peakFrameTimeMs);
    std::printf("AI state changes:     %d\n", s.totalAIStateChanges);
    std::printf("Named entities:       %zu\n", sim.names().size());
    std::printf("FAT-P components:     19/19\n");

    std::printf("\nSystems registered:   %zu\n", sim.scheduler().systemCount());
    std::printf("  AI          (writes: AIComponent | reads: Position, Health)\n");
    std::printf("  Movement    (writes: Position | reads: Velocity)\n");
    std::printf("  Turret      (writes: TurretTag | reads: Position, EnemyTag)\n");
    std::printf("  Collision   (reads: BulletTag, Position, DamageDealer, "
                "EnemyTag, Health)\n");
    std::printf("  Damage      (writes: Health | reads: BulletTag, Position, "
                "DamageDealer, EnemyTag)\n");
    std::printf("  Cleanup     (writes: Health, Position, Score | reads: "
                "EnemyTag, BulletTag)\n");

    return 0;
}
