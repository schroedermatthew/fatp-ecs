#pragma once

/**
 * @file OwningGroup.h
 * @brief Owning group for zero-probe simultaneous iteration over multiple
 *        tightly-packed component arrays.
 */

// Overview:
//
// An OwningGroup<A, B, ...> maintains a contiguous prefix in each owned
// ComponentStore's dense array so that all entities belonging to the group
// occupy indices [0, groupSize) in every store simultaneously.
//
// Iteration consequence: each(func) walks indices 0..groupSize-1 and reads
// component data directly by index from each store — no sparse lookups, no
// intersection probes. This delivers the maximum possible throughput for
// multi-component iteration: essentially raw array traversal.
//
// Cost model:
//   add<T>(entity)     — O(1) + up to N swaps (N = owned type count)
//   remove<T>(entity)  — O(1) + up to N swaps
//   each(func)         — O(groupSize), zero cache misses beyond the arrays
//
// Invariant maintenance:
//   The group intercepts onComponentAdded and onComponentRemoved signals for
//   every owned type. When an entity acquires the last missing owned component,
//   it is moved into the group prefix (one swapDenseEntries per store). When
//   it loses any owned component, it is swapped out of the prefix before the
//   erase proceeds (onComponentRemoved fires before the store erase).
//
// Ownership constraint:
//   Each ComponentStore may be owned by at most one OwningGroup per Registry.
//   This is enforced by Registry::group<Ts...>() at construction time.
//   Violating it would corrupt both groups' invariants.
//
// FAT-P components used:
//   - Signal / ScopedConnection: hooks into Registry EventBus
//   - ComponentStore::swapDenseEntries: in-place reorder primitive
//   - ComponentStore::getDenseIndex: locate entity in dense array

#include <cstddef>
#include <functional>
#include <tuple>

#include <fat_p/Signal.h>

#include "ComponentStore.h"
#include "Entity.h"
#include "EventBus.h"

namespace fatp_ecs
{

// =============================================================================
// IOwningGroup — type-erased base for Registry storage
// =============================================================================

/// @brief Abstract base so Registry can store OwningGroup<Ts...> type-erased.
class IOwningGroup
{
public:
    virtual ~IOwningGroup() = default;
    IOwningGroup() = default;
    IOwningGroup(const IOwningGroup&) = delete;
    IOwningGroup& operator=(const IOwningGroup&) = delete;
    IOwningGroup(IOwningGroup&&) = delete;
    IOwningGroup& operator=(IOwningGroup&&) = delete;
};

// =============================================================================
// OwningGroup
// =============================================================================

/**
 * @brief Owning group over a fixed set of component types.
 *
 * Created via Registry::group<Ts...>(). Not intended to be stored by
 * value across frames — the Registry owns it.
 *
 * @tparam Ts Owned component types (2 or more recommended for benefit).
 *
 * @example
 * @code
 *   auto& grp = registry.group<Position, Velocity>();
 *
 *   // Each frame — no sparse probes, pure array walk:
 *   grp.each([](Entity e, Position& p, Velocity& v) {
 *       p.x += v.dx;
 *       p.y += v.dy;
 *   });
 * @endcode
 */
template <typename... Ts>
class OwningGroup : public IOwningGroup
{
    static_assert(sizeof...(Ts) >= 2,
                  "OwningGroup requires at least two component types");

public:
    // =========================================================================
    // Construction (called by Registry::group<Ts...>())
    // =========================================================================

    explicit OwningGroup(ComponentStore<Ts>*... stores, EventBus& events)
        : mStores(stores...)
        , mGroupSize(0)
    {
        // Seed: walk the smallest store and add qualifying entities.
        // Then wire signals for future changes.
        seedFromExistingEntities();
        connectSignals(events);
    }

    // Move-only (ScopedConnections are non-copyable).
    OwningGroup(const OwningGroup&) = delete;
    OwningGroup& operator=(const OwningGroup&) = delete;
    OwningGroup(OwningGroup&&) noexcept = default;
    OwningGroup& operator=(OwningGroup&&) noexcept = default;

    // =========================================================================
    // Iteration
    // =========================================================================

    /**
     * @brief Iterate all group entities with direct array access — no probes.
     *
     * Callback signature: void(Entity, Ts&...)
     */
    template <typename Func>
    void each(Func&& func)
    {
        for (std::size_t i = 0; i < mGroupSize; ++i)
        {
            // Entity comes from store 0's dense array; all stores are
            // in sync so any store's dense[i] names the same entity.
            Entity entity = std::get<0>(mStores)->dense()[i];
            invokeAt(std::forward<Func>(func), entity, i,
                     std::index_sequence_for<Ts...>{});
        }
    }

    template <typename Func>
    void each(Func&& func) const
    {
        for (std::size_t i = 0; i < mGroupSize; ++i)
        {
            Entity entity = std::get<0>(mStores)->dense()[i];
            invokeAtConst(std::forward<Func>(func), entity, i,
                          std::index_sequence_for<Ts...>{});
        }
    }

    /**
     * @brief Number of entities currently in the group.
     */
    [[nodiscard]] std::size_t size() const noexcept
    {
        return mGroupSize;
    }

    /**
     * @brief True if no entities are in the group.
     */
    [[nodiscard]] bool empty() const noexcept
    {
        return mGroupSize == 0;
    }

    /**
     * @brief True if entity is currently a member of the group.
     */
    [[nodiscard]] bool contains(Entity entity) const noexcept
    {
        const std::size_t di =
            std::get<0>(mStores)->getDenseIndex(entity);
        return di < mGroupSize;
    }

private:
    std::tuple<ComponentStore<Ts>*...> mStores;
    std::size_t mGroupSize{0};
    std::vector<fat_p::ScopedConnection> mConnections;

