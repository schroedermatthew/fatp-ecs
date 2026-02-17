// test_clear.cpp — Verify FAT-P Registry::clear() at scale
//
// Tests that clear() + recreate works correctly across many cycles
// at 1K, 10K, 100K, and 1M entities. If clear() has a bug (leaking
// sparse array state, broken free list), this will crash or OOM.
//
// Build: cl /std:c++20 /O2 /EHsc /I../include /I../../FatP/include test_clear.cpp
//    or: add as a test target in CMake

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include <fatp_ecs/Registry.h>

static int gPass = 0;
static int gFail = 0;

#define CHECK(expr, msg)                                             \
    do {                                                             \
        if (!(expr)) {                                               \
            std::cout << "  FAIL: " << msg << "\n"; ++gFail;        \
        } else {                                                     \
            ++gPass;                                                 \
        }                                                            \
    } while (0)

struct Position { float x = 0.f; float y = 0.f; };
struct Velocity { float dx = 0.f; float dy = 0.f; };

void testClearAndRecreate(std::size_t N, int cycles)
{
    std::cout << "clear() + recreate: N=" << N << ", cycles=" << cycles << "\n";

    fatp_ecs::Registry reg;

    for (int c = 0; c < cycles; ++c)
    {
        // Create N entities with components
        std::vector<fatp_ecs::Entity> ents;
        ents.reserve(N);
        for (std::size_t i = 0; i < N; ++i)
        {
            auto e = reg.create();
            reg.add<Position>(e, Position{float(i), float(i)});
            reg.add<Velocity>(e, Velocity{1.f, 1.f});
            ents.push_back(e);
        }

        CHECK(reg.entityCount() == N,
              "cycle " + std::to_string(c) + " entity count == N");

        // Verify components are accessible
        for (std::size_t i = 0; i < N; ++i)
        {
            auto* p = reg.tryGet<Position>(ents[i]);
            if (!p) { CHECK(false, "component missing before clear"); break; }
        }

        // Clear everything
        reg.clear();

        CHECK(reg.entityCount() == 0,
              "cycle " + std::to_string(c) + " entity count == 0 after clear");
    }

    std::cout << "  OK\n";
}

void testClearThenIterateIsEmpty(std::size_t N)
{
    std::cout << "clear() then iterate is empty: N=" << N << "\n";

    fatp_ecs::Registry reg;

    for (std::size_t i = 0; i < N; ++i)
    {
        auto e = reg.create();
        reg.add<Position>(e, Position{float(i), 0.f});
    }

    reg.clear();

    std::size_t count = 0;
    reg.view<Position>().each(
        [&](fatp_ecs::Entity, Position&) { ++count; });

    CHECK(count == 0, "view should be empty after clear");
    std::cout << "  OK\n";
}

void testClearDoesNotCorruptNewEntities(std::size_t N, int cycles)
{
    std::cout << "clear() does not corrupt new entities: N=" << N
              << ", cycles=" << cycles << "\n";

    fatp_ecs::Registry reg;

    for (int c = 0; c < cycles; ++c)
    {
        for (std::size_t i = 0; i < N; ++i)
        {
            auto e = reg.create();
            reg.add<Position>(e, Position{float(c * N + i), 0.f});
        }

        // Verify iteration count matches
        std::size_t viewCount = 0;
        reg.view<Position>().each(
            [&](fatp_ecs::Entity, Position&) { ++viewCount; });

        CHECK(viewCount == N,
              "cycle " + std::to_string(c) + " view count == N (got " +
              std::to_string(viewCount) + ")");

        reg.clear();
    }

    std::cout << "  OK\n";
}

void testStaleHandlesAfterClear(std::size_t N)
{
    std::cout << "stale handles after clear: N=" << N << "\n";

    fatp_ecs::Registry reg;

    std::vector<fatp_ecs::Entity> oldEnts;
    oldEnts.reserve(N);
    for (std::size_t i = 0; i < N; ++i)
    {
        auto e = reg.create();
        reg.add<Position>(e, Position{float(i), 0.f});
        oldEnts.push_back(e);
    }

    reg.clear();

    // Old handles should not be alive
    bool anyAlive = false;
    for (auto e : oldEnts)
    {
        if (reg.isAlive(e)) { anyAlive = true; break; }
    }
    CHECK(!anyAlive, "no old handles should be alive after clear");

    // Old handles should not have components
    bool anyHas = false;
    for (auto e : oldEnts)
    {
        if (reg.has<Position>(e)) { anyHas = true; break; }
    }
    CHECK(!anyHas, "no old handles should have components after clear");

    std::cout << "  OK\n";
}

int main()
{
    std::cout << "=== FAT-P Registry::clear() Stress Test ===\n\n";

    // Escalating scale
    testClearAndRecreate(1'000, 20);
    testClearAndRecreate(10'000, 15);
    testClearAndRecreate(100'000, 13);
    testClearAndRecreate(1'000'000, 13);  // same as benchmark: 13 cycles

    testClearThenIterateIsEmpty(100'000);
    testClearDoesNotCorruptNewEntities(100'000, 13);

    testStaleHandlesAfterClear(100'000);

    std::cout << "\n=== Results: " << gPass << " passed, " << gFail
              << " failed ===\n";
    return gFail > 0 ? 1 : 0;
}
