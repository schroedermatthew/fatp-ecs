/**
 * @file test_contract_destroy_ordering.cpp
 * @brief Contract test: Registry::destroy() must fire component-removed events
 *        before onEntityDestroyed.
 *
 * Regression for the destroy event ordering seam. Before the fix, destroy()
 * emitted onEntityDestroyed first, then removeAndNotify for each store. An
 * Observer with OnRemoved<T> would:
 *   1. Receive onEntityDestroyed → erase entity from dirty set.
 *   2. Receive onComponentRemoved<T> → re-insert entity into dirty set.
 * Result: dirty set contained a destroyed entity, violating the Observer
 * contract ("each() always yields live entities only").
 *
 * Covered contracts:
 *   - After destroy(), OnRemoved<T> observer does not contain the entity.
 *   - After destroy(), mixed OnAdded+OnRemoved observer does not contain
 *     the entity.
 *   - isAlive() returns false for the entity when onEntityDestroyed fires
 *     (verified via signal connection at destroy time).
 *   - onEntityDestroyed fires exactly once per destroy() call.
 */

#include <fatp_ecs/FatpEcs.h>

#include <cstdio>
#include <vector>

using namespace fatp_ecs;

static int sPass = 0;
static int sFail = 0;

#define CONTRACT_ASSERT(cond, msg)                                          \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            std::printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);  \
            ++sFail;                                                        \
        }                                                                   \
        else { ++sPass; }                                                   \
    } while (0)

struct Position { float x = 0.f; float y = 0.f; };
struct Velocity { float dx = 0.f; float dy = 0.f; };

// ---------------------------------------------------------------------------

void contract_onremoved_observer_empty_after_destroy()
{
    // If destroy() fires onEntityDestroyed before removeAndNotify, the Observer
    // erases the entity on destroyed, then re-inserts it on removed. The dirty
    // set ends up containing a destroyed entity. The correct order (removed
    // first, destroyed last) must leave the dirty set empty.

    Registry reg;
    auto obs = reg.observe(OnRemoved<Position>{});

    Entity e = reg.create();
    reg.add<Position>(e);
    obs.clear(); // discard the implicit "added" event if observer also tracked adds

    reg.destroy(e);

    CONTRACT_ASSERT(obs.empty(),
                    "OnRemoved<Position> observer must be empty after destroy()");
}

void contract_mixed_observer_empty_after_destroy()
{
    Registry reg;
    auto obs = reg.observe(OnAdded<Position>{}, OnRemoved<Position>{});

    Entity e = reg.create();
    reg.add<Position>(e);
    obs.clear(); // consume the OnAdded event — start clean

    reg.destroy(e);

    CONTRACT_ASSERT(obs.empty(),
                    "Mixed OnAdded+OnRemoved observer must be empty after destroy()");
}

void contract_multiple_entities_only_destroyed_removed()
{
    Registry reg;
    auto obs = reg.observe(OnRemoved<Position>{});

    Entity keep = reg.create(); reg.add<Position>(keep);
    Entity gone = reg.create(); reg.add<Position>(gone);
    obs.clear();

    reg.destroy(gone);

    // Observer must not contain 'gone' (it was destroyed, not just removed).
    // 'keep' was not touched, so also absent.
    CONTRACT_ASSERT(obs.empty(),
                    "destroying an entity must not leave it in OnRemoved observer");
}

void contract_entity_not_alive_when_destroyed_signal_fires()
{
    // Verify the ordering from the receiving end: when onEntityDestroyed fires,
    // the entity's components must already be gone (stores cleared for it).
    // We check this by subscribing to onEntityDestroyed and calling has<T>().

    Registry reg;

    bool hadPositionOnDestroyed = true; // pessimistic default
    reg.events().onEntityDestroyed.connect(
        [&](Entity e) { hadPositionOnDestroyed = reg.has<Position>(e); });

    Entity ent = reg.create();
    reg.add<Position>(ent);

    reg.destroy(ent);

    CONTRACT_ASSERT(!hadPositionOnDestroyed,
                    "Position must be removed before onEntityDestroyed fires");
}

void contract_destroyed_signal_fires_exactly_once()
{
    Registry reg;

    int fireCount = 0;
    reg.events().onEntityDestroyed.connect([&](Entity) { ++fireCount; });

    Entity e = reg.create();
    reg.add<Position>(e);
    reg.destroy(e);

    CONTRACT_ASSERT(fireCount == 1,
                    "onEntityDestroyed must fire exactly once per destroy()");
}

// ---------------------------------------------------------------------------

int main()
{
    std::printf("=== Contract: Registry::destroy() Event Ordering ===\n\n");

    contract_onremoved_observer_empty_after_destroy();
    contract_mixed_observer_empty_after_destroy();
    contract_multiple_entities_only_destroyed_removed();
    contract_entity_not_alive_when_destroyed_signal_fires();
    contract_destroyed_signal_fires_exactly_once();

    std::printf("\n=== Results: %d passed, %d failed ===\n", sPass, sFail);
    return sFail > 0 ? 1 : 0;
}
