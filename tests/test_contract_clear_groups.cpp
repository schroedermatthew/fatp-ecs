/**
 * @file test_contract_clear_groups.cpp
 * @brief Contract test: Registry::clear() must reset group state.
 *
 * Regression for the seam between Registry::clear() and OwningGroup /
 * NonOwningGroup. Before the fix, clear() did not call reset() on groups,
 * leaving mGroupSize (OwningGroup) and mEntities (NonOwningGroup) stale.
 * A subsequent each() would walk past the end of now-empty dense arrays —
 * undefined behaviour, silent data corruption, or crash.
 *
 * Covered contracts:
 *   - After clear(), owning group size is 0.
 *   - After clear(), owning group each() visits zero entities.
 *   - After clear(), non-owning group size is 0.
 *   - After clear(), non-owning group each() visits zero entities.
 *   - After clear() + repopulate, groups re-populate correctly.
 *   - Multiple clear() cycles are stable.
 */

#include <fatp_ecs/FatpEcs.h>

#include <cstdio>
#include <vector>

using namespace fatp_ecs;

static int sPass = 0;
static int sFail = 0;

#define CONTRACT_ASSERT(cond, msg)                                          \\\n    do                                                                      \\\n    {                                                                       \\\n        if (!(cond))                                                        \\\n        {                                                                   \\\n            std::printf(\"  FAIL: %s (%s:%d)\\n\", msg, __FILE__, __LINE__);  \\\n            ++sFail;                                                        \\\n        }                                                                   \\\n        else { ++sPass; }                                                   \\\n    } while (0)

struct Position { float x = 0.f; float y = 0.f; };
struct Velocity { float dx = 1.f; float dy = 0.f; };

// ---------------------------------------------------------------------------
// Owning group contracts
// ---------------------------------------------------------------------------

void contract_owning_group_size_zero_after_clear()
{
    Registry reg;
    auto& grp = reg.group<Position, Velocity>();

    for (int i = 0; i < 100; ++i)
    {
        Entity e = reg.create();
        reg.add<Position>(e);
        reg.add<Velocity>(e);
    }
    CONTRACT_ASSERT(grp.size() == 100, "owning group seeded with 100 entities");

    reg.clear();

    CONTRACT_ASSERT(grp.size() == 0,
                    "owning group size must be 0 after Registry::clear()");
}

void contract_owning_group_each_no_op_after_clear()
{
    Registry reg;
    auto& grp = reg.group<Position, Velocity>();

    for (int i = 0; i < 50; ++i)
    {
        Entity e = reg.create();
        reg.add<Position>(e);
        reg.add<Velocity>(e);
    }

    reg.clear();

    int visitCount = 0;
    grp.each([&](Entity, Position&, Velocity&) { ++visitCount; });

    CONTRACT_ASSERT(visitCount == 0,
                    "owning group each() must visit 0 entities after clear()");
}

void contract_owning_group_repopulates_after_clear()
{
    Registry reg;
    auto& grp = reg.group<Position, Velocity>();

    for (int i = 0; i < 30; ++i)
    {
        Entity e = reg.create();
        reg.add<Position>(e);
        reg.add<Velocity>(e);
    }

    reg.clear();

    // Repopulate
    for (int i = 0; i < 20; ++i)
    {
        Entity e = reg.create();
        reg.add<Position>(e);
        reg.add<Velocity>(e);
    }

    CONTRACT_ASSERT(grp.size() == 20,
                    "owning group must re-populate to 20 after clear + repopulate");

    int visitCount = 0;
    grp.each([&](Entity, Position&, Velocity&) { ++visitCount; });
    CONTRACT_ASSERT(visitCount == 20,
                    "owning group each() must visit 20 entities after repopulate");
}

// ---------------------------------------------------------------------------
// Non-owning group contracts
// ---------------------------------------------------------------------------

void contract_non_owning_group_size_zero_after_clear()
{
    Registry reg;
    auto& grp = reg.non_owning_group<Position, Velocity>();

    for (int i = 0; i < 100; ++i)
    {
        Entity e = reg.create();
        reg.add<Position>(e);
        reg.add<Velocity>(e);
    }
    CONTRACT_ASSERT(grp.size() == 100, "non-owning group seeded with 100 entities");

    reg.clear();

    CONTRACT_ASSERT(grp.size() == 0,
                    "non-owning group size must be 0 after Registry::clear()");
}

void contract_non_owning_group_each_no_op_after_clear()
{
    Registry reg;
    auto& grp = reg.non_owning_group<Position, Velocity>();

    for (int i = 0; i < 50; ++i)
    {
        Entity e = reg.create();
        reg.add<Position>(e);
        reg.add<Velocity>(e);
    }

    reg.clear();

    int visitCount = 0;
    grp.each([&](Entity, Position&, Velocity&) { ++visitCount; });

    CONTRACT_ASSERT(visitCount == 0,
                    "non-owning group each() must visit 0 entities after clear()");
}

void contract_non_owning_group_repopulates_after_clear()
{
    Registry reg;
    auto& grp = reg.non_owning_group<Position, Velocity>();

    for (int i = 0; i < 30; ++i)
    {
        Entity e = reg.create();
        reg.add<Position>(e);
        reg.add<Velocity>(e);
    }

    reg.clear();

    for (int i = 0; i < 15; ++i)
    {
        Entity e = reg.create();
        reg.add<Position>(e);
        reg.add<Velocity>(e);
    }

    CONTRACT_ASSERT(grp.size() == 15,
                    "non-owning group must re-populate to 15 after clear + repopulate");
}

// ---------------------------------------------------------------------------
// Multi-cycle stability
// ---------------------------------------------------------------------------

void contract_multiple_clear_cycles_stable()
{
    Registry reg;
    auto& owning    = reg.group<Position, Velocity>();
    auto& nonOwning = reg.non_owning_group<Position, Velocity>();

    for (int cycle = 0; cycle < 10; ++cycle)
    {
        for (int i = 0; i < 25; ++i)
        {
            Entity e = reg.create();
            reg.add<Position>(e);
            reg.add<Velocity>(e);
        }

        CONTRACT_ASSERT(owning.size() == 25,
                        "owning group size == 25 during cycle");
        CONTRACT_ASSERT(nonOwning.size() == 25,
                        "non-owning group size == 25 during cycle");

        reg.clear();

        CONTRACT_ASSERT(owning.size() == 0,
                        "owning group size == 0 after clear");
        CONTRACT_ASSERT(nonOwning.size() == 0,
                        "non-owning group size == 0 after clear");
    }
}

// ---------------------------------------------------------------------------

int main()
{
    std::printf("=== Contract: Registry::clear() + Groups ===\n\n");

    std::printf("OwningGroup:\n");
    contract_owning_group_size_zero_after_clear();
    contract_owning_group_each_no_op_after_clear();
    contract_owning_group_repopulates_after_clear();

    std::printf("NonOwningGroup:\n");
    contract_non_owning_group_size_zero_after_clear();
    contract_non_owning_group_each_no_op_after_clear();
    contract_non_owning_group_repopulates_after_clear();

    std::printf("Multi-cycle stability:\n");
    contract_multiple_clear_cycles_stable();

    std::printf("\n=== Results: %d passed, %d failed ===\n", sPass, sFail);
    return sFail > 0 ? 1 : 0;
}
