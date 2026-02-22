#pragma once

/**
 * @file Observer.h
 * @brief Reactive observer that accumulates entities dirtied by component
 *        lifecycle events between frames.
 */

// Overview:
//
// An Observer watches a set of component signals and accumulates the entities
// that become "dirty" — those whose component state changed since the observer
// was last cleared. Systems that need to react to change (physics, AI, render
// proxies) consume the dirty set each frame and call clear() when done.
//
// Trigger types available via the fluent builder on Registry::observe():
//   OnAdded<T>    — entity enters dirty set when component T is added
//   OnRemoved<T>  — entity enters dirty set when component T is removed
//   OnUpdated<T>  — entity enters dirty set when component T is patched
//
// The dirty set is a SparseSet<Entity, EntityIndex>, which:
//   - Deduplicates naturally: entity patched 10× this frame appears once
//   - Provides O(1) insert and O(1) remove (for entity-destroyed cleanup)
//   - Iterates densely (cache-friendly)
//
// Stale entity cleanup: the Observer connects to onEntityDestroyed and removes
// destroyed entities from the dirty set immediately. Users always iterate a
// clean, live entity set.
//
// Lifetime: the Observer holds ScopedConnections. Destroying the Observer
// automatically disconnects all signal listeners — no manual cleanup needed.
//
// FAT-P components used:
//   - SparseSet<Entity, EntityIndex>: dirty set storage
//   - Signal / ScopedConnection: reactive wiring to EventBus

#include <cstddef>
#include <functional>

#include <fat_p/SparseSet.h>
#include <fat_p/Signal.h>

#include "Entity.h"
#include "EventBus.h"
#include "TypeId.h"

namespace fatp_ecs
{

class Registry;

// =============================================================================
// Trigger tag types
// =============================================================================

/// @brief Trigger: dirty when component T is added to an entity.
template <typename T>
struct OnAdded {};

/// @brief Trigger: dirty when component T is removed from an entity.
template <typename T>
struct OnRemoved {};

/// @brief Trigger: dirty when component T is patched (updated in-place).
template <typename T>
struct OnUpdated {};

// =============================================================================
// Observer
// =============================================================================

/**
 * @brief Accumulates entities dirtied by component lifecycle events.
 *
 * Created via Registry::observe<Triggers...>() where Triggers is a mix of
 * OnAdded<T>, OnRemoved<T>, and OnUpdated<T> tags.
 *
 * @example
 * @code
 *   // Accumulate entities whose Position changed (added or patched):
 *   auto obs = registry.observe(OnAdded<Position>{}, OnUpdated<Position>{});
 *
 *   // Each frame:
 *   obs.each([](Entity e) { rebuildSpatialHash(e); });
 *   obs.clear();
 *
 *   // Or consume-and-clear in one pass:
 *   obs.each([](Entity e) { ... });
 *   obs.clear();
 * @endcode
 *
 * @note Thread-safety: NOT thread-safe. Use on a single thread.
 */
class Observer
{
public:
    /// @brief Functor type called for each dirty entity.
    using EachFn = std::function<void(Entity)>;

    Observer() = default;

    // Move-only: connections are non-copyable.
    Observer(const Observer&) = delete;
    Observer& operator=(const Observer&) = delete;
    Observer(Observer&&) noexcept = default;
    Observer& operator=(Observer&&) noexcept = default;

    // =========================================================================
    // Iteration
    // =========================================================================

    /**
     * @brief Call func for each entity in the dirty set.
     *
     * Does not clear the set — call clear() explicitly when done.
     */
    void each(EachFn func) const
    {
        for (Entity entity : mDirty)
        {
            func(entity);
        }
    }

    /**
     * @brief Number of dirty entities accumulated since last clear().
     */
    [[nodiscard]] std::size_t count() const noexcept
    {
        return mDirty.size();
    }

    /**
     * @brief True if no entities are dirty.
     */
    [[nodiscard]] bool empty() const noexcept
    {
        return mDirty.empty();
    }

    /**
     * @brief Clear the dirty set. Call at the end of each frame after processing.
     */
    void clear()
    {
        mDirty.clear();
    }

    // =========================================================================
    // Internal: connection builders (called by Registry::observe())
    // =========================================================================

    /// @brief Wire to onComponentAdded<T> — mark entity dirty when T is added.
    template <typename T>
    void connectAdded(EventBus& events)
    {
        mConnections.push_back(
            events.onComponentAdded<T>().connect(
                [this](Entity entity, T&) { markDirty(entity); }));
    }

    /// @brief Wire to onComponentRemoved<T> — mark entity dirty when T is removed.
    template <typename T>
    void connectRemoved(EventBus& events)
    {
        mConnections.push_back(
            events.onComponentRemoved<T>().connect(
                [this](Entity entity) { markDirty(entity); }));
    }

    /// @brief Wire to onComponentUpdated<T> — mark entity dirty when T is patched.
    template <typename T>
    void connectUpdated(EventBus& events)
    {
        mConnections.push_back(
            events.onComponentUpdated<T>().connect(
                [this](Entity entity, T&) { markDirty(entity); }));
    }

    /// @brief Wire to onEntityDestroyed — remove destroyed entities from dirty set.
    void connectEntityDestroyed(EventBus& events)
    {
        mConnections.push_back(
            events.onEntityDestroyed.connect(
                [this](Entity entity) { mDirty.erase(entity); }));
    }

private:
    fat_p::SparseSet<Entity, EntityIndex> mDirty;

    // SmallVector would be ideal here, but Signal's ScopedConnection is
    // non-copyable and non-movable in some implementations. Use std::vector
    // with reserve() in Registry::observe() to avoid reallocations.
    std::vector<fat_p::ScopedConnection> mConnections;

    void markDirty(Entity entity)
    {
        mDirty.insert(entity);
    }
};

} // namespace fatp_ecs
