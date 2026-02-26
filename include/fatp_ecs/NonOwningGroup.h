#pragma once

/**
 * @file NonOwningGroup.h
 * @brief Non-owning group for cached multi-component iteration without
 *        reordering any ComponentStore's dense array.
 */

// Overview:
//
// A NonOwningGroup<A, B, ...> tracks which entities currently possess all
// listed component types, maintaining that set in its own internal entity
// list. It does NOT rearrange any ComponentStore — the stores remain fully
// under their normal ownership (view, sort, other groups, etc.).
//
// Iteration consequence: each(func) walks the internal entity list and reads
// component data via direct sparse lookups. No ownership conflicts arise, so
// the same types can be used in multiple non-owning groups, or alongside
// owning groups for non-overlapping owned types.
//
// Trade-offs vs OwningGroup:
//   - No store reordering  → stores stay cache-coherent for other users
//   - Slightly slower each() → one sparse lookup per component per entity
//   - Maintains its own entity list → small per-group memory overhead
//   - Compatible with sorting, other groups, and Views for the same types
//
// Cost model:
//   add<T>(entity)     — O(1) amortised (signal handler + push_back)
//   remove<T>(entity)  — O(n) swap-erase from entity list
//   each(func)         — O(groupSize * numTypes) sparse lookups
//
// Invariant maintenance:
//   Hooks onComponentAdded / onComponentRemoved for every listed type.
//   When an entity gains its last missing type, it is appended to mEntities.
//   When it loses any listed type, it is swap-erased from mEntities.
//
// FAT-P components used:
//   - Signal / ScopedConnection : hooks into Registry EventBus
//   - TypedIComponentStore<T>   : typed store interface for sparse lookups

#include <algorithm>
#include <cstddef>
#include <tuple>
#include <vector>

#include <fat_p/Signal.h>

#include "ComponentStore.h"
#include "Entity.h"
#include "EventBus.h"

namespace fatp_ecs
{

// =============================================================================
// INonOwningGroup — type-erased base for Registry storage
// =============================================================================

/// @brief Abstract base so Registry can store NonOwningGroup<Ts...> type-erased.
class INonOwningGroup
{
public:
    virtual ~INonOwningGroup() = default;
    INonOwningGroup() = default;
    INonOwningGroup(const INonOwningGroup&) = delete;
    INonOwningGroup& operator=(const INonOwningGroup&) = delete;
    INonOwningGroup(INonOwningGroup&&) = delete;
    INonOwningGroup& operator=(INonOwningGroup&&) = delete;

    /// @brief Clear tracked entity list without destroying the group object.
    ///
    /// Called by Registry::clear() after all component stores and the entity
    /// allocator have been cleared. Empties mEntities so that each() is a
    /// no-op on a fresh registry. Signal connections are preserved: the group
    /// will re-populate via onComponentAdded as new entities are created.
    virtual void reset() noexcept = 0;
};

// =============================================================================
// NonOwningGroup
// =============================================================================

/**
 * @brief Non-owning group over a fixed set of component types.
 *
 * Created via Registry::non_owning_group<Ts...>(). The Registry owns the
 * group lifetime. Unlike OwningGroup, the same types may be used concurrently
 * in Views, sorts, or other non-owning groups.
 *
 * @tparam Ts Component types to track (1 or more).
 *
 * @example
 * @code
 *   auto& grp = registry.non_owning_group<Position, Velocity>();
 *
 *   grp.each([](Entity e, Position& p, Velocity& v) {
 *       p.x += v.dx;
 *       p.y += v.dy;
 *   });
 * @endcode
 */
template <typename... Ts>
class NonOwningGroup : public INonOwningGroup
{
    static_assert(sizeof...(Ts) >= 1,
                  "NonOwningGroup requires at least one component type.");

public:
    // =========================================================================
    // Construction (called by Registry::non_owning_group<Ts...>())
    // =========================================================================

    explicit NonOwningGroup(TypedIComponentStore<Ts>*... stores, EventBus& events)
        : mStores(stores...)
    {
        seedFromExistingEntities();
        connectSignals(events);
    }