    // =========================================================================
    // Seeding — called once at construction
    // =========================================================================

    void seedFromExistingEntities()
    {
        // Walk the first store's entities; for each that qualifies (has all
        // owned types), move it into the group prefix.
        auto* pivot = std::get<0>(mStores);
        const std::size_t total = pivot->size();

        for (std::size_t i = 0; i < total; )
        {
            Entity entity = pivot->dense()[i];
            if (entityHasAllTypes(entity))
            {
                moveIntoGroup(entity);
                // After moveIntoGroup, entity is at index mGroupSize-1.
                // The entity that was at mGroupSize-1 is now at i — re-check i.
                // Actually moveIntoGroup swaps entity to mGroupSize (then increments),
                // so the new entity at [old mGroupSize - 1... wait, let's think:
                // Before call: mGroupSize = k. Entity is at pos i (i >= k trivially
                // since we only look at unprocessed entries). After swapDenseEntries(i, k)
                // entity is at k, mGroupSize becomes k+1. Next i to check is k+1, but
                // we advance i past k. So just ++i is correct.
                ++i;
            }
            else
            {
                ++i;
            }
        }
    }

    // =========================================================================
    // Signal wiring
    // =========================================================================

    void connectSignals(EventBus& events)
    {
        connectAdded(events, std::index_sequence_for<Ts...>{});
        connectRemoved(events, std::index_sequence_for<Ts...>{});
    }

    template <std::size_t... Is>
    void connectAdded(EventBus& events, std::index_sequence<Is...>)
    {
        (connectAddedForType<std::tuple_element_t<Is, std::tuple<Ts...>>>(events), ...);
    }

    template <typename T>
    void connectAddedForType(EventBus& events)
    {
        mConnections.push_back(
            events.onComponentAdded<T>().connect(
                [this](Entity entity, T&) { onComponentAdded(entity); }));
    }

    template <std::size_t... Is>
    void connectRemoved(EventBus& events, std::index_sequence<Is...>)
    {
        (connectRemovedForType<std::tuple_element_t<Is, std::tuple<Ts...>>>(events), ...);
    }

    template <typename T>
    void connectRemovedForType(EventBus& events)
    {
        mConnections.push_back(
            events.onComponentRemoved<T>().connect(
                [this](Entity entity) { onComponentRemoved(entity); }));
    }

    // =========================================================================
    // Event handlers
    // =========================================================================

    // Called after a component of any owned type is added to entity.
    // If entity now has all owned types, move it into the group prefix.
    void onComponentAdded(Entity entity)
    {
        if (entityHasAllTypes(entity))
        {
            moveIntoGroup(entity);
        }
    }

    // Called before a component of any owned type is removed from entity.
    // If entity is currently in the group, swap it out of the prefix first.
    void onComponentRemoved(Entity entity)
    {
        if (entityInGroup(entity))
        {
            moveOutOfGroup(entity);
        }
    }

    // =========================================================================
    // Group maintenance helpers
    // =========================================================================

    [[nodiscard]] bool entityHasAllTypes(Entity entity) const noexcept
    {
        return entityHasAllImpl(entity, std::index_sequence_for<Ts...>{});
    }

    template <std::size_t... Is>
    [[nodiscard]] bool entityHasAllImpl(Entity entity,
                                        std::index_sequence<Is...>) const noexcept
    {
        return (std::get<Is>(mStores)->has(entity) && ...);
    }

    [[nodiscard]] bool entityInGroup(Entity entity) const noexcept
    {
        const std::size_t di = std::get<0>(mStores)->getDenseIndex(entity);
        return di < mGroupSize;
    }

    // Swap entity into position mGroupSize in every store, then increment.
    // Pre-condition: entity has all owned components, mGroupSize <= its current
    // dense index in every store.
    void moveIntoGroup(Entity entity)
    {
        // Already in group (seeding can call this multiple times on concurrent
        // add-all scenarios — guard).
        if (entityInGroup(entity))
        {
            return;
        }
        swapIntoFront(entity, std::index_sequence_for<Ts...>{});
        ++mGroupSize;
    }

    // Swap entity to position mGroupSize-1 in every store, then decrement.
    // Pre-condition: entity is in the group.
    void moveOutOfGroup(Entity entity)
    {
        --mGroupSize;
        swapToPosition(entity, mGroupSize, std::index_sequence_for<Ts...>{});
    }

    template <std::size_t... Is>
    void swapIntoFront(Entity entity, std::index_sequence<Is...>)
    {
        (swapEntityToPosition<Is>(entity, mGroupSize), ...);
    }

    template <std::size_t... Is>
    void swapToPosition(Entity entity, std::size_t target, std::index_sequence<Is...>)
    {
        (swapEntityToPosition<Is>(entity, target), ...);
    }

    template <std::size_t I>
    void swapEntityToPosition(Entity entity, std::size_t targetPos)
    {
        auto* store = std::get<I>(mStores);
        const std::size_t currentPos = store->getDenseIndex(entity);
        if (currentPos != targetPos)
        {
            store->swapDenseEntries(currentPos, targetPos);
        }
    }

    // =========================================================================
    // Iteration helpers
    // =========================================================================

    template <typename Func, std::size_t... Is>
    void invokeAt(Func&& func, Entity entity, std::size_t i,
                  std::index_sequence<Is...>)
    {
        func(entity, std::get<Is>(mStores)->dataAtUnchecked(i)...);
    }

    template <typename Func, std::size_t... Is>
    void invokeAtConst(Func&& func, Entity entity, std::size_t i,
                       std::index_sequence<Is...>) const
    {
        func(entity, std::get<Is>(mStores)->dataAtUnchecked(i)...);
    }
};

} // namespace fatp_ecs
