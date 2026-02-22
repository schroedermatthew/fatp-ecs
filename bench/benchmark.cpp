// benchmark.cpp — FAT-P ECS vs EnTT Benchmark
//
// Uses fat_p::bench::BenchmarkRunner for guideline-compliant benchmarking:
//   - Round-robin execution with randomized order per run
//   - BenchmarkScope (Windows priority/affinity)
//   - CPU frequency monitoring with throttle detection
//   - Statistics: median (primary), mean, stddev, CI95
//   - FATP_BENCH_* environment variable configuration
//   - Cooldown between sections
//
// Build: cmake -B build -DFATP_ECS_BUILD_BENCH=ON ...
//        cmake --build build --config Release --target benchmark
//
// Run:   build\Release\benchmark.exe
//
// Env:   FATP_BENCH_BATCHES=25        (default: 15 Windows, 50 Linux)
//        FATP_BENCH_WARMUP_RUNS=5     (default: 3)
//        FATP_BENCH_NO_COOLDOWN=1     (skip inter-benchmark delays)
//        FATP_BENCH_NO_SCOPE=1        (skip priority/affinity)
//        FATP_BENCH_NO_STABILIZE=1    (skip CPU stabilization)
//        FATP_BENCH_VERBOSE_STATS=1   (show detailed statistics)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include <fat_p/FatPBenchmarkRunner.h>
#include <fatp_ecs/Registry.h>

// ============================================================================
// EnTT — suppress MSVC warnings from third-party headers
// ============================================================================
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4189 4324 4702)
#endif

#include <entt/entity/registry.hpp>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

using namespace fat_p::bench;

// ============================================================================
// Shared Components
// ============================================================================
struct Position { float x = 0.0f; float y = 0.0f; };
struct Velocity { float dx = 0.0f; float dy = 0.0f; };
struct Health   { int hp = 100; int maxHp = 100; };

// ============================================================================
// DCE sink (volatile, guideline-compliant)
// ============================================================================
static volatile uint64_t gSink = 0;

inline void snk(uint64_t v) { gSink += v; }
inline void snk(float v)    { uint32_t b; std::memcpy(&b, &v, 4); gSink += b; }
inline void snk(int v)      { gSink += static_cast<uint64_t>(static_cast<uint32_t>(v)); }

// ============================================================================
// Round-robin comparison using runner infrastructure
//
// Uses runner's Timer, Statistics, and config (warmup, measured runs, seed,
// cooldown). Each measured run shuffles library execution order so all
// libraries observe the same distribution of machine states.
// ============================================================================

using BenchFn = std::function<void()>;

void roundRobinCompare(
    BenchmarkRunner& runner,
    const std::string& caseName,
    const std::vector<std::string>& names,
    const std::vector<BenchFn>& setups,
    const std::vector<BenchFn>& benches,
    std::size_t N)
{
    const auto& cfg = runner.config();
    std::mt19937 rng(static_cast<unsigned>(cfg.seed));

    std::size_t nLibs = names.size();
    std::vector<std::vector<double>> allSamples(nLibs);

    if (!cfg.noCooldown)
    {
        cooldownDelay(cfg.cooldownCaseMs, nullptr, cfg.verboseStats);
    }

    // Warmup
    for (std::size_t w = 0; w < cfg.warmupRuns; ++w)
    {
        for (std::size_t i = 0; i < nLibs; ++i)
        {
            setups[i]();
            benches[i]();
        }
    }

    // Measured runs with round-robin randomization
    std::vector<std::size_t> order(nLibs);
    std::iota(order.begin(), order.end(), 0);

    for (std::size_t run = 0; run < cfg.measuredRuns; ++run)
    {
        std::shuffle(order.begin(), order.end(), rng);
        for (std::size_t idx : order)
        {
            setups[idx]();

            Timer timer;
            timer.start();
            benches[idx]();
            double elapsed = timer.elapsedNs();

            allSamples[idx].push_back(nsPerOp(elapsed, N));
        }
    }

    // Print results
    std::cout << "  " << caseName << ":\n";
    for (std::size_t i = 0; i < nLibs; ++i)
    {
        auto stats = Statistics::compute(std::move(allSamples[i]));
        stats.printComparison(std::cout, names[i].c_str());
    }
    std::cout << "\n";
}