    // Move-only (ScopedConnections are non-copyable).
    NonOwningGroup(const NonOwningGroup&) = delete;
    NonOwningGroup& operator=(const NonOwningGroup&) = delete;
    NonOwningGroup(NonOwningGroup&&) noexcept = default;
    NonOwningGroup& operator=(NonOwningGroup&&) noexcept = default;

    // =========================================================================
    // Reset (called by Registry::clear())
    // =========================================================================

    /// @brief Clear tracked entity list so each() is a no-op after Registry::clear().
    ///
    /// Does NOT disconnect signals or destroy stores. After Registry::clear()
    /// repopulates entities, onComponentAdded will fire and mEntities will
    /// re-fill normally.
    void reset() noexcept override
    {
        mEntities.clear();
    }

    // =========================================================================
    // Iteration
    // =========================================================================

    /**
     * @brief Iterate all group entities.
     *
     * Callback signature: void(Entity, Ts&...)
     *
     * Uses sparse lookups per component per entity. For maximum throughput
     * with 2+ components consider OwningGroup if ownership is acceptable.
     */
    template <typename Func>
    void each(Func&& func)
    {
        for (Entity entity : mEntities)
        {
            invokeOnEntity(std::forward<Func>(func), entity,
                           std::index_sequence_for<Ts...>{});
        }
    }

    template <typename Func>
    void each(Func&& func) const
    {
        for (Entity entity : mEntities)
        {
            invokeOnEntityConst(std::forward<Func>(func), entity,
                                std::index_sequence_for<Ts...>{});
        }
    }

    // =========================================================================
    // Queries
    // =========================================================================

    /// @brief Number of entities currently in the group.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return mEntities.size();
    }

    /// @brief True if no entities are in the group.
    [[nodiscard]] bool empty() const noexcept
    {
        return mEntities.empty();
    }

    /// @brief True if entity is a current member of the group.
    [[nodiscard]] bool contains(Entity entity) const noexcept
    {
        return std::find(mEntities.begin(), mEntities.end(), entity)
               != mEntities.end();
    }

private:
    std::tuple<TypedIComponentStore<Ts>*...> mStores;
    std::vector<Entity>                      mEntities;
    std::vector<fat_p::ScopedConnection>     mConnections;

    // =========================================================================
    // Seeding — populate mEntities from entities already in the stores
    // =========================================================================

    void seedFromExistingEntities()
    {
        // Walk the first store (arbitrary pivot); any entity that has all
        // listed types is eligible.
        auto* pivot       = std::get<0>(mStores);
        const auto& dense = pivot->dense();

        for (Entity entity : dense)
        {
            if (entityHasAllTypes(entity))
            {
                mEntities.push_back(entity);
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

    void onComponentAdded(Entity entity)
    {
        // Guard: already tracked (another owned type was added after the first).
        if (entityTracked(entity))
        {
            return;
        }
        if (entityHasAllTypes(entity))
        {
            mEntities.push_back(entity);
        }
    }

    void onComponentRemoved(Entity entity)
    {
        // Swap-erase for O(1) removal (order of mEntities is unspecified).
        auto it = std::find(mEntities.begin(), mEntities.end(), entity);
        if (it != mEntities.end())
        {
            *it = mEntities.back();
            mEntities.pop_back();
        }
    }

    // =========================================================================
    // Membership helpers
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

    [[nodiscard]] bool entityTracked(Entity entity) const noexcept
    {
        return std::find(mEntities.begin(), mEntities.end(), entity)
               != mEntities.end();
    }

    // =========================================================================
    // Iteration helpers
    // =========================================================================

    template <typename Func, std::size_t... Is>
    void invokeOnEntity(Func&& func, Entity entity, std::index_sequence<Is...>)
    {
        func(entity, *std::get<Is>(mStores)->tryGet(entity)...);
    }

    template <typename Func, std::size_t... Is>
    void invokeOnEntityConst(Func&& func, Entity entity,
                             std::index_sequence<Is...>) const
    {
        func(entity, *std::get<Is>(mStores)->tryGet(entity)...);
    }
};

} // namespace fatp_ecs
