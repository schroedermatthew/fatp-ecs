#pragma once

/**
 * @file EventBus.h
 * @brief Signal-based event system for entity and component lifecycle.
 */

// FAT-P components used:
// - Signal: Observer pattern with SmallVector-backed slot storage
//   - ScopedConnection: RAII connection lifetime management
// - FastHashMap: Type-erased storage for per-component-type signals
//
// Entity signals (onEntityCreated, onEntityDestroyed) are direct members.
// Component signals (onComponentAdded<T>, onComponentRemoved<T>,
// onComponentUpdated<T>) are stored type-erased in a FastHashMap keyed by
// TypeId, lazily created on first listener connection to avoid overhead for
// unobserved component types.

#include <cstdint>
#include <memory>

#include <fat_p/FastHashMap.h>
#include <fat_p/Signal.h>

#include "Entity.h"
#include "TypeId.h"

namespace fatp_ecs
{

// =============================================================================
// Type-Erased Signal Interface
// =============================================================================

/// @brief Abstract base for type-erased component event signals.
class IComponentSignalPair
{
public:
    virtual ~IComponentSignalPair() = default;
    IComponentSignalPair() = default;
    IComponentSignalPair(const IComponentSignalPair&) = delete;
    IComponentSignalPair& operator=(const IComponentSignalPair&) = delete;
    IComponentSignalPair(IComponentSignalPair&&) = delete;
    IComponentSignalPair& operator=(IComponentSignalPair&&) = delete;
};

/**
 * @brief Concrete signal pair for component type T.
 *
 * @tparam T The component type.
 */
template <typename T>
class ComponentSignalPair : public IComponentSignalPair
{
public:
    fat_p::Signal<void(Entity, T&)> onAdded;
    fat_p::Signal<void(Entity, T&)> onUpdated;
    fat_p::Signal<void(Entity)>     onRemoved;
};

// =============================================================================
// EventBus
// =============================================================================

// The EventBus provides entity-level signals as direct public members and
// component-level signals via template accessors. Component signals are
// lazily allocated: emitComponentAdded/Removed check for an existing signal
// pair before emitting, so types nobody listens to incur no allocation.

/// @brief Central event hub for ECS lifecycle events.
class EventBus
{
public:
    EventBus() = default;
    ~EventBus() = default;

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    EventBus(EventBus&&) noexcept = default;
    EventBus& operator=(EventBus&&) noexcept = default;

    // =========================================================================
    // Entity Lifecycle Signals
    // =========================================================================

    /// @brief Fired when a new entity is created. Signature: void(Entity).
    fat_p::Signal<void(Entity)> onEntityCreated;

    /// @brief Fired before an entity is destroyed. Signature: void(Entity).
    fat_p::Signal<void(Entity)> onEntityDestroyed;

    // =========================================================================
    // Component Event Access
    // =========================================================================

    /**
     * @brief Returns the onComponentAdded signal for type T.
     *
     * @tparam T The component type.
     * @return Reference to the signal. Created lazily if needed.
     */
    template <typename T>
    fat_p::Signal<void(Entity, T&)>& onComponentAdded()
    {
        return ensureSignalPair<T>()->onAdded;
    }

    /**
     * @brief Returns the onComponentRemoved signal for type T.
     *
     * @tparam T The component type.
     * @return Reference to the signal. Created lazily if needed.
     */
    template <typename T>
    fat_p::Signal<void(Entity)>& onComponentRemoved()
    {
        return ensureSignalPair<T>()->onRemoved;
    }

    /**
     * @brief Returns the onComponentUpdated signal for type T.
     *
     * Fired by Registry::patch<T>() after the component has been modified
     * in-place. Signature: void(Entity, T&).
     *
     * @tparam T The component type.
     * @return Reference to the signal. Created lazily if needed.
     */
    template <typename T>
    fat_p::Signal<void(Entity, T&)>& onComponentUpdated()
    {
        return ensureSignalPair<T>()->onUpdated;
    }