// ============================================================================
// 1. Create Entities
// ============================================================================

void section1_Create(BenchmarkRunner& runner)
{
    runner.section("1. CREATE ENTITIES")
          .contract("Allocate N entities, no components. Includes entity ID sink to prevent DCE.");

    for (auto N : {1'000u, 10'000u, 100'000u, 1'000'000u})
    {
        std::unique_ptr<fatp_ecs::Registry> fReg;
        std::unique_ptr<entt::registry> e32Reg;
        std::unique_ptr<entt::basic_registry<uint64_t>> e64Reg;

        roundRobinCompare(runner, "N=" + std::to_string(N),
            {"fatp_ecs", "entt-32", "entt-64"},
            },
            {
                [&] { for (std::size_t i = 0; i < N; ++i) snk(fReg->create().get()); },
                [&] { for (std::size_t i = 0; i < N; ++i) { auto e = e32Reg->create(); snk(static_cast<uint64_t>(static_cast<std::underlying_type_t<entt::entity>>(e))); } },
                [&] { for (std::size_t i = 0; i < N; ++i) snk(static_cast<uint64_t>(e64Reg->create())); },
            },
            N);
    }
}

// ============================================================================
// 2. Destroy Entities
// ============================================================================

void section2_Destroy(BenchmarkRunner& runner)
{
    runner.section("2. DESTROY ENTITIES")
          .contract("Create N entities in setup (untimed), then destroy all. Measures destroy throughput.");

    for (auto N : {1'000u, 10'000u, 100'000u, 1'000'000u})
    {
        std::unique_ptr<fatp_ecs::Registry> fReg;
        std::vector<fatp_ecs::Entity> fEnts;
        std::unique_ptr<entt::registry> e32Reg;
        std::vector<entt::entity> e32Ents;
        std::unique_ptr<entt::basic_registry<uint64_t>> e64Reg;
        std::vector<uint64_t> e64Ents;

        roundRobinCompare(runner, "N=" + std::to_string(N),
            {"fatp_ecs", "entt-32", "entt-64"},
            },
            {
                [&] { for (auto e : fEnts) fReg->destroy(e); },
                [&] { for (auto e : e32Ents) e32Reg->destroy(e); },
                [&] { for (auto e : e64Ents) e64Reg->destroy(e); },
            },
            N);
    }
}

// ============================================================================
// 3. Add 1 Component
// ============================================================================

void section3_Add1(BenchmarkRunner& runner)
{
    runner.section("3. ADD 1 COMPONENT (Position)")
          .contract("Create N entities in setup, then add Position to each. Measures component attachment.");

    for (auto N : {1'000u, 10'000u, 100'000u, 1'000'000u})
    {
        std::unique_ptr<fatp_ecs::Registry> fReg;
        std::vector<fatp_ecs::Entity> fEnts;
        std::unique_ptr<entt::registry> e32Reg;
        std::vector<entt::entity> e32Ents;
        std::unique_ptr<entt::basic_registry<uint64_t>> e64Reg;
        std::vector<uint64_t> e64Ents;

        roundRobinCompare(runner, "N=" + std::to_string(N),
            {"fatp_ecs", "entt-32", "entt-64"},
            },
            {
                [&] { for (auto e : fEnts) fReg->add<Position>(e, 1.0f, 2.0f); },
                [&] { for (auto e : e32Ents) e32Reg->emplace<Position>(e, 1.0f, 2.0f); },
                [&] { for (auto e : e64Ents) e64Reg->emplace<Position>(e, 1.0f, 2.0f); },
            },
            N);
    }
}

// ============================================================================
// 4. Add 3 Components
// ============================================================================

void section4_Add3(BenchmarkRunner& runner)
{
    runner.section("4. ADD 3 COMPONENTS (Position + Velocity + Health)")
          .contract("Create N entities in setup, then add Position+Velocity+Health to each.");

    for (auto N : {1'000u, 10'000u, 100'000u, 1'000'000u})
    {
        std::unique_ptr<fatp_ecs::Registry> fReg;
        std::vector<fatp_ecs::Entity> fEnts;
        std::unique_ptr<entt::registry> e32Reg;
        std::vector<entt::entity> e32Ents;
        std::unique_ptr<entt::basic_registry<uint64_t>> e64Reg;
        std::vector<uint64_t> e64Ents;

        roundRobinCompare(runner, "N=" + std::to_string(N),
            {"fatp_ecs", "entt-32", "entt-64"},
            },
            {
                [&] { for (auto e : fEnts) { fReg->add<Position>(e, 1.f, 2.f); fReg->add<Velocity>(e, 3.f, 4.f); fReg->add<Health>(e, 100, 100); } },
                [&] { for (auto e : e32Ents) { e32Reg->emplace<Position>(e, 1.f, 2.f); e32Reg->emplace<Velocity>(e, 3.f, 4.f); e32Reg->emplace<Health>(e, 100, 100); } },
                [&] { for (auto e : e64Ents) { e64Reg->emplace<Position>(e, 1.f, 2.f); e64Reg->emplace<Velocity>(e, 3.f, 4.f); e64Reg->emplace<Health>(e, 100, 100); } },
            },
            N);
    }
}

// ============================================================================
// 5. Remove Component
// ============================================================================

void section5_Remove(BenchmarkRunner& runner)
{
    runner.section("5. REMOVE COMPONENT (Position)")
          .contract("Create N entities with Position in setup, then remove Position from each.");

    for (auto N : {1'000u, 10'000u, 100'000u, 1'000'000u})
    {
        std::unique_ptr<fatp_ecs::Registry> fReg;
        std::vector<fatp_ecs::Entity> fEnts;
        std::unique_ptr<entt::registry> e32Reg;
        std::vector<entt::entity> e32Ents;
        std::unique_ptr<entt::basic_registry<uint64_t>> e64Reg;
        std::vector<uint64_t> e64Ents;

        roundRobinCompare(runner, "N=" + std::to_string(N),
            {"fatp_ecs", "entt-32", "entt-64"},
            },
            {
                [&] { for (auto e : fEnts) fReg->remove<Position>(e); },
                [&] { for (auto e : e32Ents) e32Reg->remove<Position>(e); },
                [&] { for (auto e : e64Ents) e64Reg->remove<Position>(e); },
            },
            N);
    }
}

// ============================================================================
// 6. Get Component
// ============================================================================

void section6_Get(BenchmarkRunner& runner)
{
    runner.section("6. GET COMPONENT (Position)")
          .contract("Create N entities with Position in setup (persistent), then get Position for each. Sink value to prevent DCE.");

    for (auto N : {1'000u, 10'000u, 100'000u, 1'000'000u})
    {
        std::unique_ptr<fatp_ecs::Registry> fReg;
        std::vector<fatp_ecs::Entity> fEnts;
        std::unique_ptr<entt::registry> e32Reg;
        std::vector<entt::entity> e32Ents;
        std::unique_ptr<entt::basic_registry<uint64_t>> e64Reg;
        std::vector<uint64_t> e64Ents;

        roundRobinCompare(runner, "N=" + std::to_string(N),
            {"fatp_ecs", "entt-32", "entt-64"},
            },
            {
                [&] { for (auto e : fEnts) { auto& p = fReg->get<Position>(e); snk(p.x); } },
                [&] { for (auto e : e32Ents) { auto& p = e32Reg->get<Position>(e); snk(p.x); } },
                [&] { for (auto e : e64Ents) { auto& p = e64Reg->get<Position>(e); snk(p.x); } },
            },
            N);
    }
}

// ============================================================================
// 7. 1-Component Iteration
// ============================================================================

void section7_Iter1(BenchmarkRunner& runner)
{
    runner.section("7. 1-COMPONENT ITERATION (Position)")
          .contract("N entities with Position. Iterate via view, update + sink.");

    for (auto N : {1'000u, 10'000u, 100'000u, 1'000'000u})
    {
        std::unique_ptr<fatp_ecs::Registry> fReg;
        std::unique_ptr<entt::registry> e32Reg;
        std::unique_ptr<entt::basic_registry<uint64_t>> e64Reg;

        roundRobinCompare(runner, "N=" + std::to_string(N),
            {"fatp_ecs", "entt-32", "entt-64"},
            },
            {
                [&] { fReg->view<Position>().each([](fatp_ecs::Entity, Position& p) { p.x += 1.0f; snk(p.x); }); },
                [&] { e32Reg->view<Position>().each([](auto, Position& p) { p.x += 1.0f; snk(p.x); }); },
                [&] { e64Reg->view<Position>().each([](auto, Position& p) { p.x += 1.0f; snk(p.x); }); },
            },
            N);
    }
}

// ============================================================================
// 8. 2-Component Iteration
// ============================================================================

void section8_Iter2(BenchmarkRunner& runner)
{
    runner.section("8. 2-COMPONENT ITERATION (Position + Velocity)")
          .contract("N entities with Position+Velocity. Apply velocity to position.");

    for (auto N : {1'000u, 10'000u, 100'000u, 1'000'000u})
    {
        std::unique_ptr<fatp_ecs::Registry> fReg;
        std::unique_ptr<entt::registry> e32Reg;
        std::unique_ptr<entt::basic_registry<uint64_t>> e64Reg;

        roundRobinCompare(runner, "N=" + std::to_string(N),
            {"fatp_ecs", "entt-32", "entt-64"},
            },
            {
                [&] { fReg->view<Position, Velocity>().each([](fatp_ecs::Entity, Position& p, Velocity& v) { p.x += v.dx; p.y += v.dy; snk(p.x); }); },
                [&] { e32Reg->view<Position, Velocity>().each([](auto, Position& p, Velocity& v) { p.x += v.dx; p.y += v.dy; snk(p.x); }); },
                [&] { e64Reg->view<Position, Velocity>().each([](auto, Position& p, Velocity& v) { p.x += v.dx; p.y += v.dy; snk(p.x); }); },
            },
            N);
    }
}

// ============================================================================
// 9. 2-Component Sparse Iteration
// ============================================================================

void section9_Sparse(BenchmarkRunner& runner)
{
    runner.section("9. 2-COMPONENT SPARSE ITERATION")
          .contract("N Position, N/2 Velocity (even entities only). Tests sparse join efficiency.");

    for (auto N : {1'000u, 10'000u, 100'000u, 1'000'000u})
    {
        std::unique_ptr<fatp_ecs::Registry> fReg;
        std::unique_ptr<entt::registry> e32Reg;
        std::unique_ptr<entt::basic_registry<uint64_t>> e64Reg;

        roundRobinCompare(runner, "N=" + std::to_string(N),
            {"fatp_ecs", "entt-32", "entt-64"},
            },
            N / 2); // N/2 entities have both components
    }
}

// ============================================================================
// 10. 3-Component Iteration
// ============================================================================

void section10_Iter3(BenchmarkRunner& runner)
{
    runner.section("10. 3-COMPONENT ITERATION (Position + Velocity + Health)")
          .contract("N entities with Position+Velocity+Health. Iterate all three.");

    for (auto N : {1'000u, 10'000u, 100'000u, 1'000'000u})
    {
        std::unique_ptr<fatp_ecs::Registry> fReg;
        std::unique_ptr<entt::registry> e32Reg;
        std::unique_ptr<entt::basic_registry<uint64_t>> e64Reg;

        roundRobinCompare(runner, "N=" + std::to_string(N),
            {"fatp_ecs", "entt-32", "entt-64"},
            },
            {
                [&] { fReg->view<Position, Velocity, Health>().each([](fatp_ecs::Entity, Position& p, Velocity& v, Health& h) { p.x += v.dx; h.hp -= 1; snk(p.x); snk(h.hp); }); },
                [&] { e32Reg->view<Position, Velocity, Health>().each([](auto, Position& p, Velocity& v, Health& h) { p.x += v.dx; h.hp -= 1; snk(p.x); snk(h.hp); }); },
                [&] { e64Reg->view<Position, Velocity, Health>().each([](auto, Position& p, Velocity& v, Health& h) { p.x += v.dx; h.hp -= 1; snk(p.x); snk(h.hp); }); },
            },
            N);
    }
}

// ============================================================================
// 11. Fragmented Iteration
// ============================================================================

void section11_Frag(BenchmarkRunner& runner)
{
    runner.section("11. FRAGMENTED ITERATION")
          .contract("Create 2N, destroy odd indices, iterate remaining N with Position. Tests post-deletion density.");

    for (auto N : {10'000u, 100'000u})
    {
        std::unique_ptr<fatp_ecs::Registry> fReg;
        std::unique_ptr<entt::registry> e32Reg;
        std::unique_ptr<entt::basic_registry<uint64_t>> e64Reg;

        roundRobinCompare(runner, "N=" + std::to_string(N),
            {"fatp_ecs", "entt-32", "entt-64"},
            },
            N);
    }
}

// ============================================================================
// 12. Mixed Create/Destroy (Churn)
// ============================================================================

void section12_Churn(BenchmarkRunner& runner)
{
    runner.section("12. MIXED CREATE/DESTROY (churn)")
          .contract("Pre-create N entities, then create+destroy N more (alternating). Churn stress test.");

    for (auto N : {10'000u, 100'000u})
    {
        std::unique_ptr<fatp_ecs::Registry> fReg;
        std::vector<fatp_ecs::Entity> fEnts;
        std::unique_ptr<entt::registry> e32Reg;
        std::vector<entt::entity> e32Ents;
        std::unique_ptr<entt::basic_registry<uint64_t>> e64Reg;
        std::vector<uint64_t> e64Ents;

        roundRobinCompare(runner, "N=" + std::to_string(N),
            {"fatp_ecs", "entt-32", "entt-64"},
            },
            {
                [&] { for (std::size_t i = 0; i < N; ++i) { auto e = fReg->create(); fReg->destroy(fEnts[i]); fEnts[i] = e; } },
                [&] { for (std::size_t i = 0; i < N; ++i) { auto e = e32Reg->create(); e32Reg->destroy(e32Ents[i]); e32Ents[i] = e; } },
                [&] { for (std::size_t i = 0; i < N; ++i) { auto e = e64Reg->create(); e64Reg->destroy(e64Ents[i]); e64Ents[i] = e; } },
            },
            N);
    }
}

// ============================================================================
// Main
// ============================================================================

int main()
{
    auto runner = makeRunner("fatp_ecs vs EnTT");

    std::cout << "\nCompetitors:\n";
    std::cout << "  [x] fatp_ecs (primary, 64-bit entities)\n";
    std::cout << "  [x] EnTT (32-bit entities)\n";
    std::cout << "  [x] EnTT (64-bit entities)\n";
    std::cout << "\nDesign Invariants:\n";
    std::cout << "  1. Round-robin execution with randomized order per run\n";
    std::cout << "  2. Setup/teardown outside timed regions\n";
    std::cout << "  3. All libraries observe same distribution of machine states\n";
    std::cout << "  4. Medians are the primary reported statistic\n";
    std::cout << "\nEntity size: fatp=64-bit, entt-32=32-bit, entt-64=64-bit\n";
    std::cout.flush();

    section1_Create(runner);
    section2_Destroy(runner);
    section3_Add1(runner);
    section4_Add3(runner);
    section5_Remove(runner);
    section6_Get(runner);
    section7_Iter1(runner);
    section8_Iter2(runner);
    section9_Sparse(runner);
    section10_Iter3(runner);
    section11_Frag(runner);
    section12_Churn(runner);

    std::cout << "\nDone. (sink=" << gSink << ")\n";
    return 0;
}
