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
// Component signals (onComponentAdded<T>, onComponentRemoved<T>) are stored
// type-erased in a FastHashMap keyed by TypeId, lazily created on first
// listener connection to avoid overhead for unobserved component types.

#include <memory>

#include <fat_p/FastHashMap.h>
#include <fat_p/Signal.h>

#include "Entity.h"

namespace fatp_ecs
{

// Forward declarations — typeId<T>() is defined in Registry.h
using TypeId = std::size_t;

template <typename T>
TypeId typeId() noexcept;

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
    fat_p::Signal<void(Entity)> onRemoved;
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

private:
    template <typename T>
    ComponentSignalPair<T>* ensureSignalPair()
    {
        const TypeId tid = typeId<T>();

        auto* existing = mSignals.find(tid);
        if (existing != nullptr)
        {
            return static_cast<ComponentSignalPair<T>*>(existing->get());
        }

        auto pair = std::make_unique<ComponentSignalPair<T>>();
        auto* raw = pair.get();
        mSignals.insert(tid, std::move(pair));
        return raw;
    }

    template <typename T>
    ComponentSignalPair<T>* getSignalPair()
    {
        const TypeId tid = typeId<T>();
        auto* val = mSignals.find(tid);
        if (val == nullptr)
        {
            return nullptr;
        }
        return static_cast<ComponentSignalPair<T>*>(val->get());
    }

    fat_p::FastHashMap<TypeId, std::unique_ptr<IComponentSignalPair>> mSignals;
};

} // namespace fatp_ecs