    // =========================================================================
    // Internal: Emit Helpers (called by Registry)
    // =========================================================================

    /// @brief Emit component-added event if listeners exist for type T.
    template <typename T>
    void emitComponentAdded(Entity entity, T& component)
    {
        auto* pair = getSignalPair<T>();
        if (pair != nullptr)
        {
            pair->onAdded.emit(entity, component);
        }
    }

    /// @brief Emit component-removed event if listeners exist for type T.
    template <typename T>
    void emitComponentRemoved(Entity entity)
    {
        auto* pair = getSignalPair<T>();
        if (pair != nullptr)
        {
            pair->onRemoved.emit(entity);
        }
    }

    /// @brief Emit component-updated event if listeners exist for type T.
    template <typename T>
    void emitComponentUpdated(Entity entity, T& component)
    {
        auto* pair = getSignalPair<T>();
        if (pair != nullptr)
        {
            pair->onUpdated.emit(entity, component);
        }
    }

private:
    template <typename T>
    ComponentSignalPair<T>* ensureSignalPair()
    {
        const TypeId tid = typeId<T>();

        auto* existing = mSignals.find(tid);
        if (existing != nullptr)
        {
            auto* raw = static_cast<ComponentSignalPair<T>*>(existing->get());
            if (tid < kSignalCacheSize)
            {
                mSignalCache[tid] = raw;
            }
            return raw;
        }

        auto pair = std::make_unique<ComponentSignalPair<T>>();
        auto* raw = pair.get();
        mSignals.insert(tid, std::move(pair));
        if (tid < kSignalCacheSize)
        {
            mSignalCache[tid] = raw;
        }
        return raw;
    }

    template <typename T>
    ComponentSignalPair<T>* getSignalPair()
    {
        const TypeId tid = typeId<T>();

        // Fast path: flat cache. kAbsent sentinel means we already confirmed
        // no pair exists for this type — skip the FastHashMap lookup entirely.
        if (tid < kSignalCacheSize)
        {
            auto* cached = mSignalCache[tid];
            if (cached == kAbsentSentinel())
            {
                return nullptr;
            }
            if (cached != nullptr)
            {
                return static_cast<ComponentSignalPair<T>*>(cached);
            }
        }

        auto* val = mSignals.find(tid);
        if (val == nullptr)
        {
            // Cache the negative result so future calls skip the hash lookup.
            if (tid < kSignalCacheSize)
            {
                mSignalCache[tid] = kAbsentSentinel();
            }
            return nullptr;
        }
        auto* raw = static_cast<ComponentSignalPair<T>*>(val->get());
        if (tid < kSignalCacheSize)
        {
            mSignalCache[tid] = raw;
        }
        return raw;
    }

    // Returns a sentinel pointer value meaning "looked up, confirmed absent."
    // Reinterprets 0x1 as a pointer — never a valid object address.
    // Used to distinguish "not yet cached" (nullptr) from "cached: no pair" (kAbsentSentinel).
    static IComponentSignalPair* kAbsentSentinel() noexcept
    {
        return reinterpret_cast<IComponentSignalPair*>(std::uintptr_t{1});
    }

    static constexpr std::size_t kSignalCacheSize = 64;

    fat_p::FastHashMap<TypeId, std::unique_ptr<IComponentSignalPair>> mSignals;

    // Flat cache indexed by TypeId. Three states:
    //   nullptr           — not yet looked up for this TypeId
    //   kAbsentSentinel() — looked up, no signal pair registered
    //   other pointer     — pointer to the live ComponentSignalPair<T>
    //
    // When a listener is first connected (ensureSignalPair), the cache entry
    // is updated from kAbsentSentinel to the real pointer, so subsequent
    // emitComponentAdded calls will find it.
    std::array<IComponentSignalPair*, kSignalCacheSize> mSignalCache{};
};

} // namespace fatp_ecs
